// rdk_stream.h: PC → RDK X5 低清帧 TCP/JPEG 推流 + RDK 事件回传
//
// 拓扑: PC(192.168.50.1) 作 TCP 服务端监听, RDK X5(192.168.50.2) 作客户端连入。
// 协议（小端, 同一条 TCP 连接全双工）:
//   帧  PC→RDK: "INFR" | u32 payload_len | u64 ts_ms | JPEG payload
//   事件 RDK→PC: "INEV" | u32 payload_len | JSON payload
//
// 线程模型:
//   - OnFrame() 由实时拼接回调线程调用: 只做降采样 RGBA→BGR24 + 更新最新帧槽（~0.3ms）
//   - 内部 sender 线程按 target_fps 取最新帧槽 → WIC JPEG 编码 → 发送
//   - 内部 recv 线程解析 "INEV" 事件, 回调 / 计数
//   - 断线自动回到 accept 等待, RDK 端负责重连

#pragma once

#include <cstdint>
#include <functional>
#include <string>

class RdkStreamSender {
public:
    struct Options {
        uint16_t port = 9999;     // 监听端口（RDK 连入）
        int target_fps = 10;      // 发送帧率（计划: 5-10fps）
        int jpeg_quality = 80;    // JPEG 质量
        int width = 480;         // 推流分辨率（计划: 480x240）
        int height = 240;
    };

    RdkStreamSender();
    ~RdkStreamSender();

    RdkStreamSender(const RdkStreamSender&) = delete;
    RdkStreamSender& operator=(const RdkStreamSender&) = delete;

    // 启动监听 + 发送/接收线程（失败返回 false, 链路可降级为不推流继续跑）
    bool Start(const Options& opt);
    void Stop();

    // 实时拼接回调线程调用: 喂最新拼接帧（RGBA 平面数据）
    void OnFrame(const uint8_t* rgba, int linesize, int width, int height, int64_t timestamp);

    // CameraSDK 音频回调线程调用: 喂 X4 麦克风音频包（内部排队丢旧, 不阻塞回调线程）
    void OnAudio(const uint8_t* data, size_t size, int64_t timestamp);

    // RDK 事件 JSON 回调（recv 线程调用, 处理要快）
    void SetEventCallback(std::function<void(const std::string& json)> cb);

    // 统计
    bool ClientConnected() const;
    uint64_t SentFrames() const;
    uint64_t ReceivedEvents() const;
    std::string LastEvent() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};
