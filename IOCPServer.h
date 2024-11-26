#pragma once
#include <winsock2.h>
#include <windows.h>
#include <mswsock.h>
#include <iostream>
#include <string>
#include <memory>
#include <Mswsock.h>
#include "ThreadPool.h"

#pragma comment(lib, "Ws2_32.lib")  // 确保链接基础的 Winsock 库
#pragma comment(lib, "Mswsock.lib") // 链接 Mswsock 库以解决 AcceptEx 的符号

#define BUFFER_SIZE 1024


// 每个连接的上下文
struct PER_IO_DATA {
	OVERLAPPED overlapped;
	WSABUF wsaBuf;
	char buffer[BUFFER_SIZE];
	int operation; // 0: recv, 1: send
	SOCKET socket;
};

struct PER_SOCKET_CONTEXT {
	SOCKET socket;
};

class IOCPServer
{
public:
	IOCPServer(u_short port = 8080);
	int start();
private:
	static ThreadPool* g_pool;

	HANDLE m_hIOCP;
	u_short m_port;
	SOCKET m_listenSocket;

	void AcceptConnection();
	void IOWorkerThread();
	void HandleIOCompletion(DWORD bytesTransferred, PER_SOCKET_CONTEXT* pContext, PER_IO_DATA* pIoData);
};

