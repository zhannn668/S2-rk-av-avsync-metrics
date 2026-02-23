#pragma once

#include <stdint.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AVSYNC_MAX_VJ 128
#define AVSYNC_MAX_AJ 256

typedef struct AvSync{
    pthread_mutex_t mu;

    uint64_t expected_video_delta_us;

    int has_video0;
    int has_audio0;
    uint64_t video0_us;
    uint64_t audio0_us;

    int offset_locked;
    int64_t offset_us; // audio0 - video0

    int has_last_video;
    int has_last_audio;
    uint64_t last_video_us;
    uint64_t last_audio_us;

    double vj_ms[AVSYNC_MAX_VJ];
    int vj_n;
    double aj_ms[AVSYNC_MAX_AJ];
    int aj_n;

    double res_ms[128];
    int res_n;

    double off_ms[128];
    int off_n;

    uint32_t last_audio_frames;
    uint32_t last_audio_sr;
    int has_last_audio_meta;
    int has_last_audio_arrival;
    uint64_t last_audio_arrival_us;

    int drift_base_set;
    uint64_t drift_t0_us;
    double residual0_ms;
} AvSync;


/*
 * 初始化 AvSync 模块。
 *
 * @param s        AvSync 实例
 * @param video_fps 视频帧率（用于计算 video expected delta）
 */
int avsync_init(AvSync *s, int video_fps);

/* 释放资源（主要是 mutex） */
void avsync_deinit(AvSync *s);

/*
 * 输入：视频 PTS（微秒，基于 CLOCK_MONOTONIC）。
 * 在 h264 sink 消费端调用最合适（代表下游真实看到的节奏）。
 */
void avsync_on_video(AvSync *s, uint64_t video_pts_us);

/*
 * 输入：音频 PTS（微秒，基于 CLOCK_MONOTONIC），以及该 chunk 的 frames/sample_rate。
 * 其中 frames 为“每声道帧数”，sample_rate 为 Hz。
 */
void avsync_on_audio(AvSync *s, uint64_t audio_pts_us, uint32_t frames, uint32_t sample_rate);

/*
 * 每秒报告一次（打印到日志）。
 *
 * @param now_us 当前 monotonic 时间（微秒）
 */
void avsync_report_1s(AvSync *s, uint64_t now_us);

#ifdef __cplusplus
}
#endif