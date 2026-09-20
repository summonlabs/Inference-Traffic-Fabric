cmake_minimum_required(VERSION 3.25)

project(ItfDownstreamConsumer LANGUAGES CXX)

# This project is deliberately independent: it consumes an installed
# Inference Traffic Fabric package and never reaches into the source tree.
find_package(InferenceTrafficFabric 1.0 REQUIRED)

add_executable(itf_consumer main.cpp)
target_link_libraries(itf_consumer PRIVATE itf::net itf::accel)

set_target_properties(itf_consumer PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
if(MSVC)
  target_compile_options(itf_consumer PRIVATE /W4 /WX /permissive- /utf-8)
else()
  target_compile_options(itf_consumer PRIVATE -Wall -Wextra -Werror)
endif()
