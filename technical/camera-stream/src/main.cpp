// x4_live_demo: 「独拍自由」最小验证工程
// 链路: 发现 X4 → 打开 → 预览流(双路鱼眼 H264) → RealTimeStitcher 实时拼接
//       → 拼接帧抽帧存 BMP + 帧率统计（验证 Demo 最高风险链路）
//
// 用法: x4_live_demo [--duration 30] [--res low|high] [--stitch template|dynamic]
//                    [--size 960x480] [--frames-dir ./frames] [--models <dir>] [--debug]

#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <thread>
#include <csignal>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <algorithm>
#include <filesystem>

#include <camera/camera.h>
#include <camera/device_discovery.h>
#include <camera/photography_settings.h>
#include <ins_realtime_stitcher.h>
#include <ins_stitcher.h>

#include "rdk_stream.h"
#include "photo_output.h"

namespace fs = std::filesystem;

// ---------------- 全局状态 ----------------
static std::atomic<bool> g_stop{ false };

// 统计计数器
static std::atomic<uint64_t> g_video_packets{ 0 };   // CameraSDK 收到的视频包（双路合计）
static std::atomic<uint64_t> g_gyro_packets{ 0 };
static std::atomic<uint64_t> g_audio_packets{ 0 };   // X4 麦克风音频包
static std::atomic<uint64_t> g_stitched_frames{ 0 }; // 拼接输出的帧数
static std::atomic<int> g_out_width{ 0 };
static std::atomic<int> g_out_height{ 0 };
static std::atomic<int> g_out_format{ 0 };

// RDK 推流（可选链路）
static std::unique_ptr<RdkStreamSender> g_rdk_sender;

// 出图（事件驱动抓拍 + 自动美化）
static std::unique_ptr<MomentCapture> g_moments;

// 相机 SD 卡录像（可选）
static bool g_record_sd = false;

// RDK 推流分辨率（事件 bbox 坐标系, 用于归一化裁切提示）
static int g_rdk_w = 480;
static int g_rdk_h = 240;

static void SignalHandler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        std::cout << "\n[main] 收到退出信号..." << std::endl;
        g_stop = true;
    }
}

// ---------------- BMP 保存（无第三方依赖） ----------------
// 输入为 RGBA 平面数据（RealTimeStitcher 回调格式），保存为 24 位 BMP
static bool SaveBMP24(const std::string& path, const uint8_t* rgba, int linesize, int width, int height) {
    const int row_size = ((width * 3 + 3) / 4) * 4; // 4 字节对齐
    const uint32_t data_size = static_cast<uint32_t>(row_size) * static_cast<uint32_t>(height);
    const uint32_t file_size = 54 + data_size;

    std::vector<uint8_t> buf(file_size, 0);
    // BITMAPFILEHEADER (14 字节)
    buf[0] = 'B'; buf[1] = 'M';
    memcpy(&buf[2], &file_size, 4);
    const uint32_t offset = 54;
    memcpy(&buf[10], &offset, 4);
    // BITMAPINFOHEADER (40 字节)
    const uint32_t hdr_size = 40;
    memcpy(&buf[14], &hdr_size, 4);
    memcpy(&buf[18], &width, 4);
    memcpy(&buf[22], &height, 4); // 正值 = 自底向上
    const uint16_t planes = 1, bpp = 24;
    memcpy(&buf[26], &planes, 2);
    memcpy(&buf[28], &bpp, 2);
    memcpy(&buf[34], &data_size, 4);

    for (int y = 0; y < height; y++) {
        const uint8_t* src = rgba + static_cast<size_t>(height - 1 - y) * linesize;
        uint8_t* dst = buf.data() + 54 + static_cast<size_t>(y) * row_size;
        for (int x = 0; x < width; x++) {
            dst[x * 3 + 0] = src[x * 4 + 2]; // B
            dst[x * 3 + 1] = src[x * 4 + 1]; // G
            dst[x * 3 + 2] = src[x * 4 + 0]; // R
        }
    }

    FILE* fp = nullptr;
    if (fopen_s(&fp, path.c_str(), "wb") != 0 || !fp) {
        return false;
    }
    const bool ok = fwrite(buf.data(), 1, buf.size(), fp) == buf.size();
    fclose(fp);
    return ok;
}

// BGR24 紧凑数组存 24 位 BMP（第三视角校正帧用）
static bool SaveBMP24BGR(const std::string& path, const uint8_t* bgr, int width, int height) {
    const int row_size = ((width * 3 + 3) / 4) * 4;
    const uint32_t data_size = static_cast<uint32_t>(row_size) * static_cast<uint32_t>(height);

    std::vector<uint8_t> buf(54 + data_size, 0);
    buf[0] = 'B'; buf[1] = 'M';
    const uint32_t file_size = static_cast<uint32_t>(buf.size());
    memcpy(&buf[2], &file_size, 4);
    const uint32_t offset = 54, hdr_size = 40;
    memcpy(&buf[10], &offset, 4);
    memcpy(&buf[14], &hdr_size, 4);
    memcpy(&buf[18], &width, 4);
    memcpy(&buf[22], &height, 4);
    const uint16_t planes = 1, bpp = 24;
    memcpy(&buf[26], &planes, 2);
    memcpy(&buf[28], &bpp, 2);
    memcpy(&buf[34], &data_size, 4);

    for (int y = 0; y < height; y++) {
        memcpy(buf.data() + 54 + static_cast<size_t>(y) * row_size,
               bgr + static_cast<size_t>(height - 1 - y) * width * 3,
               static_cast<size_t>(width) * 3);
    }

    FILE* fp = nullptr;
    if (fopen_s(&fp, path.c_str(), "wb") != 0 || !fp) {
        return false;
    }
    const bool ok = fwrite(buf.data(), 1, buf.size(), fp) == buf.size();
    fclose(fp);
    return ok;
}

// ---------------- 相机流回调 → 转发给实时拼接器 ----------------
class StitchDelegate : public ins_camera::StreamDelegate {
public:
    explicit StitchDelegate(const std::shared_ptr<ins::RealTimeStitcher>& stitcher)
        : stitcher_(stitcher) {}

    void OnAudioData(const uint8_t* data, size_t size, int64_t timestamp) override {
        // X4 麦克风音频 → RDK 推流通道(INAF) → ws_server 解码做呼救声检测
        g_audio_packets++;
        static std::atomic<bool> probed{ false };
        if (!probed.exchange(true)) {
            std::cout << "[音频] 首包 size=" << size << " ts=" << timestamp << " head=";
            for (size_t i = 0; i < size && i < 8; i++) {
                std::cout << std::hex << std::setw(2) << std::setfill('0')
                          << static_cast<int>(data[i]) << " " << std::dec;
            }
            std::cout << std::endl;
        }
        if (g_rdk_sender) g_rdk_sender->OnAudio(data, size, timestamp);
    }

    void OnVideoData(const uint8_t* data, size_t size, int64_t timestamp,
                     uint8_t streamType, int stream_index) override {
        // 双路鱼眼流（stream_index 0/1）直接喂给拼接器
        g_video_packets++;
        stitcher_->HandleVideoData(data, size, timestamp, streamType, stream_index);
    }

    void OnGyroData(const std::vector<ins_camera::GyroData>& data) override {
        // ins_camera::GyroData 与 ins::GyroData 布局一致（官方 demo 同款做法）
        g_gyro_packets++;
        std::vector<ins::GyroData> data_vec(data.size());
        if (!data.empty()) {
            memcpy(data_vec.data(), data.data(), data.size() * sizeof(ins_camera::GyroData));
        }
        stitcher_->HandleGyroData(data_vec);
    }

    void OnExposureData(const ins_camera::ExposureData& data) override {
        ins::ExposureData exposure{};
        exposure.timestamp = data.timestamp;
        exposure.exposure_time = data.exposure_time;
        stitcher_->HandleExposureData(exposure);
    }

private:
    std::shared_ptr<ins::RealTimeStitcher> stitcher_;
};

// ---------------- 参数 ----------------
struct Args {
    int duration_sec = 30;                       // 运行时长，0 = 手动 Ctrl+C 退出
    bool high_res = false;                       // low=1440x720 / high=3840x1920
    ins::STITCH_TYPE stitch_type = ins::STITCH_TYPE::TEMPLATE;
    int out_width = 960;
    int out_height = 480;
    std::string frames_dir = "./frames";
    std::string models_dir;                      // 为空则不设置（TEMPLATE/DYNAMIC 不依赖 AI 模型）
    int save_every_n = 30;                       // 每 N 个拼接帧存一张 BMP（约 1 秒一张）
    bool debug = false;
    // RDK 推流
    bool rdk_stream = false;                     // 启用 PC→RDK TCP/JPEG 推流
    uint16_t rdk_port = 9999;                    // 监听端口（RDK 连入）
    int rdk_fps = 10;                            // 推流帧率（计划: 5-10fps）
    int rdk_width = 480;                         // 推流分辨率（计划: 480x240）
    int rdk_height = 240;
    // 出图
    bool photos = true;                          // 出图模块（默认开, --no-photos 关）
    std::string photos_dir = "./photos";         // 成片输出目录
    int photo_interval = 3000;                   // 两张成片最小间隔 ms
    int photo_fallback = 10000;                  // 无事件兜底抓拍间隔 ms, 0=关
    float beauty_strength = 1.0f;                // 美化强度 0~1
    // 相机 SD 卡录像
    bool record_sd = false;                      // 直播流期间同步录到相机 SD 卡
    std::string videos_dir = "./videos";         // 录像导出目录（收尾自动拉取）
};

// 参数越界/除零等崩溃防护：所有需值的选项必须带参数；数值统一夹取到安全范围
static const char* NextValue(int argc, char* argv[], int& i, const char* opt_name) {
    if (i + 1 >= argc) {
        std::cerr << "[参数错误] " << opt_name << " 缺少参数值" << std::endl;
        std::exit(2);
    }
    return argv[++i];
}

static int ClampInt(int v, int lo, int hi, int dft) {
    if (v < lo || v > hi || v == 0) return dft;
    return v;
}

static Args ParseArgs(int argc, char* argv[]) {
    Args args;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--duration") {
            args.duration_sec = std::atoi(NextValue(argc, argv, i, "--duration"));
        } else if (arg == "--res") {
            const std::string res = NextValue(argc, argv, i, "--res");
            args.high_res = (res == "high" || res == "3840x1920");
        } else if (arg == "--stitch") {
            const std::string t = NextValue(argc, argv, i, "--stitch");
            if (t == "dynamic") {
                args.stitch_type = ins::STITCH_TYPE::DYNAMICSTITCH;
            } else if (t == "optflow") {
                args.stitch_type = ins::STITCH_TYPE::OPTFLOW;
            } else {
                args.stitch_type = ins::STITCH_TYPE::TEMPLATE;
            }
        } else if (arg == "--size") {
            const std::string s = NextValue(argc, argv, i, "--size");
            const auto pos = s.find('x');
            if (pos != std::string::npos) {
                args.out_width = ClampInt(std::atoi(s.substr(0, pos).c_str()), 64, 4096, 960);
                args.out_height = ClampInt(std::atoi(s.substr(pos + 1).c_str()), 64, 2048, 480);
            }
        } else if (arg == "--frames-dir") {
            args.frames_dir = NextValue(argc, argv, i, "--frames-dir");
        } else if (arg == "--models") {
            args.models_dir = NextValue(argc, argv, i, "--models");
        } else if (arg == "--save-every") {
            // 0 或负数会导致回调内 frame_idx % 0 除零崩溃 → 夹取到 ≥1
            args.save_every_n = ClampInt(std::atoi(NextValue(argc, argv, i, "--save-every")), 1, 100000, 30);
        } else if (arg == "--debug") {
            args.debug = true;
        } else if (arg == "--rdk-stream") {
            args.rdk_stream = true;
        } else if (arg == "--rdk-port") {
            const int p = std::atoi(NextValue(argc, argv, i, "--rdk-port"));
            args.rdk_port = static_cast<uint16_t>((p >= 1 && p <= 65535) ? p : 9999);
        } else if (arg == "--rdk-fps") {
            args.rdk_fps = ClampInt(std::atoi(NextValue(argc, argv, i, "--rdk-fps")), 1, 30, 10);
        } else if (arg == "--rdk-size") {
            const std::string s = NextValue(argc, argv, i, "--rdk-size");
            const auto pos = s.find('x');
            if (pos != std::string::npos) {
                args.rdk_width = ClampInt(std::atoi(s.substr(0, pos).c_str()), 64, 4096, 480);
                args.rdk_height = ClampInt(std::atoi(s.substr(pos + 1).c_str()), 64, 2048, 240);
            }
        } else if (arg == "--no-photos") {
            args.photos = false;
        } else if (arg == "--photos-dir") {
            args.photos_dir = NextValue(argc, argv, i, "--photos-dir");
        } else if (arg == "--photo-interval") {
            // 两张成片最小间隔: 夹取 [500, 60000] ms
            args.photo_interval = ClampInt(std::atoi(NextValue(argc, argv, i, "--photo-interval")), 500, 60000, 3000);
        } else if (arg == "--photo-fallback") {
            // 兜底抓拍间隔: 0=关, 夹取 [0, 60000] ms（0 会被 ClampInt 当默认, 特判）
            const int v = std::atoi(NextValue(argc, argv, i, "--photo-fallback"));
            args.photo_fallback = (v < 0 || v > 60000) ? 10000 : v;
        } else if (arg == "--beauty") {
            // 美化强度 0(原图直出)~1(满): 夹取
            args.beauty_strength = std::atof(NextValue(argc, argv, i, "--beauty"));
            if (!(args.beauty_strength >= 0.0f)) args.beauty_strength = 1.0f;  // NaN/负 → 默认
            if (args.beauty_strength > 1.0f) args.beauty_strength = 1.0f;
        } else if (arg == "--record") {
            args.record_sd = true;
        } else if (arg == "--videos-dir") {
            args.videos_dir = NextValue(argc, argv, i, "--videos-dir");
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "用法: x4_live_demo [--duration 30] [--res low|high] "
                         "[--stitch template|dynamic|optflow] [--size 960x480] "
                         "[--frames-dir ./frames] [--models <dir>] [--save-every 30] [--debug]\n"
                         "             [--rdk-stream] [--rdk-port 9999] [--rdk-fps 10] [--rdk-size 480x240]\n"
                         "             [--no-photos] [--photos-dir ./photos] [--photo-interval 3000] "
                         "[--photo-fallback 10000] [--beauty 1.0]\n"
                         "             [--record] [--videos-dir ./videos]   录像并收尾自动导出到 PC"
                      << std::endl;
            std::exit(0);
        }
    }
    return args;
}

// ---------------- 极简 JSON 字符串字段提取（RDK 事件里的 "type":"xxx"） ----------------
static std::string ExtractJsonStringField(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};
    pos = json.find('"', pos);
    if (pos == std::string::npos) return {};
    const size_t start = pos + 1;
    const size_t end = json.find('"', start);
    if (end == std::string::npos) return {};
    return json.substr(start, end - start);
}

// ---------------- 极简 JSON 数字数组提取（RDK 事件 "bbox":[x,y,w,h]） ----------------
static bool ExtractJsonNumberArray4(const std::string& json, const std::string& key, float out[4]) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return false;
    pos = json.find('[', pos + needle.size());
    if (pos == std::string::npos) return false;
    const size_t end = json.find(']', pos);
    if (end == std::string::npos) return false;

    int cnt = 0;
    size_t i = pos + 1;
    while (i < end && cnt < 4) {
        while (i < end && (json[i] == ' ' || json[i] == ',')) i++;
        if (i >= end) break;
        char* pend = nullptr;
        const double v = std::strtod(json.c_str() + i, &pend);
        if (pend == json.c_str() + i) break;   // 不是数字
        out[cnt++] = static_cast<float>(v);
        i = static_cast<size_t>(pend - json.c_str());
    }
    return cnt == 4;
}

// ---------- 循环覆盖: 目录内仅保留最新 keep 个指定后缀文件, 更旧的删除 ----------
// 用于 frames/videos 等运行产物目录, 防止长时间运行写满磁盘
static void PruneDirOldest(const std::string& dir, const std::string& ext, size_t keep) {
    std::error_code ec;
    std::vector<std::pair<fs::file_time_type, std::string>> items;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!e.is_regular_file(ec) || e.path().extension() != ext) continue;
        const auto wt = e.last_write_time(ec);
        if (ec) continue;
        items.emplace_back(wt, e.path().filename().string());
    }
    if (items.size() <= keep) return;
    std::sort(items.begin(), items.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    size_t removed = 0;
    for (size_t i = 0; i + keep < items.size(); ++i) {
        std::error_code ec2;
        fs::remove(dir + "/" + items[i].second, ec2);
        if (!ec2) removed++;
    }
    if (removed > 0) {
        std::cout << "[循环覆盖] " << dir << " 删除最旧 " << removed
                  << " 个 " << ext << " (仅保留最近 " << keep << " 个)" << std::endl;
    }
}

// ---------- 录像导出：从相机 SD 卡拉取录像文件到 PC ----------
// 文件通道走 CameraSDK 文件接口（USB）, 无需拔卡。
// 优先在文件列表中匹配 MediaUrl 的 uri；匹配不到则取列表里最新的视频文件。
static std::string PullRecordedVideo(ins_camera::Camera* cam, const ins_camera::MediaUrl& url,
                                     const std::string& out_dir) {
    std::error_code ec;
    fs::create_directories(out_dir, ec);

    const auto files = cam->GetCameraFilesList();
    if (files.empty()) return {};

    // uri 可能是完整 URL/路径, 用"文件名包含"做匹配
    std::string target;
    const auto& uris = url.OriginUrls();
    for (const auto& uri : uris) {
        const std::string name = uri.substr(uri.find_last_of("/\\") + 1);
        if (name.empty()) continue;
        for (const auto& f : files) {
            if (f.find(name) != std::string::npos) { target = f; break; }
        }
        if (!target.empty()) break;
    }

    // 兜底: 取列表最后（最新）的视频文件
    if (target.empty()) {
        for (auto it = files.rbegin(); it != files.rend(); ++it) {
            const std::string& f = *it;
            const std::string low = [&]{ std::string s = f; std::transform(s.begin(), s.end(), s.begin(), ::tolower); return s; }();
            if (low.find(".insv") != std::string::npos || low.find(".mp4") != std::string::npos) {
                target = f;
                break;
            }
        }
    }
    if (target.empty()) return {};

    // 本地落盘名: 时间戳 + 相机内扩展名
    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm);
    const std::string ext = target.substr(target.find_last_of('.') + 1);
    const std::string local = out_dir + "/rec_" + stamp + "." + ext;

    std::cout << "[录像] 导出中: " << target << std::endl;
    const bool ok = cam->DownloadCameraFile(target, local, [](int64_t done, int64_t total) {
        if (total > 0 && done >= total) {
            std::cout << "[录像] 传输完成" << std::endl;
        }
    });
    if (ok) {
        // 循环覆盖: 录像导出仅保留最近 1 个(单个约 5GB), 删更旧的
        PruneDirOldest(out_dir, "." + ext, 1);
    }
    return ok ? local : std::string();
}

int main(int argc, char* argv[]) {
    const Args args = ParseArgs(argc, argv);

    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    // ---------- 1. SDK 初始化 ----------
    ins::InitEnv();
    ins_camera::SetLogLevel(args.debug ? ins_camera::LogLevel::VERBOSE : ins_camera::LogLevel::WARNING);
    ins::SetLogLevel(args.debug ? ins::InsLogLevel::VERBOSE : ins::InsLogLevel::WARNING);
    if (!args.models_dir.empty()) {
        ins::SetModelFileRootDir(args.models_dir);
    }

    std::cout << "[1/6] 初始化 SDK 完成" << std::endl;

    // ---------- 2. 发现设备 ----------
    ins_camera::DeviceDiscovery discovery;
    auto list = discovery.GetAvailableDevices();
    if (list.empty()) {
        std::cerr << "[错误] 未发现 Insta360 设备，请确认 X4 已通过 USB 连接并开机" << std::endl;
        return -1;
    }

    for (const auto& camera : list) {
        std::cout << "  发现设备: " << camera.camera_name
                  << " | sn: " << camera.serial_number
                  << " | fw: " << camera.fw_version << std::endl;
    }

    const auto camera_type = list[0].camera_type;
    auto cam = std::make_shared<ins_camera::Camera>(list[0].info);
    discovery.FreeDeviceDescriptors(list);

    // ---------- 3. 打开相机 ----------
    if (!cam->Open()) {
        std::cerr << "[错误] 打开相机失败" << std::endl;
        return -1;
    }
    std::cout << "[2/7] 相机已打开" << std::endl;

    // ---------- 4. 配置实时拼接器 ----------
    auto stitcher = std::make_shared<ins::RealTimeStitcher>();

    // 用相机自带的标定参数构造 CameraInfo（官方 realtime demo 同款做法）
    ins::CameraInfo camera_info;
    const auto preview_param = cam->GetPreviewParam();
    camera_info.cameraName = preview_param.camera_name;
    camera_info.decode_type = static_cast<ins::VideoDecodeType>(preview_param.encode_type);
    camera_info.offset = preview_param.offset;
    camera_info.window_crop_info_.crop_offset_x = preview_param.crop_info.crop_offset_x;
    camera_info.window_crop_info_.crop_offset_y = preview_param.crop_info.crop_offset_y;
    camera_info.window_crop_info_.dst_width = preview_param.crop_info.dst_width;
    camera_info.window_crop_info_.dst_height = preview_param.crop_info.dst_height;
    camera_info.window_crop_info_.src_width = preview_param.crop_info.src_width;
    camera_info.window_crop_info_.src_height = preview_param.crop_info.src_height;
    camera_info.gyro_timestamp = preview_param.delay_timestamp;
    camera_info.sweep_timestamp = preview_param.sweep_time;

    stitcher->SetCameraInfo(camera_info);
    stitcher->SetStitchType(args.stitch_type);
    stitcher->EnableFlowState(true);
    stitcher->SetOutputSize(args.out_width, args.out_height);

    std::atomic<uint64_t> saved_frames{ 0 };
    stitcher->SetStitchRealTimeDataCallback(
        [&](uint8_t* data[4], int linesize[4], int width, int height, int format, int64_t timestamp) {
            g_stitched_frames++;
            g_out_width = width;
            g_out_height = height;
            g_out_format = format;

            // 喂 RDK 推流（内部只做降采样, 不阻塞拼接回调）
            if (g_rdk_sender) {
                g_rdk_sender->OnFrame(data[0], linesize[0], width, height, timestamp);
            }

            // 喂出图模块（内部只做抓拍判定+帧拷贝, 美化在 worker 线程）
            if (g_moments) {
                g_moments->OnStitchFrame(data[0], linesize[0], width, height, timestamp);
            }

            const uint64_t frame_idx = g_stitched_frames.load();
            if (frame_idx % args.save_every_n == 0) {
                std::string path = args.frames_dir + "/frame_"
                    + std::to_string(frame_idx) + "_ts" + std::to_string(timestamp);

                // 双输出: 有人物位置 → 全景原图 + 第三视角校正图（带位置定位标注）各存一张
                MomentCapture::CropHint hint;
                if (g_moments && g_moments->LatestCropHint(&hint)) {
                    std::vector<uint8_t> bgr(static_cast<size_t>(width) * height * 3);
                    for (int y = 0; y < height; y++) {
                        const uint8_t* src = data[0] + static_cast<size_t>(y) * linesize[0];
                        uint8_t* dst = bgr.data() + static_cast<size_t>(y) * width * 3;
                        for (int x = 0; x < width; x++) {
                            dst[x * 3 + 0] = src[x * 4 + 2];
                            dst[x * 3 + 1] = src[x * 4 + 1];
                            dst[x * 3 + 2] = src[x * 4 + 0];
                        }
                    }
                    const float cxn = hint.x + hint.w * 0.5f;
                    const float cyn = hint.y + hint.h * 0.5f;
                    const float lon0 = (1.0f - 2.0f * cxn) * 3.14159265f;
                    float lat0 = (0.5f - cyn) * 3.14159265f;
                    lat0 = std::min(1.4f, std::max(-1.4f, lat0));
                    float fov_y = (hint.h * 3.14159265f) / 0.7f;
                    fov_y = std::min(1.92f, std::max(0.60f, fov_y));
                    const float fov_x = 2.0f * std::atan(std::tan(fov_y * 0.5f) * 0.75f);
                    std::vector<uint8_t> view;
                    ReprojectRectilinear(bgr.data(), width, height, lon0, lat0, fov_x, 480, 640, view);
                    AnnotatePersonPosition(view.data(), 480, 640, bgr.data(), width, height,
                                           hint.x, hint.y, hint.w, hint.h, lon0, lat0, fov_x);
                    if (SaveBMP24(path + "_pano.bmp", data[0], linesize[0], width, height)) {
                        saved_frames++;
                    }
                    if (SaveBMP24BGR(path + "_view.bmp", view.data(), 480, 640)) {
                        saved_frames++;
                    }
                    // 位置定位日志: 人物在全景球面上的方位（路演时可口头讲解的量化信息）
                    const float az_deg = lon0 * 180.0f / 3.14159265f;
                    const float el_deg = lat0 * 180.0f / 3.14159265f;
                    std::cout << "[位置] 第" << frame_idx << "帧 人物方位=" << az_deg
                              << "° 俯仰=" << el_deg << "° bbox=(" << hint.x << "," << hint.y
                              << "," << hint.w << "," << hint.h << ")" << std::endl;
                } else {
                    path += ".bmp";
                    if (SaveBMP24(path, data[0], linesize[0], width, height)) {
                        saved_frames++;
                    }
                }
                // 循环覆盖: 调试抽帧只保留最近 60 张 BMP, 防止写满磁盘
                PruneDirOldest(args.frames_dir, ".bmp", 60);
            }
        });

    stitcher->SetStitchStateCallback([&](int error, const char* err_info) {
        std::cerr << "[拼接错误] code=" << error << " info=" << (err_info ? err_info : "") << std::endl;
        g_stop = true;
    });

    std::cout << "[3/7] 拼接器已配置 (type="
              << (args.stitch_type == ins::STITCH_TYPE::TEMPLATE ? "template"
                 : args.stitch_type == ins::STITCH_TYPE::DYNAMICSTITCH ? "dynamic" : "optflow")
              << ", 输出=" << args.out_width << "x" << args.out_height << ")" << std::endl;

    // ---------- 5. 启动 RDK 推流（可选链路, 失败不阻塞主演示） ----------
    if (args.rdk_stream) {
        g_rdk_sender = std::make_unique<RdkStreamSender>();
        RdkStreamSender::Options rdk_opt;
        rdk_opt.port = args.rdk_port;
        rdk_opt.target_fps = args.rdk_fps;
        rdk_opt.width = args.rdk_width;
        rdk_opt.height = args.rdk_height;
        g_rdk_w = args.rdk_width;
        g_rdk_h = args.rdk_height;
        g_rdk_sender->SetEventCallback([](const std::string& json) {
            std::cout << "[RDK事件] " << json << std::endl;
            // D0: person_enter → 立即抓拍; D1 规则引擎加 "moment_*" 事件同样触发
            if (g_moments) {
                // bbox（推流坐标系）→ 归一化人物位置, 供出片时第三视角裁切
                float bb[4];
                if (ExtractJsonNumberArray4(json, "bbox", bb) && bb[2] > 1.0f && bb[3] > 1.0f &&
                    bb[2] <= static_cast<float>(g_rdk_w) && bb[3] <= static_cast<float>(g_rdk_h)) {
                    MomentCapture::CropHint hint;
                    hint.x = bb[0] / g_rdk_w;
                    hint.y = bb[1] / g_rdk_h;
                    hint.w = bb[2] / g_rdk_w;
                    hint.h = bb[3] / g_rdk_h;
                    g_moments->UpdateCropHint(hint);
                }
                const std::string type = ExtractJsonStringField(json, "type");
                if (type == "person_enter" || type.rfind("moment", 0) == 0) {
                    g_moments->RequestCapture(type);
                }
            }
        });
        if (!g_rdk_sender->Start(rdk_opt)) {
            std::cerr << "[警告] RDK 推流启动失败, 本次运行降级为不推流" << std::endl;
            g_rdk_sender.reset();
        }
    }

    // ---------- 5.5 启动出图模块（默认开, 失败不阻塞主演示） ----------
    if (args.photos) {
        g_moments = std::make_unique<MomentCapture>();
        MomentCapture::Options m_opt;
        m_opt.out_dir = args.photos_dir;
        m_opt.min_interval_ms = args.photo_interval;
        m_opt.fallback_interval_ms = args.photo_fallback;
        m_opt.strength = args.beauty_strength;
        if (!g_moments->Start(m_opt)) {
            std::cerr << "[警告] 出图模块启动失败, 本次运行不出片" << std::endl;
            g_moments.reset();
        }
    }

    // ---------- 6. 启动预览流 ----------
    fs::create_directories(args.frames_dir);

    // X4 及以后机型需要先切 LIVEVIEW 子模式并设置预览分辨率（CameraSDK 官方示例要求）
    if (camera_type >= ins_camera::CameraType::Insta360X4) {
        if (!cam->SetVideoSubMode(ins_camera::SubVideoMode::VIDEO_LIVEVIEW)) {
            std::cerr << "[警告] 切换 LIVEVIEW 子模式失败，继续尝试" << std::endl;
        }
        ins_camera::RecordParams record_params;
        record_params.resolution = args.high_res
            ? ins_camera::VideoResolution::RES_3840_1920P30
            : ins_camera::VideoResolution::RES_1440_720P30;
        record_params.bitrate = 0;
        if (!cam->SetVideoCaptureParams(record_params, ins_camera::CameraFunctionMode::FUNCTION_MODE_LIVE_STREAM)) {
            std::cerr << "[警告] 设置预览分辨率失败，继续尝试" << std::endl;
        }
    }

    std::shared_ptr<ins_camera::StreamDelegate> delegate = std::make_shared<StitchDelegate>(stitcher);
    cam->SetStreamDelegate(delegate);

    ins_camera::LiveStreamParam param;
    param.video_resolution = args.high_res
        ? ins_camera::VideoResolution::RES_3840_1920P30
        : ins_camera::VideoResolution::RES_1440_720P30;
    param.lrv_video_resulution = ins_camera::VideoResolution::RES_1440_720P30;
    param.video_bitrate = args.high_res ? (1024 * 1024 * 4) : (1024 * 1024 / 2);
    param.enable_audio = true;   // X4 麦克风音频（呼救声检测数据源）
    param.using_lrv = false;

    if (!cam->StartLiveStreaming(param)) {
        std::cerr << "[错误] 启动预览流失败" << std::endl;
        cam->Close();
        return -1;
    }
    stitcher->StartStitch();
    std::cout << "[6/7] 预览流 + 实时拼接已启动 ("
              << (args.high_res ? "3840x1920" : "1440x720") << "P30)" << std::endl;

    // ---------- 6.5 相机 SD 卡录像（可选, 与直播流并行; 失败不影响主演示） ----------
    if (args.record_sd) {
        if (cam->StartRecording()) {
            g_record_sd = true;
            std::cout << "[录像] 相机 SD 卡录像已开始" << std::endl;
        } else {
            std::cerr << "[警告] SD 卡录像启动失败（直播子模式下可能不支持）, 继续演示" << std::endl;
        }
    }

    // ---------- 7. 运行 + 统计 ----------
    std::cout << "[7/7] 采集中... 时长 " << args.duration_sec << " 秒（Ctrl+C 可提前结束）" << std::endl;

    const auto start_time = std::chrono::steady_clock::now();
    uint64_t last_report_frames = 0;
    auto last_report_time = start_time;
    int elapsed = 0;

    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        elapsed++;

        // 每 5 秒打印一次帧率统计
        if (elapsed % 5 == 0) {
            const auto now = std::chrono::steady_clock::now();
            const double dt = std::chrono::duration<double>(now - last_report_time).count();
            const uint64_t frames = g_stitched_frames.load();
            const double fps = (frames - last_report_frames) / dt;
            std::cout << "  [统计] " << elapsed << "s | 拼接帧=" << frames
                      << " | fps=" << std::fixed << std::setprecision(1) << fps
                      << " | 视频包=" << g_video_packets.load()
                      << " | 陀螺仪包=" << g_gyro_packets.load()
                      << " | 音频包=" << g_audio_packets.load()
                      << " | 输出=" << g_out_width.load() << "x" << g_out_height.load();
            if (g_rdk_sender) {
                std::cout << " | RDK=" << (g_rdk_sender->ClientConnected() ? "已连接" : "未连接")
                          << " 发送" << g_rdk_sender->SentFrames() << "帧"
                          << " 收事件" << g_rdk_sender->ReceivedEvents();
            }
            if (g_moments) {
                std::cout << " | 成片" << g_moments->CapturedCount() << "张";
            }
            std::cout << std::endl;
            last_report_frames = frames;
            last_report_time = now;
        }

        if (args.duration_sec > 0 && elapsed >= args.duration_sec) {
            break;
        }
    }

    // ---------- 收尾 ----------
    std::cout << "停止中..." << std::endl;
    std::string pulled_video;
    if (g_record_sd) {
        const auto url = cam->StopRecording();
        std::cout << "[录像] SD 卡录像已停止" << std::endl;
        // 文件通道: 通过 USB 直接把录像从相机 SD 卡拉到 PC, 无需拔卡
        pulled_video = PullRecordedVideo(cam.get(), url, args.videos_dir);
        if (!pulled_video.empty()) {
            std::cout << "[录像] 已导出: " << pulled_video << std::endl;
        } else {
            std::cout << "[录像] 导出失败或无文件（可用 App/读卡器兜底）" << std::endl;
        }
        g_record_sd = false;
    }
    if (g_rdk_sender) {
        g_rdk_sender->Stop();
    }
    if (g_moments) {
        g_moments->Stop();
    }
    cam->StopLiveStreaming();
    stitcher->CancelStitch();
    cam->Close();

    const double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
    std::cout << "\n========== 验证结果 ==========" << std::endl;
    std::cout << "运行时长       : " << std::fixed << std::setprecision(1) << total << " s" << std::endl;
    std::cout << "CameraSDK 视频包: " << g_video_packets.load() << std::endl;
    std::cout << "陀螺仪数据包   : " << g_gyro_packets.load() << std::endl;
    std::cout << "拼接输出帧数   : " << g_stitched_frames.load() << std::endl;
    std::cout << "平均拼接帧率   : " << std::fixed << std::setprecision(1)
              << g_stitched_frames.load() / (total > 0 ? total : 1.0) << " fps" << std::endl;
    std::cout << "输出分辨率     : " << g_out_width.load() << "x" << g_out_height.load()
              << " (format=" << g_out_format.load() << ")" << std::endl;
    std::cout << "已保存抽帧 BMP : " << saved_frames.load()
              << " 张 → " << fs::absolute(args.frames_dir).string() << std::endl;
    if (g_rdk_sender) {
        std::cout << "RDK 推流       : 发送 " << g_rdk_sender->SentFrames() << " 帧, 收到事件 "
                  << g_rdk_sender->ReceivedEvents() << " 条" << std::endl;
        std::cout << "最后事件       : " << g_rdk_sender->LastEvent() << std::endl;
        std::cout << "判定: 发送帧数>0 且事件数>0 → PC→RDK 推流链路验证通过" << std::endl;
    }
    if (g_moments) {
        std::cout << "出图           : " << g_moments->CapturedCount() << " 张成片 → "
                  << fs::absolute(args.photos_dir).string() << std::endl;
        std::cout << "最后成片       : " << g_moments->LastOutput() << std::endl;
        std::cout << "判定: 成片>0 → 相机→拼接→美化→出图 链路验证通过" << std::endl;
    }
    if (!pulled_video.empty()) {
        std::cout << "录像导出       : " << pulled_video << std::endl;
        std::cout << "判定: 文件存在 → USB 文件通道（SD卡→PC）验证通过" << std::endl;
    }
    std::cout << "==============================" << std::endl;
    std::cout << "判定: 拼接帧数 > 0 且 fps 接近 30 → 链路验证通过" << std::endl;

    return 0;
}
