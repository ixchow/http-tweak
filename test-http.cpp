#include "http.hpp"


int main(int argc, char **argv) {
	http::server server(8080);

	while (true) {
		server.poll([&](http::request const &request, std::unique_ptr< http::response > response) {
			if (request.method == "GET" && request.url == "/") {
				response->headers.emplace_back("Content-Type", "text/html");
				response->body = "<html><body>Hello World.</body></html>";
			} else {
				response->status.code = 404;
				response->status.message = "Not Found";
				response->body = "<html><body>Not Found</body></html>";
			}
		}, 1.0 / 60.0);
	}
}
