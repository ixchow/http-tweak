#pragma once

#ifdef TWEAK_ENABLE

#include <string>
#include <functional>
#include <cstdint>

#define TWEAK_CONFIG(addr,ui) tweak::config(addr,ui)
#define TWEAK(var) tweak::tweak var ## _tweak = tweak::make_tweak(#var, &var)
#define TWEAK_HINT(var, hint) tweak::tweak var ## _tweak = tweak::make_tweak_hint(#var, &var, hint)
#define TWEAK_SYNC() tweak::sync()

namespace tweak {

void config(
	uint16_t port, //port to bind; always binds to loopback
	std::string const &ui_file //filename of ui html to serve
);

void sync(); //update tweaked items

//generic handler for tweaked items, which end up tracked in a global list.
//serialize/deserialize are called from tweak::sync();
struct tweak {
	tweak(
		std::string name, //passsed to ui
		std::string hint, //passed to ui to provide editing help
		std::function< std::string(void) > const &serialize,
		std::function< void(std::string) > const &deserialize
		);
	~tweak();
	std::string name;
	std::string hint;
	std::function< std::string(void) > serialize;
	std::function< void(std::string) > deserialize;
};

//-------- simple values --------

//helpers to for construction of tweak objects for simple values:
inline std::string make_hint(int *) { return "int"; }
inline std::string make_hint(float *) { return "float"; }
inline std::string make_hint(double *) { return "double"; }

template< typename T >
inline std::function< std::string(void) > make_serialize(T *val) {
	return [val]() -> std::string { return std::to_string(*val); };
}

inline std::function< void(std::string) > make_deserialize(int *val) {
	return [val](std::string str) { *val = std::stoi(str); };
}
inline std::function< void(std::string) > make_deserialize(float *val) {
	return [val](std::string str) { *val = std::stof(str); };
}
inline std::function< void(std::string) > make_deserialize(double *val) {
	return [val](std::string str) { *val = std::stod(str); };
}

template< typename T >
tweak make_tweak(std::string name, T *val) {
	return tweak(name, make_hint(val), make_serialize(val), make_deserialize(val));
}

template< typename T >
tweak make_tweak_hint(std::string name, T *val, std::string hint) {
	return tweak(name, hint, make_serialize(val), make_deserialize(val));
}

} //namespace tweak

#else //TWEAK_ENABLE
//If tweak is not enabled, all TWEAK_*() statements are no-ops:

#define TWEAK_CONFIG(addr,ui)
#define TWEAK(var)
#define TWEAK_HINT(var, hint)
#define TWEAK_SYNC()

#endif //TWEAK_ENABLE
