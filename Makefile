.PHONY : all clean

all : test

tweak.o : tweak.cpp tweak.hpp
	g++ -Wall -Werror -DTWEAK_ENABLE -o tweak.o -c tweak.cpp

test.o : test.cpp tweak.hpp
	g++ -Wall -Werror -DTWEAK_ENABLE -o test.o -c test.cpp

test : test.o tweak.o
	g++ -Wall -Werror -DTWEAK_ENABLE -o test test.o tweak.o

test-http : test-http.cpp http.hpp
	g++ -Wall -Werror -DTWEAK_ENABLE -o test-http test-http.cpp

clean :
	rm -f test test.o tweak.o
