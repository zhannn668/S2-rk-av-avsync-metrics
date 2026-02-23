#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>

#include "lib/utils/log.h"
#include "app_config.h"
#include "av_stats.h"
#include "lib/media/video/v4l2_capture.h"
#include "encoder_mpp.h"
#include "sink.h"
#include "audio_capture.h"

#include "rkav/bqueue.h"
#include "rkav/types.h"
#include "rkav/time.h"
#include <lib/media/sync/avsync.h>


// ============ Global ============
static atomic_int g_stop = 0;
static AvStats g_stats;
static AvSync g_avsync;

static BQueue g_raw_vq;  // VideoFrame*
static BQueue g_h264_q;  // EncodedPacket*
static BQueue g_aud_q;   // AudioChunk*

static atomic_uint_fast64_t g_video_pts_delta_us;
static atomic_uint_fast64_t g_audio_pts_delta_us;

static void request_stop(void)
{
    int prev = atomic_exchange(&g_stop, 1);
    if (prev == 0) {
        bq_close(&g_raw_vq);
        bq_close(&g_h264_q);
        bq_close(&g_aud_q);
    }
}

static int should_stop(void)
{
    return atomic_load(&g_stop) != 0;
}

// ============ Free helpers ============
static void free_video_frame(VideoFrame *vf)
{
    if (!vf) return;
    if (vf->data) free(vf->data);
    free(vf);
}

static void free_audio_chunk(AudioChunk *ac)
{
    if (!ac) return;
    if (ac->data) free(ac->data);
    free(ac);
}

static void free_encoded_packet(EncodedPacket *p)
{
    if (!p) return;
    if (p->data) free(p->data);
    free(p);
}

typedef struct {
    const AppConfig *cfg;
} ThreadArgs;

static void *signal_thread(void *arg)
{
    (void)arg;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);

    int sig = 0;
    // 等待 Ctrl+C / kill
    if (sigwait(&set, &sig) == 0) {
        LOGW("[signal] caught signal=%d, stopping...", sig);
        request_stop();
    }
    return NULL;
}

typedef struct {
    unsigned int sec;
} TimerArgs;

static void *timer_thread(void *arg)
{
    TimerArgs *t = (TimerArgs *)arg;
    if (!t || t->sec == 0) return NULL;

    unsigned int left = t->sec;
    while (!should_stop() && left > 0) {
        sleep(1);
        left--;
    }

    if (!should_stop()) {
        LOGI("[timer] reached %u sec, stopping...", t->sec);
        request_stop();
    }
    return NULL;
}

static void *stats_thread(void *arg)
{
    (void)arg;
    while (!should_stop()) {
        sleep(1);
        av_stats_tick_print(&g_stats);

        size_t vq = bq_size(&g_raw_vq);
        size_t hq = bq_size(&g_h264_q);
        size_t aq = bq_size(&g_aud_q);
        LOGI("[Q] raw=%zu/%zu h264=%zu/%zu audio=%zu/%zu",
             vq, bq_capacity(&g_raw_vq),
             hq, bq_capacity(&g_h264_q),
             aq, bq_capacity(&g_aud_q));

        uint64_t vdu = atomic_load(&g_video_pts_delta_us);
        uint64_t adu = atomic_load(&g_audio_pts_delta_us);
        if (vdu) {
            LOGI("[PTS] video_delta=%.3fms", (double)vdu / 1000.0);
        } else {
            LOGI("[PTS] video_delta=n/a");
        }
        if (adu) {
            LOGI("[PTS] audio_delta=%.3fms", (double)adu / 1000.0);
        } else {
            LOGI("[PTS] audio_delta=n/a");
        }

        avsync_report_1s(&g_avsync, rkav_now_monotonic_us());
    }
    return NULL;
}

static void *video_capture_thread(void *arg)
{
    ThreadArgs *ta = (ThreadArgs *)arg;
    const AppConfig *cfg = ta->cfg;

    V4L2Capture cap;
    if(v4l2_capture_open(&cap,cfg->video_device,cfg->width,cfg->height) != 0){
        LOGE("[video_cap] open failed");
        request_stop();
        return NULL;
    }
    if(v4l2_capture_start(&cap) != 0){
        LOGE("[video_cap] start failed");
        v4l2_capture_close(&cap);
        request_stop();
        return NULL;
    }

    uint64_t frame_id = 0;

    int has_seq = 0;
    uint32_t last_seq = 0;

    while(!should_stop()){
        int index = -1;
        void *data = NULL;
        size_t len = 0;

        int ret = v4l2_capture_dqbuf(&cap,&index,&data,&len);
        if(ret == 1){
            usleep(1000);
            continue;
        }

        if (ret != 0) {
            LOGE("[video_cap] dqbuf failed");
            av_stats_add_drop(&g_stats, 1);
            usleep(1000);
            continue;
        }

        if (!has_seq) {
            last_seq = cap.last_sequence;
            has_seq = 1;
        } else {
            uint32_t cur = cap.last_sequence;
            if (cur > last_seq + 1) {
                av_stats_add_drop(&g_stats, (uint64_t)(cur - last_seq - 1));
            }
            last_seq = cur;
        }
        // 产出点打 monotonic timestamp
        uint64_t pts_us = rkav_now_monotonic_us();

        VideoFrame *vf = (VideoFrame *)calloc(1, sizeof(VideoFrame));
        if (!vf) {
            av_stats_add_drop(&g_stats, 1);
            v4l2_capture_qbuf(&cap, index);
            continue;
        }

        vf->data = (uint8_t *)malloc(len);
        if (!vf->data) {
            free(vf);
            av_stats_add_drop(&g_stats, 1);
            v4l2_capture_qbuf(&cap, index);
            continue;
        }
        memcpy(vf->data, data, len);
        vf->size = len;
        vf->w = cfg->width;
        vf->h = cfg->height;
        vf->stride = cfg->width; // 先按 width，当需要更准再从 VIDIOC_G_FMT 取 stride
        vf->pts_us = pts_us;
        vf->frame_id = frame_id++;

        // raw 队列满就丢（稳定优先）
        int pr = bq_try_push(&g_raw_vq, vf);
        if (pr == 1) {
            // full
            av_stats_add_drop(&g_stats, 1);
            free_video_frame(vf);
        } else if (pr < 0) {
            free_video_frame(vf);
            v4l2_capture_qbuf(&cap, index);
            break;
        }

        v4l2_capture_qbuf(&cap, index);

    }
    v4l2_capture_close(&cap);
    return NULL;
}


static void *video_encode_thread(void *arg)
{
    ThreadArgs *ta = (ThreadArgs *)arg;
    const AppConfig *cfg = ta->cfg;

    EncoderMPP enc;
    if (encoder_mpp_init(&enc, cfg->width, cfg->height, cfg->fps,
                         cfg->bitrate, MPP_VIDEO_CodingAVC) != 0) {
        LOGE("[video_enc] encoder init failed");
        request_stop();
        return NULL;
    }

    while (!should_stop()) {
        void *item = NULL;
        int r = bq_pop(&g_raw_vq, &item);
        if (r == 0) break; // closed & empty
        if (r < 0) {
            av_stats_add_drop(&g_stats, 1);
            continue;
        }

        VideoFrame *vf = (VideoFrame *)item;

        uint8_t *pkt_data = NULL;
        size_t pkt_size = 0;
        bool key = false;

        int er = encoder_mpp_encode_packet(&enc, vf->data, vf->size,
                                           &pkt_data, &pkt_size, &key);
        if (er != 0) {
            av_stats_add_drop(&g_stats, 1);
            free_video_frame(vf);
            continue;
        }

        if (pkt_data && pkt_size > 0) {
            EncodedPacket *ep = (EncodedPacket *)calloc(1, sizeof(EncodedPacket));
            if (!ep) {
                free(pkt_data);
                av_stats_add_drop(&g_stats, 1);
            } else {
                ep->data = pkt_data;
                ep->size = pkt_size;
                ep->pts_us = vf->pts_us;
                ep->is_keyframe = key;

                int pr = bq_push(&g_h264_q, ep);
                if (pr != 0) {
                    free_encoded_packet(ep);
                    break;
                }

                av_stats_inc_video_frame(&g_stats);
                av_stats_add_enc_bytes(&g_stats, (uint64_t)pkt_size);
            }
        }

        free_video_frame(vf);

    }
    encoder_mpp_deinit(&enc);
    return NULL;

}

static void *audio_capture_thread(void *arg)
{
    ThreadArgs *ta = (ThreadArgs *)arg;
    const AppConfig *cfg = ta->cfg;

    AudioCapture ac;
    if (audio_capture_open(&ac, cfg->audio_device, cfg->sample_rate, (int)cfg->channels) != 0) {
        LOGE("[audio_cap] open failed");
        request_stop();
        return NULL;
    }

    // 起始 pts 用 monotonic，后续靠采样计数推进
    uint64_t pts_us = rkav_now_monotonic_us();

    size_t chunk_bytes = (size_t)ac.frames_per_period * ac.bytes_per_frame;

    while (!should_stop()) {
        uint8_t *buf = (uint8_t *)malloc(chunk_bytes);
        if (!buf) {
            av_stats_add_drop(&g_stats, 1);
            usleep(1000);
            continue;
        }

        ssize_t n = audio_capture_read(&ac, buf, chunk_bytes);
        if (n <= 0) {
            free(buf);
            if (!should_stop()) usleep(1000);
            continue;
        }

        uint32_t frames = (uint32_t)(n / ac.bytes_per_frame);

        AudioChunk *chunk = (AudioChunk *)calloc(1, sizeof(AudioChunk));
        if (!chunk) {
            free(buf);
            av_stats_add_drop(&g_stats, 1);
            continue;
        }

        chunk->data = buf;
        chunk->bytes = (size_t)n;
        chunk->sample_rate = (int)ac.sample_rate;
        chunk->channels = ac.channels;
        chunk->bytes_per_sample = 2; // S16LE
        chunk->frames = frames;
        chunk->pts_us = pts_us;

        // 推进 pts：frames 是“每声道帧数”
        pts_us += (uint64_t)frames * 1000000ULL / (uint64_t)ac.sample_rate;

        int pr = bq_push(&g_aud_q, chunk);
        if (pr != 0) {
            free_audio_chunk(chunk);
            break;
        }
    }

    audio_capture_close(&ac);
    return NULL;
}

static void *h264_sink_thread(void *arg)
{
    ThreadArgs *ta = (ThreadArgs *)arg;
    const AppConfig *cfg = ta->cfg;

    FILE *fp = fopen(cfg->output_path_h264, "wb");
    if (!fp) {
        LOGE("[h264_sink] open file failed: %s", cfg->output_path_h264);
        request_stop();
        return NULL;
    }
    LOGI("[h264_sink] opened: %s", cfg->output_path_h264);

    uint64_t last_pts = 0;

    while (!should_stop()) {
        void *item = NULL;
        int r = bq_pop(&g_h264_q, &item);
        if (r == 0) break;
        if (r < 0) continue;

        EncodedPacket *ep = (EncodedPacket *)item;
        if (last_pts && ep->pts_us > last_pts) {
            atomic_store(&g_video_pts_delta_us, ep->pts_us - last_pts);
        }
        last_pts = ep->pts_us;

        avsync_on_video(&g_avsync, ep->pts_us);


        if (ep->data && ep->size) {
            size_t w = fwrite(ep->data, 1, ep->size, fp);
            if (w != ep->size) {
                LOGW("[h264_sink] partial write: %zu/%zu", w, ep->size);
                request_stop();
            }
        }

        free_encoded_packet(ep);
    }

    fclose(fp);
    LOGI("[h264_sink] closed");
    return NULL;
}

static void *pcm_sink_thread(void *arg)
{
    ThreadArgs *ta = (ThreadArgs *)arg;
    const AppConfig *cfg = ta->cfg;

    FILE *fp = fopen(cfg->output_path_pcm, "wb");
    if (!fp) {
        LOGE("[pcm_sink] open file failed: %s", cfg->output_path_pcm);
        request_stop();
        return NULL;
    }
    LOGI("[pcm_sink] opened: %s", cfg->output_path_pcm);

    uint64_t last_pts = 0;

    while (!should_stop()) {
        void *item = NULL;
        int r = bq_pop(&g_aud_q, &item);
        if (r == 0) break;
        if (r < 0) continue;

        AudioChunk *ac = (AudioChunk *)item;
        if (last_pts && ac->pts_us > last_pts) {
            atomic_store(&g_audio_pts_delta_us, ac->pts_us - last_pts);
        }
        last_pts = ac->pts_us;

        avsync_on_audio(&g_avsync, ac->pts_us, ac->frames, (uint32_t)ac->sample_rate);

        if (ac->data && ac->bytes) {
            size_t w = fwrite(ac->data, 1, ac->bytes, fp);
            if (w != ac->bytes) {
                LOGW("[pcm_sink] partial write: %zu/%zu", w, ac->bytes);
                request_stop();
            }
        }

        av_stats_inc_audio_chunk(&g_stats);
        free_audio_chunk(ac);
    }

    fclose(fp);
    LOGI("[pcm_sink] closed");
    return NULL;
}


int main(int argc, char **argv)
{
    // 用 sigwait 捕获信号：先把 SIGINT/SIGTERM block 掉，避免异步 signal handler
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    AppConfig cfg;
    app_config_load_default(&cfg);
    if (app_config_parse_args(&cfg, argc, argv) != 0) {
        app_config_print_usage(argv[0]);
        return -1;
    }

    app_config_print_summary(&cfg);

    av_stats_init(&g_stats);
    atomic_store(&g_video_pts_delta_us, 0);
    atomic_store(&g_audio_pts_delta_us, 0);

    avsync_init(&g_avsync, cfg.fps);
    
    // 队列容量：稳定优先（raw 小一点，h264/audio 稍大一点）
    if (bq_init(&g_raw_vq, 8) != 0 ||
        bq_init(&g_h264_q, 64) != 0 ||
        bq_init(&g_aud_q, 256) != 0) {
        LOGE("[main] queue init failed");
        return -1;
    }

    ThreadArgs ta = { .cfg = &cfg };
    TimerArgs  targs = { .sec = cfg.duration_sec };

    pthread_t th_sig, th_timer, th_stat;
    pthread_t th_vcap, th_venc;
    pthread_t th_acap, th_h264sink, th_pcmsink;

    if (pthread_create(&th_sig, NULL, signal_thread, NULL) != 0) {
        LOGE("[main] pthread_create signal failed");
        return -1;
    }

    if (cfg.duration_sec > 0) {
        if (pthread_create(&th_timer, NULL, timer_thread, &targs) != 0) {
            LOGE("[main] pthread_create timer failed");
            request_stop();
        }
    }

    if (pthread_create(&th_stat, NULL, stats_thread, NULL) != 0) {
        LOGE("[main] pthread_create stats failed");
        request_stop();
    }

    if (pthread_create(&th_vcap, NULL, video_capture_thread, &ta) != 0) {
        LOGE("[main] pthread_create video_cap failed");
        request_stop();
    }
    if (pthread_create(&th_venc, NULL, video_encode_thread, &ta) != 0) {
        LOGE("[main] pthread_create video_enc failed");
        request_stop();
    }

    if (pthread_create(&th_acap, NULL, audio_capture_thread, &ta) != 0) {
        LOGE("[main] pthread_create audio_cap failed");
        request_stop();
    }

    if (pthread_create(&th_h264sink, NULL, h264_sink_thread, &ta) != 0) {
        LOGE("[main] pthread_create h264_sink failed");
        request_stop();
    }
    if (pthread_create(&th_pcmsink, NULL, pcm_sink_thread, &ta) != 0) {
        LOGE("[main] pthread_create pcm_sink failed");
        request_stop();
    }

    // join
    pthread_join(th_vcap, NULL);
    pthread_join(th_acap, NULL);
    pthread_join(th_venc, NULL);
    pthread_join(th_h264sink, NULL);
    pthread_join(th_pcmsink, NULL);

    // 停止后 stats/signal/timer 也收尾
    request_stop();
    pthread_join(th_stat, NULL);

    // signal 线程默认会一直阻塞，手动发一个 stop 后它仍在 sigwait：
    // 这里不强杀：让用户 Ctrl+C 或 kill；但为了让程序能自然退出，我们在 stop 后发送 SIGTERM 给自己
    pthread_kill(th_sig, SIGTERM);
    pthread_join(th_sig, NULL);

    if (cfg.duration_sec > 0) {
        pthread_join(th_timer, NULL);
    }

    // 清理队列
    bq_destroy(&g_raw_vq);
    bq_destroy(&g_h264_q);
    bq_destroy(&g_aud_q);

    avsync_deinit(&g_avsync);
    LOGI("[main] done. video=%s audio=%s", cfg.output_path_h264, cfg.output_path_pcm);
    return 0;
}
