#pragma once

/*
 * Single-header-file HTTP 1.1 server.
 *  with 'no fancy stuff'
 *
 * Usage:
 * 
 * http::server server(port);
 *
 * for (;;) {
 *     server.poll([](http::request const &request, std::unique_ptr< http::response > response){
 *       if (request.method == "GET" && request.url == "/") {
 *           response->body = "<html><body>Hello World.</body></html>";
 *       } else {
 *           response->status.code = 404;
 *           response->status.message = "Not Found";
 *           response->body = "<html><body>Not Found</body></html>";
 *       }
 *       //'response' is sent to client during the next poll() after destroyed, so std::move() it if you want to leave response outstanding for a while
 *    }, 1.0);
 * }
 *
 *
 * Thread-safety:
 *  - don't call 'poll()' from within 'poll()'
 *  - don't call 'poll()' from multiple threads at once
 *  - it is safe to pass ownership of a response to another thread, even if it outlives the server
 *
 */

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <atomic>
#include <list>
#include <iostream>

#include <cassert>
#include <cstring> //for memset()
#include <cmath> //for lround(), floor()
#include <algorithm>

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
	~response();
};

#ifdef _WIN32
typedef SOCKET socket;
typedef int ssize_t;
#define MSG_DONTWAIT 0 //on windows, sockets are set to non-blocking with an ioctl
#else
typedef int socket;
const socket INVALID_SOCKET = -1;
#endif

struct server {
	server(uint16_t port);
	~server();

	//receive data, pass any new requests to handle_request, and send any pending responses:
	// (timeout is in seconds, and is how long poll() will wait for activity before returning)
	void poll(std::function< void(request const &, std::unique_ptr< response >) > const &handle_request, double timeout = 0.0);

	//------- internals -------
	socket listen_socket = INVALID_SOCKET;
	std::list< client > clients;
	bool polling = false;
	std::unique_ptr< char[] > buffer; //buffer used for recv() in poll(), kept around to avoid repeated de- / re- allocation
};


//------ internals ------

struct message {
	std::atomic< bool > ready;
	std::string data;
	std::shared_ptr< message > next;

	message() : ready(false) { }
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

	uint32_t body_remains = 0; //bytes of Body content that remain (set by Content-Length: header)
	std::string line = ""; //current line being parsed
	void reset() {
		*this = incoming_request();
	}
	bool parse_bytes(char const *begin, ssize_t count, std::function< void() > const &on_finish) {
		for (char const *c = begin; c < begin + count; ++c) {
			if (mode == RequestLine || mode == HeaderLine) {
				line += *c;
				//once line is complete, parse it:
				if (line.size() >= 2 && line[line.size() - 2] == '\r' && line[line.size()-1] == '\n') {
					line.erase(line.size()-2); //trim CRLF

					if (mode == RequestLine) {
						if (line.size() != 0) { //ignore empty lines before request
							//parse "method SP url SP version"
							std::string::size_type i1 = line.find(' ');
							if (i1 == std::string::npos) return false;
							std::string::size_type i2 = line.find(' ', i1+1);
							if (i1 == std::string::npos) return false;
							method = line.substr(0, i1);
							url = line.substr(i1+1, i2-(i1+1));
							std::string version = line.substr(i2+1);
							if (version.substr(0,7) != "HTTP/1.") return false; //unsupported http version

							mode = HeaderLine; //next lines are headers
						}
					} else { assert(mode == HeaderLine);
						if (line.size() == 0) {
							//empty line separates headers from body

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

							//set body_remains based on size of body passed in content header:
							for (auto const &nf : headers) {
								if (case_insensitive_equals(nf.first, "Content-Length")) {
									body_remains = std::stoul(nf.second);
								}
							}

							mode = Body;
							if (body_remains == 0) {
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
				assert(body_remains != 0);
				body += *c;
				--body_remains;
				if (body_remains == 0) {
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

inline server::server(uint16_t port) {

	#ifdef _WIN32
	{ //init winsock:
		WSADATA info;
		if (WSAStartup((2 << 8) | 2, &info) != 0) {
			throw std::runtime_error("WSAStartup failed.");
		}
	}
	#endif

	//----------- socket setup -----------
	{ //create socket:
		listen_socket = ::socket(AF_INET, SOCK_STREAM, 0);
		if (listen_socket == -1) {
			throw std::system_error(errno, std::system_category(), "failed to create socket");
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
			std::cerr << "[server::server] WARNING: Failed to set socket reuse address." << std::endl;
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
			closesocket(listen_socket);
			throw std::system_error(errno, std::system_category(), "failed to bind socket");
		}
	}
	{ //listen on socket
		int ret = ::listen(listen_socket, 5);
		if (ret < 0) {
			closesocket(listen_socket);
			throw std::system_error(errno, std::system_category(), "failed to listen on socket");
		}
	}


	std::cout << "[server::server] Server started at localhost:" << port << "." << std::endl;
}

inline server::~server() {
	#ifdef _WIN32
	WSACleanup();
	#endif //_WIN32
	closesocket(listen_socket);
}

inline void server::poll(std::function< void(request const &, std::unique_ptr< response > response) > const &handle_request, double timeout) {
	assert(!polling && "You must not call server.poll() from within a server.poll() callback.");

	polling = true;

	fd_set read_fds, write_fds;
	FD_ZERO(&read_fds);
	FD_ZERO(&write_fds);

	int max = listen_socket;
	FD_SET(listen_socket, &read_fds);

	for (auto c : clients) {
		max = std::max< int >(max, c.socket);
		FD_SET(c.socket, &read_fds);
		if (c.first_message && c.first_message->ready.load(std::memory_order_acquire)) {
			FD_SET(c.socket, &write_fds);
		}
	}

	{ //wait (a bit) for sockets' data to become available:
		struct timeval tv;
		tv.tv_sec = std::lround(std::floor(timeout));
		tv.tv_usec = std::lround((timeout - std::floor(timeout)) * 1e6);
		#ifdef _WIN32
		//On windows nfds is ignored -- https://msdn.microsoft.com/en-us/library/windows/desktop/ms740141(v=vs.85).aspx
		//needed? (void)max; //suppress unused variable warning
		int ret = select(max + 1, &read_fds, &write_fds, NULL, &tv);
		#else
		int ret = select(max + 1, &read_fds, &write_fds, NULL, &tv);
		#endif

		if (ret < 0) {
			std::cerr << "[server::poll] Select returned an error; will attempt to read/write anyway." << std::endl;
		} else if (ret == 0) {
			//nothing to read or write.
			polling = false;
			return;
		}
	}

	//add new clients as needed:
	if (FD_ISSET(listen_socket, &read_fds)) {
		socket got = accept(listen_socket, NULL, NULL);
		if (got == INVALID_SOCKET) {
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
				std::cerr << "[server::poll] client connected on " << clients.back().socket << "." << std::endl; //INFO
			}
		}
	}

	
	const uint32_t BufferSize = 20000;
	if (!buffer) buffer = std::make_unique< char[] >(BufferSize);

	//process requests:
	for (auto &c : clients) {
		if (!FD_ISSET(c.socket, &read_fds)) continue; //don't bother if not marked readable

		char *buf = buffer.get();
		ssize_t ret = recv(c.socket, buf, BufferSize, MSG_DONTWAIT);
		if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			//~no problem~ but no data
		} else if (ret <= 0 || ret > (ssize_t)BufferSize) {
			//~problem~ so remove client
			if (ret == 0) {
				std::cerr << "[server::poll] port closed, disconnecting." << std::endl;
			} else if (ret < 0) {
				std::cerr << "[server::poll] recv() returned error " << errno << ", disconnecting." << std::endl;
			} else {
				std::cerr << "[server::poll] recv() returned strange number of bytes, disconnecting." << std::endl;
			}
			closesocket(c.socket);
			c.socket = INVALID_SOCKET;
		} else { //ret > 0
			bool r = c.incoming_request.parse_bytes(buf, ret, [&](){

				std::shared_ptr< message > *message_ptr = &c.first_message;
				while (message_ptr->get()) {
					message_ptr = &(message_ptr->get()->next);
				}

				std::shared_ptr< message > shared_message = std::make_shared< message >();

				std::unique_ptr< response > response(std::make_unique< response >());
				response->weak_message = shared_message;

				*message_ptr = std::move(shared_message);
				handle_request( c.incoming_request, std::move(response) );
			});
			if (!r) {
				std::cerr << "[server::poll] Failed parsing request, closing connection." << std::endl;
				closesocket(c.socket);
				c.socket = INVALID_SOCKET;
			}
		}
	}

	//process responses:
	for (auto &c : clients) {
		if (c.socket == INVALID_SOCKET) continue; //will reap later
		if (!FD_ISSET(c.socket, &write_fds)) continue; //don't bother if not marked writable
		while (c.first_message && c.first_message->ready.load(std::memory_order_acquire)) {
			std::string &data = c.first_message->data;
			ssize_t ret = send(c.socket, data.data(), data.size(), MSG_DONTWAIT);
			if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
				//~no problem~, but don't keep trying
				break;
			} else if (ret <= 0 || ret > (ssize_t)data.size()) {
				if (ret < 0) {
					std::cerr << "[server::poll] send() returned error " << errno << ", disconnecting." << std::endl;
				} else { assert(ret == 0 || ret > (ssize_t)data.size());
					std::cerr << "[server::poll] send() returned strange number of bytes [" << ret << " of " << data.size() << "], disconnecting." << std::endl;
				}
				closesocket(c.socket);
				c.socket = INVALID_SOCKET;
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

	polling = false;
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

	{ //mark ready to send:
		shared_message->data = std::move(message);
		shared_message->ready.store(true, std::memory_order_release);
	}

}


} //namespace http
