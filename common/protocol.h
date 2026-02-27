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
    char desc_str[sizeof("0102:01020304:0102030405060708:01020304:0102030405060708090a0b0c0d0e0f10")];
    BufferType type;         // Mark this is primary or mirror buffer
};