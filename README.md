# S1-rk-av-queue-pts

一个面向 **Rockchip RK35xx（RK3568 / RK3588）** 的 **S1 阶段音视频数据面工程**，  
在 S0（直连采集/编码）的基础上，完成了 **队列化 + 时间戳（PTS）重构**。

> 目标：为后续 **A/V 同步、推流、录制、AI 分支、多订阅者** 打下稳定的数据面地基。

---

## 核心特性（S1）

### 1️⃣ 生产者 → 队列 → 消费者（数据面解耦）
- 视频采集、视频编码、音频采集、文件写入 **完全线程解耦**
- 使用 **有界阻塞队列（mutex + condvar）**，稳定优先
- 明确背压边界，避免“跑着跑着内存爆炸”

队列划分：
- `RawVideoQueue`：原始 NV12 帧  
- `H264Queue`：编码后的 H.264 packet  
- `AudioQueue`：PCM 音频块  

---

### 2️⃣ 统一时间戳（PTS）策略
- **视频 PTS**  
  - 在 V4L2 `DQBUF` 成功后打 `CLOCK_MONOTONIC`
- **音频 PTS**  
  - 起始时间取 monotonic  
  - 后续通过 **采样计数累计推进**（避免 now() 抖动）

> 这是后续做 A/V sync、RTMP、MP4 的关键地基。

---

### 3️⃣ 实时统计与验收日志
程序每秒输出：

- `[STAT]`
  - `video_fps`
  - `enc_bitrate`
  - `audio_chunks_per_sec`
  - `drop_count`
- `[Q]`
  - 各队列当前深度 / 容量
- `[PTS]`
  - `video_delta`（帧间隔，≈33.3ms @30fps）
  - `audio_delta`（≈21.333ms @1024/48k）

示例：
```
[Q] raw=0/8 h264=0/64 audio=0/256
[PTS] video_delta=33.347ms
[PTS] audio_delta=21.333ms
```

---

## 目录结构（S1）

```
S1-rk-av-queue-pts/
├─ include/rkav/
│  ├─ types.h        # VideoFrame / AudioChunk / EncodedPacket
│  ├─ bqueue.h       # 有界阻塞队列
│  └─ time.h         # monotonic 时间工具
├─ src/
│  ├─ main.c
│  ├─ v4l2_capture.c
│  ├─ encoder_mpp.c
│  ├─ audio_capture.c
│  ├─ bqueue.c
│  ├─ av_stats.c
│  ├─ sink.c
│  └─ time.c
├─ docs/
│  └─ EXPERIMENT.md
├─ Makefile
└─ README.md
```

---

## 编译

```bash
make -j
```

> 如果你的系统使用 `-lmpp`：
```bash
make -j MPP_LIB=-lmpp
```

---

## 运行

### 默认运行（10 秒）
```bash
./s1_rk_queue
```

### 自定义参数示例
```bash
./s1_rk_queue   --video-dev /dev/video0   --size 1280x720   --fps 30   --bitrate 2000000   --audio-dev hw:0,0   --sr 48000   --ch 2   --sec 10   --out-h264 out.h264   --out-pcm out.pcm
```

---

## 输出文件
- `out.h264`：H.264 Annex-B 码流  
- `out.pcm`：s16le 原始 PCM 音频  

验证：
```bash
ffplay -f h264 out.h264
ffplay -f s16le -ar 48000 -ac 2 out.pcm
```

---

## 当前阶段说明
- 本仓库对应 **S1：队列 + PTS 数据面**
- 尚未包含：
  - IPC / daemon
  - 多订阅者 fan-out
  - A/V 同步与封装
- 这些将在后续 `rk-av-framework` 阶段引入

---

## License
TBD
