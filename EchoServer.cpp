#include "IOCPServer.h"

int main() {
	IOCPServer server(8080);
	server.start();

	return 0;
}
