# Host tests for the XR pointer: picking, dragging, hover, the beam and the hand checks, plus
# PNG snapshots of what the eye would see. Plain C++ - no Android toolchain, no OpenXR, no
# headset - so the interaction can be exercised on a desktop before anyone puts the goggles on.
#
#   cmake -S app/xr/src/main/cpp/tests -B build/xr-tests && cmake --build build/xr-tests -j
#   XRI_SNAPSHOT_DIR=build/xr-tests/snapshots ctest --test-dir build/xr-tests --output-on-failure

cmake_minimum_required(VERSION 3.14)
project(XrInteractionTests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
set(gtest_force_shared_crt OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(interaction_test
    XrInteraction_test.cpp
    Snapshot.cpp
    ../XrInteraction.cpp
)
target_include_directories(interaction_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_options(interaction_test PRIVATE -Wall -Wextra)
target_link_libraries(interaction_test GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(interaction_test)
