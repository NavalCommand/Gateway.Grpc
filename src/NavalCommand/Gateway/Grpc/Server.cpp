#include <NavalCommand/Gateway/Api/Codec.h>
#include <NavalCommand/Gateway/Grpc/Server.h>
namespace NavalCommand::Gateway::Grpc {
Server::Server(Api::IProvider &p, std::shared_ptr<spdlog::logger> logger)
    : provider_(p), logger_(std::move(logger)) {}
Server::~Server() { Stop(); }
std::expected<void, Api::Error> Server::Start(const std::string &endpoint) {
  if (server_ || !endpoint.starts_with("127.0.0.1:"))
    return std::unexpected(Api::Error::Malformed);
  grpc::ServerBuilder b;
  b.SetMaxReceiveMessageSize(1048576);
  b.SetMaxSendMessageSize(1048576);
  int port = 0;
  b.AddListeningPort(endpoint, grpc::InsecureServerCredentials(), &port);
  b.RegisterService(this);
  server_ = b.BuildAndStart();
  if (!server_ || port == 0)
    return std::unexpected(Api::Error::Busy);
  if (logger_)
    logger_->info("Gateway listening {}", endpoint);
  return {};
}
void Server::Stop() {
  if (server_) {
    server_->Shutdown(std::chrono::system_clock::now() +
                      std::chrono::seconds(1));
    server_->Wait();
    server_.reset();
  }
}
grpc::Status Server::GetView(grpc::ServerContext *, const Api::Wire::Query *,
                             Api::Wire::WorldSnapshot *out) {
  auto d = provider_.View();
  if (!d || !d->ready)
    return {grpc::StatusCode::UNAVAILABLE, "provider_not_ready"};
  *out = Api::Encode(*d);
  return grpc::Status::OK;
}
grpc::Status Server::Connect(grpc::ServerContext *, const Api::Wire::Query *,
                             Api::Wire::Session *out) {
  auto d = provider_.View();
  if (!d)
    return {grpc::StatusCode::UNAVAILABLE, "provider_not_ready"};
  out->set_schema_version(1);
  out->set_session_id(d->sessionId);
  out->set_observer_id(d->observerId);
  out->set_ready(d->ready);
  return grpc::Status::OK;
}
grpc::Status Server::DiscoverOrders(grpc::ServerContext *,
                                    const Api::Wire::Query *q,
                                    Api::Wire::OrderDiscovery *out) {
  auto d = provider_.View();
  if (!d)
    return {grpc::StatusCode::UNAVAILABLE, "provider_not_ready"};
  out->set_session_id(d->sessionId);
  out->set_entity_id(q->entity_id());
  if (q->session_id() != d->sessionId)
    return {grpc::StatusCode::FAILED_PRECONDITION, "stale_session"};
  for (auto &u : d->units)
    if (u.id == q->entity_id()) {
      for (auto &e : u.orders) {
        auto x = out->add_orders();
        x->set_kind(e.kind);
        x->set_availability(
            static_cast<Api::Wire::Availability>(e.availability));
        x->set_reason(e.reason);
        x->set_argument_semantics(e.arguments);
      }
      return grpc::Status::OK;
    }
  return {grpc::StatusCode::NOT_FOUND, "wrong_side_or_invalid_entity"};
}
grpc::Status Server::SubmitOrder(grpc::ServerContext *,
                                 const Api::Wire::OrderRequest *q,
                                 Api::Wire::OrderResult *out) {
  *out = Api::Encode(provider_.Submit(Api::Decode(*q)));
  return grpc::Status::OK;
}
grpc::Status Server::GetStatus(grpc::ServerContext *, const Api::Wire::Query *q,
                               Api::Wire::OrderResult *out) {
  *out = Api::Encode(provider_.Status(q->session_id(), q->request_id()));
  return grpc::Status::OK;
}
} // namespace NavalCommand::Gateway::Grpc
