#include <NavalCommand/Gateway/Grpc/MockProvider.h>
#include <NavalCommand/Gateway/Grpc/Server.h>
#include <NavalCommand/Orders/Api/RouteCommand.h>
#include <gtest/gtest.h>
#include <mutex>
using namespace NavalCommand;

namespace {

class SnapshotProvider final : public Gateway::Api::IProvider {
	std::mutex m_Mutex;
	World::Api::View m_Snapshot;

public:
	void SetSnapshot(World::Api::View snapshot) {
		std::lock_guard lock(m_Mutex);
		m_Snapshot = std::move(snapshot);
	}

	std::expected<World::Api::View, Gateway::Api::Error> View() override {
		std::lock_guard lock(m_Mutex);
		return m_Snapshot;
	}

	Orders::Api::Result Submit(const Orders::Api::Request &request) override {
		return {request.sessionId, request.requestId, Orders::Api::Stage::Rejected, "fixture_read_only"};
	}

	Orders::Api::Result Status(const std::string &sessionId, const std::string &requestId) override {
		return {sessionId, requestId, Orders::Api::Stage::Unknown};
	}
};

}

TEST(Server, MissingEligibilityNeverMeansExecutable) {
	SnapshotProvider provider;
	World::Api::View fixtureView;
	fixtureView.sessionId = "fixture";
	fixtureView.ready = true;
	World::Api::UnitInstance unit;
	unit.Identity = Orders::Api::OwnUnitId{"own"};
	fixtureView.Instances.push_back(unit);
	provider.SetSnapshot(fixtureView);
	Gateway::Grpc::Server server(provider, {});
	ASSERT_TRUE(server.Start("127.0.0.1:50059"));
	auto client = navalcommand::gateway::v1::Gateway::NewStub(grpc::CreateChannel("127.0.0.1:50059", grpc::InsecureChannelCredentials()));
	navalcommand::gateway::v1::Query query;
	query.set_session_id("fixture");
	query.mutable_subject()->set_own_unit_id("own");
	navalcommand::gateway::v1::OrderDiscovery discovery;
	grpc::ClientContext missingContext;
	EXPECT_EQ(client->DiscoverOrders(&missingContext, query, &discovery).error_code(), grpc::StatusCode::FAILED_PRECONDITION);
	using Collection = World::Api::ObservedCollection<Orders::Api::Eligibility>;
	fixtureView.Instances[0].OrderEligibility = World::Api::OrderEligibilityState{{Collection::Coverage::Partial, {}}};
	provider.SetSnapshot(fixtureView);
	grpc::ClientContext partialContext;
	ASSERT_TRUE(client->DiscoverOrders(&partialContext, query, &discovery).ok());
	EXPECT_EQ(discovery.eligibility().coverage(), navalcommand::gateway::v1::COVERAGE_PARTIAL);
	EXPECT_EQ(discovery.eligibility().items_size(), 0);
	fixtureView.Instances[0].Identity = std::monostate{};
	provider.SetSnapshot(fixtureView);
	navalcommand::gateway::v1::WorldSnapshot snapshot;
	grpc::ClientContext invalidContext;
	EXPECT_EQ(client->GetView(&invalidContext, {}, &snapshot).error_code(), grpc::StatusCode::FAILED_PRECONDITION);
	server.Stop();
}
TEST(Provider, RejectionsDuplicatesAndSessionReset) {
	Gateway::Grpc::MockProvider provider;
	Orders::Api::Request request{"mock-session", "id", {"unit-1"}, 1, Orders::Api::RouteCommand{62, -10}};
	auto first = provider.Submit(request);
	auto latitude = provider.View()->Instances[0].Kinematics.Position.Value().Coordinates.latitudeDeg;
	EXPECT_EQ(provider.Submit(request).stage, first.stage);
	EXPECT_EQ(provider.View()->Instances[0].Kinematics.Position.Value().Coordinates.latitudeDeg, latitude);
	provider.Reset("new");
	EXPECT_EQ(provider.Submit(request).reason, "stale_session");
	request.sessionId = "new";
	provider.Block();
	EXPECT_EQ(provider.Submit(request).reason, "protected_native_action");
	EXPECT_EQ(provider.View()->Instances[0].Kinematics.Position.Value().Coordinates.latitudeDeg, 61.96);
}
TEST(Provider, BoundedLedger) {
	Gateway::Grpc::MockProvider provider;
	for (int requestIndex = 0; requestIndex < 128; ++requestIndex) {
		Orders::Api::Request request{"mock-session", std::to_string(requestIndex), {"invalid"}, 1, {}};
		EXPECT_EQ(provider.Submit(request).reason, "wrong_side_or_invalid_entity");
	}
	EXPECT_EQ(provider.Submit({"mock-session", "overflow", {"unit-1"}, 1, {}}).reason, "ledger_full");
}
TEST(Provider, SimulatedRouteSequence) {
	Gateway::Grpc::MockProvider provider;
	using namespace Orders::Api;
	Request request{"mock-session", "A", {"unit-1"}, 1, RouteCommand{62, -10, RouteMode::ReplaceRoute}};
	ASSERT_EQ(provider.Submit(request).routeAfter.size(), 1);
	request.requestId = "B";
	request.routeCommand->routeMode = RouteMode::AppendWaypoint;
	ASSERT_EQ(provider.Submit(request).routeAfter.size(), 2);
	EXPECT_EQ(provider.Status("mock-session", "B").stage, Stage::RouteMutated);
	request.requestId = "C";
	ASSERT_EQ(provider.Submit(request).routeAfter.size(), 3);
	EXPECT_EQ(provider.Submit(request).routeAfter.size(), 3);
	request.requestId = "D";
	request.routeCommand->routeMode = RouteMode::ReplaceRoute;
	auto receipt = provider.Submit(request);
	EXPECT_EQ(receipt.routeBefore.size(), 3);
	ASSERT_EQ(receipt.routeAfter.size(), 1);
	EXPECT_EQ(receipt.routeAfter[0].label, "NC-D");
	EXPECT_EQ(receipt.stage, Stage::RouteMutated);
	EXPECT_EQ(provider.Status("mock-session", "D").stage, Stage::MovementObserved);
	request.requestId = "idle-waypoint-active";
	request.routeCommand->routeMode = RouteMode::IdleWaypoint;
	EXPECT_EQ(provider.Submit(request).reason, "native_task_selection_active");
	request.requestId = "unsupported-launch";
	request.routeCommand.reset();
	request.facilityLaunch = FacilityLaunch{"class", "squadron", "loadout", 1};
	EXPECT_EQ(provider.Submit(request).reason, "unsupported_order");
}
TEST(Server, RealGrpcAndLoopbackGuard) {
	Gateway::Grpc::MockProvider provider;
	Gateway::Grpc::Server server(provider, {});
	EXPECT_FALSE(server.Start("0.0.0.0:50059"));
	ASSERT_TRUE(server.Start("127.0.0.1:50059"));
	auto client = navalcommand::gateway::v1::Gateway::NewStub(grpc::CreateChannel("127.0.0.1:50059", grpc::InsecureChannelCredentials()));
	grpc::ClientContext context;
	navalcommand::gateway::v1::WorldSnapshot snapshot;
	ASSERT_TRUE(client->GetView(&context, {}, &snapshot).ok());
	EXPECT_EQ(snapshot.instances_size(), 2);
	EXPECT_TRUE(snapshot.instances(0).subject().has_own_unit_id());
	EXPECT_TRUE(snapshot.instances(1).subject().has_track_id());
	EXPECT_TRUE(snapshot.instances(1).kinematics().position().has_estimated());
	navalcommand::gateway::v1::Query discoveryQuery;
	discoveryQuery.set_session_id("mock-session");
	discoveryQuery.mutable_subject()->set_own_unit_id("unit-1");
	navalcommand::gateway::v1::OrderDiscovery discovery;
	grpc::ClientContext ownContext;
	ASSERT_TRUE(client->DiscoverOrders(&ownContext, discoveryQuery, &discovery).ok());
	EXPECT_EQ(discovery.eligibility().items_size(), 3);
	EXPECT_EQ(discovery.eligibility().coverage(), navalcommand::gateway::v1::COVERAGE_COMPLETE);
	discoveryQuery.mutable_subject()->set_own_unit_id("contact-1");
	grpc::ClientContext trackContext;
	EXPECT_EQ(client->DiscoverOrders(&trackContext, discoveryQuery, &discovery).error_code(), grpc::StatusCode::NOT_FOUND);
	Orders::Api::Request trackRequest{"mock-session", "contact-command", {"contact-1"}, 1, Orders::Api::RouteCommand{62, -10}};
	EXPECT_EQ(provider.Submit(trackRequest).reason, "wrong_side_or_invalid_entity");
	discoveryQuery.mutable_subject()->set_track_id("unit-1");
	grpc::ClientContext taggedTrackContext;
	EXPECT_EQ(client->DiscoverOrders(&taggedTrackContext, discoveryQuery, &discovery).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
	navalcommand::gateway::v1::OrderRequest trackWire;
	trackWire.set_session_id("mock-session");
	trackWire.set_request_id("tagged-track-command");
	trackWire.mutable_subject()->set_track_id("unit-1");
	navalcommand::gateway::v1::OrderResult rejected;
	grpc::ClientContext trackOrderContext;
	EXPECT_EQ(client->SubmitOrder(&trackOrderContext, trackWire, &rejected).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
	server.Stop();
}
