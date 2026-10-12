add_library(NavalCommand.Gateway.Grpc.Mock INTERFACE)
target_include_directories(NavalCommand.Gateway.Grpc.Mock INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(NavalCommand.Gateway.Grpc.Mock INTERFACE NavalCommand.Gateway.Grpc)
