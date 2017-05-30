#pragma once

#ifdef TWEAK_ENABLE

#include <string>
#include <functional>

#define TWEAK_CONFIG(addr) tweak::config(addr)
#define TWEAK(var) tweak::tweak var ## _tweak = tweak::make_tweak(#var, &var)
#define TWEAK_SYNC() tweak::sync()

namespace tweak {

void config(uint16_t port); //port to bind on; always binds to loopback

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

//helpers to for construction of tweak objects for simple values:
inline std::string make_hint(float *) {
	return "float";
}

inline std::function< std::string(void) > make_serialize(float *f) {
	return [f]() -> std::string { return std::to_string(*f); };
}

inline std::function< void(std::string) > make_deserialize(float *f) {
	return [f](std::string val) { *f = std::stof(val); };
}

template< typename T >
tweak make_tweak(std::string name, T *val) {
	return tweak(name, make_hint(val), make_serialize(val), make_deserialize(val));
}


} //namespace tweak

#else //TWEAK_ENABLE
//If tweak is not enabled, all TWEAK_*() statements are no-ops:

#define TWEAK_CONFIG(addr)
#define TWEAK(var)
#define TWEAK_SYNC()

#endif //TWEAK_ENABLE
