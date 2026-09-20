cmake_minimum_required(VERSION 3.25)

project(
  InferenceTrafficFabric
  VERSION 1.0.0
  DESCRIPTION "Vendor-neutral C++20 runtime for governing AI inference network traffic"
  HOMEPAGE_URL "https://github.com/summonlabs/Inference-Traffic-Fabric"
  LANGUAGES CXX
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 20)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# The library is portable C++20 with no third-party dependencies beyond the
# platform threading and socket libraries.
if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

option(ITF_BUILD_TOOLS "Build the coordinator service and CLI tooling" ON)
option(ITF_BUILD_EXAMPLES "Build the examples" ON)
option(ITF_BUILD_TESTS "Build the test suite" ON)
option(ITF_STRICT_WARNINGS "Treat first-party compiler warnings as errors" ON)
option(ITF_ENABLE_ASAN "Build with AddressSanitizer where the toolchain supports it" OFF)
set(ITF_CUDA "AUTO" CACHE STRING "Accelerator probe backend: AUTO, ON or OFF")
set_property(CACHE ITF_CUDA PROPERTY STRINGS AUTO ON OFF)

if(ITF_STRICT_WARNINGS)
  if(MSVC)
    set(ITF_WARNING_FLAGS /W4 /WX /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8)
  else()
    set(ITF_WARNING_FLAGS -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion)
  endif()
endif()

if(ITF_ENABLE_ASAN)
  if(MSVC)
    add_compile_options(/fsanitize=address)
    add_link_options(/fsanitize=address)
  else()
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
  endif()
  add_compile_definitions(ITF_ASAN_ENABLED=1)
endif()

function(itf_configure_target target)
  target_include_directories(
    ${target}
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
           "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  )
  if(ITF_STRICT_WARNINGS)
    # Warning flags are C++-only: nvcc must not receive host warning switches.
    foreach(itf_flag IN LISTS ITF_WARNING_FLAGS)
      target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${itf_flag}>")
    endforeach()
  endif()
  set_target_properties(
    ${target} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
  )
endfunction()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Core library: deterministic policy and state machine. No sockets.
# ---------------------------------------------------------------------------
add_library(
  itf_core
  src/error.cpp
  src/time.cpp
  src/crc32c.cpp
  src/codec.cpp
  src/log.cpp
  src/traffic.cpp
  src/authority.cpp
  src/policy.cpp
  src/evidence.cpp
  src/decision.cpp
  src/accounting.cpp
  src/ledger.cpp
  src/serialize.cpp
  src/coordinator.cpp
  src/persistence.cpp
)
add_library(itf::core ALIAS itf_core)
set_target_properties(itf_core PROPERTIES EXPORT_NAME core)
itf_configure_target(itf_core)
target_link_libraries(itf_core PUBLIC Threads::Threads)

# ---------------------------------------------------------------------------
# Network library: framing, sessions, client.
# ---------------------------------------------------------------------------
add_library(
  itf_net
  src/wire.cpp
  src/messages.cpp
  src/net.cpp
  src/session.cpp
  src/client.cpp
)
add_library(itf::net ALIAS itf_net)
set_target_properties(itf_net PROPERTIES EXPORT_NAME net)
itf_configure_target(itf_net)
target_link_libraries(itf_net PUBLIC itf_core PRIVATE Threads::Threads)
if(WIN32)
  target_link_libraries(itf_net PRIVATE ws2_32)
endif()

# ---------------------------------------------------------------------------
# Accelerator probe: a real device binding when CUDA is available, otherwise
# an explicitly UNSUPPORTED stub.
# ---------------------------------------------------------------------------
set(ITF_CUDA_ENABLED OFF)
if(NOT ITF_CUDA STREQUAL "OFF")
  include(CheckLanguage)
  check_language(CUDA)
  if(CMAKE_CUDA_COMPILER)
    enable_language(CUDA)
    set(ITF_CUDA_ENABLED ON)
  elseif(ITF_CUDA STREQUAL "ON")
    message(FATAL_ERROR "ITF_CUDA=ON but no CUDA compiler was found")
  endif()
endif()

if(ITF_CUDA_ENABLED)
  # The accelerator backend is a shared library so that the toolkit
  # dependency is resolved inside it and never leaks into the exported package.
  add_library(itf_accel SHARED src/accel_common.cpp src/accel_cuda_host.cpp src/accel_cuda.cu)
  set_target_properties(
    itf_accel
    PROPERTIES CUDA_STANDARD 17 CUDA_STANDARD_REQUIRED ON WINDOWS_EXPORT_ALL_SYMBOLS ON
  )
  target_include_directories(itf_accel PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
else()
  add_library(itf_accel src/accel_common.cpp src/accel_stub.cpp)
endif()
add_library(itf::accel ALIAS itf_accel)
set_target_properties(itf_accel PROPERTIES EXPORT_NAME accel)
itf_configure_target(itf_accel)
target_compile_definitions(itf_accel PRIVATE ITF_CUDA_ENABLED=${ITF_CUDA_ENABLED})
if(ITF_CUDA_ENABLED)
  target_compile_definitions(itf_accel PUBLIC ITF_ACCEL_BACKEND_CUDA=1)
endif()

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------
add_library(itf_service STATIC tools/service.cpp)
add_library(itf::service ALIAS itf_service)
itf_configure_target(itf_service)
target_include_directories(itf_service PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/tools")
target_link_libraries(itf_service PUBLIC itf_net itf_accel)

if(ITF_BUILD_TOOLS)
  add_executable(itf_coordinator tools/itf_coordinator.cpp)
  itf_configure_target(itf_coordinator)
  target_link_libraries(itf_coordinator PRIVATE itf_service)

  add_executable(itfctl tools/itfctl.cpp)
  itf_configure_target(itfctl)
  target_link_libraries(itfctl PRIVATE itf_service)

  add_executable(itf_sim tools/itf_sim.cpp)
  itf_configure_target(itf_sim)
  target_link_libraries(itf_sim PRIVATE itf_service)
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
if(ITF_BUILD_EXAMPLES)
  foreach(example itf_example_policy itf_example_lifecycle itf_example_transport itf_example_accel)
    add_executable(${example} examples/${example}.cpp)
    itf_configure_target(${example})
    target_link_libraries(${example} PRIVATE itf_net itf_accel)
  endforeach()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(ITF_BUILD_TESTS)
  enable_testing()
  add_library(
    itf_test_support STATIC
    tests/support/test.cpp
    tests/support/test_support.cpp
    tests/support/cluster.cpp
  )
  itf_configure_target(itf_test_support)
  target_link_libraries(itf_test_support PUBLIC itf_net itf_accel itf_service)
  target_include_directories(itf_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/tests")

  function(itf_add_test name category)
    add_executable(${name} tests/${category}/${name}.cpp)
    itf_configure_target(${name})
    target_link_libraries(${name} PRIVATE itf_test_support)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS "${category}")
  endfunction()

  itf_add_test(test_foundation unit)
  itf_add_test(test_traffic_classification unit)
  itf_add_test(test_policy_engine unit)
  itf_add_test(test_ledger_lifecycle unit)
  itf_add_test(test_decision_explain unit)
  itf_add_test(test_accounting_closure unit)
  itf_add_test(test_integration_lifecycle integration)
  itf_add_test(test_scale_indexes integration)
  itf_add_test(test_property_state_machine property)
  itf_add_test(test_property_starvation property)
  itf_add_test(test_concurrency_authority concurrency)
  itf_add_test(test_concurrency_server concurrency)
  itf_add_test(test_adversarial_codec adversarial)
  itf_add_test(test_adversarial_state adversarial)
  itf_add_test(test_adversarial_sequences adversarial)
  itf_add_test(test_persistence_format persistence)
  itf_add_test(test_persistence_recovery persistence)
  itf_add_test(test_protocol_session protocol)
  itf_add_test(test_protocol_replay protocol)
  itf_add_test(test_multiprocess_serving multiprocess)
  itf_add_test(test_multiprocess_restart multiprocess)
endif()

# ---------------------------------------------------------------------------
# Install and export
# ---------------------------------------------------------------------------
install(
  TARGETS itf_core itf_net itf_accel
  EXPORT InferenceTrafficFabricTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
if(ITF_BUILD_TOOLS)
  install(TARGETS itf_coordinator itfctl itf_sim RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES LICENSE README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/InferenceTrafficFabric OPTIONAL)

install(
  EXPORT InferenceTrafficFabricTargets
  NAMESPACE itf::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/InferenceTrafficFabric
)

configure_package_config_file(
  cmake/InferenceTrafficFabricConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/InferenceTrafficFabricConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/InferenceTrafficFabric
)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/InferenceTrafficFabricConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)
install(
  FILES "${CMAKE_CURRENT_BINARY_DIR}/InferenceTrafficFabricConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/InferenceTrafficFabricConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/InferenceTrafficFabric
)
