add_library(Gateway.Grpc.Mock INTERFACE)
target_include_directories(Gateway.Grpc.Mock INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(Gateway.Grpc.Mock INTERFACE Gateway.Grpc)
