// common/rdma_utils.h
#pragma once

#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Helper to calculate log base 2 (used for atomic configurations)
uint32_t u32log2(uint32_t x);

// QP state machine transition functions
int modify_qp_to_init(struct ibv_qp *qp, struct ibv_qp_attr *qp_attr, int attr_mask);
int modify_qp_to_rtr(struct ibv_qp *qp, struct ibv_qp_attr *qp_attr, struct mlx5dv_ah *dv_ah, int attr_mask);
int modify_qp_to_rts(struct ibv_qp *qp, struct ibv_qp_attr *qp_attr, int attr_mask);

// Enable Memory Management Offload (MMO) for DCI QP
int qp_enable_mmo(struct ibv_qp *qp);

// Unified wrapper for QP state transitions
int modify_qp(struct ibv_qp *qp, struct ibv_qp_attr *qp_attr, struct mlx5dv_ah *dv_ah, int attr_mask);

// Standard Verbs QP State Machine for Network RC QP (QP2)
int modify_rc_qp_to_init(struct ibv_qp *qp, int ib_port);
int modify_rc_qp_to_rtr(struct ibv_qp *qp, uint32_t remote_qpn, uint16_t remote_lid, uint8_t *remote_gid, uint32_t remote_psn, int ib_port);
int modify_rc_qp_to_rts(struct ibv_qp *qp, uint32_t local_psn);

#ifdef __cplusplus
}
#endif