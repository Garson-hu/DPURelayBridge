// common/rdma_utils.cpp
#include "rdma_utils.h"
#include <errno.h>
#include <string.h>

extern "C" {
    #include "cgmk_legacy/devx_prm.h"
    #include "cgmk_legacy/mlx5_ifc.h"
}

// Calculate log base 2
uint32_t u32log2(uint32_t x) {
    if (x == 0) return 0;
    return 31u - __builtin_clz(x);
}

// Transition QP to INIT state
int modify_qp_to_init(struct ibv_qp *qp, struct ibv_qp_attr *qp_attr, int attr_mask) {
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

// Transition QP to RTR (Ready To Receive) state
int modify_qp_to_rtr(struct ibv_qp *qp, struct ibv_qp_attr *qp_attr, struct mlx5dv_ah *dv_ah, int attr_mask) {
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

// Transition QP to RTS (Ready To Send) state
int modify_qp_to_rts(struct ibv_qp *qp, struct ibv_qp_attr *qp_attr, int attr_mask) {
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

// Enable MMO (Memory Management Offload) on DCI QP
int qp_enable_mmo(struct ibv_qp *qp) {
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

// Wrapper function to transition QP state based on requested state
int modify_qp(struct ibv_qp *qp, struct ibv_qp_attr *qp_attr, struct mlx5dv_ah *dv_ah, int attr_mask) {
    if (!(attr_mask & IBV_QP_STATE)) return -EINVAL;
    switch (qp_attr->qp_state) {
        case IBV_QPS_INIT: return modify_qp_to_init(qp, qp_attr, attr_mask);
        case IBV_QPS_RTR:  return modify_qp_to_rtr(qp, qp_attr, dv_ah, attr_mask);
        case IBV_QPS_RTS:  return modify_qp_to_rts(qp, qp_attr, attr_mask);
        default:           return -EINVAL;
    }
}