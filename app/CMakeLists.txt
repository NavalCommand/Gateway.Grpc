add_executable(NavalCommand.Gateway.Grpc.MockHost NavalCommand/Gateway/Grpc/MockHost.cpp)
target_link_libraries(NavalCommand.Gateway.Grpc.MockHost PRIVATE NavalCommand.Gateway.Grpc.Mock)
NavalCommandConfigureTarget(NavalCommand.Gateway.Grpc.MockHost)
