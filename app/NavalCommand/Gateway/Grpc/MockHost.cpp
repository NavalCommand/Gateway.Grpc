#include <NavalCommand/Gateway/Grpc/MockProvider.h>
#include <NavalCommand/Gateway/Grpc/Server.h>
#include <iostream>

int main(int argc, char** argv)
{
	NavalCommand::Gateway::Grpc::MockProvider provider;
	NavalCommand::Gateway::Grpc::Server server{provider};
	const std::string endpoint = argc > 1 ? argv[1] : "127.0.0.1:50051";
	if (!server.Start(endpoint))
	{
		std::cerr << "Mock gateway startup failed\n";
		return 1;
	}
	std::cout << "Synthetic mock gateway on " << endpoint << "; press Enter to stop\n";
	std::cin.get();
	server.Stop();
	return 0;
}
