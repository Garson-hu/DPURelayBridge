// dpu/main.cpp
#include <iostream>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <getopt.h>

#include "../common/protocol.h"
#include "../common/logger.h"

#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>

extern "C" {
    #include "../common/cgmk_legacy/cgmk_utils.h"
    #include "../common/cgmk_legacy/cross_gvmi_mkey.h"
}

// ---------------------------------
// DPU receiver main function
// ---------------------------------
int main(int argc, char *argv[]) {

    init_logger();
    // The device we use is mlx5_0 (e.g. represent the external InfiniBand/Ethernet port)
    const char *ib_devname = "mlx5_0";
    uint16_t listen_port = 1234;
    
    SPDLOG_INFO("============================================");
    SPDLOG_INFO("  DPU Relay Engine (Phase 2) Started        ");
    SPDLOG_INFO("  Local IB Device : {}", ib_devname);
    SPDLOG_INFO("  Listening Port  : {}", listen_port);
    SPDLOG_INFO("============================================");

    // -----------------------------------------------------
    // Step A: Find and open IB device
    // -----------------------------------------------------
    SPDLOG_DEBUG("Initializing DPU InfiniBand device...");
    struct ibv_device **dev_list = ibv_get_device_list(NULL);
    struct ibv_device *ib_dev = NULL;

    for (int i = 0; dev_list[i]; ++i) {
        if (!strcmp(ibv_get_device_name(dev_list[i]), ib_devname)) {
            ib_dev = dev_list[i];
            break;
        }
    }

    if (!ib_dev) {
        SPDLOG_ERROR("Failed to find IB device: {}", ib_devname);
        exit(EXIT_FAILURE);
    }
    struct ibv_context *context = ibv_open_device(ib_dev);
    struct ibv_pd *pd = ibv_alloc_pd(context);

    if (!pd) {
        SPDLOG_ERROR("Failed to allocate PD.");
        exit(EXIT_FAILURE);
    }

    SPDLOG_DEBUG("PD allocated successfully.");

    // -------------------------------------------------------------------
    // Step B: Start listening for Host connections
    // -------------------------------------------------------------------
    SPDLOG_DEBUG("Starting to listen for Host connections...");

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(listen_port);
    
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        SPDLOG_ERROR("Bind failed on port {}", listen_port);
        exit(EXIT_FAILURE);
    }

    listen(server_fd, 3);
    SPDLOG_INFO("Waiting for Host Agent to connect and share keys...");

    int client_socket = accept(server_fd, NULL, NULL);
    if (client_socket < 0) {
        SPDLOG_ERROR("Accept failed.");
        exit(EXIT_FAILURE);
    }
    SPDLOG_INFO("Host Agent connected successfully!");

    // -------------------------------------------------------------------
    // Step C: Receive Host memory metadata
    // -------------------------------------------------------------------

    // HostMemInfo primary_info, mirror_info;
    HostMemInfo primary_info;

    ssize_t ret1 = recv(client_socket, &primary_info, sizeof(HostMemInfo), MSG_WAITALL);
    // ssize_t ret2 = recv(client_socket, &mirror_info, sizeof(HostMemInfo), MSG_WAITALL);

    // if (ret1 != sizeof(HostMemInfo) || ret2 != sizeof(HostMemInfo)) {
    //     SPDLOG_ERROR("Failed to receive full HostMemInfo structures.");
    //     exit(EXIT_FAILURE);
    // }
    
    if (ret1 != sizeof(HostMemInfo)) {
        SPDLOG_ERROR("Failed to receive full HostMemInfo structures.");
        exit(EXIT_FAILURE);
    }

    SPDLOG_INFO("Received credentials!");
    
    // -------------------------------------------------------------------
    // Step D: use DevX to create cross-domain mkeys for the primary and mirror buffers
    // -------------------------------------------------------------------
    
    SPDLOG_DEBUG("Creating Alias MKeys mapping to Host memory...");

    struct cgmk_mr_crossing* primary_alias = cgmk_mr_crossing_reg(pd, primary_info.desc_str, strlen(primary_info.desc_str) + 1);
    if (!primary_alias) {
        SPDLOG_ERROR("Failed to create Primary Alias MKey.");
        exit(EXIT_FAILURE);
    }

    // struct cgmk_mr_crossing* mirror_alias = cgmk_mr_crossing_reg(pd, mirror_info.desc_str, strlen(mirror_info.desc_str) + 1);
    // if (!mirror_alias) {
    //     SPDLOG_ERROR("Failed to create Mirror Alias MKey.");
    //     exit(EXIT_FAILURE);
    // }
        
    SPDLOG_INFO("Alias MKeys for primary and mirror buffers created successfully!");
    
    // -------------------------------------------------------------------
    // Step E: Release Host (send ACK to let Host go to sleep)
    // -------------------------------------------------------------------
    SPDLOG_INFO("Alias setup complete. Releasing Host Agent...");
    const char* ack = "OK";
    send(client_socket, ack, strlen(ack), 0);
    close(client_socket);
    close(server_fd);

    SPDLOG_INFO("Host CPU is now completely offloaded. DPU is holding the keys.");

    // Clean up resources (will keep until the program ends in actual application)
    dereg_cgmk_mr_crossing(primary_alias);
    // dereg_cgmk_mr_crossing(mirror_alias);
    ibv_dealloc_pd(pd);
    ibv_close_device(context);
    ibv_free_device_list(dev_list);

    return 0;
}
