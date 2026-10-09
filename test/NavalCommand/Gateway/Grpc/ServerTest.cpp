#include <NavalCommand/Gateway/Grpc/MockProvider.h>
#include <NavalCommand/Gateway/Grpc/Server.h>
#include <gtest/gtest.h>
using namespace NavalCommand;
TEST(Provider, RejectionsDuplicatesAndSessionReset) {
	Gateway::Grpc::MockProvider p;
	Orders::Api::Request r{"mock-session", "id", "unit-1", 1, Orders::Api::Waypoint{62, -10}};
	auto first = p.Submit(r);
	auto latitude = p.View()->units[0].position.latitudeDeg;
	EXPECT_EQ(p.Submit(r).stage, first.stage);
	EXPECT_EQ(p.View()->units[0].position.latitudeDeg, latitude);
	p.Reset("new");
	EXPECT_EQ(p.Submit(r).reason, "stale_session");
	r.sessionId = "new";
	p.Block();
	EXPECT_EQ(p.Submit(r).reason, "protected_native_action");
	EXPECT_EQ(p.View()->units[0].position.latitudeDeg, 61.96);
}
TEST(Provider, BoundedLedger) {
	Gateway::Grpc::MockProvider p;
	for (int i = 0; i < 128; ++i) {
		Orders::Api::Request r{"mock-session", std::to_string(i), "invalid", 1, {}};
		EXPECT_EQ(p.Submit(r).reason, "wrong_side_or_invalid_entity");
	}
	EXPECT_EQ(p.Submit({"mock-session", "overflow", "unit-1", 1, {}}).reason, "ledger_full");
}
TEST(Server, RealGrpcAndLoopbackGuard) {
	Gateway::Grpc::MockProvider p;
	Gateway::Grpc::Server server(p, {});
	EXPECT_FALSE(server.Start("0.0.0.0:50059"));
	ASSERT_TRUE(server.Start("127.0.0.1:50059"));
	auto client = navalcommand::gateway::v1::Gateway::NewStub(grpc::CreateChannel("127.0.0.1:50059", grpc::InsecureChannelCredentials()));
	grpc::ClientContext c;
	navalcommand::gateway::v1::WorldSnapshot w;
	ASSERT_TRUE(client->GetView(&c, {}, &w).ok());
	EXPECT_EQ(w.own_units_size(), 1);
	EXPECT_EQ(w.tracks_size(), 0);
	server.Stop();
}
