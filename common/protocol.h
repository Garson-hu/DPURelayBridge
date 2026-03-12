#pragma once
#include <stdint.h>
#include <stddef.h>

/* 
 * Primary Buffer: the buffer that is used by the Local Host
 * Mirror Buffer: the buffer that is used by the Remote Host
 */
enum BufferType {
    BUFFER_PRIMARY = 0,
    BUFFER_MIRROR  = 1
};

// Host send to local DPU's memory info
struct HostMemInfo {
    char desc_str[256];
    BufferType type;         // Mark this is primary or mirror buffer
} __attribute__((packed));


struct DpuRdmaInfo {
    uint32_t qp_num;     // Network RC QP number, unique for each DPU
    uint16_t lid;        // Local Identifier
    uint8_t  gid[16];    // Global Identifier (RoCEv2)
    uint32_t rkey;       // RKey of the remote DPU's ring_buf
    uint64_t vaddr;      // Start address of the remote DPU's ring_buf
} __attribute__((packed));