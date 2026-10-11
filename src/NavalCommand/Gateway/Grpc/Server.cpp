#include <NavalCommand/Gateway/Api/Codec.h>
#include <NavalCommand/Gateway/Grpc/Server.h>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <string_view>

namespace NavalCommand::Gateway::Grpc {
namespace {
constexpr int MAXIMUM_MESSAGE_BYTES = 1'048'576;
constexpr unsigned MAXIMUM_TCP_PORT = 65'535;
constexpr auto SHUTDOWN_GRACE_PERIOD = std::chrono::seconds(1);
constexpr std::string_view LOOPBACK_PREFIX = "127.0.0.1:";
}
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
	response->set_schema_version(4);
	response->set_session_id(providerView->Snapshot.SessionId);
	response->mutable_observer()->set_value(providerView->Snapshot.Observer.Value);
	response->set_ready(providerView->Snapshot.Ready);
	return grpc::Status::OK;
}

grpc::Status Server::GetView(grpc::ServerContext *, const Api::Wire::Query *query, Api::Wire::ObserverSnapshot *response) {
	auto providerView = m_Provider.View();
	if (!providerView || !providerView->Snapshot.Ready) {
		return {grpc::StatusCode::UNAVAILABLE, "provider_not_ready"};
	}
	if (query->session_id().empty() || query->observer().value().empty()) {
		return {grpc::StatusCode::INVALID_ARGUMENT, "session_and_observer_required"};
	}
	if (query->session_id() != providerView->Snapshot.SessionId) {
		return {grpc::StatusCode::FAILED_PRECONDITION, "stale_session"};
	}
	if (query->observer().value() != providerView->Snapshot.Observer.Value) {
		return {grpc::StatusCode::NOT_FOUND, "wrong_observer"};
	}
	try {
		*response = Api::Encode(providerView->Snapshot);
	} catch (const std::invalid_argument &) {
		return {grpc::StatusCode::FAILED_PRECONDITION, "invalid_provider_observation"};
	}
	return grpc::Status::OK;
}

grpc::Status Server::DiscoverOrders(grpc::ServerContext *, const Api::Wire::Query *query, Api::Wire::OrderDiscovery *response) {
	auto providerView = m_Provider.View();
	if (!providerView || !providerView->Snapshot.Ready) {
		return {grpc::StatusCode::UNAVAILABLE, "provider_not_ready"};
	}
	if (query->observation().value().empty() || query->observer().value().empty()) {
		return {grpc::StatusCode::INVALID_ARGUMENT, "observer_and_observation_required"};
	}
	if (query->session_id() != providerView->Snapshot.SessionId) {
		return {grpc::StatusCode::FAILED_PRECONDITION, "stale_session"};
	}
	if (query->observer().value() != providerView->Snapshot.Observer.Value) {
		return {grpc::StatusCode::NOT_FOUND, "wrong_observer_or_invalid_entity"};
	}
	const auto unit = std::ranges::find_if(providerView->Snapshot.Units, [&](const auto &observation) {
		return observation.Identity.Value == query->observation().value();
	});
	if (unit == providerView->Snapshot.Units.end()) {
		return {grpc::StatusCode::NOT_FOUND, "wrong_observer_or_invalid_entity"};
	}
	const auto availability = std::ranges::find_if(providerView->OrderAvailability, [&](const auto &entry) {
		return entry.Observer == providerView->Snapshot.Observer &&
			entry.Observation.Value == query->observation().value();
	});
	if (availability == providerView->OrderAvailability.end()) {
		return {grpc::StatusCode::FAILED_PRECONDITION, "order_availability_unknown"};
	}
	try {
		*response = Api::Encode(*availability);
		response->set_session_id(providerView->Snapshot.SessionId);
	} catch (const std::invalid_argument &) {
		return {grpc::StatusCode::FAILED_PRECONDITION, "invalid_provider_observation"};
	}
	return grpc::Status::OK;
}

grpc::Status Server::SubmitOrder(grpc::ServerContext *, const Api::Wire::OrderRequest *query, Api::Wire::OrderResult *response) {
	try {
		*response = Api::Encode(m_Provider.Submit(Api::Decode(*query)));
	} catch (const std::invalid_argument &) {
		return {grpc::StatusCode::INVALID_ARGUMENT, "invalid_order_payload"};
	}
	return grpc::Status::OK;
}

grpc::Status Server::GetStatus(grpc::ServerContext *, const Api::Wire::Query *query, Api::Wire::OrderResult *response) {
	if (query->observer().value().empty()) {
		return {grpc::StatusCode::INVALID_ARGUMENT, "observer_required"};
	}
	auto providerView = m_Provider.View();
	if (!providerView || !providerView->Snapshot.Ready) {
		return {grpc::StatusCode::UNAVAILABLE, "provider_not_ready"};
	}
	if (query->session_id() == providerView->Snapshot.SessionId &&
		query->observer().value() != providerView->Snapshot.Observer.Value) {
		return {grpc::StatusCode::NOT_FOUND, "wrong_observer_or_invalid_request"};
	}
	*response = Api::Encode(m_Provider.Status(query->session_id(), query->request_id()));
	return grpc::Status::OK;
}
}
