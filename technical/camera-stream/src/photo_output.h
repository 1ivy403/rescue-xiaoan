// photo_output.h: 「出图」模块 —— 事件驱动的精彩瞬间抓拍 + 自动美化 → 最终成片 JPG
//
// 数据流:
//   拼接回调线程 OnStitchFrame() 持续喂帧（只做抓拍判定 + 帧拷贝, ~1ms）
//   任意线程 RequestCapture(reason) 打抓拍请求（RDK 事件线程 / 主线程）
//   内部 worker 线程: RGBA→BGR → 自动美化 → WIC JPEG → photos/ 落盘
//
// 美化流水线（纯 CPU, 无第三方依赖, 960x480 单帧 < 10ms）:
//   1) 自动色阶: 每通道直方图 0.5% 百分位线性拉伸（去灰蒙蒙）
//   2) 饱和度:   亮度不变前提下 +15% 饱和（更通透）
//   3) 锐化:     3x3 盒模糊 unsharp mask（边缘增强）
//
// 第三视角重投影: RDK 检测到人后把 bbox（归一化）通过 UpdateCropHint() 持续喂入;
// 出片时若 hint 在有效期内, 以人物方向为视心把全景球面重投影成 480x640 透视平面
// （普通相机视角, 消除球面弯曲）, 文件名追加 "_crop"; 无有效 hint 则输出整幅全景。
// 校正图可再用 AnnotatePersonPosition() 叠加「位置定位」: 人物绿框 + 底部全景小地图红框。
//
// 兜底: 长时间无事件时按 fallback_interval 自动抓拍——演示中 RDK 断连也保证有产出。
//
// 输出:
//   <out_dir>/moment_<序号>_ts<时间戳>_<原因>.jpg   美化后成片
//   <out_dir>/raw/moment_<序号>_ts<时间戳>.bmp      原始帧（路演前后对比展示）

#pragma once

#include <cstdint>
#include <string>
#include <vector>

// 等距柱状全景(BGR24) → 透视平面(BGR24) 重投影（第三视角出片的核心变换, 供测试工具复用）
void ReprojectRectilinear(const uint8_t* src_bgr24, int sw, int sh,
                          float lon0_rad, float lat0_rad, float fov_x_rad,
                          int dw, int dh, std::vector<uint8_t>& dst_bgr24);

// 位置定位标注（就地修改重投影校正图 BGR24）:
//   1) 人物绿框: bbox 中心/角跨度按针孔模型换算到校正平面（视心=bbox 中心时居中）
//   2) 底部全景小地图: 全景 squeeze 成窄条并压暗, 红框标记人物方位, 白竖线标记当前视线中心
// pano_bgr 为原始全景帧（供小地图）; hint_* 为归一化 bbox（全景坐标系）; lon0/lat0/fov_x
// 必须与生成该校正图时传给 ReprojectRectilinear 的参数一致。
void AnnotatePersonPosition(uint8_t* view_bgr, int vw, int vh,
                            const uint8_t* pano_bgr, int pw, int ph,
                            float hint_x, float hint_y, float hint_w, float hint_h,
                            float lon0_rad, float lat0_rad, float fov_x_rad);
class MomentCapture {
public:
    struct Options {
        std::string out_dir = "./photos";     // 成片输出目录
        int min_interval_ms = 3000;           // 两张成片最小间隔（防连拍轰炸）
        int fallback_interval_ms = 10000;     // 无事件兜底抓拍间隔, 0=禁用
        bool save_raw = true;                 // 同时存原始 BMP（对比展示）
        int jpeg_quality = 92;                // 成片 JPEG 质量
        float strength = 1.0f;                // 美化强度 0(关)~1.0(满)
        int crop_hint_ttl_ms = 2000;          // 人物位置提示有效期, 超龄出片不裁切
    };

    // 人物位置提示（归一化 0~1, 全景帧内 bbox xywh; 由 RDK 事件持续刷新）
    struct CropHint {
        float x = 0, y = 0, w = 0, h = 0;
    };

    MomentCapture();
    ~MomentCapture();

    MomentCapture(const MomentCapture&) = delete;
    MomentCapture& operator=(const MomentCapture&) = delete;

    // 启动 worker 线程（失败返回 false, 主流程可继续跑）
    bool Start(const Options& opt);
    void Stop();

    // 拼接回调线程调用: 每帧喂入, 内部完成抓拍判定（快, 不阻塞拼接）
    void OnStitchFrame(const uint8_t* rgba, int linesize, int width, int height, int64_t timestamp);

    // 任意线程调用: 请求抓拍（受 min_interval 节流, 请求不会丢只会延迟）
    void RequestCapture(const std::string& reason);

    // 任意线程调用: 刷新人物位置提示（RDK 事件线程; 供出片时第三视角裁切）
    void UpdateCropHint(const CropHint& hint);

    // 查询: 人物位置提示是否在有效期内（true 时 *out 为最新提示）, 供外部抽帧复用
    bool LatestCropHint(CropHint* out) const;

    // 统计
    uint64_t CapturedCount() const;
    std::string LastOutput() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};
