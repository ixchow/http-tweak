#ifdef TWEAK_ENABLE

#include "tweak.hpp"

#include <unordered_set>
#include <map>
#include <cassert>
#include <algorithm>
#include <mutex>
#include <thread>

#ifdef _WIN32
	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN 1
	#endif

	#include <winsock2.h>
	#undef max

	typedef SOCKET Socket;
	const Socket InvalidSocket = INVALID_SOCKET;
	typedef int ssize_t;
#else //Linux / OSX:

	#include <sys/types.h>
	#include <sys/socket.h>
	#include <arpa/inet.h>
	#include <netinet/ip.h>
	#include <unistd.h>

	typedef int Socket;
	const Socket InvalidSocket = -1;

	#define closesocket close
#endif


//helpers used to convert utf8 strings to/from utf8-encoded JSON string values:
bool json_to_utf8(std::string const &data, std::string *out, std::string *error = nullptr);
std::string utf8_to_json(std::string const &data);

namespace tweak {

namespace {
	//internal data, plus paranoia about global initialization order:
	struct internal {
		std::mutex mutex;
		std::unordered_set< tweak * > tweaks;
		uint16_t port = 1138;

		uint32_t state_serial = 0;
		std::string state = "";

		std::map< std::string, std::string > received; //filled by server thread, cleared by sync()

		std::unique_ptr< std::thread > server_thread;
	};

	internal &get_internal() {
		static internal internal;
		return internal;
	}
}

void wake_server(); //helper that wakes (or starts) server.

tweak::tweak(
	std::string _name,
	std::string _hint,
	std::function< std::string(void) > const &_serialize,
	std::function< void(std::string) > const &_deserialize
		) : name(_name), hint(_hint), serialize(_serialize), deserialize(_deserialize) {
	auto &internal = get_internal();

	std::unique_lock< std::mutex > lock(internal.mutex);

	internal.tweaks.insert(this);
}

tweak::~tweak() {
	auto &internal = get_internal();

	std::unique_lock< std::mutex > lock(internal.mutex);

	auto f = internal.tweaks.find(this);
	assert(f != internal.tweaks.end());
	internal.tweaks.erase(f);
}

void config(uint16_t port) {
	auto &internal = get_internal();
	std::unique_lock< std::mutex > lock(internal.mutex);
	internal.port = port;

	if (internal.server_thread) wake_server(); //wake server if thread exists so it can reconfigure itself.
}

void sync() {
	auto &internal = get_internal();
	std::unique_lock< std::mutex > lock(internal.mutex);

	//Read all adjustments from the server, and encode current state and hits:
	std::map< std::string, std::string > state;
	for (auto tweak : internal.tweaks) {
		auto f = internal.received.find(tweak->name);
		if (f != internal.received.end()) {
			tweak->deserialize(f->second);
		}
		state[tweak->name] = "{\"hint\":" + utf8_to_json(tweak->hint) + ",\"value\":" + utf8_to_json(tweak->serialize()) + "}";
	}
	internal.received.clear();

	std::string all_state = "{\n";
	for (auto const &name_value : state) {
		if (&name_value != &(*state.begin())) all_state += ",\n";
		all_state += utf8_to_json(name_value.first) + ":" + name_value.second;
	}
	all_state += "\n}";

	//if state has changed, wake server:
	if (internal.state != all_state) {
		all_state = internal.state;
		++internal.state_serial;
		wake_server(); //state changed, server might want to send it out.
	}
}



static void server_main(internal *_internal) {
	assert(_internal);
	auto &internal = *_internal;
	//TODO: be an HTTP server (!)

	int16_t port = 0;
	Socket socket = InvalidSocket;

	auto init = [&](){
		{ //create socket:
			socket = ::socket(AF_INET, SOCK_STREAM, 0);
			if (socket == InvalidSocket) {
				std::cerr << "Failed to create socket" << std::endl;
				return;
			}
		}

		{ //make it okay to reuse port
		#ifdef WINDOWS
			BOOL one = TRUE;
			int ret = setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast< const char * >(&one), sizeof(one));
		#else
			int one = 1;
			int ret = setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		#endif
			if (ret != 0) {
				std::cerr << "WARNING: Failed to set socket reuse address." << std::endl;
			}
		}

		{ //bind to address
			struct sockaddr_in addr;
			memset(&addr, 0, sizeof(addr));
			addr.sin_family = AF_INET;
			addr.sin_port = htons(port);
			addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			int ret = bind(socket, reinterpret_cast< sockaddr * >(&addr), sizeof(addr));
			if (ret < 0) {
				#ifdef WINDOWS
				static char buffer[1000];
				if (0 != strerror_s(buffer, errno)) {
					buffer[0] = '\0';
				}
				std::cerr << "ERROR: Failed to bind socket (" << buffer << ")" << std::endl;
				#else
				std::cerr << "ERROR: Failed to bind socket (" << strerror(errno) << ")" << std::endl;
				#endif
				closesocket(socket);
				socket = InvalidSocket;
				return;
			}
		}

		{ //listen on socket
			int ret = listen(socket, 5);
			if (ret < 0) {
				std::cerr << "ERROR: Failed to listen on socket." << std::endl;
				closesocket(socket);
				socket = InvalidSocket;
				return;
			}
		}
	}; //init()



}

void wake_server() {
	#ifdef WINDOWS
	static bool inited = false;
	if (!inited) {
		WSADATA info;
		int ret = WSAStartup((2 << 8) | 2, &info);
		if (ret != 0) {
			LOG_ERROR("Failed to initialize sockets");
			return;
		}
		inited = true;
	}
	#endif

	auto &internal = get_internal();
	std::unique_lock< std::mutex > lock(internal.mutex);
	if (!internal.server_thread) {
		internal.server_thread.reset(new std::thread(server_main, &internal));
	}
	
}


} //namespace tweak


static bool json_to_utf8(std::string const &data, std::string *out, std::string *error = nullptr) {
	assert(*_out);
	auto it = std::back_inserter(*out);
	auto c = data.begin();

	auto read_hex4 = [&]() -> uint16_t {
		uint32_t val = 0;
		for (uint_fast32_t i = 0; i < 4; ++i) {
			val *= 16;
			if (c == data.end()) throw std::runtime_error("Unicode escape includes end-of-string.");
			else if (*c >= '0' && *c <= '9') val += (*c - '0');
			else if (*c >= 'a' && *c <= 'f') val += (*c - 'a') + 10;
			else if (*c >= 'a' && *c <= 'F') val += (*c - 'A') + 10;
			else throw std::runtime_error("Unicode escape contains invalid hex digit.");
			++c;
		}
		assert(val <= 0xffff);
		return val;
	};

	try {
		if (c == data.end() || *c != '"') throw std::runtime_error("String doesn't start with quote.");
		++c;
		while (c != data.end() && *c != '"') {
			if (*c == '\\') {
				++c;
				if (c == data.end()) throw std::runtime_error("End-of-string follows backslash.");
				if (*c == '"' || *c == '\\' || *c == '/') { *it = *c; ++it; ++c; }
				else if (*c == 'b') { *it = '\b'; ++it; ++c; }
				else if (*c == 'f') { *it = '\f'; ++it; ++c; }
				else if (*c == 'n') { *it = '\n'; ++it; ++c; }
				else if (*c == 'r') { *it = '\r'; ++it; ++c; }
				else if (*c == 't') { *it = '\t'; ++it; ++c; }
				else if (*c == 'u') {
					++c;
					uint32_t val = read_hex4();
	
					if ((val & 0xfc00) == 0xd800) {
						if (c == data.end() || *c != '\\') throw std::runtime_error("Missing backslash in second part of surrogate pair.");
						++c;
						if (c == data.end() || *c != 'u') throw std::runtime_error("Missing 'u' in second part of surrogate pair.");
						++c;
	
						uint32_t val2 = read_hex4();
						if ((val2 & 0xfc00) != 0xdc00) {
							throw std::runtime_error("Missing second half of surrogate pair.");
						}
						val = 0x01000 + ( ((val & 0x03ff) << 10) | (val2 & 0x03ff) );
						assert(val <= 0x10ffff);
					}
	
					if (val <= 0x7f) {
						*it = val; ++it;
					} else if (val <= 0x7ff) {
						*it = 0xC0 | (val >> 7); ++it;
						*it = 0x80 | (val & 0x3f); ++it;
					} else if (val <= 0xffff) {
						*it = 0xe0 | (val >> 12); ++it;
						*it = 0x80 | ((val >> 6) & 0x3f); ++it;
						*it = 0x80 | (val & 0x3f); ++it;
					} else if (val <= 0x10ffff) {
						*it = 0xf0 | ((val >> 18) & 0x7); ++it;
						*it = 0x80 | ((val >> 12) & 0x3f); ++it;
						*it = 0x80 | ((val >> 6) & 0x3f); ++it;
						*it = 0x80 | (val & 0x3f); ++it;
					} else {
						assert(0 && "will never decode a val this big");
					}
	
				}
			} else {
				*it = *c; ++it;
				++c;
			}
		}
		if (c == data.end() || *c != '"') throw std::runtime_error("String doesn't end with quote.");
		++c;
		if (c != data.end()) throw std::runtime_error("Trailing characters after string.");
	} catch (std::runtime_error e) {
		if (error) *error = e.what();
		return false;
	}
	return true;
}

static std::string utf8_to_json(std::string const &data) {
	std::string ret;
	auto it = std::back_inserter(ret);

	*it = '"'; ++it;
	for (auto c = data.begin(); c != data.end(); ++c) {
		if (*c == '\\' || *c == '"') {
			*it = '\\'; ++it; *it = *c; ++it;
		} else {
			*it = *c; ++it;
		}
	}
	*it = '"'; ++it;
	return ret;
}


#endif //TWEAK_ENABLE
