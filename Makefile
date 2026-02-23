# ==== Toolchain & paths (优先使用 Buildroot environment-setup 导出的变量) ====

SDK_OUT        ?= $(HOME)/rk3568_linux_sdk/buildroot/output/rockchip_atk_dlrk3568
DEFAULT_SYSROOT := $(SDK_OUT)/host/aarch64-buildroot-linux-gnu/sysroot

# sysroot：优先 SYSROOT，其次 SDKTARGETSYSROOT，最后默认
SYSROOT ?= $(if $(SDKTARGETSYSROOT),$(SDKTARGETSYSROOT),$(DEFAULT_SYSROOT))

# 交叉编译器：如果环境里已经有 CC 就用环境的，否则用 Buildroot host/bin 里的
CROSS_PREFIX ?= $(SDK_OUT)/host/bin/aarch64-buildroot-linux-gnu-
CC ?= $(CROSS_PREFIX)gcc

# （预留）FFMPEG & MPP 安装前缀
FFMPEG_PREFIX ?= /home/zhan/ffmpeg_rk3568/install/usr

# ==== Includes ====
CFLAGS  := --sysroot=$(SYSROOT)
CFLAGS  += -D_POSIX_C_SOURCE=200809L
CFLAGS  += -D_XOPEN_SOURCE=700
CFLAGS  += -I$(SYSROOT)/usr/include
CFLAGS  += -I$(SYSROOT)/usr/include/rockchip
CFLAGS  += -I$(FFMPEG_PREFIX)/include
CFLAGS  += -I.
CFLAGS  += -I./app
CFLAGS  += -I./lib/core
CFLAGS  += -I./lib/media/audio
CFLAGS  += -I./lib/media/video
CFLAGS  += -I./plugins/sink_file
CFLAGS  += -I./include
CFLAGS  += -D_GNU_SOURCE
CFLAGS  += -O2 -Wall -Wextra -std=gnu11



# ==== Libs ====
LDFLAGS := --sysroot=$(SYSROOT)
LDFLAGS += -L$(SYSROOT)/usr/lib
LDFLAGS += -L$(SYSROOT)/usr/lib/rockchip
LDFLAGS += -L$(FFMPEG_PREFIX)/lib

# 线程/ALSA/MPP
LIBS    := -lpthread -lasound -lrockchip_mpp -lrt
# 如果你的系统是 -lmpp：make MPP_LIB=-lmpp
# MPP_LIB ?= -lrockchip_mpp


# ==== Sources ====
SRCS := \
    app/main.c \
    lib/utils/log.c \
    lib/media/video/v4l2_capture.c \
    lib/media/video/encoder_mpp.c \
    lib/media/audio/audio_capture.c \
    plugins/sink_file/sink.c \
    app/app_config.c \
    lib/core/av_stats.c \
    lib/media/buffer/bqueue.c \
    lib/utils/time.c \
    lib/media/sync/avsync.c
OBJS   := $(SRCS:.c=.o)

# 目标输出（你可以改名）
TARGET := bin/s2_rk_avsync


# ==== Rules ====
.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LIBS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
