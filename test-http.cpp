#include "http.hpp"


int main(int argc, char **argv) {
	http::server server;
	server.handle_request = [](http::request const &request, std::unique_ptr< http::response > response) {
		std::cout << request.method << " " << request.url << std::endl;
	};
	server.listen(8080);
}
