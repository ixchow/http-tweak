HTTP-Tweak: Adjust (anything) in your app, at runtime, in a browser.
-----------

Usage:
	#include <tweak.hpp>

	//during program init (optional)
	TWEAK_CONFIG("localhost:8888", "tweak-ui.html"); //address to bind on + ui HTML file

	float JumpHeight = 1.0f; //default value
	TWEAK(JumpHeight); //set as adjustable (within the scope of TWEAK_INIT)

	while (1) {
		TWEAK_SYNC(); //update tweaked values
		//... rest of runloop ...
	}


Note:
	During build: -DTWEAK_ENABLE , link against tweak.cpp .
