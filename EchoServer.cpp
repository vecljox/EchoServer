// EchoServer.cpp

#include <winsock2.h>
#include <windows.h>
#include <mswsock.h>
#include <iostream>
#include <string>
#include "ThreadPool.h"

#pragma comment(lib, "Ws2_32.lib")

#define BUFFER_SIZE 1024

// 每个连接的上下文
struct PER_IO_DATA {
	OVERLAPPED overlapped;
	WSABUF wsaBuf;
	char buffer[BUFFER_SIZE];
	int operation; // 0: recv, 1: send
};

struct PER_SOCKET_CONTEXT {
	SOCKET socket;
};

// 全局IOCP句柄
HANDLE g_hIOCP = NULL;

// 线程池引用
ThreadPool* g_pool = nullptr;

// 接受连接的函数
void AcceptConnection(SOCKET listenSocket) {
	SOCKET acceptSocket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (acceptSocket == INVALID_SOCKET) {
		std::cerr << "Failed to create accept socket: " << WSAGetLastError() << "\n";
		return;
	}

	// 分配上下文
	PER_SOCKET_CONTEXT* pContext = new PER_SOCKET_CONTEXT();
	pContext->socket = acceptSocket;

	PER_IO_DATA* pIoData = new PER_IO_DATA();
	ZeroMemory(pIoData, sizeof(PER_IO_DATA));

	// 准备接受操作
	pIoData->operation = 0; // recv

	pIoData->wsaBuf.len = BUFFER_SIZE;
	pIoData->wsaBuf.buf = pIoData->buffer;

	// 将新套接字与IOCP关联，如果失败则关闭套接字
	if (CreateIoCompletionPort((HANDLE)acceptSocket, g_hIOCP, (ULONG_PTR)pContext, 0) == NULL) {
		std::cerr << "CreateIoCompletionPort failed: " << GetLastError() << "\n";
		closesocket(acceptSocket);
		delete pIoData;
		delete pContext;
		return;
	}

	// 开始异步接收
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
}

// 处理完成的I/O操作
void HandleIOCompletion(DWORD bytesTransferred, PER_SOCKET_CONTEXT* pContext, PER_IO_DATA* pIoData) {
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

		// 分配发送操作的 OVERLAPPED 结构
		ZeroMemory(&pIoData->overlapped, sizeof(OVERLAPPED));

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

// 线程池中的工作线程函数，负责等待IOCP并处理完成的I/O
void IOWorkerThread() {
	while (true) {
		DWORD bytesTransferred;
		ULONG_PTR completionKey;
		LPOVERLAPPED lpOverlapped;

		BOOL status = GetQueuedCompletionStatus(
			g_hIOCP,
			&bytesTransferred,
			&completionKey,
			&lpOverlapped,
			INFINITE
		);

		if (!status) {
			std::cerr << "GetQueuedCompletionStatus failed: " << GetLastError() << "\n";
			continue;
		}

		if (completionKey == NULL) {
			// 监听套接字有新的连接，需要接受
			AcceptConnection((SOCKET)completionKey); // 这里completionKey为NULL，应该传listenSocket
			// 需要调整，这里需要传递listenSocket
			// 先省略，后面修正
			continue;
		}

		PER_SOCKET_CONTEXT* pContext = (PER_SOCKET_CONTEXT*)completionKey;
		PER_IO_DATA* pIoData = (PER_IO_DATA*)lpOverlapped;

		HandleIOCompletion(bytesTransferred, pContext, pIoData);
	}
}

int main() {
	// 初始化 Winsock
	WSADATA wsaData;
	int iResult;
	iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (iResult != 0) {
		std::cerr << "WSAStartup failed: " << iResult << "\n";
		return 1;
	}

	// 创建监听套接字
	SOCKET listenSocket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (listenSocket == INVALID_SOCKET) {
		std::cerr << "Failed to create listen socket: " << WSAGetLastError() << "\n";
		WSACleanup();
		return 1;
	}

	// 设置为非阻塞模式（可选）
	// u_long iMode = 1;
	// ioctlsocket(listenSocket, FIONBIO, &iMode);

	// 绑定地址
	sockaddr_in serverAddr;
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons(8080); // 监听端口 8080
	serverAddr.sin_addr.s_addr = INADDR_ANY;

	if (bind(listenSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
		std::cerr << "Bind failed: " << WSAGetLastError() << "\n";
		closesocket(listenSocket);
		WSACleanup();
		return 1;
	}

	// 开始监听
	if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
		std::cerr << "Listen failed: " << WSAGetLastError() << "\n";
		closesocket(listenSocket);
		WSACleanup();
		return 1;
	}

	// 创建IO完成端口
	g_hIOCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	if (g_hIOCP == NULL) {
		std::cerr << "CreateIoCompletionPort failed: " << GetLastError() << "\n";
		closesocket(listenSocket);
		WSACleanup();
		return 1;
	}

	// 将监听套接字与IOCP关联
	if (CreateIoCompletionPort((HANDLE)listenSocket, g_hIOCP, (ULONG_PTR)NULL, 0) == NULL) {
		std::cerr << "CreateIoCompletionPort failed: " << GetLastError() << "\n";
		CloseHandle(g_hIOCP);
		closesocket(listenSocket);
		WSACleanup();
		return 1;
	}

	// 创建线程池
	ThreadPool pool(std::thread::hardware_concurrency());
	g_pool = &pool;

	// 启动线程池中的每个线程作为IOCP的工作线程
	size_t numThreads = std::thread::hardware_concurrency();
	for (size_t i = 0; i < numThreads; ++i) {
		pool.enqueue(IOWorkerThread);
	}

	// 主线程专门负责接受新的连接
	std::cout << "Echo server is running on port 8080...\n";

	// 主线程循环，接受新的连接
	while (true) {
		SOCKET clientSocket = accept(listenSocket, NULL, NULL);
		if (clientSocket == INVALID_SOCKET) {
			int err = WSAGetLastError();
			if (err == WSAEWOULDBLOCK) {
				// 没有新的连接，继续
				continue;
			}
			std::cerr << "Accept failed: " << err << "\n";
			break;
		}

		// 分配上下文
		PER_SOCKET_CONTEXT* pContext = new PER_SOCKET_CONTEXT();
		pContext->socket = clientSocket;

		PER_IO_DATA* pIoData = new PER_IO_DATA();
		// ZeroMemory(pIoData, sizeof(PER_IO_DATA));

		// 将新套接字与IOCP关联
		if (CreateIoCompletionPort((HANDLE)clientSocket, g_hIOCP, (ULONG_PTR)pContext, 0) == NULL) {
			std::cerr << "CreateIoCompletionPort failed: " << GetLastError() << "\n";
			closesocket(clientSocket);
			delete pIoData;
			delete pContext;
			continue;
		}

		// 开始异步接收
		pIoData->operation = 0; // recv
		pIoData->wsaBuf.len = BUFFER_SIZE;
		pIoData->wsaBuf.buf = pIoData->buffer;

		DWORD flags = 0;
		DWORD bytesRecv = 0;
		if (WSARecv(clientSocket, &pIoData->wsaBuf, 1, &bytesRecv, &flags,
			&pIoData->overlapped, NULL) == SOCKET_ERROR) {
			if (WSAGetLastError() != WSA_IO_PENDING) {
				std::cerr << "WSARecv failed: " << WSAGetLastError() << "\n";
				closesocket(clientSocket);
				delete pIoData;
				delete pContext;
				continue;
			}
		}
	}

	// 清理资源（永远不会到达这里）
	CloseHandle(g_hIOCP);
	closesocket(listenSocket);
	WSACleanup();

	return 0;
}
