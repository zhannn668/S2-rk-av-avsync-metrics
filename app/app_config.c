#include "app_config.h"
#include "lib/utils/log.h"
#include <getopt.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static int parse_size(const char *s, int *w, int *h)
{
    if (!s || !w || !h) return -1;

    int ww = 0;
    int hh = 0;
    if (sscanf(s, "%dx%d", &ww, &hh) != 2 && sscanf(s, "%dX%d", &ww, &hh) != 2) {
        return -1;
    }
    if (ww <= 0 || hh <= 0) return -1;

    *w = ww;
    *h = hh;
    return 0;
}

int app_config_load_default(AppConfig *cfg)
{
    if (!cfg) return -1;
    memset(cfg, 0, sizeof(*cfg));

    cfg->video_device = "/dev/video0";
    cfg->width = 1280;
    cfg->height = 720;
    cfg->fps = 30;
    cfg->bitrate = 2000000;
    cfg->v4l2_fourcc = 0;

    cfg->audio_device = "hw:0.0";
    cfg->sample_rate = 48000;
    cfg->channels = 2;
    cfg->audio_chunks_ms = 20;

    cfg->sink_type = "file";
    cfg->output_path_h264 = "output.h264";
    cfg->output_path_pcm = "output.pcm";
    cfg->duration_sec = 20;

    return 0;
}

void app_config_print_summary(const AppConfig *cfg)
{
    if (!cfg) return;

    LOGI("[CFG] video: dev=%s size=%dx%d fps=%d bitrate=%d fourcc=0x%08x",
        cfg->video_device ? cfg->video_device : "(null)",
        cfg->width, cfg->height, cfg->fps, cfg->bitrate, cfg->v4l2_fourcc);
    LOGI("[CFG] audio: dev=%s sr=%u ch=%u chunk_ms=%u",
        cfg->audio_device ? cfg->audio_device : "(null)",
        cfg->sample_rate, cfg->channels, cfg->audio_chunks_ms);
    LOGI("[CFG] out: sink=%s h264=%s pcm=%s sec=%u",
        cfg->sink_type ? cfg->sink_type : "(null)",
        cfg->output_path_h264 ? cfg->output_path_h264 : "(null)",
        cfg->output_path_pcm ? cfg->output_path_pcm : "(null)",
        cfg->duration_sec);
}

void app_config_print_usage(const char *prog) //当用户传 -h/--help 或者遇到未知参数时会用到
{
    fprintf(stderr,
        "Usage:\n"
        "  %s [options]\n\n"
        "Options:\n"
        "  --video-dev <path>       Video device node (default: /dev/video0)\n"
        "  --size <WxH>             Capture size (default: 1280x720)\n"
        "  --fps <n>                Capture fps (default: 30)\n"
        "  --bitrate <bps>          H.264 target bitrate (default: 2000000)\n"
        "  --audio-dev <dev>        ALSA capture device (default: hw:0,0)\n"
        "  --sr <hz>                Audio sample rate (default: 48000)\n"
        "  --ch <n>                 Audio channels (default: 2)\n"
        "  --sec <n>                Record duration seconds (default: 10)\n"
        "  --out-h264 <file>        Output H.264 file (default: out.h264)\n"
        "  --out-pcm <file>         Output PCM file (default: out.pcm)\n"
        "  -h, --help               Show this help\n\n"
        "Examples:\n"
        "  %s --video-dev /dev/video0 --size 1920x1080 --fps 30 --bitrate 4000000 --sec 10\n"
        "  %s --out-h264 out.h264 --out-pcm out.pcm --sec 10\n",
        prog, prog, prog);
}

int app_config_parse_args(AppConfig *cfg,int argc,char **argv)
{
    if(!cfg) return -1;

        enum {
        OPT_VIDEO_DEV = 1000,
        OPT_SIZE,
        OPT_FPS,
        OPT_BITRATE,
        OPT_AUDIO_DEV,
        OPT_SR,
        OPT_CH,
        OPT_SEC,
        OPT_OUT_H264,
        OPT_OUT_PCM,
    };

    static const struct option long_opts[] = {
    {"video-dev", required_argument, 0, OPT_VIDEO_DEV},
    {"size",      required_argument, 0, OPT_SIZE},
    {"fps",       required_argument, 0, OPT_FPS},
    {"bitrate",   required_argument, 0, OPT_BITRATE},
    {"audio-dev", required_argument, 0, OPT_AUDIO_DEV},
    {"sr",        required_argument, 0, OPT_SR},
    {"ch",        required_argument, 0, OPT_CH},
    {"sec",       required_argument, 0, OPT_SEC},
    {"out-h264",  required_argument, 0, OPT_OUT_H264},
    {"out-pcm",   required_argument, 0, OPT_OUT_PCM},
    {"help",      no_argument,       0, 'h'},
    {0,0,0,0}
    };

    int c;

    while((c = getopt_long(argc, argv,"h",long_opts,NULL)) != -1){
        switch (c){
            case OPT_VIDEO_DEV:  cfg->video_device = optarg; break;
            case OPT_SIZE: 
                if(parse_size(optarg,&cfg->width,&cfg->height) != 0){
                    LOGE("[CFG] Invalid size format: %s\n", optarg);
                    return -1;
                }
                break;
            case OPT_FPS:       cfg->fps = atoi(optarg); break;
            case OPT_BITRATE:   cfg->bitrate = atoi(optarg); break;
            case OPT_AUDIO_DEV: cfg->audio_device = optarg; break;
            case OPT_SR:        cfg->sample_rate = (unsigned)atoi(optarg); break;
            case OPT_CH:        cfg->channels = (unsigned)atoi(optarg); break;
            case OPT_SEC:       cfg->duration_sec = (unsigned int)atoi(optarg); break;
            case OPT_OUT_H264:  cfg->output_path_h264 = optarg; break;
            case OPT_OUT_PCM:   cfg->output_path_pcm = optarg; break;
            case 'h':
            default:
            app_config_print_usage(argv[0]);
                exit(0);
        }
    }

    if (cfg->fps <= 0) cfg->fps = 30;
    if (cfg->width <= 0 || cfg->height <= 0) {
        LOGE("[CFG] invalid size: %dx%d", cfg->width, cfg->height);
        return -1;
    }
    if (cfg->bitrate <= 0) cfg->bitrate = 2000000;
    if (cfg->sample_rate == 0) cfg->sample_rate = 48000;
    if (cfg->channels == 0) cfg->channels = 2;

    return 0;
}