#include "tweak.hpp"

#include <iostream>
#include <thread>
#include <chrono>

int main(int argc, char **argv) {
	float value = 1.0;
	TWEAK(value);
	float range_value = 0.5;
	TWEAK_HINT(range_value, "float 0.0 1.0");
	float other_value = 0.5;
	TWEAK_HINT(other_value, "");
	while (1) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		float old_value = value;
		float old_range_value = range_value;
		float old_other_value = other_value;
		TWEAK_SYNC();
		if (old_value != value) std::cout << "value: " << value << ".\n";
		if (old_range_value != range_value) std::cout << "range_value: " << range_value << ".\n";
		if (old_other_value != other_value) std::cout << "other_value: " << other_value << ".\n";
	}
}
