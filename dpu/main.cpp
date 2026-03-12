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
    #include "../common/rdma_utils.h"
}

#define RING_BUF_SIZE 16 * 1024 * 1024 // 16MB

#define IB_DEVNAME "mlx5_0"
#define LISTEN_PORT 1234
#define RQ_NUM_DESC 16

// ---------------------------------
// DPU receiver main function
// ---------------------------------
int main(int argc, char *argv[]) {

    init_logger();
    // The device we use is mlx5_0 (e.g. represent the external InfiniBand/Ethernet port)
    
    SPDLOG_INFO("============================================");
    SPDLOG_INFO("  DPU Relay Engine (Phase 2) Started        ");
    SPDLOG_INFO("  Local IB Device : {}", IB_DEVNAME);
    SPDLOG_INFO("  Listening Port  : {}", LISTEN_PORT);
    SPDLOG_INFO("============================================");

    // -----------------------------------------------------
    // Step A: Find and open IB device
    // -----------------------------------------------------
    SPDLOG_DEBUG("Initializing DPU InfiniBand device...");
    struct ibv_device **dev_list = ibv_get_device_list(NULL);
    struct ibv_device *ib_dev = NULL;

    for (int i = 0; dev_list[i]; ++i) {
        if (!strcmp(ibv_get_device_name(dev_list[i]), IB_DEVNAME)) {
            ib_dev = dev_list[i];
            break;
        }
    }

    if (!ib_dev) {
        SPDLOG_ERROR("Failed to find IB device: {}", IB_DEVNAME);
        exit(EXIT_FAILURE);
    }
    struct ibv_context *context = ibv_open_device(ib_dev);
    struct ibv_pd *pd = ibv_alloc_pd(context);

    if (!pd) {
        SPDLOG_ERROR("Failed to allocate PD.");
        exit(EXIT_FAILURE);
    }

    // -----------------------------------------------------
    // Step A1: Register local memory
    // -----------------------------------------------------
    SPDLOG_DEBUG("Allocating and registering DPU local ring buffer...");
    
    // Set buffer size to 16MB for payload relay
    size_t ring_buf_size = RING_BUF_SIZE;

    void *outbound_buf = nullptr; // Data from Host. Buffer for sending data to remote DPU
    void *inbound_buf = nullptr; // Data from remote DPU. Buffer for receiving data from remote DPU

    // Allocate page-aligned memory for both directions
    if (posix_memalign(&outbound_buf, sysconf(_SC_PAGESIZE), ring_buf_size) != 0 ||
        posix_memalign(&inbound_buf, sysconf(_SC_PAGESIZE), ring_buf_size) != 0) {
        SPDLOG_ERROR("Failed to allocate outbound/inbound buffers.");
        exit(EXIT_FAILURE);
    }

    memset(outbound_buf, 0, ring_buf_size);
    memset(inbound_buf, 0, ring_buf_size);

    // Register Memory Regions (MRs) with full access permissions
    int access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;


    struct ibv_mr *outbound_mr = ibv_reg_mr(pd, outbound_buf, ring_buf_size, access_flags);
    struct ibv_mr *inbound_mr = ibv_reg_mr(pd, inbound_buf, ring_buf_size, access_flags);

    if (!outbound_mr || !inbound_mr) {
        SPDLOG_ERROR("Failed to register MRs for ring buffers.");
        exit(EXIT_FAILURE);
    }
    
    SPDLOG_INFO("Outbound and Inbound ring buffers registered successfully. Size: {} bytes", RING_BUF_SIZE);

    // -----------------------------------------------------
    // Step A2: Init DCI QP and Enable MMO 
    // -----------------------------------------------------
    SPDLOG_DEBUG("Init DCI QP and Enable MMO...");

    // 1. Create CQ
    struct ibv_cq_init_attr_ex cq_init_attr_ex = {};
    cq_init_attr_ex.cqe = RQ_NUM_DESC;
    cq_init_attr_ex.cq_context = NULL;
    cq_init_attr_ex.channel = NULL;
    cq_init_attr_ex.comp_vector = 0;

    struct ibv_cq_ex *cq_ex = ibv_create_cq_ex(context, &cq_init_attr_ex);
    if (!cq_ex) {
        SPDLOG_ERROR("Couldn't create CQ (errno={})", errno);
        exit(EXIT_FAILURE);
    }

    // 2. Create QP (DCI with MEMCPY flag)
    struct ibv_qp_init_attr_ex qp_init_attr_ex = {};
    qp_init_attr_ex.sq_sig_all = 1;
    qp_init_attr_ex.send_cq = ibv_cq_ex_to_cq(cq_ex);
    qp_init_attr_ex.recv_cq = ibv_cq_ex_to_cq(cq_ex);
    qp_init_attr_ex.cap.max_send_wr = 1;
    qp_init_attr_ex.cap.max_recv_wr = 0;
    qp_init_attr_ex.cap.max_send_sge = 1;
    qp_init_attr_ex.cap.max_recv_sge = 0;
    qp_init_attr_ex.qp_type = IBV_QPT_RC;
    qp_init_attr_ex.send_ops_flags = IBV_QP_EX_WITH_RDMA_WRITE |
                                     IBV_QP_EX_WITH_SEND |
                                     IBV_QP_EX_WITH_RDMA_READ;
    qp_init_attr_ex.pd = pd;
    qp_init_attr_ex.comp_mask = IBV_QP_INIT_ATTR_PD | IBV_QP_INIT_ATTR_SEND_OPS_FLAGS;

    struct mlx5dv_qp_init_attr qp_init_attr_dv = {};
    qp_init_attr_dv.comp_mask = MLX5DV_QP_INIT_ATTR_MASK_SEND_OPS_FLAGS;
    qp_init_attr_dv.send_ops_flags = MLX5DV_QP_EX_WITH_MEMCPY;

    struct ibv_qp *qp = mlx5dv_create_qp(context, &qp_init_attr_ex, &qp_init_attr_dv);
    if (!qp) {
        SPDLOG_ERROR("Couldn't create QP (errno={})", errno);
        exit(EXIT_FAILURE);
    }

    struct ibv_qp_ex *qp_ex = ibv_qp_to_qp_ex(qp);
    if (!qp_ex) {
        SPDLOG_ERROR("Couldn't create QP_EX (errno={})", errno);
        exit(EXIT_FAILURE);
    }

    struct mlx5dv_qp_ex *mqp_ex = mlx5dv_qp_ex_from_ibv_qp_ex(qp_ex);
    if (!mqp_ex) {
        SPDLOG_ERROR("Couldn't create DV QP (errno={})", errno);
        exit(EXIT_FAILURE);
    }

    // 3. Modify QP to INIT state
    struct ibv_qp_attr qp_attr = {};
    qp_attr.qp_state = IBV_QPS_INIT;
    qp_attr.pkey_index = 0;
    qp_attr.port_num = 1;
    qp_attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE |
                              IBV_ACCESS_REMOTE_WRITE |
                              IBV_ACCESS_REMOTE_READ;
    
    int attr_mask = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
    int ret_qp = modify_qp(qp, &qp_attr, NULL, attr_mask);
    if (ret_qp) {
        SPDLOG_ERROR("Couldn't modify QP to INIT (errno={})", errno);
        exit(EXIT_FAILURE);
    }

    // 4. Enable MMO (Memory Management Offload) - 核心跨域依赖
    ret_qp = qp_enable_mmo(qp);
    if (ret_qp) {
        SPDLOG_ERROR("Can't enable MMO. err={}", ret_qp);
        exit(EXIT_FAILURE);
    }

    // 5. Modify QP to RTR
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_state = IBV_QPS_RTR;
    qp_attr.path_mtu = IBV_MTU_1024;
    qp_attr.ah_attr.is_global = 0;
    qp_attr.ah_attr.src_path_bits = 0;
    qp_attr.ah_attr.port_num = 1;
    attr_mask = IBV_QP_STATE | IBV_QP_PATH_MTU | IBV_QP_AV;
    
    ret_qp = modify_qp(qp, &qp_attr, NULL, attr_mask);
    if (ret_qp) {
        SPDLOG_ERROR("Couldn't modify QP to RTR (errno={})", errno);
        exit(EXIT_FAILURE);
    }

    // 6. Modify QP to RTS
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_state = IBV_QPS_RTS;
    qp_attr.timeout = 12;
    qp_attr.retry_cnt = 3;
    qp_attr.rnr_retry = 3;
    qp_attr.sq_psn = 0;
    qp_attr.max_rd_atomic = 1;
    attr_mask = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
    
    ret_qp = modify_qp(qp, &qp_attr, NULL, attr_mask);
    if (ret_qp) {
        SPDLOG_ERROR("Couldn't modify QP to RTS (errno={})", errno);
        exit(EXIT_FAILURE);
    }

    SPDLOG_INFO("DCI QP Initialized and MMO Enabled successfully!");

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
    address.sin_port = htons(LISTEN_PORT);
    
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        SPDLOG_ERROR("Bind failed on port {}", LISTEN_PORT);
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

    HostMemInfo primary_info;
    HostMemInfo mirror_info;

    ssize_t ret1 = recv(client_socket, &primary_info, sizeof(HostMemInfo), MSG_WAITALL);
    ssize_t ret2 = recv(client_socket, &mirror_info, sizeof(HostMemInfo), MSG_WAITALL);

    if (ret1 != sizeof(HostMemInfo) || ret2 != sizeof(HostMemInfo)) {
        SPDLOG_ERROR("Failed to receive full HostMemInfo structures.");
        exit(EXIT_FAILURE);
    }

    primary_info.desc_str[sizeof(primary_info.desc_str) - 1] = '\0';
    mirror_info.desc_str[sizeof(mirror_info.desc_str) - 1] = '\0';

    SPDLOG_INFO("sizeof(desc_str) = {}", strlen(primary_info.desc_str));
    SPDLOG_INFO("desc_str = {}", primary_info.desc_str);
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

    struct cgmk_mr_crossing* mirror_alias = cgmk_mr_crossing_reg(pd, mirror_info.desc_str, strlen(mirror_info.desc_str) + 1);
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

    SPDLOG_INFO("Host CPU is now completely offloaded. DPU is holding the keys.");

    // Clean up resources (will keep until the program ends in actual application)
    dereg_cgmk_mr_crossing(primary_alias);
    dereg_cgmk_mr_crossing(mirror_alias);

    if (qp) ibv_destroy_qp(qp);
    if (cq_ex) ibv_destroy_cq(ibv_cq_ex_to_cq(cq_ex));
    
    if (outbound_mr) ibv_dereg_mr(outbound_mr);
    if (inbound_mr) ibv_dereg_mr(inbound_mr);

    if (outbound_buf) free(outbound_buf);
    if (inbound_buf) free(inbound_buf);
    
    ibv_dealloc_pd(pd);
    ibv_close_device(context);
    ibv_free_device_list(dev_list);

    return 0;
}
