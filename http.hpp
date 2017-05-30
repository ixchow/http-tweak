#pragma once

/*
 * Single-header-file HTTP 1.1 server.
 *  with 'no fancy stuff'
 *
 * Usage:
 * 
 * http::server server;
 *
 * server.handle_request = [](http::request const &request, std::unique_ptr< http::response > response){
 *   if (request.url == '/') {
 *       if (request.method
 *       response.body = ;
 *   }
 *   -> 'response' is sent to client when destroyed, so std::move() it if you want to leave response outstanding for a while
 * };
 *
 * server.listen(port);
 *
 * Thread-safety:
 *  - server doesn't use threads; it's select()-based.
 *  - server will call handle_request from the thread that called 'listen'
 *  - it is safe to pass ownership of a response to another thread (as long as you make sure it doesn't outlive the server)
 *  - it is safe to call server.stop() from another thread. (again, make sure the other thread's reference doesn't outlive the server)
 *
 *
 */

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <cassert>
#include <condition_variable>
#include <list>
#include <iostream>

#include <cstring> //for memset()

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#include <winsock2.h>
#undef max
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <unistd.h>

#define closesocket close
#endif


namespace http {

struct internals;
struct message;
struct server;
struct client;

//----------------- public interface -------------------

struct request {
	std::string method;
	std::string url;
	std::vector< std::pair< std::string, std::string > > headers;
	std::string body;
};

struct response {
	struct {
		int code = 200;
		std::string message = "OK";
	} status;
	std::vector< std::pair< std::string, std::string > > headers;
	//NOTE: 'Content-Length:' header will be automatically set based on body.
	std::string body;

	//------ internals ------
	std::weak_ptr< message > weak_message;
	server *owner;
	~response();
};

#ifdef _WIN32
typedef SOCKET socket;
//typedef int ssize_t; //maybe?
#else
typedef int socket;
const socket INVALID_SOCKET = -1;
#endif

struct server {
	std::function< void(request &, std::unique_ptr< response > response) > handle_request;
	void listen(uint16_t port);
	void stop();

	//------- internals -------
	std::mutex mutex;
	bool quit_flag = false;
	bool running = false;
	std::condition_variable cv; //used to block/notify threads calling
	socket wake_socket = INVALID_SOCKET; //used to wake from select() as needed

	void wake(); //used to poke server when blocking on select()

	std::list< client > clients;
};


//------ internals ------


struct message {
	bool ready = false;
	std::string data;
	std::shared_ptr< message > next;
};

struct client {
	int socket;
	std::shared_ptr< message > first_message;
};

inline void server::listen(uint16_t port) {
	std::unique_lock< std::mutex > lock(mutex);
	if (quit_flag) return; //don't bother starting up if already instructed to quit
	running = true;

	int listen_socket = -1;

	#ifdef _WIN32
	static bool inited = [](){
		WSADATA info;
		int ret = WSAStartup((2 << 8) | 2, &info);
		if (ret != 0) {
			return false;
		} else {
			return true;
		}
	}();
	if (!inited) {
		std::cerr << "[server::listen] ERROR: WSAStartup failed." << std::endl;
		return;
	}
	#endif

	//----------- socket setup -----------
	{ //create socket:
		listen_socket = ::socket(AF_INET, SOCK_STREAM, 0);
		if (listen_socket == -1) {
			std::cerr << "Failed to create socket." << std::endl;
			return;
		}
	}
	{ //make it okay to reuse port:
	#ifdef _WIN32
		BOOL one = TRUE;
		int ret = setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast< const char * >(&one), sizeof(one));
	#else
		int one = 1;
		int ret = setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	#endif
		if (ret != 0) {
			std::cerr << "[server::listen] ERROR: Failed to set socket reuse address." << std::endl;
		}
	}
	{ //bind to address
		struct sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		int ret = bind(listen_socket, reinterpret_cast< sockaddr * >(&addr), sizeof(addr));
		if (ret < 0) {
			#ifdef WINDOWS
			static char buffer[1000];
			if (0 != strerror_s(buffer, errno)) {
				buffer[0] = '\0';
			}
			std::cerr << "[server::listen] Failed to bind socket (" << buffer << ")" << std::endl;
			#else
			std::cerr << "[server::listen] Failed to bind socket (" << strerror(errno) << ")" << std::endl;
			#endif
			closesocket(listen_socket);
			listen_socket = INVALID_SOCKET;
			return;
		}
	}

	std::cout << "[server::listen] Server started at localhost:" << port << "." << std::endl;


	//----------- end socket setup -----------


	//TODO: set up port and such.

	while (!quit_flag) {

		for (auto c : clients) {
			//TODO: add to read fds, error fds
			if (c.first_message && c.first_message->ready) {
				//TODO: add to write fds
			}
		}
		lock.unlock();

		//TODO: select()

		lock.lock();

		//TODO: figure out what can be written to or read from.


	}
	running = false;
	cv.notify_all();
}

inline void server::stop() {
	std::unique_lock< std::mutex > lock(mutex);
	quit_flag = true;
	//if stop() is called during listen(), waits for listen() to quit, otherwise returns immediately:
	if (running) {
		cv.wait(lock);
		assert(!running);
	}
}

inline void server::wake() {
	//TODO: poke wake_socket with one byte or a datagram or something
}

inline response::~response() {
	//does the message slot still exist?
	std::shared_ptr< message > strong_message = weak_message.lock();
	if (!strong_message) return;

	//if so, build the message:
	std::string message;
	message += "HTTP/1.1 " + std::to_string(status.code) + " " + status.message + "\r\n";
	for (auto const &kv : headers) {
		message += kv.first + ": " + kv.second + "\r\n";
	}
	message += "Content-Length: " + std::to_string(body.size()) + "\r\n";
	message += "\r\n";
	message += body;

	{ //copy to send queue:
		std::unique_lock< std::mutex > lock(owner->mutex);
		strong_message->data = std::move(message);
		strong_message->ready = true;
	}
	//wake server:
	owner->wake();
}


} //namespace http
