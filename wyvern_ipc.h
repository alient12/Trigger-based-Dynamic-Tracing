// wyvern_ipc.h
#pragma once
#include <stdint.h>

#define WYVERN_SHM_MAGIC   0x575956524E495043ULL /* "WYVRNIPC" */
#define WYVERN_SHM_VERSION 1
#define WYVERN_SHM_NAME    "/wyvern_ipc"

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t size;
    uint64_t seq;

    uint64_t ts_ns;        // sender timestamp (CLOCK_MONOTONIC)
    int32_t  target_pid;   // TGID you want to control
    int32_t  target_tid;   // TID (optional)
    int32_t  action;       // your action code (generic)

    // Optional: sender can hint these, daemon can ignore.
    int32_t  tracer_type;  // e.g. WYVERN=1, LTTNG=2 ...
    int32_t  tracer_state; // e.g. RUNNING=1, STOPPED=2 ...
    int32_t  reserved;

    uint8_t  user_data[64]; // generic payload for future
} WyvernShm;