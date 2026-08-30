// rdk_stream.cpp: 见 rdk_stream.h 头部说明
// JPEG 编码使用 Windows 自带 WIC（无第三方依赖）, 网络使用 Winsock2。

#include "rdk_stream.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincodec.h>
#include <objbase.h>

#include <atomic>
#include <cstring>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

// ---------------- 崩溃取证: VectoredHandler 直接写文件（不可被吞） ----------------
static void CrashLogRaw(const char* buf, size_t len) {
    HANDLE f = CreateFileA("E:\\ljx\\hackson\\x4-live-demo\\crash_log.txt",
                           FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, buf, static_cast<DWORD>(len), &written, nullptr);
    CloseHandle(f);
}

static LONG CALLBACK CrashVectoredHandler(PEXCEPTION_POINTERS ep) {
    if (ep && ep->ExceptionRecord &&
        ep->ExceptionRecord->ExceptionCode == 0xC0000005) {
        const auto* er = ep->ExceptionRecord;
        HMODULE mod = nullptr;
        char mod_name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(er->ExceptionAddress), &mod) && mod) {
            GetModuleFileNameA(mod, mod_name, MAX_PATH);
        }
        char buf[512];
        int n = snprintf(buf, sizeof(buf),
                         "[CRASH] AV addr=%p module=%s base_off=0x%llx type=%llu target=0x%llx\n",
                         er->ExceptionAddress, mod_name,
                         mod ? (unsigned long long)((char*)er->ExceptionAddress - (char*)mod) : 0ull,
                         er->NumberParameters >= 1 ? (unsigned long long)er->ExceptionInformation[0] : 0ull,
                         er->NumberParameters >= 2 ? (unsigned long long)er->ExceptionInformation[1] : 0ull);
        if (n > 0) CrashLogRaw(buf, static_cast<size_t>(n));
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

namespace {
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

constexpr char kFrameMagic[4] = { 'I', 'N', 'F', 'R' }; // PC→RDK 帧
constexpr char kEventMagic[4] = { 'I', 'N', 'E', 'V' }; // RDK→PC 事件
constexpr char kAudioMagic[4] = { 'I', 'N', 'A', 'F' }; // PC→RDK X4 麦克风音频
constexpr uint32_t kMaxPayload = 4u * 1024 * 1024;     // 4MB 上限

bool RecvExact(SOCKET fd, void* buf, int len) {
    auto* p = static_cast<char*>(buf);
    int remain = len;
    while (remain > 0) {
        const int n = ::recv(fd, p, remain, 0);
        if (n <= 0) return false;
        p += n;
        remain -= n;
    }
    return true;
}

bool SendAll(SOCKET fd, const void* buf, int len) {
    const auto* p = static_cast<const char*>(buf);
    int remain = len;
    while (remain > 0) {
        const int n = ::send(fd, p, remain, 0);
        if (n <= 0) return false;
        p += n;
        remain -= n;
    }
    return true;
}

// BGR24 → JPEG（WIC, 无第三方依赖）。须在本线程先初始化 COM。
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

        // JPEG 质量
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

        // 从 IStream 读回 JPEG 数据
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

} // namespace

struct RdkStreamSender::Impl {
    Options opt;
    std::atomic<bool> running{ false };

    // TCP
    SOCKET listen_fd = INVALID_SOCKET;
    std::atomic<SOCKET> client_fd{ INVALID_SOCKET };
    std::thread accept_thread;
    std::thread send_thread;

    // 最新帧槽（拼接回调线程写, sender 线程读）
    std::mutex frame_mutex;
    std::vector<uint8_t> frame_bgr;
    uint64_t frame_ts = 0;

    // X4 音频包队列（CameraSDK 回调线程写, sender 线程读; 满则丢最旧, 不阻塞回调）
    std::mutex audio_mutex;
    std::vector<std::vector<uint8_t>> audio_queue;   // 每包: INAF头(16B) + payload
    static constexpr size_t kMaxAudioPackets = 128;  // AAC ~40包/s → 约 3s 缓冲

    // 事件
    std::function<void(const std::string&)> event_cb;
    std::thread recv_thread;
    std::mutex recv_thread_mutex;   // 保护 recv_thread 替换/汇合（AcceptLoop 与 Stop 并发竞态会崩）
    std::mutex event_mutex;
    std::string last_event;

    // 统计
    std::atomic<uint64_t> sent_frames{ 0 };
    std::atomic<uint64_t> recv_events{ 0 };

    // ---------------- accept 循环（单客户端, 新连接顶掉旧连接） ----------------
    void AcceptLoop() {
        while (running.load()) {
            sockaddr_in addr{};
            int addr_len = sizeof(addr);
            SOCKET c = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&addr), &addr_len);
            if (c == INVALID_SOCKET) break; // Stop() 关闭监听 socket
            if (!running.load()) { closesocket(c); break; }

            char ip[64] = { 0 };
            inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
            std::cout << "[RDK推流] RDK 已连入: " << ip << std::endl;

            // 顶掉旧连接
            SOCKET old = client_fd.exchange(c);
            if (old != INVALID_SOCKET) closesocket(old);

            // 为新连接启动接收线程（加锁：与 Stop() 的 join 并发时 std::thread 赋值/汇合竞态会崩）
            {
                std::lock_guard<std::mutex> lk(recv_thread_mutex);
                if (recv_thread.joinable()) recv_thread.join();
                recv_thread = std::thread([this, c] { RecvLoop(c); });
            }
        }
    }

    // ---------------- 事件接收循环 ----------------
    void RecvLoop(SOCKET fd) {
        std::vector<uint8_t> payload;
        while (running.load() && client_fd.load() == fd) {
            uint8_t hdr[8];
            if (!RecvExact(fd, hdr, sizeof(hdr))) break;
            if (memcmp(hdr, kEventMagic, 4) != 0) break; // 协议错位, 断开重来
            uint32_t len = 0;
            memcpy(&len, hdr + 4, 4);
            if (len == 0 || len > kMaxPayload) break;

            payload.resize(len);
            if (!RecvExact(fd, payload.data(), static_cast<int>(len))) break;

            recv_events++;
            {
                std::lock_guard<std::mutex> lk(event_mutex);
                last_event.assign(reinterpret_cast<const char*>(payload.data()), len);
            }
            if (event_cb) {
                event_cb(std::string(reinterpret_cast<const char*>(payload.data()), len));
            }
        }
        // 若自己仍是当前客户端, 摘除自己
        SOCKET expect = fd;
        client_fd.compare_exchange_strong(expect, INVALID_SOCKET);
        closesocket(fd);
        if (running.load()) {
            std::cout << "[RDK推流] RDK 连接断开, 等待重连..." << std::endl;
        }
    }

    // ---------------- 发送循环（降采样后的最新帧槽 → JPEG → 发送） ----------------
    void SendLoop() {
        // WIC 需要 COM（本线程）
        const bool co_new = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));

        const auto interval = std::chrono::milliseconds(1000 / (opt.target_fps > 0 ? opt.target_fps : 1));
        auto next_tick = std::chrono::steady_clock::now() + interval;

        std::vector<uint8_t> bgr;
        std::vector<uint8_t> jpeg;
        uint64_t ts = 0;
        uint64_t tick = 0;

        while (running.load()) {
            {
                std::lock_guard<std::mutex> lk(frame_mutex);
                bgr = frame_bgr; // 拷贝（480x240x3 ≈ 345KB, 足够快）
                ts = frame_ts;
            }
            tick++;

            const SOCKET fd = client_fd.load();
            if (fd != INVALID_SOCKET && !bgr.empty()) {
                jpeg.clear();
                if (EncodeJpegWIC(bgr.data(), opt.width, opt.height, opt.jpeg_quality, jpeg)) {
                    // 发送侧同样遵守 kMaxPayload：异常超大帧直接拒发（防协议错位/内存异常）
                    if (jpeg.size() > kMaxPayload) {
                        std::cerr << "[RDK推流] JPEG 帧异常超大 (" << jpeg.size()
                                  << " 字节), 拒发跳过" << std::endl;
                        continue;
                    }
                    uint8_t hdr[16];
                    memcpy(hdr, kFrameMagic, 4);
                    const uint32_t len = static_cast<uint32_t>(jpeg.size());
                    memcpy(hdr + 4, &len, 4);
                    memcpy(hdr + 8, &ts, 8);
                    if (SendAll(fd, hdr, sizeof(hdr)) && SendAll(fd, jpeg.data(), static_cast<int>(jpeg.size()))) {
                        sent_frames++;
                    } else {
                        std::cerr << "[RDK推流] 发送失败, 断开等待重连" << std::endl;
                        shutdown(fd, SD_BOTH);
                    }
                } else {
                    std::cerr << "[RDK推流] JPEG 编码失败, 跳过本帧" << std::endl;
                }
            }

            // X4 麦克风音频包整队列冲发（丢旧保新, 每包自带 INAF 头）
            {
                std::lock_guard<std::mutex> lk(audio_mutex);
                const SOCKET afd = client_fd.load();
                if (afd != INVALID_SOCKET) {
                    for (const auto& pkt : audio_queue) {
                        if (!SendAll(afd, pkt.data(), static_cast<int>(pkt.size()))) break;
                    }
                }
                audio_queue.clear();
            }

            std::this_thread::sleep_until(next_tick);
            next_tick += interval;
            const auto now = std::chrono::steady_clock::now();
            if (next_tick < now) next_tick = now + interval; // 掉帧后重置节拍
        }

        if (co_new) CoUninitialize();
    }
};RdkStreamSender::RdkStreamSender() : impl_(new Impl()) {}

RdkStreamSender::~RdkStreamSender() {
    if (impl_) {
        Stop();
        delete impl_;
    }
}

bool RdkStreamSender::Start(const Options& opt_in) {
    if (impl_->running.load()) return true;
    AddVectoredExceptionHandler(1, CrashVectoredHandler);

    // 参数安全夹取（防其他调用方构造坏 Options：0/负值尺寸会除零, fps≤0 卡死）
    Options opt = opt_in;
    if (opt.target_fps < 1) opt.target_fps = 1;
    if (opt.target_fps > 30) opt.target_fps = 30;
    if (opt.jpeg_quality < 10) opt.jpeg_quality = 10;
    if (opt.jpeg_quality > 100) opt.jpeg_quality = 100;
    if (opt.width < 64 || opt.width > 4096) opt.width = 480;
    if (opt.height < 64 || opt.height > 2048) opt.height = 240;
    if (opt.port == 0) opt.port = 9999;
    impl_->opt = opt;

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "[RDK推流] WSAStartup 失败" << std::endl;
        return false;
    }

    impl_->listen_fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (impl_->listen_fd == INVALID_SOCKET) {
        std::cerr << "[RDK推流] 创建 socket 失败" << std::endl;
        return false;
    }

    // 允许演示中快速重启
    BOOL reuse = TRUE;
    setsockopt(impl_->listen_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(opt.port);
    if (::bind(impl_->listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        ::listen(impl_->listen_fd, 1) == SOCKET_ERROR) {
        std::cerr << "[RDK推流] bind/listen 失败 (port=" << opt.port << ", err=" << WSAGetLastError() << ")" << std::endl;
        closesocket(impl_->listen_fd);
        impl_->listen_fd = INVALID_SOCKET;
        return false;
    }

    impl_->running = true;
    impl_->accept_thread = std::thread([this] { impl_->AcceptLoop(); });
    impl_->send_thread = std::thread([this] { impl_->SendLoop(); });

    std::cout << "[RDK推流] 已监听 0.0.0.0:" << opt.port
              << " (等待 RDK 连入, 目标 " << opt.target_fps << "fps, "
              << opt.width << "x" << opt.height << ")" << std::endl;
    return true;
}

void RdkStreamSender::Stop() {
    if (!impl_ || !impl_->running.exchange(false)) return;

    // 先关 socket 解除阻塞
    if (impl_->listen_fd != INVALID_SOCKET) {
        closesocket(impl_->listen_fd);
        impl_->listen_fd = INVALID_SOCKET;
    }
    if (impl_->client_fd.load() != INVALID_SOCKET) {
        shutdown(impl_->client_fd.load(), SD_BOTH);
    }

    if (impl_->accept_thread.joinable()) impl_->accept_thread.join();
    {
        std::lock_guard<std::mutex> lk(impl_->recv_thread_mutex);
        if (impl_->recv_thread.joinable()) impl_->recv_thread.join();
    }
    if (impl_->send_thread.joinable()) impl_->send_thread.join();

    if (impl_->client_fd.load() != INVALID_SOCKET) {
        closesocket(impl_->client_fd.exchange(INVALID_SOCKET));
    }
    WSACleanup();
}

void RdkStreamSender::OnFrame(const uint8_t* rgba, int linesize, int width, int height,
                              int64_t timestamp) {
    if (!impl_ || !impl_->running.load()) return;

    // 输入防御：坏尺寸（0/负/超界）直接丢弃本帧，不触碰内存（除零/越界）
    const int dw = impl_->opt.width;
    const int dh = impl_->opt.height;
    if (!rgba || width <= 0 || height <= 0 || linesize < width * 4 ||
        dw <= 0 || dh <= 0 || dw > 4096 || dh > 2048) {
        return;
    }

    // 最近邻降采样 RGBA → BGR24（拼接回调线程调用, 必须快: 480x240 ≈ 0.3ms）
    std::lock_guard<std::mutex> lk(impl_->frame_mutex);
    impl_->frame_bgr.resize(static_cast<size_t>(dw) * dh * 3);
    for (int y = 0; y < dh; y++) {
        const int sy = y * height / dh;
        const uint8_t* src = rgba + static_cast<size_t>(sy) * linesize;
        uint8_t* dst = impl_->frame_bgr.data() + static_cast<size_t>(y) * dw * 3;
        for (int x = 0; x < dw; x++) {
            const int sx = x * width / dw;
            dst[x * 3 + 0] = src[sx * 4 + 2]; // B
            dst[x * 3 + 1] = src[sx * 4 + 1]; // G
            dst[x * 3 + 2] = src[sx * 4 + 0]; // R
        }
    }
    impl_->frame_ts = static_cast<uint64_t>(timestamp);
}

void RdkStreamSender::OnAudio(const uint8_t* data, size_t size, int64_t timestamp) {
    if (!impl_ || !impl_->running.load()) return;
    if (!data || size == 0 || size > kMaxPayload) return;

    // 打包 INAF | u32 len | u64 ts | payload，入队（满则丢最旧, 不阻塞 CameraSDK 回调线程）
    std::vector<uint8_t> pkt(16 + size);
    memcpy(pkt.data(), kAudioMagic, 4);
    const uint32_t len = static_cast<uint32_t>(size);
    memcpy(pkt.data() + 4, &len, 4);
    memcpy(pkt.data() + 8, &timestamp, 8);
    memcpy(pkt.data() + 16, data, size);

    std::lock_guard<std::mutex> lk(impl_->audio_mutex);
    if (impl_->audio_queue.size() >= Impl::kMaxAudioPackets) {
        impl_->audio_queue.erase(impl_->audio_queue.begin());
    }
    impl_->audio_queue.push_back(std::move(pkt));
}

void RdkStreamSender::SetEventCallback(std::function<void(const std::string& json)> cb) {
    if (impl_) impl_->event_cb = std::move(cb);
}

bool RdkStreamSender::ClientConnected() const {
    return impl_ && impl_->client_fd.load() != INVALID_SOCKET;
}

uint64_t RdkStreamSender::SentFrames() const {
    return impl_ ? impl_->sent_frames.load() : 0;
}

uint64_t RdkStreamSender::ReceivedEvents() const {
    return impl_ ? impl_->recv_events.load() : 0;
}

std::string RdkStreamSender::LastEvent() const {
    if (!impl_) return {};
    std::lock_guard<std::mutex> lk(impl_->event_mutex);
    return impl_->last_event;
}
