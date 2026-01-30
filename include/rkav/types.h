#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 连续 NV12：Y(WH) + UV(WH/2)
typedef struct {
    uint8_t  *data;
    size_t    size;
    int       w;
    int       h;
    int       stride;     // bytes per line (Y)
    uint64_t  pts_us;     // CLOCK_MONOTONIC timestamp (microseconds)
    uint64_t  frame_id;
} VideoFrame;

// 交错 PCM (LRLR...)，frames 表示“每声道采样帧数”
typedef struct {
    uint8_t  *data;
    size_t    bytes;
    int       sample_rate;
    int       channels;
    int       bytes_per_sample; // e.g. 2 for S16LE
    uint32_t  frames;           // per-channel frames
    uint64_t  pts_us;           // base + accumulated by sample count
} AudioChunk;

// 编码后的 H264（AnnexB）包
typedef struct {
    uint8_t  *data;
    size_t    size;
    uint64_t  pts_us;
    bool      is_keyframe;
} EncodedPacket;

#ifdef __cplusplus
}
#endif
