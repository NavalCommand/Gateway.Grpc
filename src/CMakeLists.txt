add_library(NavalCommand.Gateway.Grpc STATIC NavalCommand/Gateway/Grpc/Server.cpp)
target_include_directories(NavalCommand.Gateway.Grpc PUBLIC "${PROJECT_SOURCE_DIR}/public")
target_link_libraries(NavalCommand.Gateway.Grpc PUBLIC NavalCommand.Gateway.Api NavalCommand.Gateway.Grpc.Api)
NavalCommandConfigureTarget(NavalCommand.Gateway.Grpc)
