#include "IOCPServer.h"

ThreadPool* IOCPServer::g_pool = nullptr;

void IOCPServer::AcceptConnection()
{
	SOCKET acceptSocket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (acceptSocket == INVALID_SOCKET) {
		std::cerr << "Failed to create accept socket: " << WSAGetLastError() << "\n";
		return;
	}

	DWORD bytesReceived;
	char outputBuffer[2 * (sizeof(sockaddr_in) + 16)]{};
	PER_IO_DATA* pIoData = new PER_IO_DATA();
	pIoData->socket = acceptSocket;
	if (AcceptEx(m_listenSocket, acceptSocket, outputBuffer, 0,
		sizeof(sockaddr_in) + 16, sizeof(sockaddr_in) + 16,
		&bytesReceived, &pIoData->overlapped)) {
		// 连接成功
	}
	else {
		DWORD dwError = GetLastError();
		if (dwError != ERROR_IO_PENDING) {
			// 处理错误
			delete pIoData;
			return;
		}
	}
}

int IOCPServer::start()
{
	// 初始化 Winsock
	WSADATA wsaData;
	int iResult;
	iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (iResult != 0) {
		std::cerr << "WSAStartup failed: " << iResult << "\n";
		return 1;
	}

	// 创建监听套接字
	m_listenSocket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (m_listenSocket == INVALID_SOCKET) {
		std::cerr << "Failed to create listen socket: " << WSAGetLastError() << "\n";
		WSACleanup();
		return 1;
	}

	// 绑定地址
	sockaddr_in serverAddr;
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons(m_port);
	serverAddr.sin_addr.s_addr = INADDR_ANY;

	if (bind(m_listenSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
		std::cerr << "Bind failed: " << WSAGetLastError() << "\n";
		closesocket(m_listenSocket);
		WSACleanup();
		return 1;
	}

	// 开始监听
	if (listen(m_listenSocket, SOMAXCONN) == SOCKET_ERROR) {
		std::cerr << "Listen failed: " << WSAGetLastError() << "\n";
		closesocket(m_listenSocket);
		WSACleanup();
		return 1;
	}

	// 创建IO完成端口
	m_hIOCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	if (m_hIOCP == NULL) {
		std::cerr << "CreateIoCompletionPort failed: " << GetLastError() << "\n";
		closesocket(m_listenSocket);
		WSACleanup();
		return 1;
	}

	// 将监听套接字与IOCP关联
	PER_SOCKET_CONTEXT* pContext = new PER_SOCKET_CONTEXT();
	pContext->socket = m_listenSocket;

	if (CreateIoCompletionPort((HANDLE)m_listenSocket, m_hIOCP, (ULONG_PTR)pContext, 0) == NULL) {
		std::cerr << "CreateIoCompletionPort failed: " << GetLastError() << "\n";
		CloseHandle(m_hIOCP);
		closesocket(m_listenSocket);
		WSACleanup();
		return 1;
	}

	// 创建线程池
	ThreadPool pool(std::thread::hardware_concurrency());
	g_pool = &pool;

	// 启动线程池中的每个线程作为IOCP的工作线程
	size_t numThreads = std::thread::hardware_concurrency();
	for (size_t i = 0; i < numThreads; ++i) {
		//pool.enqueue(IOWorkerThread);
		pool.enqueue(
			[this]() {
				IOWorkerThread();
			}
		);
	}

	// 主线程专门负责接受新的连接
	char buffer[50];
	sprintf_s(buffer, "IOCP server is running on port %d...\n", m_port);
	std::cout << buffer;
	AcceptConnection();

	getchar();

	//CloseHandle(m_hIOCP);
	//closesocket(m_listenSocket);
	//WSACleanup();
	return 0;
}


IOCPServer::IOCPServer(u_short port)
	:m_port{ port }, m_hIOCP{ NULL }
{
}

void IOCPServer::IOWorkerThread()
{
	while (true) {
		DWORD bytesTransferred;
		ULONG_PTR completionKey;
		LPOVERLAPPED lpOverlapped;

		BOOL status = GetQueuedCompletionStatus(
			m_hIOCP,
			&bytesTransferred,
			&completionKey,
			&lpOverlapped,
			INFINITE
		);

		if (!status) {
			std::cerr << "GetQueuedCompletionStatus failed: " << GetLastError() << "\n";
			continue;
		}

		PER_SOCKET_CONTEXT* pContext = (PER_SOCKET_CONTEXT*)completionKey;
		PER_IO_DATA* pIoData = (PER_IO_DATA*)lpOverlapped;

		HandleIOCompletion(bytesTransferred, pContext, pIoData);
	}
}

void IOCPServer::HandleIOCompletion(DWORD bytesTransferred, PER_SOCKET_CONTEXT* pContext, PER_IO_DATA* pIoData)
{
	if (pContext->socket == m_listenSocket) {
		SOCKET acceptSocket = pIoData->socket;

		// 分配上下文
		PER_SOCKET_CONTEXT* pContext = new PER_SOCKET_CONTEXT();
		pContext->socket = acceptSocket;

		// 将新套接字与IOCP关联，如果失败则关闭套接字
		if (CreateIoCompletionPort((HANDLE)acceptSocket, m_hIOCP, (ULONG_PTR)pContext, 0) == NULL) {
			std::cerr << "CreateIoCompletionPort failed: " << GetLastError() << "\n";
			closesocket(acceptSocket);
			//delete pIoData;
			delete pContext;
			return;
		}

		PER_IO_DATA* pIoData = new PER_IO_DATA();
		pIoData->operation = 0; // recv
		pIoData->wsaBuf.len = BUFFER_SIZE;
		pIoData->wsaBuf.buf = pIoData->buffer;
		DWORD flags = 0;
		DWORD bytesRecv = 0;
		if (WSARecv(acceptSocket, &pIoData->wsaBuf, 1, &bytesRecv, &flags,
			&pIoData->overlapped, NULL) == SOCKET_ERROR) {
			if (WSAGetLastError() != WSA_IO_PENDING) {
				std::cerr << "WSARecv failed: " << WSAGetLastError() << "\n";
				closesocket(acceptSocket);
				delete pIoData;
				delete pContext;
				return;
			}
		}

		AcceptConnection();
		return;
	}

	if (bytesTransferred == 0) {
		// 客户端关闭连接
		closesocket(pContext->socket);
		delete pIoData;
		delete pContext;
		return;
	}

	if (pIoData->operation == 0) { // recv
		// 将接收到的数据回显给客户端
		pIoData->operation = 1; // 设置为发送操作
		pIoData->wsaBuf.len = bytesTransferred;
		//pIoData->wsaBuf.buf = pIoData->buffer;

		//// 分配发送操作的 OVERLAPPED 结构
		//ZeroMemory(&pIoData->overlapped, sizeof(OVERLAPPED));

		// 开始异步发送
		DWORD bytesSent = 0;
		if (WSASend(pContext->socket, &pIoData->wsaBuf, 1, &bytesSent, 0,
			&pIoData->overlapped, NULL) == SOCKET_ERROR) {
			if (WSAGetLastError() != WSA_IO_PENDING) {
				std::cerr << "WSASend failed: " << WSAGetLastError() << "\n";
				closesocket(pContext->socket);
				delete pIoData;
				delete pContext;
				return;
			}
		}
	}
	else if (pIoData->operation == 1) { // send
		// 完成发送后，重新开始接收
		pIoData->operation = 0; // 设置为接收操作
		pIoData->wsaBuf.len = BUFFER_SIZE;
		pIoData->wsaBuf.buf = pIoData->buffer;

		// 分配接受操作的 OVERLAPPED 结构
		ZeroMemory(&pIoData->overlapped, sizeof(OVERLAPPED));

		// 开始异步接收
		DWORD flags = 0;
		DWORD bytesRecv = 0;
		if (WSARecv(pContext->socket, &pIoData->wsaBuf, 1, &bytesRecv, &flags,
			&pIoData->overlapped, NULL) == SOCKET_ERROR) {
			if (WSAGetLastError() != WSA_IO_PENDING) {
				std::cerr << "WSARecv failed: " << WSAGetLastError() << "\n";
				closesocket(pContext->socket);
				delete pIoData;
				delete pContext;
				return;
			}
		}
	}
}
