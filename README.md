# S2-rk-av-avsync-metrics（S2：音画同步观测与指标验证）

> 文件名说明：本 README 使用英文文件名（便于仓库规范），内容使用中文（便于开发与复盘）。
>
> **阶段目标**：先“看见问题”，再谈修正。S2 只做 **A/V 对齐指标统计与打印**，不做 mux/推流、不丢包、不重采样、不调速。

---

## 1. 项目定位：S1 → S2 → S3

本仓库基于 **S1-rk-av-queue-pts**（队列 + PTS 数据面）演进到 S2：

- **S1**：把视频帧 / 音频块改成“队列 + 时间戳（PTS）”形式输出（哪怕先只打印 PTS）。
- **S2（本仓库）**：实现 **AvSync 指标模块**，输出关键指标：
  - `av_offset_ms = video_pts - audio_pts`
  - `aligned_residual_ms`（音频主时钟对齐后残差）
  - `drift_msps`（偏差随时间增长速度，ms/s）
  - `video_jitter_ms p50/p95`
  - `audio_jitter_ms p50/p95`
- **S3（下一步）**：在 S2 指标证据基础上，S3 服务化骨架（rkavd + rkavctl）（控制面成型）。


> S2 的价值：给出“量化证据”，回答：
> 1) 是否在跑飞？  
> 2) 跑飞方向（视频慢/快，音频慢/快）？  
> 3) 抖动分布（p50/p95）？  
> 4) 后续应选哪类校正策略？

---

## 2. 核心设计：音频主时钟（Audio Master Clock）

### 2.1 为什么选音频主时钟？
音频天然具有稳定采样率（例如 48kHz），更适合做“主时间轴”：
- 音频 PTS 可以由 **采样计数累加** 推进，抖动小；
- 视频帧更容易受到调度、ISP、编码负载影响。

### 2.2 启动对齐（锁定 offset）
首次同时拿到音频/视频的 PTS 后锁定初始 offset：

- `offset_us = audio0_us - video0_us`
- `video_pts_aligned = video_pts + offset_us`
- 对齐残差：
  - `aligned_residual_ms = (video_pts_aligned - audio_pts) / 1000`

残差围绕 0 抖动 → 同步稳定  
残差单边线性增长/减小 → 存在 drift（跑飞）

---

## 3. 目录与模块（建议结构）

典型结构（可按实际仓库为准）：

```
S2-rk-av-avsync-metrics/
├── Makefile
├── README_*.md
├── include/
│   └── rkav/ ...
└── src/
    ├── main.c              # 线程编排：capture/encode/sink/stats
    ├── avsync.h            # AvSync 接口 + AvSync struct（注意点见 Troubleshooting）
    ├── avsync.c            # AvSync 实现：offset/drift/jitter
    ├── log.h / log.c
    ├── ...（v4l2/mpp/alsa/queue）
```

---

## 4. 运行流程（数据面）

**视频链路（示例）**：
1) V4L2 dequeue -> 产生 `video_pts`（单调时钟）
2) MPP 编码 -> 产生 `EncodedPacket`
3) 入队列 -> sink 线程消费 -> 写入 `out.h264`
4) **sink 消费处**喂给 `avsync_on_video()`

**音频链路（示例）**：
1) ALSA 读取 PCM -> `audio_pts`（起点 monotonic + samples 累加推进）
2) 入队列 -> sink 线程消费 -> 写入 `out.pcm`
3) **sink 消费处**喂给 `avsync_on_audio()`

**统计线程**：
- 每秒打印 `STAT/Q/PTS`
- 同时调用 `avsync_report_1s()` 输出 A/V 对齐报告

> 关键：**AvSync 输入点放在 sink 线程**更能代表“下游实际体验”，不会把生产侧抖动混进结果。

---

## 5. 指标定义与解释

### 5.1 av_offset_ms（原始偏差）
```
av_offset_ms = (video_pts - audio_pts) / 1000
```
含义：此时刻视频时间轴相对音频领先（正）或落后（负）。

### 5.2 aligned_residual_ms（音频主时钟对齐残差）
```
offset_us = audio0_us - video0_us
aligned_residual_ms = ( (video_pts + offset_us) - audio_pts ) / 1000
```
含义：启动对齐后，视频与音频是否仍逐渐漂移。

### 5.3 drift_msps（漂移速率）
```
drift_msps = Δ(aligned_residual_ms) / Δ(time_s)
```
单位：ms/s（每秒漂移多少毫秒）

方向解释：
- drift_msps > 0 ：视频更快 / 音频更慢
- drift_msps < 0 ：视频更慢 / 音频更快
- drift_msps ≈ 0：长期稳定（允许短期抖动）

### 5.4 jitter（抖动 p50/p95）
每秒统计采样间隔偏离“理论间隔”的绝对值：

视频（fps=30）理论间隔约 33.333ms  
音频（period=1024@48k）理论间隔约 21.333ms

```
jitter = | actual_interval - expected_interval |
```

- p50：中位数抖动
- p95：尾部抖动（更能体现调度峰值）

---

## 6. 使用教程（编译 / 部署 / 运行）

### 6.1 编译（交叉编译示例）
在 x86 主机上使用 RK3568 buildroot 工具链：

```bash
make
```

若需要清理：
```bash
make clean
make
```

### 6.2 部署到板端运行
把生成的二进制（例如 `bin/s2_rk_avsync`）拷到板端执行。

### 6.3 运行（示例）
默认 10 秒：

```bash
./s2_rk_avsync
```

跑 10 分钟验收：

```bash
./s2_rk_avsync --sec 600
```

输出文件：
- `out.h264`
- `out.pcm`

---

## 7. 典型运行日志（实测样例）

启动与设备初始化：

```
[I] [CFG] video=/dev/video0 1280x720@30 bitrate=2000000 | audio=hw:0,0 48000Hz ch=2 | out=out.h264,out.pcm | sec=10
[I] [v4l2] format set: 1280x720 NV12M
[I] [mpp_enc] init ok 1280x720 fps=30 bitrate=2000000
[I] [audio] opened device=hw:0,0, 48000 Hz, ch=2, period=1024 frames, 4 B/frame
```

锁定 offset（音频主时钟）：

```
[I] [AVSYNC] locked offset_us=-71337 (audio0=88193048, video0=88264385)
```

每秒对齐报告（示例）：

```
[I] [AVSYNC] av_offset_ms=22.326 aligned_residual_ms=-49.011 drift_msps=-4.056803 (video_slower_or_audio_faster)
           | v_jitter_ms p50=0.306 p95=0.905 | a_jitter_ms p50=0.000 p95=0.000
```

> 注意：上述 drift 为早期口径下的输出示例（见第 8 章“问题复盘：统计口径导致的假漂移/放大漂移”）。

---

## 8. 问题复盘：为什么会出现“看似线性跑飞”的 drift？

### 8.1 现象
在早期实现中，`avsync_report_1s()` 用的是：
- `last_video_pts` 与 `last_audio_pts` 直接相减

由于音频 chunk 频率（约 47/s）高于视频帧率（30/s），并且 report 的触发点固定在“每秒定时器时刻”，会出现：

- report 时刻音频 `last_audio_pts` 很可能刚更新
- 视频 `last_video_pts` 可能距离上一帧有 0~33ms 的“相位差”
- 这种相位差在每秒采样中会表现为“系统性偏移”，甚至看起来像线性趋势
- 结果：drift_msps 被污染（假漂移）或被放大

### 8.2 正确口径：视频事件配对（Video-event pairing）
改进策略：

- 每来一个 **视频帧**（avsync_on_video），用“最近的 audio_pts（last_audio_pts）”配对，计算：
  - `off_ms = video - audio`
  - `res_ms = (video + offset) - audio`
- 每秒 report 时，对这一秒内收集到的 residual 样本做排序，取 p50/p95
- drift 用 “residual p50” 来计算，更稳定、更可信

这可以显著降低“采样相位”对 drift 的影响，使 S2 输出更接近真实同步状态。

---

## 9. Troubleshooting（踩坑记录 + 解决方案）

本节记录实际开发中遇到的典型报错与修复方式，方便后续快速定位。

### 9.1 编译错误：AvSync 存储大小未知（incomplete type）
**报错**：
```
src/main.c:24:15: 错误： ‘g_avsync’的存储大小未知
static AvSync g_avsync;
```

**原因**：`avsync.h` 只有前向声明 `typedef struct AvSync AvSync;`，结构体不完整，无法在 main.c 里定义实体。

**解决方案**（二选一）：
1) **推荐**：把 `struct AvSync { ... }` 的完整定义放进 `avsync.h`
2) 或者改 main.c 使用 `AvSync*` 指针并提供 create/destroy（改动更大）

---

### 9.2 链接错误：undefined reference to avsync_xxx
**报错**：
```
undefined reference to `avsync_report_1s'
undefined reference to `avsync_on_video'
undefined reference to `avsync_on_audio'
undefined reference to `avsync_init'
undefined reference to `avsync_deinit'
```

**原因**：链接时没有把 `src/avsync.c` 编进目标（Makefile SRCS/OBJS 缺失）。

**解决方案**：
- Makefile 的 SRCS 列表里加入：
  - `src/avsync.c`
- 或 OBJS 列表里加入：
  - `src/avsync.o`

---

### 9.3 git apply 补丁损坏
**报错**：
```
error: 补丁在第 XX 行损坏
```

**原因**：终端复制粘贴 patch 时，出现丢行、重复行、`+/-` 破坏、缩进错位等。

**解决方案**：
- 不建议手敲 patch  
- 优先用“按文件修改”的方式：
  - 直接 `vi src/avsync.h / src/avsync.c / Makefile` 修改

---

### 9.4 drift 输出看起来异常大（例如 -4ms/s）
**现象**：
- `aligned_residual_ms` 每秒持续变负
- `drift_msps` 约 -4ms/s
- 但音频 jitter p50/p95 显示 0（音频 PTS 很稳定）

**原因**（高概率）：
- drift 统计口径使用了 `last_video_pts` 与 `last_audio_pts` 的“秒表抽样”，受音频更新频率更高影响产生相位偏差（measurement artifact）

**解决方案**：
- 改为“视频事件配对”的 residual 样本收集（见第 8.2 节）
- 每秒用 residual 样本的 p50 计算 drift

---

## 10. 验收标准（S2）

### 10.1 运行时长
- 连续运行 **10 分钟**（600 秒）

### 10.2 判定
- `aligned_residual_ms` 不持续单边线性跑飞（允许抖动）
- `drift_msps` 长期接近 0（允许噪声）
- 能明确判断方向：
  - drift > 0：视频更快/音频更慢
  - drift < 0：视频更慢/音频更快

---

## 11. 下一步（S3 建议方向）

当 S2 能稳定复现并量化 drift 后，S3 可以做：

- 视频：重复/丢帧（以音频为主时钟对齐）
- 音频：轻量时间伸缩或重采样（更平滑）
- 统一播放/输出时钟：在 sink 层实现“同步控制器”

---

## 12. 快速 FAQ

### Q1：为什么 audio_jitter p50/p95 经常是 0？
因为音频 PTS 多数情况下由“采样计数累加”推进，理论间隔固定（例如 1024@48k → 21.333ms），所以 jitter 很小是正常的。

### Q2：video_jitter p95 偶尔变大正常吗？
正常。视频链路会受 ISP、编码、调度影响，p95 体现的是尾部调度峰值。

### Q3：S2 为什么不做丢包/重采样？
因为 S2 的目标是**先观测与定位**。校正属于 S3，不要在没有证据时“盲调”。

---

## 13. 维护建议（对未来自己/别人友好）

- 每次改 AvSync 口径，都跑一次：
  - 30 秒（快速验证）
  - 10 分钟（验收）
- 保留关键日志作为证据（可保存到 `docs/`）
- 任何 drift 结论都要基于：
  - 配对口径（视频事件配对）
  - p50/p95 的分布，而不是单点

---

**Done.**
