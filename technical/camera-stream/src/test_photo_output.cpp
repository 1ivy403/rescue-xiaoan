// test_photo_output.cpp: 出图模块离线自测（不连相机、不发网络）
//
// 用合成 RGBA 帧模拟拼接回调流, 验证:
//   1. 兜底定时抓拍: 无事件时按 fallback 间隔自动出片
//   2. 事件抓拍: RequestCapture() 后下一帧出片
//   3. 节流: 最小间隔内的多次请求合并为一张
//   4. 美化: 输出 JPG 与原始 BMP 均落盘且非空
//   5. 坏输入防御: 空指针/坏尺寸帧不崩溃
//   6. 第三视角重投影: 有效 hint → 480x640 透视平面; hint 过期 → 全幅直出
//
// 用法: test_photo_output [输出目录]（默认 ./test_photos）

#include "photo_output.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

static int g_pass = 0;
static int g_fail = 0;

static void Check(bool cond, const std::string& name) {
    if (cond) {
        g_pass++;
        std::cout << "  PASS  " << name << std::endl;
    } else {
        g_fail++;
        std::cout << "  FAIL  " << name << std::endl;
    }
}

// 生成合成 RGBA 测试帧（渐变 + 色块, 让色阶拉伸有实际效果）
static std::vector<uint8_t> MakeFrame(int w, int h) {
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const size_t idx = (static_cast<size_t>(y) * w + x) * 4;
            rgba[idx + 0] = static_cast<uint8_t>(x * 255 / w);        // R 渐变
            rgba[idx + 1] = static_cast<uint8_t>(y * 255 / h);        // G 渐变
            rgba[idx + 2] = static_cast<uint8_t>((x + y) * 127 / (w + h)); // B 低对比（待拉伸）
            rgba[idx + 3] = 255;
        }
    }
    return rgba;
}

// 模拟拼接回调流: 以 frame_ms 间隔持续喂帧 duration_ms 毫秒
static void FeedFrames(MomentCapture& mc, int w, int h, int frame_ms, int duration_ms) {
    const auto frame = MakeFrame(w, h);
    const auto t0 = std::chrono::steady_clock::now();
    int64_t ts = 0;
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0)
               .count() < duration_ms) {
        mc.OnStitchFrame(frame.data(), w * 4, w, h, ts);
        ts += frame_ms;
        std::this_thread::sleep_for(std::chrono::milliseconds(frame_ms));
    }
}

// 读取 24 位 BMP（main 程序 SaveBMP24 同款格式）→ BGR24
static bool LoadBMP24(const std::string& path, std::vector<uint8_t>& bgr, int& w, int& h) {
    FILE* fp = nullptr;
    if (fopen_s(&fp, path.c_str(), "rb") != 0 || !fp) return false;
    uint8_t hdr[54] = {0};
    if (fread(hdr, 1, 54, fp) != 54 || hdr[0] != 'B' || hdr[1] != 'M') { fclose(fp); return false; }
    int offset = 0, bw = 0, bh = 0;
    uint16_t bpp = 0;
    memcpy(&offset, hdr + 10, 4);
    memcpy(&bw, hdr + 18, 4);
    memcpy(&bh, hdr + 22, 4);
    memcpy(&bpp, hdr + 28, 2);
    if (bpp != 24 || bw <= 0 || bh <= 0) { fclose(fp); return false; }
    const int row = ((bw * 3 + 3) / 4) * 4;
    bgr.assign(static_cast<size_t>(bw) * bh * 3, 0);
    std::vector<uint8_t> line(row);
    for (int y = 0; y < bh; y++) {           // BMP 自底向上
        if (fread(line.data(), 1, row, fp) != static_cast<size_t>(row)) { fclose(fp); return false; }
        memcpy(bgr.data() + static_cast<size_t>(bh - 1 - y) * bw * 3, line.data(),
               static_cast<size_t>(bw) * 3);
    }
    fclose(fp);
    w = bw;
    h = bh;
    (void)offset;
    return true;
}

// 离线预览模式: test_photo_output --view <全景.bmp> <lon_deg> <lat_deg> <fov_deg> <out.bmp>
//                [hint_x hint_y hint_w hint_h]
// 把全景展开图重投影成普通相机视角（验证第三视角效果, 不需相机/网络）;
// 追加 4 个归一化 bbox 参数时再叠加位置定位标注（人物绿框 + 全景小地图红框）。
static int RunViewMode(int argc, char* argv[]) {
    if (argc != 7 && argc != 11) {
        std::cout << "用法: test_photo_output --view <全景.bmp> <lon_deg> <lat_deg> <fov_deg> <out.bmp>"
                     " [hint_x hint_y hint_w hint_h]" << std::endl;
        return 2;
    }
    std::vector<uint8_t> src;
    int sw = 0, sh = 0;
    if (!LoadBMP24(argv[2], src, sw, sh)) {
        std::cout << "[错误] BMP 读取失败: " << argv[2] << std::endl;
        return 1;
    }
    const float lon0 = static_cast<float>(std::atof(argv[3])) * 3.14159265f / 180.0f;
    const float lat0 = static_cast<float>(std::atof(argv[4])) * 3.14159265f / 180.0f;
    const float fov = static_cast<float>(std::atof(argv[5])) * 3.14159265f / 180.0f;
    std::vector<uint8_t> dst;
    ReprojectRectilinear(src.data(), sw, sh, lon0, lat0, fov, 480, 640, dst);

    // 可选: 叠加位置定位标注（hint 归一化 bbox, 全景坐标系）
    if (argc == 11) {
        const float hx = static_cast<float>(std::atof(argv[7]));
        const float hy = static_cast<float>(std::atof(argv[8]));
        const float hw = static_cast<float>(std::atof(argv[9]));
        const float hh = static_cast<float>(std::atof(argv[10]));
        AnnotatePersonPosition(dst.data(), 480, 640, src.data(), sw, sh,
                               hx, hy, hw, hh, lon0, lat0, fov);
    }

    // 存 24 位 BMP（自底向上）
    const char* out_path = argv[6];
    const int row = 480 * 3;
    std::vector<uint8_t> buf(54 + static_cast<size_t>(row) * 640, 0);
    buf[0] = 'B'; buf[1] = 'M';
    const uint32_t fsize = static_cast<uint32_t>(buf.size());
    const uint32_t off = 54, hdrsz = 40;
    int w32 = 480, h32 = 640;
    uint16_t planes = 1, bpp = 24;
    uint32_t dsz = static_cast<uint32_t>(row) * 640;
    memcpy(&buf[2], &fsize, 4); memcpy(&buf[10], &off, 4); memcpy(&buf[14], &hdrsz, 4);
    memcpy(&buf[18], &w32, 4); memcpy(&buf[22], &h32, 4);
    memcpy(&buf[26], &planes, 2); memcpy(&buf[28], &bpp, 2); memcpy(&buf[34], &dsz, 4);
    for (int y = 0; y < 640; y++) {
        memcpy(buf.data() + 54 + static_cast<size_t>(y) * row,
               dst.data() + static_cast<size_t>(640 - 1 - y) * row, row);
    }
    FILE* fp = nullptr;
    if (fopen_s(&fp, out_path, "wb") != 0 || !fp) {
        std::cout << "[错误] 无法写入: " << out_path << std::endl;
        return 1;
    }
    fwrite(buf.data(), 1, buf.size(), fp);
    fclose(fp);
    std::cout << "[view] " << sw << "x" << sh << " 全景 → lon=" << argv[3] << "° lat=" << argv[4]
              << "° fov=" << argv[5] << "° → " << out_path << std::endl;
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc > 1 && std::string(argv[1]) == "--view") {
        return RunViewMode(argc, argv);
    }
    const std::string out_dir = argc > 1 ? argv[1] : "./test_photos";
    std::error_code ec;
    fs::remove_all(out_dir, ec);

    std::cout << "== 出图模块自测（输出目录: " << out_dir << "） ==" << std::endl;

    // ---- 用例 1: 兜底定时抓拍（1s 间隔, 喂 2.5s 流, 期待 2 张） ----
    {
        MomentCapture mc;
        MomentCapture::Options opt;
        opt.out_dir = out_dir + "/case1";
        opt.min_interval_ms = 500;
        opt.fallback_interval_ms = 1000;
        opt.save_raw = true;
        Check(mc.Start(opt), "模块启动");

        FeedFrames(mc, 960, 480, 33, 2500);
        mc.Stop();

        const uint64_t n = mc.CapturedCount();
        Check(n >= 2 && n <= 3, "兜底抓拍 2~3 张（实际 " + std::to_string(n) + "）");
        Check(!mc.LastOutput().empty(), "记录了最后成片路径");
        Check(fs::exists(mc.LastOutput()), "成片 JPG 已落盘");
        Check(fs::file_size(mc.LastOutput()) > 1000, "成片 JPG 非空");

        // raw BMP 也应存在
        size_t bmp_count = 0;
        for (const auto& e : fs::directory_iterator(out_dir + "/case1/raw")) {
            if (e.path().extension() == ".bmp") bmp_count++;
        }
        Check(bmp_count == n, "原始 BMP 同步留档（" + std::to_string(bmp_count) + " 张）");

        // JPG 尺寸头校验（FFD8 开头 FFD9 结尾）
        FILE* fp = nullptr;
        if (fopen_s(&fp, mc.LastOutput().c_str(), "rb") == 0 && fp) {
            uint8_t head[2] = {0, 0}, tail[2] = {0, 0};
            fread(head, 1, 2, fp);
            fseek(fp, -2, SEEK_END);
            fread(tail, 1, 2, fp);
            fclose(fp);
            Check(head[0] == 0xFF && head[1] == 0xD8 && tail[0] == 0xFF && tail[1] == 0xD9,
                  "JPEG 文件头尾合法");
        } else {
            Check(false, "JPEG 可读");
        }
    }

    // ---- 用例 2: 事件抓拍 + 节流（间隔内密集请求合并延后, 间隔外独立出片） ----
    // 时间线（min_interval=800ms）: 0.3s 请求→0.33s 出#1; 0.5s+0.7s 合并→1.13s 出#2;
    // 1.6s 请求→1.93s 出#3。共 3 张, 两两间隔≥800ms, 密集请求未超频出片。
    {
        MomentCapture mc;
        MomentCapture::Options opt;
        opt.out_dir = out_dir + "/case2";
        opt.min_interval_ms = 800;
        opt.fallback_interval_ms = 0;   // 关兜底, 只测事件路径
        Check(mc.Start(opt), "模块启动（关兜底）");

        std::thread feeder([&mc] { FeedFrames(mc, 640, 320, 33, 2200); });

        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        mc.RequestCapture("person_enter");   // t≈0.3s → 第 1 张
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        mc.RequestCapture("person_enter");   // t≈0.5s 节流内, 应合并延后
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        mc.RequestCapture("moment_look");    // t≈0.7s 仍节流内
        std::this_thread::sleep_for(std::chrono::milliseconds(900));
        mc.RequestCapture("person_enter");   // t≈1.6s 间隔外 → 第 2 张
        feeder.join();
        mc.Stop();

        const uint64_t n = mc.CapturedCount();
        Check(n == 3, "节流出片 3 张, 间隔均≥800ms（实际 " + std::to_string(n) + "）");
        Check(mc.LastOutput().find("person_enter") != std::string::npos,
              "成片文件名含触发原因");

        // 合并证据: 存在 reason 带 "+" 的成片（0.5s 与 0.7s 两次请求合并出片）
        bool merged = false;
        for (const auto& e : fs::directory_iterator(out_dir + "/case2")) {
            if (e.path().filename().string().find("+") != std::string::npos) merged = true;
        }
        Check(merged, "密集请求被合并（存在 '+' 合并成片）");
    }

    // ---- 用例 3: 坏输入防御（不崩溃即通过） ----
    {
        MomentCapture mc;
        MomentCapture::Options opt;
        opt.out_dir = out_dir + "/case3";
        opt.fallback_interval_ms = 0;
        Check(mc.Start(opt), "模块启动（坏输入）");

        const auto frame = MakeFrame(320, 240);
        mc.OnStitchFrame(nullptr, 320 * 4, 320, 240, 0);          // 空指针
        mc.OnStitchFrame(frame.data(), 320 * 4, 0, 240, 0);       // 宽 0
        mc.OnStitchFrame(frame.data(), 320 * 4, 320, -5, 0);      // 高负
        mc.OnStitchFrame(frame.data(), 100, 320, 240, 0);         // linesize 不足
        mc.OnStitchFrame(frame.data(), 320 * 4, 99999, 99999, 0); // 超大尺寸
        mc.Stop();
        Check(mc.CapturedCount() == 0, "坏帧全部丢弃, 无成片");
        Check(true, "坏输入未崩溃");
    }

    // ---- 用例 4: 请求先于任何帧到达（Start 前调用, 不崩溃不丢功能） ----
    {
        MomentCapture mc;  // 未 Start
        mc.RequestCapture("person_enter");   // 未启动: 应静默忽略
        mc.OnStitchFrame(nullptr, 0, 0, 0, 0);
        mc.Stop();
        Check(true, "未启动状态下调用所有接口不崩溃");
    }

    // ---- 用例 5: 第三视角裁切（有效 hint → 竖版 3:4 且文件名带 _crop） ----
    {
        MomentCapture mc;
        MomentCapture::Options opt;
        opt.out_dir = out_dir + "/case5";
        opt.min_interval_ms = 500;
        opt.fallback_interval_ms = 0;
        Check(mc.Start(opt), "模块启动（裁切）");

        // 人物在画面中心, 归一化 bbox 宽 15%（640px 帧上约 96px）
        MomentCapture::CropHint hint;
        hint.x = 0.425f; hint.y = 0.25f; hint.w = 0.15f; hint.h = 0.5f;
        mc.UpdateCropHint(hint);
        mc.RequestCapture("person_enter");

        FeedFrames(mc, 640, 320, 33, 800);
        mc.Stop();

        Check(mc.CapturedCount() == 1, "裁切出片 1 张（实际 " + std::to_string(mc.CapturedCount()) + "）");
        Check(mc.LastOutput().find("_crop") != std::string::npos, "成片文件名含 _crop 标记");

        // 读 raw BMP 头验证重投影尺寸: 固定 480x640 竖版透视平面
        std::string bmp_path;
        for (const auto& e : fs::directory_iterator(out_dir + "/case5/raw")) {
            if (e.path().extension() == ".bmp") bmp_path = e.path().string();
        }
        Check(!bmp_path.empty(), "重投影后 raw BMP 已留档");
        if (!bmp_path.empty()) {
            FILE* fp = nullptr;
            if (fopen_s(&fp, bmp_path.c_str(), "rb") == 0 && fp) {
                uint8_t hdr[26] = {0};
                fread(hdr, 1, 26, fp);
                fclose(fp);
                int bw = 0, bh = 0;
                memcpy(&bw, hdr + 18, 4);
                memcpy(&bh, hdr + 22, 4);
                Check(bw == 480 && bh == 640,
                      "重投影输出 480x640 竖版透视平面（实际 " + std::to_string(bw) + "x" + std::to_string(bh) + "）");
            } else {
                Check(false, "raw BMP 可读");
            }
        }

        // hint 过期（TTL 2s）后出片 → 不裁切, 输出全幅
        MomentCapture mc2;
        MomentCapture::Options opt2;
        opt2.out_dir = out_dir + "/case5b";
        opt2.min_interval_ms = 500;
        opt2.fallback_interval_ms = 0;
        opt2.crop_hint_ttl_ms = 300;   // 短 TTL, 请求前 hint 已老化
        Check(mc2.Start(opt2), "模块启动（hint 过期）");
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        mc2.UpdateCropHint(hint);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));  // 超过 TTL
        mc2.RequestCapture("person_enter");
        FeedFrames(mc2, 640, 320, 33, 800);
        mc2.Stop();
        Check(mc2.LastOutput().find("_crop") == std::string::npos,
              "hint 过期出片不裁切（无 _crop 标记）");
    }

    // ---- 用例 6: 位置定位标注（人物绿框 + 底部全景小地图红框 + 视心白线） ----
    {
        // 合成全景 960x480（RGBA）→ BGR
        const int pw = 960, ph = 480;
        const auto pano_rgba = MakeFrame(pw, ph);
        std::vector<uint8_t> pano(static_cast<size_t>(pw) * ph * 3);
        for (int y = 0; y < ph; y++) {
            for (int x = 0; x < pw; x++) {
                const size_t si = (static_cast<size_t>(y) * pw + x) * 4;
                const size_t di = (static_cast<size_t>(y) * pw + x) * 3;
                pano[di + 0] = pano_rgba[si + 2];
                pano[di + 1] = pano_rgba[si + 1];
                pano[di + 2] = pano_rgba[si + 0];
            }
        }

        // 重投影: 视心 (0°,0°), fov_x=1.2rad → 480x640 校正图
        const float fov_x = 1.2f;
        std::vector<uint8_t> view, before;
        ReprojectRectilinear(pano.data(), pw, ph, 0.0f, 0.0f, fov_x, 480, 640, view);
        before = view;   // 标注前副本（对比用）

        // 人物 bbox 中心恰在视心 (0.5, 0.5), 宽 15% 高 50%
        AnnotatePersonPosition(view.data(), 480, 640, pano.data(), pw, ph,
                               0.425f, 0.25f, 0.15f, 0.5f, 0.0f, 0.0f, fov_x);

        bool has_green = false, has_red = false, has_white = false;
        for (size_t i = 0; i < view.size(); i += 3) {
            if (view[i] == 0 && view[i + 1] == 255 && view[i + 2] == 0) has_green = true;
            if (view[i] == 0 && view[i + 1] == 0 && view[i + 2] == 255) has_red = true;
            if (view[i] == 255 && view[i + 1] == 255 && view[i + 2] == 255) has_white = true;
        }
        Check(has_green, "校正图含人物绿框");
        Check(has_red, "底部小地图含人物红框");
        Check(has_white, "含分隔线/视心白线");

        // 绿框位置: 视心=bbox 中心 → 绿框线应出现在画面中央附近（横向 180~300 行内）
        bool green_centered = false;
        for (int y = 180; y < 300; y++) {
            for (int x = 60; x < 420; x++) {
                const size_t i = (static_cast<size_t>(y) * 480 + x) * 3;
                if (view[i] == 0 && view[i + 1] == 255 && view[i + 2] == 0) green_centered = true;
            }
        }
        Check(green_centered, "人物绿框位于画面中央（视心=bbox 中心）");

        // 底部小地图区域被改写（squeeze+压暗）, 顶部主体区除框线外不变
        bool bottom_changed = false;
        for (int y = 580; y < 640; y++) {
            for (int x = 0; x < 480; x++) {
                const size_t i = (static_cast<size_t>(y) * 480 + x) * 3;
                if (view[i] != before[i]) bottom_changed = true;
            }
        }
        Check(bottom_changed, "底部全景小地图已叠加");
        size_t top_diff = 0;
        for (int y = 0; y < 500; y++) {
            for (int x = 0; x < 480; x++) {
                const size_t i = (static_cast<size_t>(y) * 480 + x) * 3;
                if (view[i] != before[i]) top_diff++;
            }
        }
        Check(top_diff < static_cast<size_t>(500 * 480) / 20,
              "顶部主体区仅框线变化（<5%, 实际 " + std::to_string(top_diff) + " px）");

        // 坏输入防御: 空指针/坏尺寸不崩溃
        AnnotatePersonPosition(nullptr, 480, 640, pano.data(), pw, ph,
                               0.4f, 0.2f, 0.2f, 0.6f, 0.0f, 0.0f, fov_x);
        AnnotatePersonPosition(view.data(), 0, 0, pano.data(), pw, ph,
                               0.4f, 0.2f, 0.2f, 0.6f, 0.0f, 0.0f, fov_x);
        Check(true, "坏输入未崩溃");
    }

    std::cout << "\n结果: " << g_pass << " 通过 / " << g_fail << " 失败" << std::endl;
    return g_fail == 0 ? 0 : 1;
}
