#pragma once
#include <NavalCommand/Gateway/Api/Codec.h>
#include <grpcpp/grpcpp.h>
#include <navalcommand/gateway/v1/service.grpc.pb.h>
#include <spdlog/logger.h>
namespace NavalCommand::Gateway::Grpc {
// Construct/start/stop on host I/O workers. Provider lifetime must exceed
// server. No host ABI or simulator references. Requests return owned provider
// values.
class Server final : private navalcommand::gateway::v1::Gateway::Service {
	Api::IProvider &m_Provider;
	std::shared_ptr<spdlog::logger> m_Logger;
	std::unique_ptr<grpc::Server> m_Server;

public:
	Server(Api::IProvider &provider, std::shared_ptr<spdlog::logger> logger);
	~Server();
	std::expected<void, Api::Error> Start(const std::string &endpoint);
	void Stop();

private:
	grpc::Status Connect(grpc::ServerContext *, const Api::Wire::Query *, Api::Wire::Session *) override;
	grpc::Status GetView(grpc::ServerContext *, const Api::Wire::Query *, Api::Wire::WorldSnapshot *) override;
	grpc::Status DiscoverOrders(grpc::ServerContext *, const Api::Wire::Query *, Api::Wire::OrderDiscovery *) override;
	grpc::Status SubmitOrder(grpc::ServerContext *, const Api::Wire::OrderRequest *, Api::Wire::OrderResult *) override;
	grpc::Status GetStatus(grpc::ServerContext *, const Api::Wire::Query *, Api::Wire::OrderResult *) override;
};
} // namespace NavalCommand::Gateway::Grpc
