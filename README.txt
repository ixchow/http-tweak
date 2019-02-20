HTTP-Tweak: Adjust (anything) in your app, at runtime, in a browser.
-----------

Usage:
	#include <tweak.hpp>

	//during program init (optional)
	TWEAK_CONFIG(8888, "tweak-ui.html"); //address to bind on + ui HTML file

	float JumpHeight = 1.0f; //default value
	TWEAK(JumpHeight); //set as adjustable (within the scope of TWEAK)

	float walkSpeed = 0.7f; //default value
	TWEAK_HINT(walkSpeed, "float 0.0 10.0"); //set as adjustable, supply a custom hint to be passed to the ui html file (with the default UI, this makes a slider with min 0 and max 10)

	while (1) {
		TWEAK_SYNC(); //update tweaked values
		//... rest of runloop ...
	}

TWEAK and TWEAK_HINT both create objects to track that the variable is to be tweaked. So you need to make sure they don't go out of scope.

Note:
	During build: -DTWEAK_ENABLE , link against tweak.cpp .
