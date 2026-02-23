#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *data;
    size_t   size;
    int w;
    int h;
    int stride;
    uint64_t pts_us;
    uint64_t frame_id;
} VideoFrame;

typedef struct{
    uint8_t * data;
    size_t bytes;
    int       sample_rate;
    int       channels;
    int       bytes_per_sample; // e.g. 2 for S16LE
    uint32_t  frames;           // per-channel frames
    uint64_t  pts_us;           // base + accumulated by sample count
} AudioChunk;

typedef struct {
    uint8_t *data;
    size_t   size;
    uint64_t pts_us;
    bool is_keyframe;
} EncodedPacket;


#ifdef __cplusplus
}
#endif