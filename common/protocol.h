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
    uint32_t vhca_id;        // Host's hardware identity ID
    uint32_t mkey;           // Memory's MKey (lkey)
    uint64_t addr;           // Virtual address start address
    size_t   length;         // Memory block size
    char     token[32];      // Cross-GVMI access password
    BufferType type;         // Mark this is primary or mirror buffer
};