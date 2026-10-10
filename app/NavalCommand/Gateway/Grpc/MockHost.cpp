#include <NavalCommand/Gateway/Grpc/MockProvider.h>
#include <NavalCommand/Gateway/Grpc/Server.h>
#include <iostream>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <thread>
constexpr auto MOCK_HOST_LIFETIME = std::chrono::seconds(120);

int main(int argc, char** argv)
{
	NavalCommand::Gateway::Grpc::MockProvider provider;
	if (argc > 2 && std::string(argv[2]) == "blocked")
	{
		provider.Block();
	}
	auto logger = spdlog::stdout_color_mt("mock-host");
	NavalCommand::Gateway::Grpc::Server server(provider, logger);
	if (!server.Start(argc > 1 ? argv[1] : "127.0.0.1:50052"))
	{
		return 2;
	}
	std::cout << "MOCK_READY" << std::endl;
	std::this_thread::sleep_for(MOCK_HOST_LIFETIME);
	server.Stop();
}
