# dpu/CMakeLists.txt

find_library(IBVERBS_LIB ibverbs REQUIRED)
find_library(MLX5_LIB mlx5 REQUIRED)

# Add both main.cpp and the newly created rdma_utils.cpp to the executable
add_executable(dpu_relay 
    main.cpp
    ../common/rdma_utils.cpp
)

# Include common headers
target_include_directories(dpu_relay PRIVATE ${CMAKE_SOURCE_DIR})

# Link library files
target_link_libraries(dpu_relay 
    ${IBVERBS_LIB} 
    ${MLX5_LIB}
    spdlog::spdlog
)