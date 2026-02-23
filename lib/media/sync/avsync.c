#include "avsync.h"
#include "lib/utils/log.h"
#include "rkav/time.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define TAG "AVSYNC"

#define RK_NAN (0.0/0.0)

#define MAX_VJ  128   // 30fps -> <=30/s
#define MAX_AJ  256   // 48k/1024 -> ~46/s

static inline double us_to_ms(double us) { return us / 1000.0; }

static inline double dabs(double x) { return x < 0.0 ? -x : x; }
static inline int is_nan(double x) { return x != x; }

static int cmp_double(const void *a, const void *b)
{
    double da = *(const double *)a;
    double db = *(const double *)b;
    return (da > db) - (da < db);
}

static double percentile_nearest(double *arr, int n, double q)
{
    if (!arr || n <= 0) return RK_NAN;
    if (q <= 0.0) return arr[0];
    if (q >= 1.0) return arr[n - 1];

    // rank = ceil(q*n)
    double x = q * (double)n;
    int rank = (int)x;
    if ((double)rank < x) rank += 1;
    if (rank < 1) rank = 1;
    if (rank > n) rank = n;
    return arr[rank - 1];
}

static void try_lock_offset(AvSync *s)
{
    if (s->offset_locked) return;
    if (s->has_video0 && s->has_audio0) {
        s->offset_us = (int64_t)s->audio0_us - (int64_t)s->video0_us;
        s->offset_locked = 1;
        LOGI("[%s] locked offset_us=%lld (audio0=%llu, video0=%llu)",
             TAG,
             (long long)s->offset_us,
             (unsigned long long)s->audio0_us,
             (unsigned long long)s->video0_us);
    }
}

int avsync_init(AvSync *s, int video_fps)
{
    if (!s) return -1;
    memset(s, 0, sizeof(*s));
    if (pthread_mutex_init(&s->mu, NULL) != 0) return -1;

    if (video_fps <= 0) video_fps = 30;
    s->expected_video_delta_us = 1000000ULL / (uint64_t)video_fps;
    return 0;
}

void avsync_deinit(AvSync *s)
{
    if (!s) return;
    pthread_mutex_destroy(&s->mu);
}

void avsync_on_video(AvSync *s, uint64_t video_pts_us)
{
    if (!s) return;
    pthread_mutex_lock(&s->mu);

    if (!s->has_video0) {
        s->has_video0 = 1;
        s->video0_us = video_pts_us;
    }

    try_lock_offset(s);

    // paired offset/residual on every VIDEO event (audio as reference)
    if (s->has_last_audio) {
        int64_t v = (int64_t)video_pts_us;
        int64_t a = (int64_t)s->last_audio_us;

        double off_ms = us_to_ms((double)(v - a));
        if (s->off_n < 128) s->off_ms[s->off_n++] = off_ms;

        if (s->offset_locked) {
            double res_ms = us_to_ms((double)((v + s->offset_us) - a));
            if (s->res_n < 128) s->res_ms[s->res_n++] = res_ms;
        }
    }

    if (s->has_last_video && video_pts_us > s->last_video_us) {
        uint64_t delta_us = video_pts_us - s->last_video_us;
        double jitter_ms = dabs(us_to_ms((double)delta_us - (double)s->expected_video_delta_us));
        if (s->vj_n < MAX_VJ) s->vj_ms[s->vj_n++] = jitter_ms;
    }

    s->has_last_video = 1;
    s->last_video_us = video_pts_us;

    pthread_mutex_unlock(&s->mu);
}

void avsync_on_audio(AvSync *s, uint64_t audio_pts_us, uint32_t frames, uint32_t sample_rate)
{
    if (!s) return;
    if (sample_rate == 0) return;
    uint64_t now_us = rkav_now_monotonic_us();
    pthread_mutex_lock(&s->mu);

    if (!s->has_audio0) {
        s->has_audio0 = 1;
        s->audio0_us = audio_pts_us;
        try_lock_offset(s);
    }

    if (s->has_last_audio_arrival && now_us > s->last_audio_arrival_us && s->has_last_audio_meta) {
        uint64_t delta_us = now_us - s->last_audio_arrival_us;

        // expected delta = previous chunk duration
        uint64_t expected_us = (uint64_t)s->last_audio_frames * 1000000ULL / (uint64_t)s->last_audio_sr;
        double jitter_ms = dabs(us_to_ms((double)delta_us - (double)expected_us));
        if (s->aj_n < MAX_AJ) s->aj_ms[s->aj_n++] = jitter_ms;
    }

    s->has_last_audio = 1;
    s->last_audio_us = audio_pts_us;
    s->last_audio_frames = frames;
    s->last_audio_sr = sample_rate;
    s->has_last_audio_meta = 1;
    s->has_last_audio_arrival = 1;
    s->last_audio_arrival_us = now_us;

    pthread_mutex_unlock(&s->mu);
}

void avsync_report_1s(AvSync *s, uint64_t now_us)
{
    if (!s) return;

    pthread_mutex_lock(&s->mu);

    const int has_v = s->has_last_video;
    const int has_a = s->has_last_audio;
    const int locked = s->offset_locked;

    uint64_t v_us = s->last_video_us;
    uint64_t a_us = s->last_audio_us;
    int64_t  off  = s->offset_us;

    // jitter percentiles
    double v_p50 = RK_NAN, v_p95 = RK_NAN;
    double a_p50 = RK_NAN, a_p95 = RK_NAN;

        // paired offset/residual percentiles (computed on video events)
    double off_p50 = RK_NAN, off_p95 = RK_NAN;
    double res_p50 = RK_NAN, res_p95 = RK_NAN;


    if (s->vj_n > 0) {
        qsort(s->vj_ms, s->vj_n, sizeof(double), cmp_double);
        v_p50 = percentile_nearest(s->vj_ms, s->vj_n, 0.50);
        v_p95 = percentile_nearest(s->vj_ms, s->vj_n, 0.95);
    }
    if (s->aj_n > 0) {
        qsort(s->aj_ms, s->aj_n, sizeof(double), cmp_double);
        a_p50 = percentile_nearest(s->aj_ms, s->aj_n, 0.50);
        a_p95 = percentile_nearest(s->aj_ms, s->aj_n, 0.95);
    }
    if (s->off_n > 0) {
        qsort(s->off_ms, s->off_n, sizeof(double), cmp_double);
        off_p50 = percentile_nearest(s->off_ms, s->off_n, 0.50);
        off_p95 = percentile_nearest(s->off_ms, s->off_n, 0.95);
    }
    if (s->res_n > 0) {
        qsort(s->res_ms, s->res_n, sizeof(double), cmp_double);
        res_p50 = percentile_nearest(s->res_ms, s->res_n, 0.50);
        res_p95 = percentile_nearest(s->res_ms, s->res_n, 0.95);
    }


    // reset bucket for next second
    s->vj_n = 0;
    s->aj_n = 0;
    s->off_n = 0;
    s->res_n = 0;


    char v50_s[32], v95_s[32], a50_s[32], a95_s[32];
    if (is_nan(v_p50)) snprintf(v50_s, sizeof(v50_s), "n/a"); else snprintf(v50_s, sizeof(v50_s), "%.3f", v_p50);
    if (is_nan(v_p95)) snprintf(v95_s, sizeof(v95_s), "n/a"); else snprintf(v95_s, sizeof(v95_s), "%.3f", v_p95);
    if (is_nan(a_p50)) snprintf(a50_s, sizeof(a50_s), "n/a"); else snprintf(a50_s, sizeof(a50_s), "%.3f", a_p50);
    if (is_nan(a_p95)) snprintf(a95_s, sizeof(a95_s), "n/a"); else snprintf(a95_s, sizeof(a95_s), "%.3f", a_p95);

       // compute offsets & drift (use paired samples on video events)
    double av_offset_ms = off_p50;     // p50 of paired (video-audio)
    double residual_ms  = res_p50;     // p50 of paired aligned residual
    double drift_msps   = RK_NAN;      // ms/s

    if (!is_nan(residual_ms) && locked) {
        if (!s->drift_base_set) {
            s->drift_base_set = 1;
            s->drift_t0_us = now_us;
            s->residual0_ms = residual_ms;
        } else if (now_us > s->drift_t0_us) {
            double elapsed_s = (double)(now_us - s->drift_t0_us) / 1000000.0;
            if (elapsed_s > 0.0) {
                drift_msps = (residual_ms - s->residual0_ms) / elapsed_s;
            }
        }
    }


    const char *dir = "n/a";
    if (!is_nan(drift_msps)) {
        if (drift_msps > 0.0) dir = "video_faster_or_audio_slower";
        else if (drift_msps < 0.0) dir = "video_slower_or_audio_faster";
        else dir = "stable";
    }

    if (is_nan(av_offset_ms)) {
        LOGI("[%s] av_offset_ms=n/a drift_msps=n/a | v_jitter_ms p50=%s p95=%s | a_jitter_ms p50=%s p95=%s",
             TAG, v50_s, v95_s, a50_s, a95_s);
    } else {
        if (locked) {
            if (is_nan(drift_msps)) drift_msps = 0.0;

            LOGI("[%s] av_offset_ms=%.3f aligned_residual_ms=%.3f drift_msps=%.6f (%s) | "
                 "v_jitter_ms p50=%s p95=%s | a_jitter_ms p50=%s p95=%s",
                 TAG,
                 av_offset_ms,
                 residual_ms,
                 drift_msps,
                 dir,
                 v50_s, v95_s, a50_s, a95_s);
        } else {
            LOGI("[%s] av_offset_ms=%.3f drift_msps=n/a | v_jitter_ms p50=%s p95=%s | a_jitter_ms p50=%s p95=%s",
                 TAG, av_offset_ms, v50_s, v95_s, a50_s, a95_s);
        }
    }

    pthread_mutex_unlock(&s->mu);
}
