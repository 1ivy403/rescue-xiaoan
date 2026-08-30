// test_rdk_stream.cpp: 隔离验证 rdk_stream 模块（无相机依赖）
// 用法: test_rdk_stream.exe [秒数]
// 喂 960x480 合成 RGBA 帧 @30fps, 本地 python 客户端收流验证。

#include "rdk_stream.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>

int main(int argc, char** argv) {
    const int run_sec = argc > 1 ? std::atoi(argv[1]) : 15;

    RdkStreamSender sender;
    RdkStreamSender::Options opt;
    opt.port = 9999;
    opt.target_fps = 10;
    opt.width = 480;
    opt.height = 240;

    sender.SetEventCallback([](const std::string& json) {
        std::cout << "[事件] " << json << std::endl;
    });
    if (!sender.Start(opt)) {
        std::cerr << "启动失败" << std::endl;
        return 1;
    }

    // 合成帧: 960x480 RGBA, 移动渐变色块
    const int W = 960, H = 480;
    std::vector<uint8_t> rgba(static_cast<size_t>(W) * H * 4);
    const auto t0 = std::chrono::steady_clock::now();
    int frames = 0;
    while (true) {
        const double t = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count();
        if (t >= run_sec) break;
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                const size_t i = (static_cast<size_t>(y) * W + x) * 4;
                rgba[i + 0] = static_cast<uint8_t>((x + static_cast<int>(t * 60)) % 256);
                rgba[i + 1] = static_cast<uint8_t>(y % 256);
                rgba[i + 2] = static_cast<uint8_t>((x + y) % 256);
                rgba[i + 3] = 255;
            }
        }
        sender.OnFrame(rgba.data(), W * 4, W, H,
                       static_cast<int64_t>(t * 1000));
        frames++;
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }

    sender.Stop();
    std::cout << "完成: 喂 " << frames << " 帧, 发送 " << sender.SentFrames()
              << " 帧, 收事件 " << sender.ReceivedEvents() << " 条" << std::endl;
    return 0;
}
