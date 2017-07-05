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
 *   if (request.method == "GET" && request.url == "/") {
 *       response->body = "<html><body>Hello World.</body></html>";
 *   } else {
 *       response->status.code = 404;
 *       response->status.message = "Not Found";
 *       response->body = "<html><body>Not Found</body></html>";
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
	struct sockaddr_in wake_addr; //used to send to wake_socket

	void wake(); //used to poke server when blocking on select()

	std::list< client > clients;
};


//------ internals ------

struct message {
	bool ready = false;
	std::string data;
	std::shared_ptr< message > next;
};

//case-insensitive equality for header field names (also, ugh, that was a mistake in the spec):
inline bool case_insensitive_equals(std::string const &a, std::string const &b) {
	if (a.size() != b.size()) return false;
	for (uint32_t i = 0; i < a.size(); ++i) {
		char ca = a[i];
		char cb = b[i];
		if (ca >= 'A' && ca <= 'Z') ca |= 0x20;
		if (cb >= 'A' && cb <= 'Z') cb |= 0x20;
		if (ca != cb) return false;
	}
	return true;
}

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
				if (line.size() >= 2 && line[line.size() - 2] == '\r' && line[line.size()-1] == '\n') {
					line.erase(line.size()-2); //trim CRLF

					if (mode == RequestLine) {
						if (line.size() == 0) {
							//ignore empty line before request
						} else {
							//parse "method SP url SP version"
							auto i1 = line.find(' ');
							if (i1 == std::string::npos) return false;
							auto i2 = line.find(' ', i1+1);
							if (i1 == std::string::npos) return false;
							method = line.substr(0, i1);
							url = line.substr(i1+1, i2-(i1+1));
							std::string version = line.substr(i2+1);
							if (version.substr(0,7) != "HTTP/1.") return false; //unsupported http version

							mode = HeaderLine;
						}
					} else { assert(mode == HeaderLine);
						if (line.size() == 0) {
							//empty line between headers and body!

							//clean up headers by trimming whitespace as per the spec:
							for (auto &nf : headers) {
								std::string value = "";
								//replace runs of ' '/'\t' with single ' ' (and ignore first such run):
								for (auto const &c : nf.second) {
									if (c == ' ' || c == '\t') {
										if (!value.empty() && value[value.size()-1] != ' ') {
											value += ' ';
										}
									} else {
										value += c;
									}
								}
								if (!value.empty() && value[value.size()-1] == ' ') value = value.substr(0, value.size()-1);
								nf.second = value;
							}

							//set content_remains based on size of body passed in content header:
							for (auto const &nf : headers) {
								if (case_insensitive_equals(nf.first, "Content-Length")) {
									content_remains = std::stoul(nf.second);
								}
							}

							mode = Body;
							if (content_remains == 0) {
								on_finish();
								reset();
							}
						} else {
							//parse "name: [space] value" or "[space] continuation-value"
							if (line[0] == ' ' || line[0] == '\t') {
								//[space] continuation-value
								if (headers.empty()) {
									return false; //nothing to continue
								}
								headers.back().second += line;
							} else {
								auto i = line.find(':');
								if (i == std::string::npos) {
									return false; //no ':' to split field name and value
								}
								headers.emplace_back(line.substr(0, i), line.substr(i+1));
							}
						}
					}
					line = ""; //clear line, now that it's parsed
				}
			} else if (mode == Body) {
				assert(content_remains != 0);
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
	//----------- socket setup (wake socket) -----------
	{
		wake_socket = ::socket(AF_INET, SOCK_DGRAM, 0);
		if (wake_socket == -1) {
			std::cerr << "Failed to create UDP wake-up socket." << std::endl;
			return;
		}
	}
	{ //bind to address
		struct sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = htons(0);
		int ret = bind(wake_socket, reinterpret_cast< sockaddr * >(&addr), sizeof(addr));
		if (ret < 0) {
			#ifdef WINDOWS
			static char buffer[1000];
			if (0 != strerror_s(buffer, errno)) {
				buffer[0] = '\0';
			}
			std::cerr << "[server::listen] Failed to bind wake-up socket (" << buffer << ")" << std::endl;
			#else
			std::cerr << "[server::listen] Failed to bind wake-up socket (" << strerror(errno) << ")" << std::endl;
			#endif
			closesocket(wake_socket);
			wake_socket = INVALID_SOCKET;
			return;
		}
	}
	{ //read back address:
		memset(&wake_addr, 0, sizeof(wake_addr));
		socklen_t addrlen = sizeof(wake_addr);
		int ret = getsockname(wake_socket, reinterpret_cast< sockaddr * >(&wake_addr), &addrlen);
		if (ret < 0) {
			std::cerr << "[server::listen] Failed to read address of wake-up socket (" << errno << ")" << std::endl;
			return;
		}
		std::cerr << "FYI, wake port is " << ntohs(wake_addr.sin_port) << std::endl; //DEBUG
	}

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

		max = std::max(max, wake_socket);
		FD_SET(wake_socket, &read_fds);

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

		//clear wake-up messages as needed:
		if (FD_ISSET(wake_socket, &read_fds)) {
			std::cout << "got 'wake' message" << std::endl; //DEBUG
			const uint32_t BufferSize = 100;
			static char *buf = new char[BufferSize];
			#ifdef _WIN32
			recv(wake_socket, buf, BufferSize, 0);
			#else
			recv(wake_socket, buf, BufferSize, MSG_DONTWAIT);
			#endif
			//...(ignore contents)...
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
						std::shared_ptr< message > *message_ptr = &c.first_message;
						while (message_ptr->get()) {
							message_ptr = &(message_ptr->get()->next);
						}

						std::shared_ptr< message > shared_message = std::make_shared< message >();

						std::unique_ptr< response > response(std::make_unique< response >());
						response->owner = this;
						response->weak_message = shared_message;

						*message_ptr = std::move(shared_message);
						lock.unlock();
						handle_request( c.incoming_request, std::move(response) );
						lock.lock();
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
						std::cerr << "[http::server] send() returned strange number of bytes [" << ret << " of " << data.size() << "], disconnecting." << std::endl;
					}
					closesocket(c.socket);
					c.socket = -1;
				} else { //ret seems reasonable
					data.erase(0, ret);
					if (data.empty()) {
						//message finished, advance:
						c.first_message = std::move(c.first_message->next);
					}
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

	closesocket(listen_socket);
	listen_socket = INVALID_SOCKET;

	closesocket(wake_socket);
	wake_socket = INVALID_SOCKET;

	running = false;
	cv.notify_all();
}


//This is a bad idea, because stop() can't be called from handle_request [deadlock], and it can't be called from a different thread [because the mutex gets deallocated, potentially, after the server stops]:
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
	//poke wake_socket with a one byte datagram:
	std::unique_lock< std::mutex > lock(mutex);
	if (wake_socket != INVALID_SOCKET) {
		std::string data = "!";
		#ifdef _WIN32
		sendto(wake_socket, data.data(), data.size(), 0, reinterpret_cast< sockaddr * >(&wake_addr), sizeof(wake_addr));
		#else
		sendto(wake_socket, data.data(), data.size(), MSG_DONTWAIT, reinterpret_cast< sockaddr * >(&wake_addr), sizeof(wake_addr));
		#endif
	}
}

inline response::~response() {
	//does the message slot still exist?
	std::shared_ptr< message > shared_message = weak_message.lock();
	if (!shared_message) return;

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
		shared_message->data = std::move(message);
		shared_message->ready = true;
	}

	//wake server:
	owner->wake();
}


} //namespace http
