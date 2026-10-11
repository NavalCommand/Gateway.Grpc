#include <NavalCommand/Gateway/Grpc/MockProvider.h>
#include <NavalCommand/Gateway/Grpc/Server.h>
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
		return {request.SessionId, request.RequestId, Orders::Api::Stage::Rejected, "fixture_read_only"};
	}
	Orders::Api::Result Status(const std::string &sessionId, const std::string &requestId) override {
		return {sessionId, requestId, Orders::Api::Stage::Unknown};
	}
};
Orders::Api::Request RouteRequest(std::string requestId, Orders::Api::RouteMode mode = Orders::Api::RouteMode::IdleWaypoint) {
	return {"mock-session", std::move(requestId), {"unit-1"}, 1, Orders::Api::RouteCommand{{62}, {-10}, mode}};
}
}

TEST(Server, MissingAvailabilityNeverMeansExecutable) {
	SnapshotProvider provider;
	Gateway::Api::ProviderObservation observation;
	observation.Snapshot.SessionId = "fixture";
	observation.Snapshot.Ready = true;
	Units::Api::UnitInstance unit;
	unit.Identity = {"own"};
	observation.Snapshot.Instances.push_back(unit);
	provider.SetSnapshot(observation);
	Gateway::Grpc::Server server(provider, {});
	ASSERT_TRUE(server.Start("127.0.0.1:50059"));
	auto client = navalcommand::gateway::v1::Gateway::NewStub(grpc::CreateChannel("127.0.0.1:50059", grpc::InsecureChannelCredentials()));
	navalcommand::gateway::v1::Query query;
	query.set_session_id("fixture");
	query.mutable_unit_id()->set_value("own");
	navalcommand::gateway::v1::OrderDiscovery discovery;
	grpc::ClientContext missingContext;
	EXPECT_EQ(client->DiscoverOrders(&missingContext, query, &discovery).error_code(), grpc::StatusCode::FAILED_PRECONDITION);
	observation.OrderAvailability.push_back({{"own"}, 1, {}});
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
	EXPECT_EQ(discovery.unit_id().value(), "own");
	observation.OrderAvailability[0].Orders = {Collection::Coverage::Complete, {}};
	provider.SetSnapshot(observation);
	grpc::ClientContext emptyContext;
	ASSERT_TRUE(client->DiscoverOrders(&emptyContext, query, &discovery).ok());
	EXPECT_EQ(discovery.eligibility().coverage(), navalcommand::gateway::v1::COVERAGE_COMPLETE);
	observation.Snapshot.Instances[0].Identity.Value.clear();
	provider.SetSnapshot(observation);
	navalcommand::gateway::v1::WorldSnapshot snapshot;
	grpc::ClientContext invalidContext;
	EXPECT_EQ(client->GetView(&invalidContext, {}, &snapshot).error_code(), grpc::StatusCode::FAILED_PRECONDITION);
	server.Stop();
}

TEST(Provider, RejectionsDuplicatesAndSessionReset) {
	Gateway::Grpc::MockProvider provider;
	auto request = RouteRequest("id");
	const auto first = provider.Submit(request);
	const auto latitude = provider.View()->Snapshot.Instances[0].Kinematics.Position.Value().Coordinates.Latitude.Value;
	EXPECT_EQ(provider.Submit(request).Stage, first.Stage);
	EXPECT_EQ(provider.View()->Snapshot.Instances[0].Kinematics.Position.Value().Coordinates.Latitude.Value, latitude);
	provider.Reset("new");
	EXPECT_EQ(provider.Submit(request).Reason, "stale_session");
	request.SessionId = "new";
	provider.Block();
	EXPECT_EQ(provider.Submit(request).Reason, "protected_native_action");
	EXPECT_EQ(provider.View()->Snapshot.Instances[0].Kinematics.Position.Value().Coordinates.Latitude.Value, 61.96);
}

TEST(Provider, IdentityIsNotAuthorityEvenWithStaleAvailability) {
	Gateway::Grpc::MockProvider provider;
	auto request = RouteRequest("enemy");
	request.Unit = provider.View()->Snapshot.Instances[1].Identity;
	EXPECT_EQ(provider.Submit(request).Reason, "wrong_side_or_invalid_entity");
	request.Unit = provider.View()->Snapshot.Instances[0].Identity;
	request.RequestId = "control-revoked";
	provider.RevokeControl();
	// The old executable overlay and reused public handle cannot confer current control.
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
	EXPECT_EQ(observation->Snapshot.Instances[0].Navigation->PlannedRoute.Value().size(), 0);
	EXPECT_FALSE(observation->Snapshot.Instances[1].Navigation.has_value());
	EXPECT_FALSE(observation->Snapshot.Instances[1].Observation.MeasuredAt.HasValue());
	provider.ForgetAvailability();
	EXPECT_EQ(provider.Submit(RouteRequest("unknown")).Reason, "order_availability_unknown");
}

TEST(Provider, BoundedLedger) {
	Gateway::Grpc::MockProvider provider;
	for (int index = 0; index < 128; ++index) {
		auto request = RouteRequest(std::to_string(index));
		request.Unit = {"invalid"};
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
	EXPECT_EQ(receipt.RouteAfter[0].Label, "NC-D");
	EXPECT_EQ(receipt.Stage, Stage::RouteMutated);
	EXPECT_EQ(provider.Status("mock-session", "D").Stage, Stage::MovementObserved);
	request.RequestId = "idle-waypoint-active";
	std::get<RouteCommand>(request.Payload).Mode = RouteMode::IdleWaypoint;
	EXPECT_EQ(provider.Submit(request).Reason, "native_task_selection_active");
	request.RequestId = "unsupported-launch";
	request.Payload = FacilityLaunch{{{"class"}, "squadron", {"loadout"}}, 1};
	EXPECT_EQ(provider.Submit(request).Reason, "unsupported_order");
}

TEST(Server, SameIdentityPathAndMalformedOrders) {
	Gateway::Grpc::MockProvider provider;
	Gateway::Grpc::Server server(provider, {});
	EXPECT_FALSE(server.Start("0.0.0.0:50059"));
	ASSERT_TRUE(server.Start("127.0.0.1:50059"));
	auto client = navalcommand::gateway::v1::Gateway::NewStub(grpc::CreateChannel("127.0.0.1:50059", grpc::InsecureChannelCredentials()));
	grpc::ClientContext context;
	navalcommand::gateway::v1::WorldSnapshot snapshot;
	ASSERT_TRUE(client->GetView(&context, {}, &snapshot).ok());
	ASSERT_EQ(snapshot.instances_size(), 2);
	EXPECT_EQ(snapshot.instances(0).unit_id().value(), "unit-1");
	EXPECT_EQ(snapshot.instances(1).unit_id().value(), "contact-1");
	EXPECT_TRUE(snapshot.instances(1).kinematics().position().has_estimated());
	EXPECT_FALSE(snapshot.instances(1).observation().measured_at_seconds().has_known());
	navalcommand::gateway::v1::Query query;
	query.set_session_id("mock-session");
	*query.mutable_unit_id() = snapshot.instances(0).unit_id();
	navalcommand::gateway::v1::OrderDiscovery discovery;
	grpc::ClientContext ownContext;
	ASSERT_TRUE(client->DiscoverOrders(&ownContext, query, &discovery).ok());
	EXPECT_EQ(discovery.eligibility().items_size(), 3);
	EXPECT_EQ(discovery.unit_id().value(), snapshot.instances(0).unit_id().value());
	*query.mutable_unit_id() = snapshot.instances(1).unit_id();
	grpc::ClientContext enemyContext;
	EXPECT_EQ(client->DiscoverOrders(&enemyContext, query, &discovery).error_code(), grpc::StatusCode::FAILED_PRECONDITION);
	auto enemy = Gateway::Api::Encode(RouteRequest("contact-command"));
	*enemy.mutable_unit_id() = snapshot.instances(1).unit_id();
	navalcommand::gateway::v1::OrderResult rejected;
	grpc::ClientContext enemyOrderContext;
	ASSERT_TRUE(client->SubmitOrder(&enemyOrderContext, enemy, &rejected).ok());
	EXPECT_EQ(rejected.reason(), "wrong_side_or_invalid_entity");
	enemy.clear_payload();
	grpc::ClientContext malformedContext;
	EXPECT_EQ(client->SubmitOrder(&malformedContext, enemy, &rejected).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
	server.Stop();
}
