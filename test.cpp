#include "tweak.hpp"

#include <unistd.h>

#include <iostream>

int main(int argc, char **argv) {
	float value = 1.0;
	TWEAK(value);
	while (1) {
		sleep(1);
		TWEAK_SYNC();
		std::cout << "Value: " << value << ".\n";
	}
}
