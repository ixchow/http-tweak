#include "tweak.hpp"

#include <iostream>
#include <thread>
#include <chrono>

int main(int argc, char **argv) {
	float value = 1.0;
	TWEAK(value);
	while (1) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		float old_value = value;
		TWEAK_SYNC();
		if (old_value != value) std::cout << "Value: " << value << ".\n";
	}
}
