#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
「第三视角的她」感知节点 —— RDK X5 端
====================================================
链路: PC(192.168.50.1, 拼接全景 480x240 JPEG) --TCP--> RDK X5 (本脚本)
本脚本: JPEG 解码 → YOLOv8n-Pose BPU 推理 → 人数/骨架检测 → 事件 JSON 回传 PC

协议（小端, 同一条 TCP 连接全双工）:
  PC→RDK 帧  : b"INFR" | u32 payload_len | u64 ts_ms | JPEG
  RDK→PC 事件: b"INEV" | u32 payload_len | JSON

事件类型（D0 链路验证 + 人数事件; D1 加动作规则引擎: 驻足/回眸/奔跑/静止超时）:
  stats          1Hz 心跳: 人数 / 推理耗时 / fps
  person_enter / person_leave

模型: yolov8n_pose_bayese_640x640_nv12.bin（RDK Model Zoo 直链已实测可用）
  输入: NV12 640x640（forward 传 (960,640) uint8 NV12）
  输出: 9 张量 = 3 尺度(80/40/20) × [score(1) | bbox_dfl(64) | kpt(51)]
  解码: 标准 Ultralytics v8 锚点式 —— DFL softmax 期望 × stride;
        kpt = (raw*2-0.5+grid+0.5)*stride, conf=sigmoid

依赖（X5 出厂自带）: hobot_dnn, opencv-python, numpy

用法:
  python3 yolo_pose_client.py                                  # 常规收流
  python3 yolo_pose_client.py --test-image /root/frame.bmp     # 本地图片自检（验证解码）
  python3 yolo_pose_client.py --probe                          # 打印模型输入/输出
  python3 yolo_pose_client.py --no-model                       # 不加载模型, 仅链路验证
  python3 yolo_pose_client.py --pc-ip 192.168.50.1 --port 9999 --score-thr 0.35
"""

import argparse
import json
import socket
import struct
import sys
import time

import cv2
import numpy as np

try:
    from hobot_dnn import pyeasy_dnn as dnn
except ImportError:
    dnn = None  # --no-model 模式仍可跑链路验证

MAGIC_FRAME = b"INFR"
MAGIC_EVENT = b"INEV"
INPUT_SIZE = 640


# ---------------------------------------------------------------- 网络原语
def recv_exact(sock, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf.extend(chunk)
    return bytes(buf)


def recv_frame(sock):
    """返回 (ts_ms, jpeg_bytes); 断线返回 (None, None); 协议错抛 ConnectionError"""
    hdr = recv_exact(sock, 16)
    if hdr is None:
        return None, None
    magic, length, ts = struct.unpack("<4sIQ", hdr)
    if magic != MAGIC_FRAME:
        raise ConnectionError("帧头错误: %r" % magic)
    if length == 0 or length > 4 * 1024 * 1024:
        raise ConnectionError("帧长度异常: %d" % length)
    payload = recv_exact(sock, length)
    if payload is None:
        return None, None
    return ts, payload


def send_event(sock, obj):
    payload = json.dumps(obj, ensure_ascii=False).encode("utf-8")
    sock.sendall(MAGIC_EVENT + struct.pack("<I", len(payload)) + payload)


def _slim_list(persons):
    """逐人压缩(供前端实时目标队列): 仅保留 bbox/score, 省略 17 个关键点"""
    return [{"bbox": [round(v, 1) for v in p["bbox"]],
             "score": round(float(p["score"]), 3)} for p in persons]


# ---------------------------------------------------------------- 图像工具
def bgr_to_nv12(img_bgr):
    """BGR(HxWx3) → NV12(H*1.5xW) uint8, BPU forward 直接吃这个"""
    h, w = img_bgr.shape[:2]
    yuv = cv2.cvtColor(img_bgr, cv2.COLOR_BGR2YUV_I420)
    y = yuv[:h]
    u = yuv[h:h + h // 4].reshape(h // 2, w // 2)
    v = yuv[h + h // 4:].reshape(h // 2, w // 2)
    uv = np.empty((h // 2, w), dtype=np.uint8)
    uv[:, 0::2] = u
    uv[:, 1::2] = v
    return np.vstack([y.reshape(h, w), uv])


# ---------------------------------------------------------------- 推理器
class YoloPoseDetector:
    STRIDES = (8, 16, 32)

    def __init__(self, model_path, score_thr=0.35, iou_thr=0.5):
        if dnn is None:
            raise RuntimeError("未找到 hobot_dnn（须在 RDK X5 上运行）")
        self.model = dnn.load(model_path)[0]
        self.score_thr = score_thr
        self.iou_thr = iou_thr
        self._sig_score = None   # score 是否需要额外 sigmoid（运行时自检）
        self._sig_kconf = None  # kpt conf 是否需要 sigmoid（运行时自检）
        print("[模型] 已加载: %s, 输入 NV12 %dx%d" % (model_path, INPUT_SIZE, INPUT_SIZE))

    @staticmethod
    def _np(out):
        return np.asarray(out.buffer) if hasattr(out, "buffer") else np.asarray(out)

    def probe(self):
        print("[probe] 输入: shape=%s dtype=%s" %
              (self.model.inputs[0].properties.shape, self.model.inputs[0].properties.dtype))
        for i, o in enumerate(self.model.outputs):
            print("[probe] output[%d]: shape=%s dtype=%s" %
                  (i, o.properties.shape, o.properties.dtype))

    # ---- 前向: 任意尺寸 BGR → 9 输出 ----
    def forward_bgr(self, img_bgr):
        resized = cv2.resize(img_bgr, (INPUT_SIZE, INPUT_SIZE))
        nv12 = bgr_to_nv12(resized)
        return self.model.forward(nv12)

    # ---- 后处理: 9 输出 → [(bbox_xywh, score, kpts17x3)] 原图坐标 ----
    def postprocess(self, outputs, orig_w, orig_h):
        all_boxes, all_scores, all_kpts = [], [], []
        for si, stride in enumerate(self.STRIDES):
            sc = self._np(outputs[3 * si]).squeeze()      # (S,S)
            bb = self._np(outputs[3 * si + 1]).squeeze()  # (S,S,64)
            kk = self._np(outputs[3 * si + 2]).squeeze()  # (S,S,51)
            S = int(sc.shape[0])

            # 锚点（像素坐标, 中心对齐）
            gv = np.arange(S, dtype=np.float32) + 0.5
            ys, xs = np.meshgrid(gv, gv, indexing="ij")   # (S,S) grid 单位
            ax, ay = xs * stride, ys * stride             # 锚点像素坐标

            # score: 原始 logit（实测为负值）, 无条件 sigmoid
            sc = 1.0 / (1.0 + np.exp(-sc))

            # bbox DFL: (S,S,64)→(S,S,4,16) softmax 期望 → ltrb 距离(×stride)
            d = bb.reshape(S, S, 4, 16).astype(np.float32)
            d = np.exp(d - d.max(axis=-1, keepdims=True))
            d /= d.sum(axis=-1, keepdims=True)
            dist = d @ np.arange(16, dtype=np.float32) * stride  # (S,S,4)
            x1 = (ax - dist[..., 0]).ravel()
            y1 = (ay - dist[..., 1]).ravel()
            x2 = (ax + dist[..., 2]).ravel()
            y2 = (ay + dist[..., 3]).ravel()

            # kpt: (S,S,17,3), ultralytics 解码
            k = kk.reshape(S, S, 17, 3).astype(np.float32)
            kx = ((k[..., 0] * 2.0 - 0.5) + xs[..., None]) * stride
            ky = ((k[..., 1] * 2.0 - 0.5) + ys[..., None]) * stride
            kc = k[..., 2]
            if self._sig_kconf is None:
                # conf 若为 logit（值域超 [-1,1]）则补 sigmoid; 已是概率则跳过
                self._sig_kconf = kc.max() > 1.0 or kc.min() < -1.0
            if self._sig_kconf:
                kc = 1.0 / (1.0 + np.exp(-kc))
            kpts = np.stack([kx, ky, kc], axis=-1).reshape(-1, 17, 3)

            all_boxes.append(np.stack([x1, y1, x2, y2], axis=-1).reshape(-1, 4))
            all_scores.append(sc.ravel())
            all_kpts.append(kpts)

        boxes = np.concatenate(all_boxes)     # (N,4) xyxy 输入像素
        scores = np.concatenate(all_scores)   # (N,)
        kpts = np.concatenate(all_kpts)        # (N,17,3)

        keep = scores > self.score_thr
        if not np.any(keep):
            return []
        boxes, scores, kpts = boxes[keep], scores[keep], kpts[keep]

        idx = self._nms(boxes, scores)
        sx, sy = orig_w / INPUT_SIZE, orig_h / INPUT_SIZE
        results = []
        for i in idx:
            x1, y1, x2, y2 = boxes[i]
            results.append({
                "bbox": [float(x1 * sx), float(y1 * sy), float((x2 - x1) * sx), float((y2 - y1) * sy)],
                "score": float(scores[i]),
                "kpts": [[float(kpts[i][j][0] * sx), float(kpts[i][j][1] * sy), float(kpts[i][j][2])]
                         for j in range(17)],
            })
        return results

    @staticmethod
    def _nms(boxes_xyxy, scores):
        x1, y1, x2, y2 = boxes_xyxy[:, 0], boxes_xyxy[:, 1], boxes_xyxy[:, 2], boxes_xyxy[:, 3]
        areas = np.maximum(0, x2 - x1) * np.maximum(0, y2 - y1)
        order = scores.argsort()[::-1]
        keep = []
        while order.size > 0:
            i = order[0]
            keep.append(i)
            xx1 = np.maximum(x1[i], x1[order[1:]])
            yy1 = np.maximum(y1[i], y1[order[1:]])
            xx2 = np.minimum(x2[i], x2[order[1:]])
            yy2 = np.minimum(y2[i], y2[order[1:]])
            inter = np.maximum(0, xx2 - xx1) * np.maximum(0, yy2 - yy1)
            iou = inter / (areas[i] + areas[order[1:]] - inter + 1e-9)
            order = order[1:][iou <= 0.5]
        return keep


KPT_PAIRS = [(5, 6), (5, 7), (7, 9), (6, 8), (8, 10), (5, 11), (6, 12),
             (11, 12), (11, 13), (13, 15), (12, 14), (14, 16)]


def draw_persons(img, persons):
    vis = img.copy()
    for p in persons:
        x, y, w, h = p["bbox"]
        cv2.rectangle(vis, (int(x), int(y)), (int(x + w), int(y + h)), (0, 255, 0), 2)
        cv2.putText(vis, "%.2f" % p["score"], (int(x), int(y) - 4),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 255, 0), 1)
        for a, b in KPT_PAIRS:
            ka, kb = p["kpts"][a], p["kpts"][b]
            if ka[2] > 0.3 and kb[2] > 0.3:
                cv2.line(vis, (int(ka[0]), int(ka[1])), (int(kb[0]), int(kb[1])), (255, 0, 0), 2)
        for kp in p["kpts"]:
            if kp[2] > 0.3:
                cv2.circle(vis, (int(kp[0]), int(kp[1])), 3, (0, 0, 255), -1)
    return vis


# ---------------------------------------------------------------- 自检模式
def run_test_image(args, detector):
    img = cv2.imread(args.test_image)
    if img is None:
        print("[错误] 读不到图片: %s" % args.test_image)
        return 1
    h, w = img.shape[:2]
    print("[test] 图片 %dx%d" % (w, h))

    t0 = time.time()
    outputs = detector.forward_bgr(img)
    infer_ms = (time.time() - t0) * 1000

    persons = detector.postprocess(outputs, w, h)

    # 诊断: 阈值前的 top 分数（判断解码/阈值是否合理）
    raw_scores = []
    for si in range(3):
        raw_scores.extend(detector._np(outputs[3 * si]).squeeze().ravel().tolist())
    top = sorted(raw_scores, reverse=True)[:5]
    print("[test][诊断] 锚点 top5 logit: %s → sigmoid: %s（阈值 %.2f）"
          % ([round(v, 2) for v in top],
             [round(1.0 / (1.0 + pow(2.718281828, -v)), 3) for v in top],
             detector.score_thr))

    print("[test] 推理 %.1f ms | 检出 %d 人" % (infer_ms, len(persons)))
    for i, p in enumerate(persons):
        print("  person[%d] score=%.3f bbox=%s" % (i, p["score"],
              [round(v, 1) for v in p["bbox"]]))
        print("    鼻子=%s 肩L=%s 肩R=%s 髋L=%s" % (
            [round(v, 1) for v in p["kpts"][0]],
            [round(v, 1) for v in p["kpts"][5]],
            [round(v, 1) for v in p["kpts"][6]],
            [round(v, 1) for v in p["kpts"][11]]))

    out_path = "/root/test_out.jpg"
    cv2.imwrite(out_path, draw_persons(img, persons))
    print("[test] 标注图已保存: %s（拷回 PC 人工确认骨架是否合理）" % out_path)
    return 0


# ---------------------------------------------------------------- 主循环
def run_loop(sock, args, detector):
    prev_persons = -1
    frames = 0
    infer_ms_acc = 0.0
    infer_cnt = 0
    last_stat = time.time()
    last_frame_t = time.time()

    while True:
        ts, jpeg = recv_frame(sock)
        if jpeg is None:
            raise ConnectionError("PC 断开")
        img = cv2.imdecode(np.frombuffer(jpeg, np.uint8), cv2.IMREAD_COLOR)
        if img is None:
            print("[warn] JPEG 解码失败, 丢帧")
            continue
        h, w = img.shape[:2]
        now = time.time()

        if detector is not None:
            t0 = time.time()
            outputs = detector.forward_bgr(img)
            persons = detector.postprocess(outputs, w, h)
            infer_ms_acc += (time.time() - t0) * 1000
            infer_cnt += 1
            n = len(persons)
            cur_best = max(persons, key=lambda p: p["score"]) if persons else None
            if n != prev_persons and prev_persons >= 0:
                evt = "person_enter" if n > prev_persons else "person_leave"
                payload = {"type": evt, "ts": int(time.time() * 1000), "persons": n}
                if n > 0:
                    payload["best"] = cur_best
                    payload["list"] = _slim_list(persons)   # 逐人 bbox/score → 前端实时目标队列
                send_event(sock, payload)
            prev_persons = n
        else:
            n = -1  # 链路测试模式
            cur_best = None

        frames += 1
        if now - last_stat >= 1.0:
            fps = frames / (now - last_stat)
            infer_ms = (infer_ms_acc / infer_cnt) if infer_cnt else 0.0
            stat = {
                "type": "stats", "ts": int(now * 1000),
                "persons": n, "frames": frames, "fps": round(fps, 1),
                "infer_ms": round(infer_ms, 1),
            }
            if cur_best is not None:
                stat["best"] = cur_best   # 供 PC 兜底出片时的第三视角裁切
            if detector is not None and n > 0:
                stat["list"] = _slim_list(persons)   # 每秒逐人快照 → 前端目标队列实时刷新
            send_event(sock, stat)
            print("[stats] 帧率 %.1f fps | 推理 %.1f ms | 人数 %d | 帧间隔 %.0f ms"
                  % (fps, infer_ms, n, (now - last_frame_t) * 1000))
            frames = 0
            infer_ms_acc = 0.0
            infer_cnt = 0
            last_stat = now
        last_frame_t = now


def main():
    ap = argparse.ArgumentParser(description="RDK X5 感知节点: 收 PC 推流 → YOLO-Pose → 事件回传")
    ap.add_argument("--pc-ip", default="192.168.50.1", help="PC 主控 IP")
    ap.add_argument("--port", type=int, default=9999, help="PC 监听端口")
    ap.add_argument("--model", default="/root/models/yolov8n_pose_bayese_640x640_nv12.bin", help="BPU 模型路径")
    ap.add_argument("--score-thr", type=float, default=0.35, help="检测置信度阈值")
    ap.add_argument("--no-model", action="store_true", help="不加载模型, 仅验证推流链路")
    ap.add_argument("--probe", action="store_true", help="只打印模型输入/输出信息后退出")
    ap.add_argument("--test-image", default=None, help="本地图片自检: 推理+解码+存标注图后退出")
    args = ap.parse_args()

    if args.probe:
        YoloPoseDetector(args.model, args.score_thr).probe()
        return 0

    detector = None
    if not args.no_model:
        try:
            detector = YoloPoseDetector(args.model, args.score_thr)
        except Exception as e:
            print("[warn] 模型加载失败(%s), 降级为链路测试模式" % e)

    if args.test_image:
        if detector is None:
            print("[错误] --test-image 需要加载模型")
            return 1
        return run_test_image(args, detector)

    print("[main] 连接 PC %s:%d ..." % (args.pc_ip, args.port))
    while True:
        try:
            sock = socket.create_connection((args.pc_ip, args.port), timeout=5)
            sock.settimeout(10)  # 10s 无新帧判定断线
            print("[main] 已连上 PC, 开始收流")
            run_loop(sock, args, detector)
        except (ConnectionError, socket.error, socket.timeout) as e:
            print("[main] 连接异常(%s), 5 秒后重连" % e)
            time.sleep(5)
        except KeyboardInterrupt:
            print("[main] 退出")
            return 0


if __name__ == "__main__":
    sys.exit(main())
