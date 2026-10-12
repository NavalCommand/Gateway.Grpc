#include <NavalCommand/Gateway/Grpc/MockProvider.h>
#include <NavalCommand/Gateway/Grpc/Server.h>
#include <gtest/gtest.h>

TEST(Server, RejectsExternalNetworkBinding)
{
	NavalCommand::Gateway::Grpc::MockProvider provider;
	NavalCommand::Gateway::Grpc::Server server{provider};
	EXPECT_FALSE(server.Start("0.0.0.0:50051"));
	ASSERT_TRUE(server.Start("127.0.0.1:0"));
	EXPECT_GT(server.Port(), 0);
	server.Stop();
}
