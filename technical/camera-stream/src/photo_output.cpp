// photo_output.cpp: 见 photo_output.h 头部说明
// JPEG 编码使用 Windows 自带 WIC（无第三方依赖, 与 rdk_stream.cpp 各持一份互不干扰）。

#include "photo_output.h"

// wincodec.h 会引入 windows.h, 其 min/max 宏与 std::min/std::max 冲突（C2589）, 必须先关掉
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <wincodec.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace {
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

constexpr int kMaxDim = 4096;                 // 单边尺寸上限（防异常大帧拖垮 CPU/内存）
constexpr size_t kMaxPixels = 4096u * 4096u;  // 像素总数上限

// 循环覆盖: 目录内仅保留最新 keep 个指定后缀文件, 更旧的删除（防止写满磁盘）
void PruneDirOldest(const std::string& dir, const std::string& ext, size_t keep) {
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
    for (size_t i = 0; i + keep < items.size(); ++i) {
        std::error_code ec2;
        fs::remove(dir + "/" + items[i].second, ec2);
    }
}

int64_t SteadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// ---------------- BGR24 → JPEG（WIC） ----------------
bool EncodeJpegWIC(const uint8_t* bgr, int width, int height, int quality,
                   std::vector<uint8_t>& out) {
    IWICImagingFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return false;

    bool ok = false;
    IStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;

    do {
        if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) break;
        if (FAILED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder))) break;
        if (FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache))) break;
        if (FAILED(encoder->CreateNewFrame(&frame, &props))) break;

        {
            LPOLESTR name = static_cast<LPOLESTR>(CoTaskMemAlloc(32 * sizeof(WCHAR)));
            if (name) {
                wcscpy_s(name, 32, L"ImageQuality");
                PROPBAG2 bag{};
                bag.dwType = PROPBAG2_TYPE_DATA;
                bag.pstrName = name;
                VARIANT v;
                VariantInit(&v);
                v.vt = VT_R4;
                v.fltVal = static_cast<float>(quality) / 100.0f;
                props->Write(1, &bag, &v);
                CoTaskMemFree(name);
            }
        }

        if (FAILED(frame->Initialize(props))) break;
        if (FAILED(frame->SetSize(width, height))) break;

        WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
        if (FAILED(frame->SetPixelFormat(&fmt))) break;

        const UINT stride = static_cast<UINT>(width) * 3;
        if (FAILED(frame->WritePixels(height, stride, stride * height,
                                      const_cast<BYTE*>(bgr)))) break;
        if (FAILED(frame->Commit())) break;
        if (FAILED(encoder->Commit())) break;

        LARGE_INTEGER zero{};
        ULARGE_INTEGER pos{};
        if (FAILED(stream->Seek(zero, STREAM_SEEK_END, &pos))) break;
        if (FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr))) break;
        out.resize(pos.QuadPart);
        ULONG read = 0;
        if (FAILED(stream->Read(out.data(), static_cast<ULONG>(out.size()), &read))) break;
        out.resize(read);
        ok = read > 0;
    } while (false);

    if (props) props->Release();
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    factory->Release();
    return ok;
}

// ---------------- 美化: 1) 自动色阶（每通道直方图百分位拉伸） ----------------
void AutoLevels(uint8_t* bgr, size_t n, float clip_pct) {
    if (n == 0) return;
    uint32_t hist[3][256] = {};
    for (size_t i = 0; i < n; i++) {
        hist[0][bgr[i * 3 + 0]]++;
        hist[1][bgr[i * 3 + 1]]++;
        hist[2][bgr[i * 3 + 2]]++;
    }

    const size_t clip_count = static_cast<size_t>(static_cast<double>(n) * clip_pct);
    uint8_t lut[3][256];
    for (int c = 0; c < 3; c++) {
        int lo = 0, hi = 255;
        size_t acc = 0;
        for (int v = 0; v < 256; v++) {          // 低端: 累计超过 clip_count 处
            acc += hist[c][v];
            if (acc > clip_count) { lo = v; break; }
        }
        acc = 0;
        for (int v = 255; v >= 0; v--) {         // 高端: 同理从高往低
            acc += hist[c][v];
            if (acc > clip_count) { hi = v; break; }
        }
        if (hi - lo < 16) {                       // 对比度本来就够, 不拉伸
            lo = 0;
            hi = 255;
        }
        const float scale = 255.0f / static_cast<float>(hi - lo);
        for (int v = 0; v < 256; v++) {
            const int nv = static_cast<int>((v - lo) * scale + 0.5f);
            lut[c][v] = static_cast<uint8_t>(std::min(255, std::max(0, nv)));
        }
    }
    for (size_t i = 0; i < n; i++) {
        bgr[i * 3 + 0] = lut[0][bgr[i * 3 + 0]];
        bgr[i * 3 + 1] = lut[1][bgr[i * 3 + 1]];
        bgr[i * 3 + 2] = lut[2][bgr[i * 3 + 2]];
    }
}

// ---------------- 美化: 2) 饱和度（亮度恒定） ----------------
void Saturate(uint8_t* bgr, size_t n, float amount) {
    for (size_t i = 0; i < n; i++) {
        const float b = static_cast<float>(bgr[i * 3 + 0]);
        const float g = static_cast<float>(bgr[i * 3 + 1]);
        const float r = static_cast<float>(bgr[i * 3 + 2]);
        const float lum = 0.299f * r + 0.587f * g + 0.114f * b;
        const float nb = lum + (b - lum) * amount;
        const float ng = lum + (g - lum) * amount;
        const float nr = lum + (r - lum) * amount;
        bgr[i * 3 + 0] = static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, nb)));
        bgr[i * 3 + 1] = static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, ng)));
        bgr[i * 3 + 2] = static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, nr)));
    }
}

// ---------------- 美化: 3) 锐化（3x3 盒模糊 unsharp mask） ----------------
void Sharpen(uint8_t* bgr, int w, int h, float amount) {
    if (w < 3 || h < 3) return;
    std::vector<uint8_t> blurred(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; y++) {
        const int y0 = std::max(0, y - 1), y1 = std::min(h - 1, y + 1);
        for (int x = 0; x < w; x++) {
            const int x0 = std::max(0, x - 1), x1 = std::min(w - 1, x + 1);
            int sum[3] = {0, 0, 0};
            for (int yy = y0; yy <= y1; yy++) {
                for (int xx = x0; xx <= x1; xx++) {
                    const size_t idx = (static_cast<size_t>(yy) * w + xx) * 3;
                    sum[0] += bgr[idx + 0];
                    sum[1] += bgr[idx + 1];
                    sum[2] += bgr[idx + 2];
                }
            }
            const size_t dst = (static_cast<size_t>(y) * w + x) * 3;
            blurred[dst + 0] = static_cast<uint8_t>(sum[0] / 9);
            blurred[dst + 1] = static_cast<uint8_t>(sum[1] / 9);
            blurred[dst + 2] = static_cast<uint8_t>(sum[2] / 9);
        }
    }
    for (size_t i = 0; i < static_cast<size_t>(w) * h * 3; i++) {
        const float v = static_cast<float>(bgr[i]) +
                        amount * static_cast<float>(bgr[i] - blurred[i]);
        bgr[i] = static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, v)));
    }
}

// ---------------- BMP 保存（原始帧留档, 与 main.cpp 同款格式） ----------------
bool SaveBMP24(const std::string& path, const uint8_t* bgr, int width, int height) {
    const int row_size = ((width * 3 + 3) / 4) * 4;
    const uint32_t data_size = static_cast<uint32_t>(row_size) * static_cast<uint32_t>(height);
    const uint32_t file_size = 54 + data_size;

    std::vector<uint8_t> buf(file_size, 0);
    buf[0] = 'B'; buf[1] = 'M';
    memcpy(&buf[2], &file_size, 4);
    const uint32_t offset = 54;
    memcpy(&buf[10], &offset, 4);
    const uint32_t hdr_size = 40;
    memcpy(&buf[14], &hdr_size, 4);
    memcpy(&buf[18], &width, 4);
    memcpy(&buf[22], &height, 4);
    const uint16_t planes = 1, bpp = 24;
    memcpy(&buf[26], &planes, 2);
    memcpy(&buf[28], &bpp, 2);
    memcpy(&buf[34], &data_size, 4);

    for (int y = 0; y < height; y++) {
        const uint8_t* src = bgr + static_cast<size_t>(height - 1 - y) * width * 3;
        uint8_t* dst = buf.data() + 54 + static_cast<size_t>(y) * row_size;
        memcpy(dst, src, static_cast<size_t>(width) * 3);
    }

    FILE* fp = nullptr;
    if (fopen_s(&fp, path.c_str(), "wb") != 0 || !fp) return false;
    const bool ok = fwrite(buf.data(), 1, buf.size(), fp) == buf.size();
    fclose(fp);
    return ok;
}

} // namespace

// ---------------- 第三视角: 等距柱状全景 → 透视平面（rectilinear）重投影 ----------------
// 把全景展开图以 (lon0,lat0) 为视线中心重新渲染成普通相机视角, 消除球面弯曲。
// fov_x_rad 决定视场角; 输出固定 dw x dh 竖版。
void ReprojectRectilinear(const uint8_t* src, int sw, int sh,
                          float lon0, float lat0, float fov_x_rad,
                          int dw, int dh, std::vector<uint8_t>& dst) {
    dst.assign(static_cast<size_t>(dw) * dh * 3, 0);
    constexpr float kPi = 3.14159265f;
    const float f = (dw * 0.5f) / std::tan(fov_x_rad * 0.5f);
    const float cx = dw * 0.5f, cy = dh * 0.5f;

    for (int v = 0; v < dh; v++) {
        for (int u = 0; u < dw; u++) {
            // 输出像素 → 视线方向（针孔模型）
            const float dx = u - cx;
            const float dy = cy - v;
            const float dz = f;
            const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
            const float nx = dx / len, ny = dy / len, nz = dz / len;

            const float lon = std::atan2(nx, nz);
            const float lat = std::asin(std::min(1.0f, std::max(-1.0f, ny)));

            // 相对视心的经度差（环形 wrap）与纬度 → 全景像素坐标
            float dlon = lon - lon0;
            while (dlon > kPi) dlon -= 2 * kPi;
            while (dlon < -kPi) dlon += 2 * kPi;
            const float eu = (0.5f + dlon / (2 * kPi)) * sw;
            const float ev = (0.5f - lat / kPi) * sh;

            // 双线性采样（经度环形 wrap, 纬度 clamp）
            const int x0 = static_cast<int>(std::floor(eu));
            const int y0 = static_cast<int>(std::floor(ev));
            const float fx = eu - x0, fy = ev - y0;
            auto wrapx = [&](int x) { x %= sw; return x < 0 ? x + sw : x; };
            const int xa = wrapx(x0), xb = wrapx(x0 + 1);
            const int ya = std::min(std::max(y0, 0), sh - 1);
            const int yb = std::min(std::max(y0 + 1, 0), sh - 1);

            uint8_t* out = dst.data() + (static_cast<size_t>(v) * dw + u) * 3;
            for (int c = 0; c < 3; c++) {
                const float p00 = src[(static_cast<size_t>(ya) * sw + xa) * 3 + c];
                const float p01 = src[(static_cast<size_t>(ya) * sw + xb) * 3 + c];
                const float p10 = src[(static_cast<size_t>(yb) * sw + xa) * 3 + c];
                const float p11 = src[(static_cast<size_t>(yb) * sw + xb) * 3 + c];
                const float val = p00 * (1 - fx) * (1 - fy) + p01 * fx * (1 - fy) +
                                  p10 * (1 - fx) * fy + p11 * fx * fy;
                out[c] = static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, val)));
            }
        }
    }
}

// ---------------- 位置定位标注: 人物绿框 + 底部全景小地图 ----------------
// 绿框: bbox 中心经纬度相对视心的偏差 → 针孔模型像素坐标（视心=bbox 中心时居中）;
//       bbox 角跨度（宽→经度 hint_w*2π, 高→纬度 hint_h*π）× 焦距 ≈ 框尺寸。
// 小地图: 全景最近邻 squeeze 成底部窄条并压暗一半, 红框标人物方位, 白竖线标当前视线
//         中心（二者重合 = 校正图正对人物, 与绿框互为印证）。
void AnnotatePersonPosition(uint8_t* view, int vw, int vh,
                            const uint8_t* pano, int pw, int ph,
                            float hint_x, float hint_y, float hint_w, float hint_h,
                            float lon0, float lat0, float fov_x) {
    if (!view || vw < 16 || vh < 16 || !pano || pw < 16 || ph < 16) return;
    if (hint_w <= 0.0f || hint_h <= 0.0f || fov_x <= 0.01f) return;
    constexpr float kPi = 3.14159265f;

    auto setpx = [&](int x, int y, uint8_t b, uint8_t g, uint8_t r) {
        if (x < 0 || x >= vw || y < 0 || y >= vh) return;
        uint8_t* p = view + (static_cast<size_t>(y) * vw + x) * 3;
        p[0] = b; p[1] = g; p[2] = r;
    };
    auto draw_rect = [&](int x0, int y0, int x1, int y1, int thick,
                         uint8_t b, uint8_t g, uint8_t r) {
        for (int t = 0; t < thick; t++) {
            for (int x = x0; x <= x1; x++) {
                setpx(x, y0 + t, b, g, r);
                setpx(x, y1 - t, b, g, r);
            }
            for (int y = y0; y <= y1; y++) {
                setpx(x0 + t, y, b, g, r);
                setpx(x1 - t, y, b, g, r);
            }
        }
    };

    const int mm_h = std::max(48, vh / 10);   // 底部小地图条高
    const int mm_y = vh - mm_h;

    // ---- 先画小地图（绿框在其上方, 不会被遮挡） ----
    for (int y = 0; y < mm_h; y++) {
        const int sy = std::min(ph - 1, y * ph / mm_h);
        for (int x = 0; x < vw; x++) {
            const int sx = std::min(pw - 1, x * pw / vw);
            const uint8_t* s = pano + (static_cast<size_t>(sy) * pw + sx) * 3;
            uint8_t* d = view + (static_cast<size_t>(mm_y + y) * vw + x) * 3;
            d[0] = s[0] / 2; d[1] = s[1] / 2; d[2] = s[2] / 2;   // 压暗衬托标注
        }
    }
    for (int t = 0; t < 2; t++) {            // 小地图顶部分隔白线
        for (int x = 0; x < vw; x++) setpx(x, mm_y + t, 255, 255, 255);
    }
    // 人物红框: 中心 = 归一化 bbox 中心（小地图横坐标与全景像素 x 同向）
    const float cxn = hint_x + hint_w * 0.5f;
    const float rw = std::max(hint_w * static_cast<float>(vw), 12.0f);
    draw_rect(static_cast<int>(cxn * vw - rw * 0.5f), mm_y + 4,
              static_cast<int>(cxn * vw + rw * 0.5f), vh - 5, 2, 0, 0, 255);  // BGR 红
    // 视心白竖线: lon0 → 全景像素比例（与 ReprojectRectilinear 相同的镜像映射）
    const int vx = static_cast<int>((1.0f - lon0 / kPi) * vw * 0.5f);
    for (int y = mm_y + 2; y < vh - 3; y++) {
        setpx(vx, y, 255, 255, 255);
        setpx(vx + 1, y, 255, 255, 255);
    }

    // ---- 人物绿框（画在小地图之上结束, 不越过分隔线） ----
    const float cyn = hint_y + hint_h * 0.5f;
    const float lon_p = (1.0f - 2.0f * cxn) * kPi;    // 全景像素 x 与经度方向相反（镜像）
    const float lat_p = (0.5f - cyn) * kPi;
    float dlon = lon_p - lon0;
    while (dlon > kPi) dlon -= 2 * kPi;
    while (dlon < -kPi) dlon += 2 * kPi;
    dlon = std::min(1.2f, std::max(-1.2f, dlon));     // 防 tan 爆炸
    const float dlat = std::min(1.2f, std::max(-1.2f, lat_p - lat0));

    const float f = (vw * 0.5f) / std::tan(fov_x * 0.5f);
    const float cu = vw * 0.5f + f * std::tan(dlon);
    const float cv = vh * 0.5f - f * std::tan(dlat);
    float bw = f * hint_w * 2 * kPi;                  // 中心处 tan θ ≈ θ
    float bh = f * hint_h * kPi;
    bw = std::min(static_cast<float>(vw) * 0.96f, std::max(12.0f, bw));
    bh = std::min(static_cast<float>(vh) * 0.96f, std::max(12.0f, bh));
    const int gy1 = std::min(static_cast<int>(cv + bh * 0.5f), mm_y - 3);
    draw_rect(static_cast<int>(cu - bw * 0.5f), static_cast<int>(cv - bh * 0.5f),
              static_cast<int>(cu + bw * 0.5f), gy1, 3, 0, 255, 0);   // BGR 绿
}

struct MomentCapture::Impl {
    Options opt;
    std::atomic<bool> running{false};
    std::thread worker;

    // 抓拍请求（任意线程写, 拼接回调线程消费）
    std::atomic<bool> request_pending{false};
    std::mutex reason_mutex;
    std::string request_reason;
    MomentCapture::CropHint crop_hint;   // 人物位置提示（同锁保护）
    int64_t crop_hint_ms = 0;            // 提示写入时刻（SteadyMs）

    // 帧交接（拼接回调线程 → worker, 新帧覆盖旧帧）
    std::mutex handoff_mutex;
    std::condition_variable handoff_cv;
    std::vector<uint8_t> frame_bgr;
    int frame_w = 0;
    int frame_h = 0;
    uint64_t frame_ts = 0;
    std::string frame_reason;
    bool has_frame = false;

    // 抓拍节流（仅拼接回调线程读写 last_capture_ms）
    int64_t start_ms = 0;
    int64_t last_capture_ms = 0;   // 0 = 尚未抓拍过

    // 统计
    std::atomic<uint64_t> captured{0};
    std::mutex log_mutex;
    std::string last_output;

    // ---------------- worker: 取帧 → 美化 → 编码 → 落盘 ----------------
    void WorkerLoop() {
        const bool co_new = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        uint64_t seq = 0;

        while (running.load()) {
            std::vector<uint8_t> bgr;
            int w = 0, h = 0;
            uint64_t ts = 0;
            std::string reason;

            {
                std::unique_lock<std::mutex> lk(handoff_mutex);
                handoff_cv.wait(lk, [this] { return has_frame || !running.load(); });
                if (!running.load()) break;
                bgr = std::move(frame_bgr);
                w = frame_w;
                h = frame_h;
                ts = frame_ts;
                reason = std::move(frame_reason);
                has_frame = false;
            }
            if (bgr.empty() || w <= 0 || h <= 0) continue;

            // ---- 第三视角重投影: hint 有效期内 → 以人物方向为视心渲染透视平面 ----
            {
                MomentCapture::CropHint hint;
                int64_t hint_ms = 0;
                {
                    std::lock_guard<std::mutex> lk(reason_mutex);
                    hint = crop_hint;
                    hint_ms = crop_hint_ms;
                }
                const bool hint_ok = hint.w > 0.002f && hint.h > 0.002f &&
                                     hint.w < 1.0f && hint.h < 1.0f;
                if (hint_ok && SteadyMs() - hint_ms < opt.crop_hint_ttl_ms) {
                    // 视心 = 人物 bbox 中心在球面上的经纬度
                    // 注意: 全景图像素 x 增大方向与视线 atan2 约定相反, 经度需取镜像
                    const float cxn = hint.x + hint.w * 0.5f;
                    const float cyn = hint.y + hint.h * 0.5f;
                    const float lon0 = (1.0f - 2.0f * cxn) * 3.14159265f;
                    float lat0 = (0.5f - cyn) * 3.14159265f;
                    lat0 = std::min(1.4f, std::max(-1.4f, lat0));  // 避开极点

                    // 视场自适应: 人物高度约占画面 70%（纬向角度与像素线性对应）
                    float fov_y = (hint.h * 3.14159265f) / 0.7f;
                    fov_y = std::min(1.92f, std::max(0.60f, fov_y));   // 34°~110°
                    const float fov_x = 2.0f * std::atan(std::tan(fov_y * 0.5f) * 0.75f);

                    std::vector<uint8_t> reproj;
                    ReprojectRectilinear(bgr.data(), w, h, lon0, lat0, fov_x, 480, 640, reproj);
                    bgr.swap(reproj);
                    w = 480;
                    h = 640;
                    reason += "_crop";
                }
            }

            const size_t n = static_cast<size_t>(w) * h;
            const float s = std::min(1.0f, std::max(0.0f, opt.strength));
            seq++;
            const std::string base = "moment_" + std::to_string(seq) + "_ts" + std::to_string(ts) +
                                     (reason.empty() ? "" : "_" + reason);

            // 原始帧留档（必须在美化前——美化是原地操作）
            if (opt.save_raw) {
                SaveBMP24(opt.out_dir + "/raw/" + base + ".bmp", bgr.data(), w, h);
            }

            // 美化流水线: 色阶 → 饱和 → 锐化（strength=0 时跳过 = 原图直出）
            if (s > 0.0f) {
                AutoLevels(bgr.data(), n, 0.005f);
                Saturate(bgr.data(), n, 1.0f + 0.15f * s);
                Sharpen(bgr.data(), w, h, 0.6f * s);
            }

            const std::string jpg_path = opt.out_dir + "/" + base + ".jpg";

            std::vector<uint8_t> jpeg;
            bool ok = EncodeJpegWIC(bgr.data(), w, h, opt.jpeg_quality, jpeg);
            if (ok) {
                FILE* fp = nullptr;
                if (fopen_s(&fp, jpg_path.c_str(), "wb") != 0 || !fp) {
                    ok = false;
                } else {
                    ok = fwrite(jpeg.data(), 1, jpeg.size(), fp) == jpeg.size();
                    fclose(fp);
                }
            }

            if (ok) {
                captured++;
                {
                    std::lock_guard<std::mutex> lk(log_mutex);
                    last_output = jpg_path;
                }
                std::cout << "[出图] 成片 #" << seq << " (" << reason << ") → "
                          << fs::absolute(jpg_path).string() << std::endl;
                // 循环覆盖: 成片留最近 150 张, 原始帧留最近 40 张
                PruneDirOldest(opt.out_dir, ".jpg", 150);
                if (opt.save_raw) {
                    PruneDirOldest(opt.out_dir + "/raw", ".bmp", 40);
                }
            } else {
                std::cerr << "[出图] 保存失败: " << jpg_path << std::endl;
            }
        }

        if (co_new) CoUninitialize();
    }
};

MomentCapture::MomentCapture() : impl_(new Impl()) {}

MomentCapture::~MomentCapture() {
    if (impl_) {
        Stop();
        delete impl_;
    }
}

bool MomentCapture::Start(const Options& opt_in) {
    if (impl_->running.load()) return true;

    // 参数安全夹取
    Options opt = opt_in;
    if (opt.min_interval_ms < 500) opt.min_interval_ms = 500;
    if (opt.min_interval_ms > 60000) opt.min_interval_ms = 60000;
    if (opt.fallback_interval_ms < 0) opt.fallback_interval_ms = 0;
    if (opt.fallback_interval_ms > 60000) opt.fallback_interval_ms = 60000;
    if (opt.fallback_interval_ms != 0 && opt.fallback_interval_ms < opt.min_interval_ms) {
        opt.fallback_interval_ms = opt.min_interval_ms;
    }
    if (opt.jpeg_quality < 10) opt.jpeg_quality = 10;
    if (opt.jpeg_quality > 100) opt.jpeg_quality = 100;
    if (opt.strength < 0.0f) opt.strength = 0.0f;
    if (opt.strength > 1.0f) opt.strength = 1.0f;
    if (opt.out_dir.empty()) opt.out_dir = "./photos";
    impl_->opt = opt;

    std::error_code ec;
    fs::create_directories(opt.out_dir, ec);
    if (ec) {
        std::cerr << "[出图] 创建输出目录失败: " << opt.out_dir << " (" << ec.message() << ")" << std::endl;
        return false;
    }
    if (opt.save_raw) {
        fs::create_directories(opt.out_dir + "/raw", ec);
    }

    impl_->start_ms = SteadyMs();
    impl_->last_capture_ms = 0;
    impl_->running = true;
    impl_->worker = std::thread([this] { impl_->WorkerLoop(); });

    std::cout << "[出图] 模块已启动 (目录=" << fs::absolute(opt.out_dir).string()
              << ", 最小间隔=" << opt.min_interval_ms << "ms"
              << ", 兜底=" << (opt.fallback_interval_ms > 0 ? std::to_string(opt.fallback_interval_ms) + "ms" : "关")
              << ", 美化强度=" << opt.strength << ")" << std::endl;
    return true;
}

void MomentCapture::Stop() {
    if (!impl_ || !impl_->running.exchange(false)) return;
    impl_->handoff_cv.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
}

void MomentCapture::OnStitchFrame(const uint8_t* rgba, int linesize, int width, int height,
                                  int64_t timestamp) {
    if (!impl_ || !impl_->running.load()) return;

    // 输入防御: 坏尺寸直接丢弃
    if (!rgba || width <= 0 || height <= 0 || width > kMaxDim || height > kMaxDim ||
        static_cast<size_t>(width) * height > kMaxPixels || linesize < width * 4) {
        return;
    }

    const int64_t now = SteadyMs();

    // 兜底定时抓拍: 距上次抓拍（或启动）超过 fallback 间隔 → 自动请求
    if (impl_->opt.fallback_interval_ms > 0) {
        const int64_t base = impl_->last_capture_ms != 0 ? impl_->last_capture_ms : impl_->start_ms;
        if (now - base >= impl_->opt.fallback_interval_ms && !impl_->request_pending.load()) {
            impl_->request_pending = true;
            {
                std::lock_guard<std::mutex> lk(impl_->reason_mutex);
                impl_->request_reason = "timer";
            }
        }
    }

    if (!impl_->request_pending.load()) return;

    // 节流: 距上次抓拍不足最小间隔 → 请求保留, 下一帧再试
    if (impl_->last_capture_ms != 0 &&
        now - impl_->last_capture_ms < impl_->opt.min_interval_ms) {
        return;
    }

    // 消费请求
    impl_->request_pending = false;
    std::string reason;
    {
        std::lock_guard<std::mutex> lk(impl_->reason_mutex);
        reason = std::move(impl_->request_reason);
        impl_->request_reason.clear();
    }
    impl_->last_capture_ms = now;  // 决策时刻即节流锚点

    // RGBA → BGR24 全分辨率拷贝（960x480 ≈ 1.4MB, ~0.5ms）
    {
        std::lock_guard<std::mutex> lk(impl_->handoff_mutex);
        impl_->frame_bgr.resize(static_cast<size_t>(width) * height * 3);
        for (int y = 0; y < height; y++) {
            const uint8_t* src = rgba + static_cast<size_t>(y) * linesize;
            uint8_t* dst = impl_->frame_bgr.data() + static_cast<size_t>(y) * width * 3;
            for (int x = 0; x < width; x++) {
                dst[x * 3 + 0] = src[x * 4 + 2]; // B
                dst[x * 3 + 1] = src[x * 4 + 1]; // G
                dst[x * 3 + 2] = src[x * 4 + 0]; // R
            }
        }
        impl_->frame_w = width;
        impl_->frame_h = height;
        impl_->frame_ts = static_cast<uint64_t>(timestamp);
        impl_->frame_reason = reason;
        impl_->has_frame = true;
    }
    impl_->handoff_cv.notify_one();
}

void MomentCapture::RequestCapture(const std::string& reason) {
    if (!impl_ || !impl_->running.load()) return;
    {
        std::lock_guard<std::mutex> lk(impl_->reason_mutex);
        if (impl_->request_reason.empty()) {
            impl_->request_reason = reason;
        } else {
            impl_->request_reason += "+" + reason;  // 合并等待中的原因
        }
    }
    impl_->request_pending = true;
}

void MomentCapture::UpdateCropHint(const CropHint& hint) {
    if (!impl_) return;
    std::lock_guard<std::mutex> lk(impl_->reason_mutex);
    impl_->crop_hint = hint;
    impl_->crop_hint_ms = SteadyMs();
}

bool MomentCapture::LatestCropHint(CropHint* out) const {
    if (!impl_) return false;
    MomentCapture::CropHint hint;
    int64_t hint_ms = 0;
    {
        std::lock_guard<std::mutex> lk(impl_->reason_mutex);
        hint = impl_->crop_hint;
        hint_ms = impl_->crop_hint_ms;
    }
    const bool ok = hint.w > 0.002f && hint.h > 0.002f && hint.w < 1.0f && hint.h < 1.0f &&
                    SteadyMs() - hint_ms < impl_->opt.crop_hint_ttl_ms;
    if (ok && out) *out = hint;
    return ok;
}

uint64_t MomentCapture::CapturedCount() const {
    return impl_ ? impl_->captured.load() : 0;
}

std::string MomentCapture::LastOutput() const {
    if (!impl_) return {};
    std::lock_guard<std::mutex> lk(impl_->log_mutex);
    return impl_->last_output;
}
