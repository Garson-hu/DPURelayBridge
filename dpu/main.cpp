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


// --------------------------------------------------------------------------------------------------------------------
// Convert the C++ communication structure back to the Legacy C expected descriptor string, and register the alias
// --------------------------------------------------------------------------------------------------------------------
struct cgmk_mr_crossing* create_alias_from_info(struct ibv_pd *pd, const HostMemInfo& info) {
    // 1. from ibv_pd extract underline PDn (Protection Domain Number)
    struct mlx5dv_pd my_pd_out;
    memset(&my_pd_out, 0, sizeof(my_pd_out));
    struct mlx5dv_obj my_pd_obj;
    memset(&my_pd_obj, 0, sizeof(my_pd_obj));
    my_pd_obj.pd.in = pd;
    my_pd_obj.pd.out = &my_pd_out;

    if (mlx5dv_init_obj(&my_pd_obj, MLX5DV_OBJ_PD)) {
        SPDLOG_ERROR("Not able to expose pdn from pd.");
        return nullptr;
    }

    // 2. Prepare DevX command structure (directly use hardware macro definitions)
    uint32_t in[DEVX_ST_SZ_DW(create_alias_obj_in)] = {0};
    uint32_t out[DEVX_ST_SZ_DW(create_alias_obj_out)] = {0};

    void *hdr = DEVX_ADDR_OF(create_alias_obj_in, in, hdr);
    void *alias_ctx = DEVX_ADDR_OF(create_alias_obj_in, in, alias_ctx);

    // 3. Fill in parameters: use safe, non-string-polluted C++ data
    DEVX_SET(general_obj_in_cmd_hdr, hdr, opcode, MLX5_CMD_OP_CREATE_GENERAL_OBJECT);
    DEVX_SET(general_obj_in_cmd_hdr, hdr, obj_type, MLX5_GENERAL_OBJ_TYPE_MKEY);
    DEVX_SET(general_obj_in_cmd_hdr, hdr, alias_object, 1);
    
    DEVX_SET(alias_context, alias_ctx, vhca_id_to_be_accessed, info.vhca_id);
    DEVX_SET(alias_context, alias_ctx, object_id_to_be_accessed, info.mkey >> 8); 

    // Fill in 32-byte cross-domain access password Token (hardware strictly requires 256 bits)
    void *access_key = DEVX_ADDR_OF(alias_context, alias_ctx, access_key);
    memcpy(access_key, info.token, 32); 

    DEVX_SET(alias_context, alias_ctx, metadata, my_pd_out.pdn);

    // 4. Send command to network card hardware!
    struct mlx5dv_devx_obj *alias = mlx5dv_devx_obj_create(pd->context, in, sizeof(in), out, sizeof(out));
    if (!alias) {
        SPDLOG_ERROR("DevX Hardware Rejected Alias! status 0x{:X}, syndrome 0x{:X}", 
                     DEVX_GET(create_alias_obj_out, out, hdr.status), 
                     DEVX_GET(create_alias_obj_out, out, hdr.syndrome));
        return nullptr;
    }

    // 5. Pack the result returned by the hardware into a structure for subsequent use
    struct cgmk_mr_crossing *mr_crossing = (struct cgmk_mr_crossing*)calloc(1, sizeof(*mr_crossing));
    mr_crossing->pd = pd;
    mr_crossing->alias_obj = alias;
    // LKey size is 32 bits (alias id == mkey idx == 24 bits)
    mr_crossing->lkey = DEVX_GET(create_alias_obj_out, out, hdr.obj_id) << 8; 
    mr_crossing->addr = (void*)info.addr;
    mr_crossing->length = info.length;
    
    return mr_crossing;
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

    HostMemInfo primary_info, mirror_info;

    ssize_t ret1 = recv(client_socket, &primary_info, sizeof(HostMemInfo), MSG_WAITALL);
    ssize_t ret2 = recv(client_socket, &mirror_info, sizeof(HostMemInfo), MSG_WAITALL);

    if (ret1 != sizeof(HostMemInfo) || ret2 != sizeof(HostMemInfo)) {
        SPDLOG_ERROR("Failed to receive full HostMemInfo structures.");
        exit(EXIT_FAILURE);
    }

    SPDLOG_INFO("Received credentials! Primary [addr: 0x{:X}, size: {}], Mirror [addr: 0x{:X}, size: {}]",
        primary_info.addr, primary_info.length, mirror_info.addr, mirror_info.length);
    
    
    // -------------------------------------------------------------------
    // Step D: use DevX to create cross-domain mkeys for the primary and mirror buffers
    // -------------------------------------------------------------------
    
    SPDLOG_DEBUG("Creating Alias MKeys mapping to Host memory...");

    struct cgmk_mr_crossing* primary_alias = create_alias_from_info(pd, primary_info);
    if (!primary_alias) {
        SPDLOG_ERROR("Failed to create Primary Alias MKey.");
        exit(EXIT_FAILURE);
    }

    struct cgmk_mr_crossing* mirror_alias = create_alias_from_info(pd, mirror_info);
    if (!mirror_alias) {
        SPDLOG_ERROR("Failed to create Mirror Alias MKey.");
        exit(EXIT_FAILURE);
    }
    
    SPDLOG_INFO("Alias MKeys for primary and mirror buffers created successfully!");
    
    // -------------------------------------------------------------------
    // Step E: Release Host (send ACK to let Host go to sleep)
    // -------------------------------------------------------------------
    SPDLOG_INFO("Alias setup complete. Releasing Host Agent...");
    const char* ack = "OK";
    send(client_socket, ack, strlen(ack), 0);
    close(client_socket);
    close(server_fd);

    SPDLOG_INFO("Host CPU is now completely offloaded.");
    SPDLOG_INFO("Phase 2 Complete! DPU is holding the keys.");

    // Clean up resources (will keep until the program ends in actual application)
    dereg_cgmk_mr_crossing(primary_alias);
    dereg_cgmk_mr_crossing(mirror_alias);
    ibv_dealloc_pd(pd);
    ibv_close_device(context);
    ibv_free_device_list(dev_list);

    return 0;
}
