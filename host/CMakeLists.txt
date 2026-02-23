# Find RDMA related system libraries
find_library(IBVERBS_LIB ibverbs REQUIRED)
find_library(MLX5_LIB mlx5 REQUIRED)

# Define the executable name to be host_agent, and the source code is main.cpp
add_executable(host_agent main.cpp)

# Allow main.cpp to reference the header files in the common directory
target_include_directories(host_agent PRIVATE ${CMAKE_SOURCE_DIR})

# Link the underlying RDMA library and the automatically downloaded spdlog
target_link_libraries(host_agent 
    ${IBVERBS_LIB} 
    ${MLX5_LIB}
    spdlog::spdlog  # FetchContent will automatically provide this target
)