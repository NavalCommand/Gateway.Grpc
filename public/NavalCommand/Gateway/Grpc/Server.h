#pragma once

#include <NavalCommand/Gateway/Api/IProvider.h>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <navalcommand/gateway/v2/service.grpc.pb.h>

namespace NavalCommand::Gateway::Grpc
{
	class Server final
	{
		class Service;
		std::unique_ptr<Service> m_Service;
		std::unique_ptr<grpc::Server> m_Server;
		int m_Port{};

	public:
		explicit Server(Api::IProvider& provider);
		~Server();
		Error::Api::Result<void> Start(const std::string& endpoint);
		void Stop();
		int Port() const;
	};
}
