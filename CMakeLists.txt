cmake_minimum_required(VERSION 3.14) # Support FetchContent
project(DpuRelayBridge CXX C)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Introduce the FetchContent module to automatically download third-party libraries
include(FetchContent)
message(STATUS "Downloading spdlog...")
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.12.0 # Use a stable version
)
FetchContent_MakeAvailable(spdlog)

# Tell CMake to process the host directory
add_subdirectory(host)