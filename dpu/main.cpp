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

#define LOCAL_BUF_SIZE 24000

# define IB_DEVNAME "mlx5_0"
# define LISTEN_PORT 1234
#define RQ_NUM_DESC 16

// const char *ib_devname = "mlx5_0";
// uint16_t listen_port = 1234;

static inline uint32_t u32log2(uint32_t x) {
    if (x == 0) return 0;
    return 31u - __builtin_clz(x);
}

static int modify_qp_to_init(struct ibv_qp *qp, struct ibv_qp_attr *qp_attr, int attr_mask) {
    uint8_t in[DEVX_ST_SZ_BYTES(rst2init_qp_in)] = {0};
    uint8_t out[DEVX_ST_SZ_BYTES(rst2init_qp_out)] = {0};
    void *qpc = DEVX_ADDR_OF(rst2init_qp_in, in, qpc);

    DEVX_SET(rst2init_qp_in, in, opcode, MLX5_CMD_OP_RST2INIT_QP);
    DEVX_SET(rst2init_qp_in, in, qpn, qp->qp_num);
    DEVX_SET(qpc, qpc, pm_state, MLX5_QPC_PM_STATE_MIGRATED);

    if (attr_mask & IBV_QP_PKEY_INDEX)
        DEVX_SET(qpc, qpc, primary_address_path.pkey_index, qp_attr->pkey_index);
    if (attr_mask & IBV_QP_PORT)
        DEVX_SET(qpc, qpc, primary_address_path.vhca_port_num, qp_attr->port_num);
    if (attr_mask & IBV_QP_ACCESS_FLAGS) {
        if (qp_attr->qp_access_flags & IBV_ACCESS_REMOTE_READ)
            DEVX_SET(qpc, qpc, rre, 1);
        if (qp_attr->qp_access_flags & IBV_ACCESS_REMOTE_WRITE)
            DEVX_SET(qpc, qpc, rwe, 1);
    }
    return mlx5dv_devx_qp_modify(qp, in, sizeof(in), out, sizeof(out));
}

static int modify_qp_to_rtr(struct ibv_qp *qp, struct ibv_qp_attr *qp_attr, struct mlx5dv_ah *dv_ah, int attr_mask) {
    uint8_t in[DEVX_ST_SZ_BYTES(init2rtr_qp_in)] = {0};
    uint8_t out[DEVX_ST_SZ_BYTES(init2rtr_qp_out)] = {0};
    void *qpc = DEVX_ADDR_OF(init2rtr_qp_in, in, qpc);

    DEVX_SET(init2rtr_qp_in, in, opcode, MLX5_CMD_OP_INIT2RTR_QP);
    DEVX_SET(init2rtr_qp_in, in, qpn, qp->qp_num);
    DEVX_SET(qpc, qpc, log_msg_max, 30);

    if (attr_mask & IBV_QP_PATH_MTU)
        DEVX_SET(qpc, qpc, mtu, qp_attr->path_mtu);
    if (attr_mask & IBV_QP_DEST_QPN)
        DEVX_SET(qpc, qpc, remote_qpn, qp_attr->dest_qp_num);
    if (attr_mask & IBV_QP_RQ_PSN)
        DEVX_SET(qpc, qpc, next_rcv_psn, qp_attr->rq_psn & 0xffffff);
    if (attr_mask & IBV_QP_TIMEOUT)
        DEVX_SET(qpc, qpc, primary_address_path.ack_timeout, qp_attr->timeout);
    if (attr_mask & IBV_QP_PKEY_INDEX)
        DEVX_SET(qpc, qpc, primary_address_path.pkey_index, qp_attr->pkey_index);
    if (attr_mask & IBV_QP_PORT)
        DEVX_SET(qpc, qpc, primary_address_path.vhca_port_num, qp_attr->port_num);
    if (attr_mask & IBV_QP_MAX_DEST_RD_ATOMIC)
        DEVX_SET(qpc, qpc, log_rra_max, u32log2(qp_attr->max_dest_rd_atomic));
    if (attr_mask & IBV_QP_MIN_RNR_TIMER)
        DEVX_SET(qpc, qpc, min_rnr_nak, qp_attr->min_rnr_timer);
    if (attr_mask & IBV_QP_AV) {
        DEVX_SET(qpc, qpc, primary_address_path.rlid, qp_attr->ah_attr.dlid);
        DEVX_SET(qpc, qpc, primary_address_path.grh, 0);
        if (qp_attr->ah_attr.sl & 0xf)
            DEVX_SET(qpc, qpc, primary_address_path.sl, qp_attr->ah_attr.sl & 0xf);
    }
    return mlx5dv_devx_qp_modify(qp, in, sizeof(in), out, sizeof(out));
}

static int modify_qp_to_rts(struct ibv_qp *qp, struct ibv_qp_attr *qp_attr, int attr_mask) {
    uint32_t in[DEVX_ST_SZ_DW(rtr2rts_qp_in)] = {0};
    uint32_t out[DEVX_ST_SZ_DW(rtr2rts_qp_out)] = {0};
    void *qpc = DEVX_ADDR_OF(rtr2rts_qp_in, in, qpc);

    DEVX_SET(rtr2rts_qp_in, in, opcode, MLX5_CMD_OP_RTR2RTS_QP);
    DEVX_SET(rtr2rts_qp_in, in, qpn, qp->qp_num);

    if (attr_mask & IBV_QP_TIMEOUT)
        DEVX_SET(qpc, qpc, primary_address_path.ack_timeout, qp_attr->timeout);
    if (attr_mask & IBV_QP_RETRY_CNT)
        DEVX_SET(qpc, qpc, retry_count, qp_attr->retry_cnt);
    if (attr_mask & IBV_QP_SQ_PSN)
        DEVX_SET(qpc, qpc, next_send_psn, qp_attr->sq_psn & 0xffffff);
    if (attr_mask & IBV_QP_RNR_RETRY)
        DEVX_SET(qpc, qpc, rnr_retry, qp_attr->rnr_retry);
    if (attr_mask & IBV_QP_MAX_QP_RD_ATOMIC)
        DEVX_SET(qpc, qpc, log_sra_max, u32log2(qp_attr->max_rd_atomic));

    return mlx5dv_devx_qp_modify(qp, in, sizeof(in), out, sizeof(out));
}

static int qp_enable_mmo(struct ibv_qp *qp) {
    uint32_t in[DEVX_ST_SZ_DW(init2init_qp_in)] = {};
    uint32_t out[DEVX_ST_SZ_DW(init2init_qp_out)] = {};
    void *qpce = DEVX_ADDR_OF(init2init_qp_in, in, qpc_data_extension);

    DEVX_SET(init2init_qp_in, in, opcode, MLX5_CMD_OP_INIT2INIT_QP);
    DEVX_SET(init2init_qp_in, in, qpc_ext, 1);
    DEVX_SET(init2init_qp_in, in, qpn, qp->qp_num);
    DEVX_SET64(init2init_qp_in, in, opt_param_mask_95_32, MLX5_QPC_OPT_MASK_32_INIT2INIT_MMO);
    DEVX_SET(qpc_ext, qpce, mmo, 1);

    return mlx5dv_devx_qp_modify(qp, in, sizeof(in), out, sizeof(out));
}

static int modify_qp(struct ibv_qp *qp, struct ibv_qp_attr *qp_attr, struct mlx5dv_ah *dv_ah, int attr_mask) {
    if (!(attr_mask & IBV_QP_STATE)) return -EINVAL;
    switch (qp_attr->qp_state) {
        case IBV_QPS_INIT: return modify_qp_to_init(qp, qp_attr, attr_mask);
        case IBV_QPS_RTR:  return modify_qp_to_rtr(qp, qp_attr, dv_ah, attr_mask);
        case IBV_QPS_RTS:  return modify_qp_to_rts(qp, qp_attr, attr_mask);
        default:           return -EINVAL;
    }
}


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
    size_t local_buf_size = LOCAL_BUF_SIZE;
    void *local_buf = calloc(1, local_buf_size);
    struct ibv_mr *local_mr = ibv_reg_mr(pd, local_buf, local_buf_size, 
                                         IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
    if (!local_mr) {
        SPDLOG_ERROR("Failed to register local MR.");
        exit(EXIT_FAILURE);
    }
    
    SPDLOG_DEBUG("PD allocated successfully.");


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

    // 4. Enable MMO (Memory Management Offload) - 这个是最核心的跨域依赖
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

    HostMemInfo primary_info, mirror_info;

    ssize_t ret1 = recv(client_socket, &primary_info, sizeof(HostMemInfo), MSG_WAITALL);
    ssize_t ret2 = recv(client_socket, &mirror_info, sizeof(HostMemInfo), MSG_WAITALL);

    if (ret1 != sizeof(HostMemInfo) || ret2 != sizeof(HostMemInfo)) {
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
    if (local_mr) ibv_dereg_mr(local_mr);
    if (local_buf) free(local_buf);

    ibv_dealloc_pd(pd);
    ibv_close_device(context);
    ibv_free_device_list(dev_list);

    return 0;
}
