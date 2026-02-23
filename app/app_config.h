#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C"{
#endif

typedef struct{
    /*Video*/
    const char *video_device;
    int width;
    int height;
    int fps;
    int bitrate;
    uint32_t v4l2_fourcc;

    /*Audio*/
    const char *audio_device;
    unsigned int sample_rate;
    unsigned int channels;
    unsigned int audio_chunks_ms;

    /*Output*/
    const char *sink_type;
    const char *output_path_h264;
    const char *output_path_pcm;
    unsigned int duration_sec;

} AppConfig;

int app_config_load_default(AppConfig *cfg);

void app_config_print_usage(const char *prog);

int app_config_parse_args(AppConfig *cfg,int argc,char **argv);

void app_config_print_summary(const AppConfig *cfg);

#ifdef __cplusplus
}
#endif