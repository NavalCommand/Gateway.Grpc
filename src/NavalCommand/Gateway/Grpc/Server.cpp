#include <NavalCommand/Gateway/Api/Codec.h>
#include <NavalCommand/Gateway/Grpc/Server.h>
#include <charconv>
#include <chrono>
#include <string_view>
namespace NavalCommand::Gateway::Grpc {
namespace {
constexpr int MAXIMUM_MESSAGE_BYTES = 1'048'576;
constexpr unsigned MAXIMUM_TCP_PORT = 65'535;
constexpr auto SHUTDOWN_GRACE_PERIOD = std::chrono::seconds(1);
constexpr std::string_view LOOPBACK_PREFIX = "127.0.0.1:";
} // namespace
Server::Server(Api::IProvider &provider, std::shared_ptr<spdlog::logger> logger) : m_Provider{provider}, m_Logger{std::move(logger)} {
}
Server::~Server() {
	Stop();
}
std::expected<void, Api::Error> Server::Start(const std::string &endpoint) {
	if (m_Server || !endpoint.starts_with(LOOPBACK_PREFIX)) {
		return std::unexpected(Api::Error::Malformed);
	}
	const auto portText = endpoint.substr(LOOPBACK_PREFIX.size());
	unsigned portNumber = 0;
	const auto portParseResult = std::from_chars(portText.data(), portText.data() + portText.size(), portNumber);
	if (portParseResult.ec != std::errc{} || portParseResult.ptr != portText.data() + portText.size() || portNumber == 0 || portNumber > MAXIMUM_TCP_PORT) {
		return std::unexpected(Api::Error::Malformed);
	}
	grpc::ServerBuilder serverBuilder;
	serverBuilder.SetMaxReceiveMessageSize(MAXIMUM_MESSAGE_BYTES);
	serverBuilder.SetMaxSendMessageSize(MAXIMUM_MESSAGE_BYTES);
	int port = 0;
	serverBuilder.AddListeningPort(endpoint, grpc::InsecureServerCredentials(), &port);
	serverBuilder.RegisterService(this);
	m_Server = serverBuilder.BuildAndStart();
	if (!m_Server || port == 0) {
		return std::unexpected(Api::Error::Busy);
	}
	if (m_Logger) {
		m_Logger->info("Gateway listening {}", endpoint);
	}
	return {};
}
void Server::Stop() {
	if (m_Server) {
		m_Server->Shutdown(std::chrono::system_clock::now() + SHUTDOWN_GRACE_PERIOD);
		m_Server->Wait();
		m_Server.reset();
	}
}
grpc::Status Server::Connect(grpc::ServerContext *, const Api::Wire::Query *, Api::Wire::Session *response) {
	auto providerView = m_Provider.View();
	if (!providerView) {
		return {grpc::StatusCode::UNAVAILABLE, "provider_not_ready"};
	}
	response->set_schema_version(1);
	response->set_session_id(providerView->sessionId);
	response->set_observer_id(providerView->observerId);
	response->set_ready(providerView->ready);
	return grpc::Status::OK;
}
grpc::Status Server::GetView(grpc::ServerContext *, const Api::Wire::Query *, Api::Wire::WorldSnapshot *response) {
	auto providerView = m_Provider.View();
	if (!providerView || !providerView->ready) {
		return {grpc::StatusCode::UNAVAILABLE, "provider_not_ready"};
	}
	*response = Api::Encode(*providerView);
	return grpc::Status::OK;
}
grpc::Status Server::DiscoverOrders(grpc::ServerContext *, const Api::Wire::Query *query, Api::Wire::OrderDiscovery *response) {
	auto providerView = m_Provider.View();
	if (!providerView) {
		return {grpc::StatusCode::UNAVAILABLE, "provider_not_ready"};
	}
	response->set_session_id(providerView->sessionId);
	response->set_entity_id(query->entity_id());
	if (query->session_id() != providerView->sessionId) {
		return {grpc::StatusCode::FAILED_PRECONDITION, "stale_session"};
	}
	for (auto &unit : providerView->units) {
		if (unit.id == query->entity_id()) {
			for (auto &eligibility : unit.orders) {
				auto wireEligibility = response->add_orders();
				wireEligibility->set_kind(eligibility.kind);
				wireEligibility->set_availability(static_cast<Api::Wire::Availability>(eligibility.availability));
				wireEligibility->set_reason(eligibility.reason);
				wireEligibility->set_argument_semantics(eligibility.arguments);
			}
			return grpc::Status::OK;
		}
	}
	return {grpc::StatusCode::NOT_FOUND, "wrong_side_or_invalid_entity"};
}
grpc::Status Server::SubmitOrder(grpc::ServerContext *, const Api::Wire::OrderRequest *query, Api::Wire::OrderResult *response) {
	*response = Api::Encode(m_Provider.Submit(Api::Decode(*query)));
	return grpc::Status::OK;
}
grpc::Status Server::GetStatus(grpc::ServerContext *, const Api::Wire::Query *query, Api::Wire::OrderResult *response) {
	*response = Api::Encode(m_Provider.Status(query->session_id(), query->request_id()));
	return grpc::Status::OK;
}
} // namespace NavalCommand::Gateway::Grpc
