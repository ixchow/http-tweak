#include "http.hpp"


int main(int argc, char **argv) {
	http::server server;
	server.handle_request = [&](http::request const &request, std::unique_ptr< http::response > response) {
		if (request.method == "GET" && request.url == "/") {
			response->headers.emplace_back("Content-Type", "text/html");
			response->body = "<html><body>Hello World.</body></html>";
		} else {
			response->status.code = 404;
			response->status.message = "Not Found";
			response->body = "<html><body>Not Found</body></html>";
		}
	};

	std::thread stop_thread([&](){
		std::cout << "Stopping server in..."; std::cout.flush();
		for (uint32_t i = 5; i > 0; --i) {
			std::cout << " " << i; std::cout.flush();
			sleep(1);
		}
		server.stop();
		std::cout << "Server reports that it is stopped." << std::endl;
	});

	server.listen(8080);

	std::cout << "Waiting to quit because of server mutex dealloc race condition." << std::endl;
	sleep(5);
}
