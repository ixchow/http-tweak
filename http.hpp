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

struct incoming_request : request {
	//state == line curently being recv'd
	enum {
		RequestLine,
		HeaderLine,
		Body,
	} mode = RequestLine;

	uint32_t content_remains = 0;
	std::string line = "";
	void reset() {
		*this = incoming_request();
	}
	bool parse_bytes(char const *begin, ssize_t count, std::function< void() > const &on_finish) {
		for (char const *c = begin; c < begin + count; ++c) {
			if (mode == RequestLine || mode == HeaderLine) {
				line += *c;
				//at end-of-line, do something:
				if (line.size() > 2 && line[line.size() - 2] == '\r' && line[line.size()-1] == '\n') {
					line.erase(line.size()-2); //trim CRLF
					if (mode == RequestLine) {
						if (line.size() == 0) {
							//ignore empty line before request
						} else {
							//TODO: parse "method SP url SP version"
						}
					} else { assert(mode == HeaderLine);
						if (line.size() == 0) {
							//empty line between headers and body!
							mode = Body;
							if (content_remains == 0) {
								on_finish();
								reset();
							}
						} else {
							//TODO: parse "name: [space] value" or "[space] continuation-value"
						}
					}
					line = ""; //clear line, now that it's parsed
				}
			} else if (mode == Body) {
				assert(content_remains == 0);
				body += *c;
				--content_remains;
				if (content_remains == 0) {
					on_finish();
					reset();
				}
			}
		}
		return true;
	}
};

struct client {
	http::socket socket;
	std::shared_ptr< message > first_message;
	http::incoming_request incoming_request;
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
	{ //listen on socket
		int ret = ::listen(listen_socket, 5);
		if (ret < 0) {
			std::cerr << "[server::listen] Failed to listen on socket." << std::endl;
			closesocket(listen_socket);
			listen_socket = INVALID_SOCKET;
			return;
		}
	}


	std::cout << "[server::listen] Server started at localhost:" << port << "." << std::endl;

	//----------- end socket setup -----------


	while (!quit_flag) {

		fd_set read_fds, write_fds;
		FD_ZERO(&read_fds);
		FD_ZERO(&write_fds);

		#ifndef _WIN32
		int max = listen_socket;
		#endif
		FD_SET(listen_socket, &read_fds);

		//TODO: wake_socket

		for (auto c : clients) {
			#ifndef _WIN32
			max = std::max(max, c.socket);
			#endif
			FD_SET(c.socket, &read_fds);
			if (c.first_message && c.first_message->ready) {
				FD_SET(c.socket, &write_fds);
			}
		}
		lock.unlock();

		{ //wait (a bit) for sockets data to become available:
			struct timeval timeout;
			timeout.tv_sec = 1; //1sec polling... kinda slow, but eventually will have wake_socket
			timeout.tv_usec = 0;
			#ifdef _WIN32
			//On windows nfds is ignored -- ttps://msdn.microsoft.com/en-us/library/windows/desktop/ms740141(v=vs.85).aspx
			int ret = select(InvalidSocket, &read_fds, &write_fds, NULL, &timeout);
			#else
			int ret = select(max + 1, &read_fds, &write_fds, NULL, &timeout);
			#endif

			if (ret < 0) {
				std::cerr << "[server::listen] Select returned an error; will attempt to read/write anyway." << std::endl;
			} else if (ret == 0) {
				//nothing to read or write.
			}
		}

		//add new clients as needed:
		if (FD_ISSET(listen_socket, &read_fds)) {
			socket got = accept(listen_socket, NULL, NULL);
			#ifdef _WIN32
			if (got == InvalidSocket) {
			#else
			if (got < 0) {
			#endif
				//oh well.
			} else {
				#ifdef _WIN32
				unsigned long one = 1;
				if (0 == ioctlsocket(got, FIONBIO, &one)) {
				#else
				{
				#endif
					clients.emplace_back();
					clients.back().socket = got;
					std::cerr << "[server::listen] client connected on " << clients.back().socket << "." << std::endl; //INFO
				}
			}
		}

		lock.lock(); //note: need this for client writes, but not for client reads. Ah, well.

		//process clients:
		for (auto &c : clients) {
			//read:
			if (FD_ISSET(c.socket, &read_fds)) {
				const uint32_t BufferSize = 40000;
				static char *buf = new char[BufferSize];
				#ifdef _WIN32
				ssize_t ret = recv(c.socket, buf, BufferSize, 0);
				#else
				ssize_t ret = recv(c.socket, buf, BufferSize, MSG_DONTWAIT);
				#endif
				if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
					//~no problem~ but no data
				} else if (ret <= 0 || ret > (ssize_t)BufferSize) {
					//~problem~ so remove client
					if (ret == 0) {
						std::cerr << "[http::server] port closed, disconnecting." << std::endl;
					} else if (ret < 0) {
						std::cerr << "[http::server] recv() returned error " << errno << ", disconnecting." << std::endl;
					} else {
						std::cerr << "[http::server] recv() returned strange number of bytes, disconnecting." << std::endl;
					}
					closesocket(c.socket);
					c.socket = -1;
					continue; //don't need to process writes
				} else { //ret > 0
					bool r = c.incoming_request.parse_bytes(buf, ret, [&](){
						std::cout << "Parsed a request." << std::endl; //DEBUG
						//TODO: make response slot, call handle_request
					});
					if (!r) {
						std::cerr << "[http::server] Failed parsing request, closing connection." << std::endl;
						closesocket(c.socket);
						c.socket = -1;
						continue; //don't need to process writes
					}
				}
			}
			//write:
			if (FD_ISSET(c.socket, &write_fds) && c.first_message && c.first_message->ready) {
				std::string &data = c.first_message->data;
				#ifdef _WIN32
				ssize_t ret = send(c.socket, data.data(), data.size(), 0);
				#else
				ssize_t ret = send(c.socket, data.data(), data.size(), MSG_DONTWAIT);
				#endif
				if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
					//~no problem~
				} else if (ret <= 0 || ret > (ssize_t)data.size()) {
					if (ret < 0) {
						std::cerr << "[http::server] send() returned error " << errno << ", disconnecting." << std::endl;
					} else { assert(ret == 0 || ret > (ssize_t)data.size());
						std::cerr << "[http::server] send() returned strange number of bytes, disconnecting." << std::endl;
					}
					closesocket(c.socket);
					c.socket = -1;
				} else { //ret seems reasonable
					data.erase(0, ret);
				}
			}
		}

		//reap closed clients:
		for (auto client = clients.begin(); client != clients.end(); /*later*/) {
			auto old = client;
			++client;
			if (old->socket == INVALID_SOCKET) {
				clients.erase(old);
			}
		}

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
