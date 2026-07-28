/* shm_protocol.h
**
** Shared Memory Protocol for TinyPlay.
** Zero False-Sharing, strict cache-line isolation, and lock-free guarantees.
*/

#pragma once

/* =========================================================================
   COMPILER-SPECIFIC HEADERS & MACROS
   Resolves C vs C++ header differences and silences strict linters.
   ========================================================================= */
#ifdef __cplusplus
#include <cstdint>
#include <atomic>
#define ATOMIC(type) std::atomic<type>
#define MAYBE_UNUSED [[maybe_unused]]
#else
#include <stdint.h>
#include <stdalign.h>
#include <stdatomic.h>
#define ATOMIC(type) _Atomic(type)
#define MAYBE_UNUSED __attribute__((unused))
#endif

#define CACHE_LINE_SIZE 128

/*
 * alignas(CACHE_LINE_SIZE) ensures the base pointer is strictly aligned to 128 bytes,
 * preventing any possible physical cache-line overlap.
 */
struct player_ctrl {
    /* =======================================================
       ZONE 1: HOST CONTROL (Kotlin -> C Core)
       Strictly 128 bytes. Written by JNI, read by Audio Engine.
       ======================================================= */
    alignas(CACHE_LINE_SIZE) union {
        struct {
            ATOMIC(uint32_t) magic;                         /* 0 */
            ATOMIC(int32_t)  command;                       /* 4 */
            ATOMIC(int32_t)  futex_waiters;                 /* 8 */
            MAYBE_UNUSED uint32_t _pad;                     /* 12 */
            alignas(8) ATOMIC(uint64_t) seek_target;        /* 16 */
            alignas(8) ATOMIC(uint64_t) exact_total_frames; /* 24 */
        };
        /* Memory padding to force the union size to exactly 128 bytes */
        MAYBE_UNUSED uint8_t _zone1[CACHE_LINE_SIZE];
    };

    /* =======================================================
       ZONE 2: ENGINE STATUS (C Core -> Kotlin)
       Strictly 128 bytes. Written by Audio Engine, read by JNI.
       ======================================================= */
    alignas(CACHE_LINE_SIZE) union {
        struct {
            ATOMIC(int32_t)  state;                    /* 128 */
            alignas(8) ATOMIC(uint64_t) current_frame; /* 136 */
            alignas(8) ATOMIC(uint64_t) total_frames;  /* 144 */
            ATOMIC(uint32_t) sample_rate;              /* 152 */
            ATOMIC(uint32_t) channels;                 /* 156 */
            ATOMIC(uint32_t) bits;                     /* 160 */
            ATOMIC(uint32_t) hw_period_size;           /* 164 */
            ATOMIC(uint32_t) hw_buffer_size;           /* 168 */
            ATOMIC(int32_t)  hw_format;                /* 172 */
            ATOMIC(int32_t)  hw_access;                /* 176 */
        };
        /* Memory padding to force the union size to exactly 128 bytes */
        MAYBE_UNUSED uint8_t _zone2[CACHE_LINE_SIZE];
    };
};

/* --- Compile-Time ABI and Safety Checks --- */
#ifdef __cplusplus
static_assert(sizeof(struct player_ctrl) == 256, "SHM Size must be exactly 256 bytes");
static_assert(offsetof(struct player_ctrl, state) == 128, "Zone 2 alignment failed");
static_assert(std::atomic<uint64_t>::is_always_lock_free, "FATAL: 64-bit atomics require locks on this target!");
static_assert(sizeof(std::atomic<int32_t>) == sizeof(int32_t), "Atomic int32_t layout mismatch, futex will crash!");
#else
_Static_assert(sizeof(struct player_ctrl) == 256, "SHM Size must be exactly 256 bytes");
_Static_assert(offsetof(struct player_ctrl, state) == 128, "Zone 2 alignment failed");
#endif