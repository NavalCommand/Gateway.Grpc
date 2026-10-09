add_executable(mock-host NavalCommand/Gateway/Grpc/MockHost.cpp)
target_link_libraries(mock-host PRIVATE Gateway.Grpc.Mock)
NavalCommandConfigureTarget(mock-host)
