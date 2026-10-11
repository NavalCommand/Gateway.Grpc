#include <NavalCommand/Gateway/Grpc/MockProvider.h>
#include <NavalCommand/Gateway/Grpc/Server.h>
#include <NavalCommand/Observations/Api/UnitObservation.h>
#include <NavalCommand/Orders/Api/RouteCommand.h>
#include <gtest/gtest.h>
#include <mutex>
using namespace NavalCommand;

namespace {
class SnapshotProvider final : public Gateway::Api::IProvider {
	std::mutex m_Mutex;
	Gateway::Api::ProviderObservation m_Observation;
public:
	void SetSnapshot(Gateway::Api::ProviderObservation observation) {
		std::lock_guard lock(m_Mutex);
		m_Observation = std::move(observation);
	}
	std::expected<Gateway::Api::ProviderObservation, Gateway::Api::Error> View() override {
		std::lock_guard lock(m_Mutex);
		return m_Observation;
	}
	Orders::Api::Result Submit(const Orders::Api::Request &request) override {
		Orders::Api::Result result;
		result.SessionId = request.SessionId;
		result.RequestId = request.RequestId;
		result.Observer = request.Observer;
		result.Stage = Orders::Api::Stage::Rejected;
		result.Reason = "fixture_read_only";
		return result;
	}
	Orders::Api::Result Status(const std::string &sessionId, const std::string &requestId) override {
		Orders::Api::Result result;
		result.SessionId = sessionId;
		result.RequestId = requestId;
		result.Stage = Orders::Api::Stage::Unknown;
		return result;
	}
};
Orders::Api::Request RouteRequest(std::string requestId, Orders::Api::RouteMode mode = Orders::Api::RouteMode::IdleWaypoint) {
	return {"mock-session", std::move(requestId), {"mock-observer"}, {"contact-alpha"}, 1, Orders::Api::RouteCommand{{62}, {-10}, mode}};
}
}

TEST(Server, MissingAvailabilityNeverMeansExecutable) {
	SnapshotProvider provider;
	Gateway::Api::ProviderObservation observation;
	observation.Snapshot.SessionId = "fixture";
	observation.Snapshot.Observer = {"fixture-observer"};
	observation.Snapshot.Ready = true;
	Observations::Api::UnitObservation unit;
	unit.Identity = {"contact-own"};
	observation.Snapshot.Units.push_back(unit);
	provider.SetSnapshot(observation);
	Gateway::Grpc::Server server(provider, {});
	ASSERT_TRUE(server.Start("127.0.0.1:50059"));
	auto client = navalcommand::gateway::v1::Gateway::NewStub(grpc::CreateChannel("127.0.0.1:50059", grpc::InsecureChannelCredentials()));
	navalcommand::gateway::v1::Query query;
	query.set_session_id("fixture");
	query.mutable_observer()->set_value("fixture-observer");
	query.mutable_observation()->set_value("contact-own");
	navalcommand::gateway::v1::OrderDiscovery discovery;
	grpc::ClientContext missingContext;
	EXPECT_EQ(client->DiscoverOrders(&missingContext, query, &discovery).error_code(), grpc::StatusCode::FAILED_PRECONDITION);
	observation.OrderAvailability.push_back({{"fixture-observer"}, {"contact-own"}, 1, {}});
	provider.SetSnapshot(observation);
	grpc::ClientContext unknownContext;
	EXPECT_EQ(client->DiscoverOrders(&unknownContext, query, &discovery).error_code(), grpc::StatusCode::FAILED_PRECONDITION);
	using Collection = Uncertainty::Api::ObservedCollection<Orders::Api::Eligibility>;
	observation.OrderAvailability[0].Orders = {Collection::Coverage::Partial, {}};
	provider.SetSnapshot(observation);
	grpc::ClientContext partialContext;
	ASSERT_TRUE(client->DiscoverOrders(&partialContext, query, &discovery).ok());
	EXPECT_EQ(discovery.eligibility().coverage(), navalcommand::gateway::v1::COVERAGE_PARTIAL);
	EXPECT_EQ(discovery.eligibility().items_size(), 0);
	EXPECT_EQ(discovery.observation().value(), "contact-own");
	observation.OrderAvailability[0].Orders = {Collection::Coverage::Complete, {}};
	provider.SetSnapshot(observation);
	grpc::ClientContext emptyContext;
	ASSERT_TRUE(client->DiscoverOrders(&emptyContext, query, &discovery).ok());
	EXPECT_EQ(discovery.eligibility().coverage(), navalcommand::gateway::v1::COVERAGE_COMPLETE);
	observation.Snapshot.Units[0].Identity.Value.clear();
	provider.SetSnapshot(observation);
	navalcommand::gateway::v1::ObserverSnapshot snapshot;
	grpc::ClientContext invalidContext;
	EXPECT_EQ(client->GetView(&invalidContext, query, &snapshot).error_code(), grpc::StatusCode::FAILED_PRECONDITION);
	server.Stop();
}

TEST(Provider, RejectionsDuplicatesAndSessionReset) {
	Gateway::Grpc::MockProvider provider;
	auto request = RouteRequest("id");
	const auto first = provider.Submit(request);
	const auto latitude = provider.View()->Snapshot.Units[0].Kinematics.Position.Value().Latitude.Value;
	EXPECT_EQ(provider.Submit(request).Stage, first.Stage);
	EXPECT_EQ(provider.View()->Snapshot.Units[0].Kinematics.Position.Value().Latitude.Value, latitude);
	provider.Reset("new");
	EXPECT_EQ(provider.Submit(request).Reason, "stale_session");
	request.SessionId = "new";
	provider.Block();
	EXPECT_EQ(provider.Submit(request).Reason, "protected_native_action");
	EXPECT_EQ(provider.View()->Snapshot.Units[0].Kinematics.Position.Value().Latitude.Value, 61.96);
}

TEST(Provider, ObserverScopedIdentityIsNotAuthority) {
	Gateway::Grpc::MockProvider provider;
	auto request = RouteRequest("false-contact");
	request.Observation = provider.View()->Snapshot.Units[1].Identity;
	EXPECT_EQ(provider.Submit(request).Reason, "wrong_side_or_invalid_entity");
	request.Observation = provider.View()->Snapshot.Units[0].Identity;
	request.RequestId = "wrong-observer";
	request.Observer = {"another-observer"};
	EXPECT_EQ(provider.Submit(request).Reason, "wrong_observer_or_invalid_entity");
	request.Observer = {"mock-observer"};
	request.RequestId = "control-revoked";
	provider.RevokeControl();
	EXPECT_EQ(provider.View()->OrderAvailability[0].Orders.At(0).Availability, Orders::Api::Availability::Executable);
	EXPECT_EQ(provider.Submit(request).Reason, "wrong_side_or_invalid_entity");
	provider.Reset("next-session");
	request.RequestId = "reused-handle-old-session";
	EXPECT_EQ(provider.Submit(request).Reason, "stale_session");
}

TEST(Provider, UnknownAvailabilityAndRouteKnowledgeRemainDistinct) {
	Gateway::Grpc::MockProvider provider;
	const auto observation = provider.View();
	ASSERT_TRUE(observation);
	EXPECT_EQ(observation->Snapshot.Units[0].Navigation->PlannedRoute.Value().size(), 0);
	EXPECT_FALSE(observation->Snapshot.Units[1].Navigation.has_value());
	EXPECT_FALSE(observation->Snapshot.Units[1].Evidence.MeasuredAt.has_value());
	provider.ForgetAvailability();
	EXPECT_EQ(provider.Submit(RouteRequest("unknown")).Reason, "order_availability_unknown");
}

TEST(Provider, BoundedLedger) {
	Gateway::Grpc::MockProvider provider;
	for (int index = 0; index < 128; ++index) {
		auto request = RouteRequest(std::to_string(index));
		request.Observation = {"invalid"};
		EXPECT_EQ(provider.Submit(request).Reason, "wrong_side_or_invalid_entity");
	}
	EXPECT_EQ(provider.Submit(RouteRequest("overflow")).Reason, "ledger_full");
}

TEST(Provider, SimulatedRouteSequence) {
	Gateway::Grpc::MockProvider provider;
	using namespace Orders::Api;
	auto request = RouteRequest("A", RouteMode::ReplaceRoute);
	ASSERT_EQ(provider.Submit(request).RouteAfter.size(), 1);
	request.RequestId = "B";
	std::get<RouteCommand>(request.Payload).Mode = RouteMode::AppendWaypoint;
	ASSERT_EQ(provider.Submit(request).RouteAfter.size(), 2);
	EXPECT_EQ(provider.Status("mock-session", "B").Stage, Stage::RouteMutated);
	request.RequestId = "C";
	ASSERT_EQ(provider.Submit(request).RouteAfter.size(), 3);
	EXPECT_EQ(provider.Submit(request).RouteAfter.size(), 3);
	request.RequestId = "D";
	std::get<RouteCommand>(request.Payload).Mode = RouteMode::ReplaceRoute;
	auto receipt = provider.Submit(request);
	EXPECT_EQ(receipt.RouteBefore.size(), 3);
	ASSERT_EQ(receipt.RouteAfter.size(), 1);
	EXPECT_EQ(receipt.RouteAfter[0].Position.Longitude.Value, -10);
	EXPECT_EQ(receipt.Stage, Stage::RouteMutated);
	EXPECT_EQ(provider.Status("mock-session", "D").Stage, Stage::MovementObserved);
	request.RequestId = "idle-waypoint-active";
	std::get<RouteCommand>(request.Payload).Mode = RouteMode::IdleWaypoint;
	EXPECT_EQ(provider.Submit(request).Reason, "native_task_selection_active");
	request.RequestId = "unsupported-launch";
	request.Payload = LaunchAircraft{{{"class"}, "squadron", {"loadout"}}, 1};
	EXPECT_EQ(provider.Submit(request).Reason, "unsupported_order");
}

TEST(Server, ObserverSnapshotAndOrderOverlayNeverExposeTruthIdentity) {
	Gateway::Grpc::MockProvider provider;
	Gateway::Grpc::Server server(provider, {});
	EXPECT_FALSE(server.Start("0.0.0.0:50059"));
	ASSERT_TRUE(server.Start("127.0.0.1:50059"));
	auto client = navalcommand::gateway::v1::Gateway::NewStub(grpc::CreateChannel("127.0.0.1:50059", grpc::InsecureChannelCredentials()));
	navalcommand::gateway::v1::Session session;
	grpc::ClientContext connectContext;
	ASSERT_TRUE(client->Connect(&connectContext, {}, &session).ok());
	EXPECT_EQ(session.schema_version(), 4);
	navalcommand::gateway::v1::Query viewQuery;
	viewQuery.set_session_id(session.session_id());
	*viewQuery.mutable_observer() = session.observer();
	grpc::ClientContext context;
	navalcommand::gateway::v1::ObserverSnapshot snapshot;
	ASSERT_TRUE(client->GetView(&context, viewQuery, &snapshot).ok());
	EXPECT_EQ(snapshot.schema_version(), 4);
	auto wrongSessionQuery = viewQuery;
	wrongSessionQuery.set_session_id("old-session");
	grpc::ClientContext wrongSessionContext;
	EXPECT_EQ(client->GetView(&wrongSessionContext, wrongSessionQuery, &snapshot).error_code(), grpc::StatusCode::FAILED_PRECONDITION);
	auto wrongObserverQuery = viewQuery;
	wrongObserverQuery.mutable_observer()->set_value("other-observer");
	grpc::ClientContext wrongObserverContext;
	EXPECT_EQ(client->GetView(&wrongObserverContext, wrongObserverQuery, &snapshot).error_code(), grpc::StatusCode::NOT_FOUND);
	ASSERT_EQ(snapshot.units_size(), 2);
	EXPECT_EQ(snapshot.units(0).identity().value(), "contact-alpha");
	EXPECT_EQ(snapshot.units(1).identity().value(), "contact-beta");
	EXPECT_TRUE(snapshot.units(1).type().has_estimated());
	EXPECT_FALSE(snapshot.units(1).evidence().measured_at_seconds().has_known());
	EXPECT_EQ(snapshot.GetDescriptor()->FindFieldByName("units")->message_type()->FindFieldByName("unit_id"), nullptr);
	EXPECT_EQ(snapshot.observer().value(), "mock-observer");
	navalcommand::gateway::v1::Query query;
	query.set_session_id("mock-session");
	*query.mutable_observer() = snapshot.observer();
	*query.mutable_observation() = snapshot.units(0).identity();
	navalcommand::gateway::v1::OrderDiscovery discovery;
	grpc::ClientContext ownContext;
	ASSERT_TRUE(client->DiscoverOrders(&ownContext, query, &discovery).ok());
	EXPECT_EQ(discovery.eligibility().items_size(), 3);
	EXPECT_EQ(discovery.observation().value(), snapshot.units(0).identity().value());
	*query.mutable_observation() = snapshot.units(1).identity();
	grpc::ClientContext falseContactContext;
	const auto unavailableDiscovery = client->DiscoverOrders(&falseContactContext, query, &discovery);
	EXPECT_EQ(unavailableDiscovery.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
	EXPECT_EQ(unavailableDiscovery.error_message(), "order_availability_unknown");
	auto request = Gateway::Api::Encode(RouteRequest("contact-command"));
	*request.mutable_observation() = snapshot.units(1).identity();
	navalcommand::gateway::v1::OrderResult rejected;
	grpc::ClientContext falseOrderContext;
	ASSERT_TRUE(client->SubmitOrder(&falseOrderContext, request, &rejected).ok());
	EXPECT_EQ(rejected.reason(), "wrong_side_or_invalid_entity");
	request.clear_payload();
	grpc::ClientContext malformedContext;
	EXPECT_EQ(client->SubmitOrder(&malformedContext, request, &rejected).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
	server.Stop();
}
