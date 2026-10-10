#include <NavalCommand/Gateway/Grpc/MockProvider.h>
#include <NavalCommand/Gateway/Grpc/Server.h>
#include <gtest/gtest.h>
using namespace NavalCommand;
TEST(Provider, RejectionsDuplicatesAndSessionReset) {
	Gateway::Grpc::MockProvider provider;
	Orders::Api::Request request{"mock-session", "id", "unit-1", 1, Orders::Api::Waypoint{62, -10}};
	auto first = provider.Submit(request);
	auto latitude = provider.View()->units[0].position.latitudeDeg;
	EXPECT_EQ(provider.Submit(request).stage, first.stage);
	EXPECT_EQ(provider.View()->units[0].position.latitudeDeg, latitude);
	provider.Reset("new");
	EXPECT_EQ(provider.Submit(request).reason, "stale_session");
	request.sessionId = "new";
	provider.Block();
	EXPECT_EQ(provider.Submit(request).reason, "protected_native_action");
	EXPECT_EQ(provider.View()->units[0].position.latitudeDeg, 61.96);
}
TEST(Provider, BoundedLedger) {
	Gateway::Grpc::MockProvider provider;
	for (int requestIndex = 0; requestIndex < 128; ++requestIndex) {
		Orders::Api::Request request{"mock-session", std::to_string(requestIndex), "invalid", 1, {}};
		EXPECT_EQ(provider.Submit(request).reason, "wrong_side_or_invalid_entity");
	}
	EXPECT_EQ(provider.Submit({"mock-session", "overflow", "unit-1", 1, {}}).reason, "ledger_full");
}
TEST(Provider, SimulatedRouteSequence) {
	Gateway::Grpc::MockProvider provider;
	using namespace Orders::Api;
	Request request{"mock-session", "A", "unit-1", 1, Waypoint{62, -10, RouteMode::ReplaceRoute}};
	ASSERT_EQ(provider.Submit(request).routeAfter.size(), 1);
	request.requestId = "B";
	request.waypoint->routeMode = RouteMode::AppendWaypoint;
	ASSERT_EQ(provider.Submit(request).routeAfter.size(), 2);
	EXPECT_EQ(provider.Status("mock-session", "B").stage, Stage::RouteMutated);
	request.requestId = "C";
	ASSERT_EQ(provider.Submit(request).routeAfter.size(), 3);
	EXPECT_EQ(provider.Submit(request).routeAfter.size(), 3);
	request.requestId = "D";
	request.waypoint->routeMode = RouteMode::ReplaceRoute;
	auto receipt = provider.Submit(request);
	EXPECT_EQ(receipt.routeBefore.size(), 3);
	ASSERT_EQ(receipt.routeAfter.size(), 1);
	EXPECT_EQ(receipt.routeAfter[0].label, "NC-D");
	EXPECT_EQ(receipt.stage, Stage::RouteMutated);
	EXPECT_EQ(provider.Status("mock-session", "D").stage, Stage::MovementObserved);
	request.requestId = "legacy-active";
	request.waypoint->routeMode = RouteMode::LegacyIdle;
	EXPECT_EQ(provider.Submit(request).reason, "native_task_selection_active");
	request.requestId = "unsupported-launch";
	request.waypoint.reset();
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
	EXPECT_EQ(snapshot.own_units_size(), 1);
	EXPECT_EQ(snapshot.tracks_size(), 0);
	server.Stop();
}
