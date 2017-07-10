#ifdef TWEAK_ENABLE

#include "tweak.hpp"
#include "http.hpp"

#include <unordered_set>
#include <map>
#include <cassert>
#include <algorithm>
#include <mutex>
#include <thread>

//helpers used to convert utf8 strings to/from utf8-encoded JSON string values:
static bool json_to_utf8(std::string const &data, std::string *out, std::string *error = nullptr);
static std::string utf8_to_json(std::string const &data);

namespace tweak {

namespace {
	struct poll {
		poll(uint32_t _serial, std::unique_ptr< http::response > &&_response) : serial(_serial), response(std::move(_response)) { }
		uint32_t serial;
		std::unique_ptr< http::response > response;
	};
	//internal data, plus paranoia about global initialization order:
	struct internal {
		std::mutex mutex;
		std::unordered_set< tweak * > tweaks;
		uint16_t port = 1138;
		std::unique_ptr< http::server > server;
		std::list< poll > polls;

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
	if (internal.server) internal.server.reset(); //kill off server so it will be started with new port.
}

void sync() {
	auto &internal = get_internal();
	std::unique_lock< std::mutex > lock(internal.mutex);

	if (!internal.server) internal.server = std::make_unique< http::server >(internal.port);

	//Read adjustments (and poll requests) from the server:
	internal.server->poll([&](http::request &request, std::unique_ptr< http::response > response){
		if (request.method == "GET" && request.url == "/") {
			//serve UI(?)
			response->body = "<html><body>hello.</body></html>";
		} else if (request.method == "GET" && request.url == "/tweaks") {
			//serve current state (...by registering a poll):
			internal.polls.emplace_back(0, std::move(response));
		} else if (request.method == "GET" && request.url.substr(0,8) == "/tweaks?") {
			//long poll:
			internal.polls.emplace_back(std::stoul(request.url.substr(8)), std::move(response));
		} else if (request.method == "POST" && request.url == "/tweaks") {
			//adjust current state
			//... TODO ...
			(void)json_to_utf8;
		} else {
			response->status.code = 404;
			response->status.message = "Not Found";
			response->body = "Not Found";
			response->headers.emplace_back("Content-Type", "text/plain");
		}
	});

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

	//if state has changed, update serial:
	if (internal.state != all_state) {
		internal.state = all_state;
		++internal.state_serial;
	}

	//respond to polls:
	for (auto poll = internal.polls.begin(); poll != internal.polls.end(); /* later */) {
		auto next = poll;
		++next;
		if (poll->serial != internal.state_serial) {
			poll->response->body = "{\"serial\":" + std::to_string(internal.state_serial) + ",\"state\":" + internal.state + "}";
			poll->response->headers.emplace_back("Content-Type", "application/json");
			internal.polls.erase(poll);
		}
		poll = next;
	}
}


} //namespace tweak


static bool json_to_utf8(std::string const &data, std::string *_out, std::string *error) {
	assert(_out);
	auto it = std::back_inserter(*_out);
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
