#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
AIRSHOT RESCUE · RDK X5 端 WebSocket 服务端
================================================
把 X5 上的全景帧 + 多模态感知事件推给 rescue-console 前端大屏，
替换页面里的假数据（前端右上「接入真实数据流」→ ws://<X5-IP>:9870）。

运行模式:
  1) --mock           全仿真事件源（无摄像头/无模型也能跑，演示兜底）
  2) --sdk 127.0.0.1:9999   ★官方 SDK 路径★: 接入 PC 端 x4_live_demo.exe 的
     实时拼接流——exe 用赛事 CameraSDK+MediaSDK 从 X4(USB) 拉双鱼眼预览流并
     实时拼接成 960x480 等距柱状全景, 本服务以「RDK 角色」连入收帧(INFR),
     转推前端 360° 全景显示, 并把理解流事件以 INEV 回传 exe(驱动其出图模块)。
     exe 侧启动: .\run.ps1 --duration 0 --rdk-stream --rdk-size 960x480 --rdk-fps 10
  3) --uvc /dev/video0   UVC 抓帧（X4 网络摄像头模式, 单镜头画面）→ JPEG 二进制推前端
  4) --yolo-bridge    本地起 TCP 扮演「PC」角色:
                      yolo_pose_client.py --pc-ip 127.0.0.1 --port 9998 连上来，
                      帧由本服务喂(INFR)，检测事件回流(INEV)后广播前端。
                      与 --sdk 组合 = exe→本服务→X5 真机 的中继全链路。
  5) --frames DIR      循环推送本地 JPEG 作为实时帧（无 UVC 硬件的演示数据源）

关键事件抽帧(默认开启, --no-evidence 关闭):
  person_enter(带 bbox)/呼救声/意识判断 → 抽当前全景帧 → 以目标方位为中心
  重投影为常规平面照片(等距柱状→针孔模型, 与 photo_output.cpp 同算法)
  → 存 evidence/ 目录 + base64 推前端证据链卡片。

依赖: 纯标准库实现 RFC6455(WebSocket)，零安装；--uvc/--yolo-bridge 需 cv2（X5 出厂自带）。
兼容: Python 3.6+（与 X5 出厂一致）。

用法示例:
  python3 ws_server.py --mock                          # 纯仿真演示
  python3 ws_server.py --sdk 127.0.0.1:9999            # ★官方 SDK 路径★ 360° 全景:
      # 先启动 exe(另一终端): .\run.ps1 --duration 0 --rdk-stream --rdk-size 960x480 --rdk-fps 10
  python3 ws_server.py --sdk 127.0.0.1:9999 --yolo-bridge --bridge-port 9998  # SDK+推理全链路:
      # 终端3: python3 yolo_pose_client.py --pc-ip 127.0.0.1 --port 9998
  python3 ws_server.py --uvc /dev/video0               # UVC 帧直推前端
  python3 ws_server.py --uvc /dev/video0 --yolo-bridge # UVC 全链路:
      # 另一终端: python3 yolo_pose_client.py --pc-ip 127.0.0.1 --port 9998
  python3 ws_server.py --frames ../rescue-console/assets/live_*.jpg   # PC 无硬件联调帧推送

下行消息(服务端 → 前端):
  binary WebSocket 帧      : JPEG 全景帧（前端直接铺进虚拟机位）
  {"type":"evt","kind":"VIS|AUD|PSE|RSK|RPT|CMD|SYS","text":"..","conf":".."}
  {"type":"person_enter"|"person_leave"|"stats", ...}   # yolo_pose_client 原生透传
  {"type":"pose","hand":"..","head":"..","call":"..","conf":0.91}
  {"type":"audio","text":"..","kw":"help","dir":"142°","conf":0.86}
  {"type":"risk","scores":[["构件松动",78],..],"advice":".."}
  {"type":"slam","dist":36.4,"robot":{"x":3.2,"y":10.6,"yaw":118}}

上行消息(前端 → 服务端, JSON):
  {"cmd":"ping"}  {"cmd":"start_talk"}  {"cmd":"stop_talk"}  {"cmd":"scout_dispatch"}
"""

import argparse
import base64
import collections
import hashlib
import json
import math
import os
import queue
import socket
import struct
import sys
import threading
import time

# cv2/numpy 优先取本地 vendor 目录(PC 侧免安装; X5 出厂自带时该目录不存在, 自动跳过)
#   py3.7+ → _pylibs311(现代 opencv: YuNet 人脸检测/CLAHE/超分增强, 建议用 `py` 启动)
#   py3.6  → _pylibs(opencv 4.5.4 wheel, 功能降级: 无人脸检测)
_here = os.path.dirname(os.path.abspath(__file__))
if sys.version_info >= (3, 7) and os.path.isdir(os.path.join(_here, "_pylibs311")):
    sys.path.insert(0, os.path.join(_here, "_pylibs311"))
else:
    sys.path.insert(0, os.path.join(_here, "_pylibs"))

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
MAGIC_FRAME = b"INFR"   # 与 yolo_pose_client.py 一致: 帧下行
MAGIC_EVENT = b"INEV"   # 与 yolo_pose_client.py 一致: 事件上行
MAGIC_AUDIO = b"INAF"   # exe→本服务: X4 麦克风音频包(AAC ADTS / PCM)


# ═══════════════════════════ 网络原语 ═══════════════════════════
def recv_exact(sock, n):
    """收满 n 字节; 断线返回 None; 空闲超时(浏览器不发数据)继续等"""
    buf = bytearray()
    while len(buf) < n:
        try:
            chunk = sock.recv(n - len(buf))
        except socket.timeout:
            continue           # 连接仍在, 只是没数据 → 保持等待
        if not chunk:
            return None
        buf.extend(chunk)
    return bytes(buf)


# ═══════════════════════════ WebSocket 协议层(RFC6455) ═══════════════════════════
def ws_accept_key(key):
    return base64.b64encode(hashlib.sha1((key + WS_GUID).encode("utf-8")).digest()).decode("ascii")


def ws_handshake(conn):
    """完成 HTTP Upgrade 握手; 成功返回 True"""
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = conn.recv(4096)
        if not chunk:
            return False
        data += chunk
        if len(data) > 65536:
            return False
    head = data.decode("utf-8", "replace")
    key = None
    for line in head.split("\r\n"):
        if line.lower().startswith("sec-websocket-key:"):
            key = line.split(":", 1)[1].strip()
    if not key:
        return False
    resp = ("HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: %s\r\n\r\n" % ws_accept_key(key))
    conn.sendall(resp.encode("ascii"))
    return True


def ws_recv_frame(conn):
    """读一帧 → (opcode, payload bytes); 断线/异常返回 None"""
    try:
        hdr = recv_exact(conn, 2)
        if hdr is None:
            return None
        opcode = hdr[0] & 0x0F
        masked = bool(hdr[1] & 0x80)
        length = hdr[1] & 0x7F
        if length == 126:
            ext = recv_exact(conn, 2)
            if ext is None:
                return None
            length = struct.unpack(">H", ext)[0]
        elif length == 127:
            ext = recv_exact(conn, 8)
            if ext is None:
                return None
            length = struct.unpack(">Q", ext)[0]
        if length > 16 * 1024 * 1024:
            return None
        mask = recv_exact(conn, 4) if masked else None
        payload = recv_exact(conn, length) if length else b""
        if payload is None:
            return None
        if mask:
            payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    except socket.error:
        return None
    return opcode, payload


def ws_send_frame(conn, opcode, payload):
    """服务器→客户端不掩码; 失败抛 socket.error"""
    hdr = bytearray()
    hdr.append(0x80 | opcode)
    n = len(payload)
    if n < 126:
        hdr.append(n)
    elif n < 65536:
        hdr.append(126)
        hdr += struct.pack(">H", n)
    else:
        hdr.append(127)
        hdr += struct.pack(">Q", n)
    conn.sendall(bytes(hdr) + payload)


def ws_send_json(conn, obj):
    ws_send_frame(conn, 0x1, json.dumps(obj, ensure_ascii=False).encode("utf-8"))


def ws_send_binary(conn, data):
    ws_send_frame(conn, 0x2, data)


# ═══════════════════════════ 客户端管理 + 广播 ═══════════════════════════
class Hub(object):
    """所有前端连接; broadcast 失败自动剔除"""

    def __init__(self):
        self.lock = threading.Lock()
        self.clients = []       # [socket, ...]
        self.tap = None         # 事件分接器: broadcast_json 时同步回调(INEV 回传 exe / 抽帧取证)
        self._frames_tx = 0     # 已推送帧数(统计)
        self._events_tx = 0

    def add(self, conn):
        with self.lock:
            self.clients.append(conn)
        print("[hub] 前端已连接 %s (在线 %d)" % (conn.getpeername(), len(self.clients)))

    def remove(self, conn):
        with self.lock:
            if conn in self.clients:
                self.clients.remove(conn)
            n = len(self.clients)
        try:
            conn.close()
        except Exception:
            pass
        print("[hub] 前端断开 (在线 %d)" % n)

    def _snapshot(self):
        with self.lock:
            return list(self.clients)

    def broadcast_json(self, obj):
        data = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self._broadcast_raw(0x1, data)
        self._events_tx += 1
        if self.tap is not None:            # 事件分接: INEV 回传 exe / 触发抽帧取证
            try:
                self.tap(obj)
            except Exception:
                pass

    def broadcast_binary(self, data):
        self._broadcast_raw(0x2, data)
        self._frames_tx += 1

    def _broadcast_raw(self, opcode, data):
        dead = []
        for c in self._snapshot():
            try:
                ws_send_frame(c, opcode, data)
            except Exception:
                dead.append(c)
        for c in dead:
            self.remove(c)

    def send_to(self, conn, obj):
        try:
            ws_send_json(conn, obj)
        except Exception:
            self.remove(conn)

    def stats_line(self):
        return "前端 %d 连接 | 已推 %d 帧 / %d 事件" % (len(self.clients), self._frames_tx, self._events_tx)


# ═══════════════════════════ 前端上行指令 ═══════════════════════════
def handle_uplink(hub, conn, payload):
    """处理前端发来的 JSON 指令; 目前以回执+广播事件为主(不自动执行)
    注意: audio 事件仅来自 AudioDetector(X4 机身麦克风), 前端/浏览器不再上报声学检测"""
    try:
        msg = json.loads(payload.decode("utf-8"))
    except (ValueError, UnicodeDecodeError):
        return
    cmd = str(msg.get("cmd", ""))
    t0 = msg.get("t")
    if cmd == "ping":
        lat = (time.time() * 1000 - t0) if isinstance(t0, (int, float)) else 0
        hub.send_to(conn, {"type": "evt", "kind": "SYS",
                           "text": "X5 心跳应答 %.1f ms" % lat, "conf": "OK"})
    elif cmd == "start_talk":
        hub.broadcast_json({"type": "evt", "kind": "CMD", "text": "双向语音对讲已开启（X5 回执）", "conf": "--"})
    elif cmd == "stop_talk":
        hub.broadcast_json({"type": "evt", "kind": "CMD", "text": "双向语音对讲已关闭", "conf": "--"})
    elif cmd == "scout_dispatch":
        hub.broadcast_json({"type": "evt", "kind": "CMD", "text": "SCOUT 任务经 X5 下发 · 机器狗西侧绕行", "conf": "--"})
    else:
        hub.send_to(conn, {"type": "evt", "kind": "SYS", "text": "未知指令: %s" % cmd[:60], "conf": "--"})


def client_loop(hub, conn):
    """每前端一条收包线程: ping/pong + 上行指令"""
    conn.settimeout(60)
    while True:
        fr = ws_recv_frame(conn)
        if fr is None:
            break
        opcode, payload = fr
        if opcode == 0x8:          # close
            break
        elif opcode == 0x9:        # ping → pong
            try:
                ws_send_frame(conn, 0xA, payload)
            except Exception:
                break
        elif opcode in (0x1, 0x2):  # text/binary 上行
            handle_uplink(hub, conn, payload)
    hub.remove(conn)


# ═══════════════════════════ YoloBridge: 桥接 yolo_pose_client.py ═══════════════════════════
class YoloBridge(threading.Thread):
    """在 X5 本地扮演 yolo_pose_client.py 的对端(PC 角色):
       - 监听 127.0.0.1:bridge_port, yolo_pose_client.py 连上后按 INFR 协议喂帧
       - 收到 INEV 事件(人数/骨架/stats)后原样广播给所有前端
       这样无需改动 yolo_pose_client.py 一行代码。"""

    def __init__(self, hub, port=9999, bind_addr="0.0.0.0"):
        threading.Thread.__init__(self, daemon=True)
        self.hub = hub
        self.port = port
        self.bind_addr = bind_addr    # 0.0.0.0: 允许真实 RDK X5 从网络连入(192.168.50.x)
        self.q = queue.Queue(maxsize=2)     # 喂帧队列(满则丢旧帧, 保证实时性)
        self.conn = None
        self.last_evt = None               # 最近一次 stats(供 /health 查询)

    # ---- 供 UvcCapture 调用 ----
    def feed_frame(self, jpeg):
        try:
            self.q.put_nowait(jpeg)
        except queue.Full:
            try:
                self.q.get_nowait()
            except queue.Empty:
                pass
            try:
                self.q.put_nowait(jpeg)
            except queue.Full:
                pass

    def run(self):
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            srv.bind((self.bind_addr, self.port))
        except socket.error as e:
            print("[bridge] 端口 %d 绑定失败(%s), yolo 桥接不可用" % (self.port, e))
            return
        srv.listen(1)
        print("[bridge] 就绪: 等待 RDK(yolo_pose_client.py) 连接 → "
              "python3 yolo_pose_client.py --pc-ip <本机IP> --port %d" % self.port)
        while True:
            try:
                conn, addr = srv.accept()
            except socket.error:
                return
            print("[bridge] yolo_pose_client 已连接 %s" % (addr,))
            self.conn = conn
            threading.Thread(target=self._recv_loop, args=(conn,), daemon=True).start()
            self._send_loop(conn)
            self.conn = None

    def _send_loop(self, conn):
        """持续喂帧: INFR | u32 len | u64 ts_ms | JPEG"""
        while True:
            try:
                jpeg = self.q.get(timeout=10)
            except queue.Empty:
                continue
            try:
                ts = struct.pack("<Q", int(time.time() * 1000))
                conn.sendall(MAGIC_FRAME + struct.pack("<I", len(jpeg)) + ts + jpeg)
            except socket.error:
                print("[bridge] 推流断开(推理端退出), 等待重连...")
                try:
                    conn.close()
                except Exception:
                    pass
                return

    def _recv_loop(self, conn):
        """收事件: INEV | u32 len | JSON → 原样透传前端"""
        try:
            while True:
                try:
                    hdr = recv_exact(conn, 8)
                except (socket.error, TimeoutError):
                    # 网络闪断(USB 网卡抖动/对端超时关闭) → Windows TCP 重传超时(10060)
                    print("[bridge] 事件通道断开(网络超时/复位), 等待推理端重连...")
                    return
                if hdr is None:
                    return
                magic, length = struct.unpack("<4sI", hdr)
                if magic != MAGIC_EVENT:
                    print("[bridge] 事件头错误: %r" % magic)
                    return
                if length == 0 or length > 1024 * 1024:
                    return
                try:
                    payload = recv_exact(conn, length)
                except (socket.error, TimeoutError):
                    return
                if payload is None:
                    return
                try:
                    evt = json.loads(payload.decode("utf-8"))
                except ValueError:
                    continue
                self.last_evt = evt
                self.hub.broadcast_json(evt)      # person_enter / stats 原样透传
        finally:
            try:
                conn.close()
            except Exception:
                pass


# ═══════════════════════════ SdkSource: 官方 SDK 拼接流接入 ═══════════════════════════
class SdkSource(threading.Thread):
    """★官方 SDK 路径★ 以「RDK 角色」连入 PC 端 x4_live_demo.exe 的实时拼接流:
       - exe(赛事 CameraSDK + MediaSDK) 从 X4(USB) 拉双鱼眼预览流, RealTimeStitcher
         实时拼接成等距柱状全景(如 960x480), 本类按 INFR 协议收帧
       - 收到帧 → 广播前端(360° 全景显示) + 喂 YoloBridge(可选, BPU 推理)
                 + 缓存最新帧(供关键事件抽帧重投影)
       - send_event(): 理解流事件以 INEV 回传 exe(驱动其第三视角出图模块)
       - 断线自动重连(3s); exe 未启动时持续等待"""

    MAX_PAYLOAD = 4 * 1024 * 1024   # 与 rdk_stream.cpp kMaxPayload 一致

    def __init__(self, addr, hub, bridge=None):
        threading.Thread.__init__(self, daemon=True)
        if ":" in str(addr):
            host, _, p = str(addr).rpartition(":")
            self.host, self.port = host, int(p)
        else:
            self.host, self.port = str(addr), 9999
        self.hub = hub
        self.bridge = bridge
        self.audio = AudioDetector(hub)   # X4 麦克风音频 → 呼救声检测
        self.audio.start()
        self.send_lock = threading.Lock()   # 保护 self.sock(事件发送线程并发)
        self.sock = None
        self.frame_lock = threading.Lock()
        self.last_jpeg = None               # 最新全景帧 JPEG(供抽帧)
        self.last_ts = 0
        self.frames_rx = 0

    def latest_frame(self):
        """() → (jpeg bytes|None, ts_ms)  供 EvidenceRecorder 抽帧"""
        with self.frame_lock:
            return self.last_jpeg, self.last_ts

    def send_event(self, obj):
        """理解流事件 INEV 回传 exe(小端: "INEV"|u32 len|JSON); 未连接/失败返回 False"""
        try:
            payload = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        except (TypeError, ValueError):
            return False
        if not payload or len(payload) > self.MAX_PAYLOAD:
            return False
        with self.send_lock:
            s = self.sock
            if s is None:
                return False
            try:
                s.sendall(MAGIC_EVENT + struct.pack("<I", len(payload)) + payload)
                return True
            except socket.error:
                return False

    def run(self):
        print("[sdk] 目标拼接流 %s:%d (exe: .\\run.ps1 --duration 0 --rdk-stream "
              "--rdk-size 1920x960 --rdk-fps 10)" % (self.host, self.port))
        while True:
            try:
                s = socket.create_connection((self.host, self.port), timeout=5)
            except socket.error as e:
                print("[sdk] 连接失败(%s), 3s 后重试..." % e)
                time.sleep(3)
                continue
            print("[sdk] 已连入 exe 拼接流 → 360° 全景转推前端")
            with self.send_lock:
                self.sock = s
            try:
                self._recv_loop(s)
            except (socket.error, OSError) as e:
                # exe 重启/崩溃时连接被重置(10054), 捕获后走下方重连逻辑
                print("[sdk] 接收异常(%s), 断开重连" % e)
            with self.send_lock:
                self.sock = None
                try:
                    s.close()
                except Exception:
                    pass
            print("[sdk] 拼接流断开(累计收帧 %d), 3s 后重连" % self.frames_rx)
            time.sleep(3)

    def _recv_loop(self, s):
        """收帧: "INFR"|u32 len|u64 ts_ms|JPEG / "INAF"|u32 len|u64 ts|音频包 (小端, 与 rdk_stream.cpp 一致)"""
        while True:
            hdr = recv_exact(s, 16)
            if hdr is None:
                return
            magic = hdr[:4]
            length = struct.unpack("<I", hdr[4:8])[0]
            if length == 0 or length > self.MAX_PAYLOAD:
                print("[sdk] 帧头错位(len=%d), 断开重连" % length)
                return
            if magic == MAGIC_AUDIO:
                audio = recv_exact(s, length)
                if audio is None:
                    return
                self.audio.feed(audio)               # X4 麦克风 → 呼救声检测
                continue
            if magic != MAGIC_FRAME:
                print("[sdk] 帧头错位(%r), 断开重连" % magic)
                return
            jpeg = recv_exact(s, length)
            if jpeg is None:
                return
            ts = struct.unpack("<Q", hdr[8:16])[0]
            self.frames_rx += 1
            with self.frame_lock:
                self.last_jpeg = jpeg       # 缓存供关键事件抽帧
                self.last_ts = ts
            self.hub.broadcast_binary(jpeg)  # 前端 360° 全景
            if self.bridge is not None:      # 喂 BPU 推理(可选)
                self.bridge.feed_frame(jpeg)


# ═══════════════════════════ AudioDetector: X4 麦克风呼救声检测 ═══════════════════════════
RESCUE_KWS = [                     # 呼救关键词（命中 → kw=help, 事件升级）
    ("救命", "help"), ("help", "help"), ("来人", "help"), ("救人", "help"),
    ("有人吗", "help"), ("听着", "help"), ("爬不出来", "help"),
    ("哎哟", "pain"), ("好痛", "pain"), ("疼", "pain"),
    ("我在这里", "here"), ("我在这", "here"), ("这里", "here"),
]

class AudioDetector(threading.Thread):
    """X4 麦克风音频 → 呼救声事件（替代浏览器麦克风检测）

    exe 把 CameraSDK OnAudioData 的包经 INAF 推来（AAC ADTS 或 PCM s16le）,
    本线程独立处理, 不阻塞视频帧转发:
      解码(PyAV) → 48k→16k 单声道 → 能量 VAD 切分语句(自适应底噪) →
      faster-whisper 转写(中文) → 关键词匹配(救命/help...) →
      {"type":"audio"} 广播前端(呼救栏渲染 + 录音回放) + INEV 回传 exe(驱动出图)
    另按 ~8Hz 广播 {"type":"audio_meter"} 实时电平驱动前端波形。"""

    SR = 16000                     # 工作采样率(whisper 输入)
    FRAME_MS = 30                  # VAD 窗
    START_FRAMES = 5               # 触发: 连续 150ms 高于阈值
    HANGOVER_FRAMES = 25           # 结束: 750ms 静音
    MAX_UTT_S = 8.0                # 语句最长 8s（到点立即出事件）
    MIN_UTT_S = 0.45               # 过短的丢弃
    COOLDOWN_S = 1.5               # 两语句事件最小间隔
    METER_HZ = 8.0

    def __init__(self, hub):
        threading.Thread.__init__(self, daemon=True)
        self.hub = hub
        self.q = collections.deque(maxlen=256)     # 原始音频包(丢旧保新)
        self.q_lock = threading.Lock()
        self.wake = threading.Event()
        self.src_rate = 48000
        self.src_ch = 2
        self.codec = None            # PyAV AAC 解码器(懒建)
        self.is_aac = None           # None=未探测; True=AAC ADTS; False=PCM
        self.np = None
        self.whisper = None          # 懒加载
        self.last_evt_t = 0.0
        self.pcm16k = bytearray()    # 工作缓冲(utterance 采集)
        self.frame_rms = []          # 近期帧 RMS(自适应底噪)
        self.in_utt = False
        self.silent_run = 0
        self.active_run = 0
        self.meter_t = 0.0

    # ---------- 对外: SdkSource 收到 INAF 时调用（绝不阻塞） ----------
    def feed(self, blob):
        with self.q_lock:
            self.q.append(blob)
        self.wake.set()

    # ---------- 解码: AAC ADTS(PyAV) / PCM s16le → mono float32 @16k ----------
    def _detect_format(self, b):
        if len(b) >= 2 and b[0] == 0xFF and (b[1] & 0xF0) == 0xF0:
            return True              # ADTS syncword
        return False

    def _decode(self, blob):
        """→ np.float32 mono @16k（失败返回 None）"""
        np = self.np
        if self.is_aac is None:
            self.is_aac = self._detect_format(blob)
            print("[audio] X4 音频格式探测: %s (首包 %d 字节)"
                  % ("AAC ADTS" if self.is_aac else "PCM s16le", len(blob)))
        if self.is_aac:
            try:
                import av                        # PyAV(自带 ffmpeg, 无系统依赖)
            except ImportError:
                print("[audio][警告] PyAV 未安装(pip install av), AAC 无法解码")
                return None
            if self.codec is None:
                self.codec = av.CodecContext.create("aac", "r")
            try:
                outs = []
                for pkt in self.codec.parse(blob):
                    for fr in self.codec.decode(pkt):
                        a = fr.to_ndarray()      # s16, shape (1, n*ch) 或 (ch, n)
                        if a.ndim == 2:
                            a = a.reshape(-1) if a.shape[0] == 1 else a.T.reshape(-1)
                        self.src_rate = fr.sample_rate or self.src_rate
                        self.src_ch = len(fr.layout.channels) or self.src_ch
                        outs.append(a)
                if not outs:
                    return None
                raw = np.concatenate(outs).astype(np.float32) / 32768.0
            except Exception as e:
                print("[audio] AAC 解码异常: %s" % e)
                self.codec = None                # 下次重建解码器
                return None
        else:
            raw = np.frombuffer(blob, dtype="<i2").astype(np.float32) / 32768.0
        # 立体声→单声道
        if self.src_ch == 2 and (len(raw) % 2) == 0:
            raw = raw.reshape(-1, 2).mean(axis=1)
        # 重采样 → 16k（整数倍抽取; 非整数倍用线性插值）
        if self.src_rate != self.SR:
            ratio = self.src_rate / self.SR
            if float(ratio).is_integer():
                raw = raw[::int(ratio)]
            else:
                n = int(len(raw) * self.SR / self.src_rate)
                raw = np.interp(np.linspace(0, len(raw) - 1, n), np.arange(len(raw)), raw)
        return raw.astype(np.float32)

    # ---------- whisper 懒加载 ----------
    def _get_whisper(self):
        if self.whisper is not None:
            return self.whisper
        try:
            os.environ.setdefault("HF_ENDPOINT", "https://hf-mirror.com")
            os.environ.setdefault("HF_HUB_DISABLE_XET", "1")
            from faster_whisper import WhisperModel
            self.whisper = WhisperModel("base", device="cpu", compute_type="int8")
            print("[audio] faster-whisper(base/int8) 已就绪")
        except Exception as e:
            print("[audio][警告] whisper 不可用(%s), 降级为能量 VAD 事件" % e)
            self.whisper = False   # False = 明确不可用
        return self.whisper

    # ---------- WAV(base64) 封装, 供前端回放 ----------
    def _wav_b64(self, pcm_float):
        import io, wave, base64
        s16 = (self.np.clip(pcm_float, -1, 1) * 32767).astype("<i2").tobytes()
        bio = io.BytesIO()
        with wave.open(bio, "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(self.SR)
            w.writeframes(s16)
        return base64.b64encode(bio.getvalue()).decode("ascii")

    # ---------- 主循环 ----------
    def run(self):
        print("[audio] X4 麦克风呼救声检测已启动（VAD + faster-whisper）")
        try:
            import numpy
            self.np = numpy
        except ImportError:
            print("[audio][错误] numpy 不可用, 检测停用")
            return
        frame_len = int(self.SR * self.FRAME_MS / 1000)
        np = self.np
        while True:
            self.wake.wait(timeout=0.5)
            self.wake.clear()
            with self.q_lock:
                blobs = list(self.q); self.q.clear()
            for blob in blobs:
                mono = self._decode(blob)
                if mono is None or len(mono) == 0:
                    continue
                # 按 30ms 帧 VAD
                for i in range(0, len(mono) - frame_len + 1, frame_len):
                    self._vad_frame(mono[i:i + frame_len])
                self._meter_tick()

    def _vad_frame(self, x):
        np = self.np
        rms = float(np.sqrt(np.mean(x * x)) + 1e-9)
        self.frame_rms.append(rms)
        if len(self.frame_rms) > 150:             # ~4.5s 历史
            self.frame_rms.pop(0)
        floor = sorted(self.frame_rms)[max(0, int(len(self.frame_rms) * 0.15) - 1)]
        thr = max(floor * 3.5, 0.006)
        loud = rms > thr
        if not self.in_utt:
            self.active_run = self.active_run + 1 if loud else 0
            if self.active_run >= self.START_FRAMES:
                self.in_utt = True
                self.silent_run = 0
                self.active_run = 0
                self.pcm16k = bytearray()         # 触发时从头采集（丢前导静音）
        else:
            self.pcm16k.extend((x * 32767).astype("<i2").tobytes())
            dur = len(self.pcm16k) / 2 / self.SR
            self.silent_run = 0 if loud else self.silent_run + 1
            if self.silent_run >= self.HANGOVER_FRAMES or dur >= self.MAX_UTT_S:
                self.in_utt = False
                self.silent_run = 0
                self._finish_utt()

    def _finish_utt(self):
        np = self.np
        pcm = np.frombuffer(bytes(self.pcm16k), dtype="<i2").astype(np.float32) / 32768.0
        self.pcm16k = bytearray()
        dur = len(pcm) / self.SR
        if dur < self.MIN_UTT_S:
            return
        now = time.time()
        if now - self.last_evt_t < self.COOLDOWN_S:
            return
        peak = float(np.max(np.abs(pcm)))
        # 转写（可用时）
        text, conf = "", min(0.99, 0.35 + peak * 0.5)
        model = self._get_whisper()
        if model:
            try:
                segments, info = model.transcribe(pcm, language="zh", beam_size=1,
                                                   vad_filter=False, condition_on_previous_text=False)
                segs = list(segments)
                text = "".join(s.text.strip() for s in segs)
                if segs:
                    lp = float(np.mean([s.avg_logprob for s in segs]))
                    conf = float(min(0.99, max(0.30, np.exp(lp))))
            except Exception as e:
                print("[audio] 转写异常: %s" % e)
        # 关键词
        kw = "voice"
        text_l = text.lower()
        for pat, tag in RESCUE_KWS:
            if pat in text or pat in text_l:
                kw = tag
                break
        if not text:
            text = "现场人声（X4 麦克风 · 峰值 %d%%）" % (peak * 100)
        self.last_evt_t = now
        evt = {"type": "audio", "text": text, "kw": kw, "dir": "--",
               "conf": round(conf, 2), "src": "X4", "dur_ms": int(dur * 1000),
               "wav_b64": self._wav_b64(pcm)}
        print("[audio] 呼救声事件: [%s] %s (conf=%.2f, %.1fs)" % (kw, text, conf, dur))
        self.hub.broadcast_json(evt)

    def _meter_tick(self):
        """实时电平 → 前端波形（节流 ~8Hz）"""
        now = time.time()
        if now - self.meter_t < 1.0 / self.METER_HZ:
            return
        self.meter_t = now
        if self.frame_rms:
            rms = self.frame_rms[-1]
            floor = sorted(self.frame_rms)[max(0, int(len(self.frame_rms) * 0.15) - 1)]
            self.hub.broadcast_json({"type": "audio_meter",
                                     "rms": round(rms, 5), "floor": round(floor, 5),
                                     "active": self.in_utt})


# ═══════════════════════════ UVC 抓帧推流 ═══════════════════════════
def uvc_probe(cv2, skip_idx=None, max_idx=5):
    """枚举本机摄像头索引, 返回 [(idx, w, h), ...]
       skip_idx: 当前已占用的索引(打开会冲突, 跳过)"""
    devs = []
    for idx in range(max_idx + 1):
        if skip_idx is not None and idx == skip_idx:
            continue
        cap = cv2.VideoCapture(idx)
        if not cap.isOpened() and sys.platform == "win32":
            cap.release()
            cap = cv2.VideoCapture(idx + cv2.CAP_DSHOW)
        if not cap.isOpened():
            cap.release()
            continue
        ok, frame = cap.read()
        if ok:
            h, w = frame.shape[:2]
            devs.append((idx, w, h))
        cap.release()
    return devs


def uvc_pick(devs):
    """从探测结果选最优设备:
       优先 2:1 宽高比(X4 等距柱状全景的签名, 如 3840x1920),
       普通摄像头是 4:3(1.33) 或 16:9(1.78); 分辨率作次要加分"""
    def score(d):
        _, w, h = d
        ar = w / float(h or 1)
        return 1.0 / (abs(ar - 2.0) + 0.15) + (w * h) / (3840.0 * 1920.0)
    return max(devs, key=score)


class UvcCapture(threading.Thread):
    """X4(网络摄像头模式) UVC 设备抓帧:
       - JPEG 二进制 → 广播前端(大屏全景画面)
       - 同一帧喂 YoloBridge(若启用) → BPU 推理 → 事件回流
       - dev="auto": 自动探测; 每 5s 复查, X4 一旦切入摄像头模式即热切换(无需重启)"""

    MAX_W = 2560   # 推流降采样上限(X4 可能输出 8K 级全景, 直推会压垮带宽)

    def __init__(self, dev, hub, bridge=None, fps=10, jpeg_q=72, width=None, height=None):
        threading.Thread.__init__(self, daemon=True)
        self.dev = int(dev) if str(dev).isdigit() else dev   # "0" → 索引(Windows 摄像头), "/dev/video0" → V4L2
        self.hub = hub
        self.bridge = bridge
        self.interval = 1.0 / max(1, fps)
        self.jpeg_q = jpeg_q
        self.width = width
        self.height = height
        self.fatal = None
        self.auto = str(dev).lower() == "auto"
        self.switch_req = None      # 探测线程 → 采集线程 的切换请求(索引)
        self.cur_idx = None         # 当前占用的索引(auto 模式记录, 供探测跳过)
        self.cur_w = 0
        self.cur_h = 0

    def _probe_loop(self):
        """auto 模式的后台探测: 发现更优设备(如 X4 插入)置 switch_req"""
        while True:
            time.sleep(5)
            try:
                devs = uvc_probe(self._cv2, skip_idx=self.cur_idx)
            except Exception:
                continue
            if not devs:
                continue
            cand = list(devs)
            if self.cur_idx is not None:
                cand.append((self.cur_idx, self.cur_w, self.cur_h))
            best = uvc_pick(cand)
            if best[0] != self.cur_idx and best[0] != self.switch_req:
                self.switch_req = best[0]
                print("[uvc][auto] 发现更优设备 #%d %dx%d → 准备切换" % best)

    def run(self):
        import cv2   # 延迟导入: mock 模式无需 cv2
        self._cv2 = cv2
        dev = self.dev
        if self.auto:
            devs = uvc_probe(cv2)
            if not devs:
                self.fatal = "未探测到任何摄像头设备"
                print("[uvc][错误] " + self.fatal)
                return
            dev, w, h = uvc_pick(devs)
            print("[uvc][auto] 探测到 %s → 选用 #%d %dx%d"
                  % (", ".join("#%d %dx%d" % d for d in devs), dev, w, h))
            threading.Thread(target=self._probe_loop, daemon=True).start()

        while True:
            cap = cv2.VideoCapture(dev)
            # Windows 下索引设备可能需 DSHOW 后端（Linux 的 V4L2 不走此分支）
            if not cap.isOpened() and isinstance(dev, int) and sys.platform == "win32":
                cap.release()
                cap = cv2.VideoCapture(dev + cv2.CAP_DSHOW)
            if self.width or self.height:
                if sys.platform == "win32":
                    # MJPG 压缩格式解锁高分辨率(YUY2 带宽不够 1080p+)
                    cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*"MJPG"))
                if self.width:
                    cap.set(cv2.CAP_PROP_FRAME_WIDTH, self.width)
                if self.height:
                    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, self.height)
            if not cap.isOpened():
                self.fatal = "打不开 %s (ls /dev/video* 确认 X4 UVC 枚举)" % dev
                print("[uvc][错误] " + self.fatal)
                if self.auto:
                    time.sleep(5)      # auto: 等设备出现后重试
                    continue
                return
            w = cap.get(cv2.CAP_PROP_FRAME_WIDTH)
            h = cap.get(cv2.CAP_PROP_FRAME_HEIGHT)
            self.cur_idx = dev if isinstance(dev, int) else None
            self.cur_w, self.cur_h = w, h
            print("[uvc] %s 打开成功 %.0fx%.0f @%.0ffps, 编码 JPEG q=%d"
                  % (dev, w, h, 1.0 / self.interval, self.jpeg_q))
            fails = 0
            reopen = False
            while not reopen:
                # auto 模式: 后台探测到更优设备(X4) → 热切换
                if self.auto and self.switch_req is not None and self.switch_req != self.cur_idx:
                    dev = self.switch_req
                    self.switch_req = None
                    print("[uvc][auto] 热切换 → 设备 #%d" % dev)
                    reopen = True
                    break
                ok, frame = cap.read()
                if not ok:
                    fails += 1
                    if fails > 30:
                        print("[uvc][错误] 连续取帧失败, 5s 后重开设备")
                        reopen = True      # 重开(支持拔插恢复)
                        break
                    time.sleep(0.1)
                    continue
                fails = 0
                fh, fw = frame.shape[:2]
                if fw > self.MAX_W:        # 超宽全景降采样, 保推流流畅
                    nw = self.MAX_W
                    nh = int(round(fh * nw / float(fw)))
                    frame = cv2.resize(frame, (nw, nh))
                ok, buf = cv2.imencode(".jpg", frame, [int(cv2.IMWRITE_JPEG_QUALITY), self.jpeg_q])
                if not ok:
                    continue
                jpeg = buf.tobytes()
                self.hub.broadcast_binary(jpeg)
                if self.bridge is not None:
                    self.bridge.feed_frame(jpeg)
                time.sleep(self.interval)
            cap.release()
            if reopen:
                time.sleep(5 if fails else 0.2)   # 失败重开歇 5s, 主动切换立即重开


# ═══════════════════════════ Mock 仿真事件源 ═══════════════════════════
MOCK_SCRIPT = [
    (0.0,  "SYS", "系统上线 · X4 UVC 模式经 USB 3.0 接入 /dev/video0 · 8K 全景流就绪", "OK"),
    (1.5,  "SYS", "RDK X5 BPU 加载模型: YOLOv8n-Pose(INT8) · MoveNet · 语义分割", "OK"),
    (3.0,  "VIS", "全景等距柱状展开完成 · 生成虚拟机位(当前朝向 078°)", "0.90"),
    (5.0,  "VIS", "检测到人体轮廓 ×3 · 最大目标 P-03 · 右后方 2 点方向", "0.93"),
    (8.0,  "VIS", "自动重构东侧走廊视角 · 虚拟机位切换完成", "0.90"),
    (11.0, "AUD", "暴雨噪声(24mm/h)中分离出咳嗽声 · 阵列 DOA 142°", "0.86"),
    (14.0, "AUD", "识别微弱呼救「救命」· 声源方向与 P-03 视觉方位一致 → 交叉验证通过", "0.86"),
    (17.0, "PSE", "P-03 持续抬手 12s · 头部回正看向镜头 · 回应呼叫 2/2 次", "0.91"),
    (20.0, "PSE", "意识清醒置信度 0.91 · 手部可活动 · 推断腿部受压", "0.91"),
    (23.0, "RSK", "上方构件松动风险 · 评分 78/100 · 建议进入前结构支撑", "HIGH"),
    (26.0, "RSK", "通道宽度 0.7m · 单人可通行 · 担架无法进入", "HIGH"),
    (29.0, "RPT", "P-03 证据档案生成完毕 · 定位回传 X12.4/Y8.7 · 距入口 36.4m", "OK"),
    (32.0, "RPT", "任务卡已生成: 优先级 1 · 先支撑→再确认受压点→后拓宽通道", "OK"),
    (35.0, "SYS", "SLAM 建图进行中 · 2m 网格 · 北向上 · 轨迹 36.4m", "OK"),
    (60.0, "VIS", "P-02 轮廓微动 · 置信度不足 · 建议第二视角确认", "0.41"),
    (62.0, "AUD", "东向微弱环境声 · 无人员声纹特征", "0.12"),
]

MOCK_POSE = {"type": "pose", "hand": "持续 12s", "head": "回正 · 看向镜头",
             "call": "2/2 次", "conf": 0.91}
MOCK_RISK = {"type": "risk",
             "scores": [["构件松动", 78], ["通道堵塞", 64], ["坍塌风险", 72],
                        ["积水", 31], ["能见度", 46]],
             "advice": "⚠ 构件松动 78 分 · 进入前需结构支撑；通道宽 0.7m，担架无法进入。"}
MOCK_SLAM = {"type": "slam", "dist": 36.4, "robot": {"x": 3.2, "y": 10.6, "yaw": 118}}
# 注意: mock 模式不再伪造 audio 面板事件 —— 呼救声检测只认 X4 机身麦克风录音(AudioDetector)


class MockSource(threading.Thread):
    """无硬件仿真: 复刻大屏叙事脚本 + 1Hz stats + 各面板数据事件"""

    def __init__(self, hub, loop=True):
        threading.Thread.__init__(self, daemon=True)
        self.hub = hub
        self.loop = loop

    def run(self):
        print("[mock] 仿真事件源启动（无硬件演示模式）")
        threading.Thread(target=self._stats_loop, daemon=True).start()
        while True:
            t0 = time.time()
            for delay, kind, text, conf in MOCK_SCRIPT:
                self._sleep_until(t0, delay)
                self.hub.broadcast_json({"type": "evt", "kind": kind, "text": text, "conf": conf})
                # 关键节点同步面板数据事件
                if kind == "VIS" and "检测到人体" in text:
                    self.hub.broadcast_json({"type": "person_enter", "ts": int(time.time() * 1000),
                                             "persons": 3, "best": {"bbox": [412, 168, 96, 214],
                                                                    "score": 0.93, "kpts": []}})
                elif kind == "PSE" and "意识清醒" in text:
                    self.hub.broadcast_json(MOCK_POSE)
                elif kind == "RSK" and "构件松动" in text:
                    self.hub.broadcast_json(MOCK_RISK)
                elif kind == "SYS" and "SLAM" in text:
                    self.hub.broadcast_json(MOCK_SLAM)
            if not self.loop:
                break
            print("[mock] 一轮叙事播完, %ds 后重播" % 20)
            time.sleep(20)

    def _stats_loop(self):
        """持续 1Hz stats 心跳(与 yolo_pose_client.py 行为一致), 任意时刻接入都有数据"""
        while True:
            self.hub.broadcast_json({"type": "stats", "ts": int(time.time() * 1000),
                                     "persons": 3, "fps": 13.7, "infer_ms": 73.0})
            time.sleep(1.0)

    @staticmethod
    def _sleep_until(t0, target):
        d = target - (time.time() - t0)
        if d > 0:
            time.sleep(d)


class FrameLoopSource(threading.Thread):
    """循环推送本地 JPEG 目录作为实时帧（无 UVC 硬件时的演示/联调数据源）
       与 UvcCapture 走完全相同的 hub.broadcast_binary + bridge.feed_frame 路径"""

    def __init__(self, folder, hub, bridge=None, fps=10):
        threading.Thread.__init__(self, daemon=True)
        self.folder = folder          # 目录 或 含通配符的 glob(如 assets/live_*.jpg)
        self.hub = hub
        self.bridge = bridge
        self.interval = 1.0 / max(1, fps)
        self.frame_lock = threading.Lock()
        self.last_jpeg = None         # 最新帧(供关键事件抽帧, 无 SDK 硬件时)
        self.last_ts = 0

    def latest_frame(self):
        """() → (jpeg bytes|None, ts_ms)  供 EvidenceRecorder 抽帧(与 SdkSource 同接口)"""
        with self.frame_lock:
            return self.last_jpeg, self.last_ts

    def _list_files(self):
        import glob
        # 1) 直接按 glob 解释(支持 live_*.jpg 精选全景帧); 排除目录本身
        files = [p for p in sorted(glob.glob(self.folder)) if os.path.isfile(p)]
        # 2) 无匹配 → 当作目录, 收集目录下全部图片
        if not files:
            for e in ("*.jpg", "*.jpeg", "*.png"):
                files.extend(glob.glob(os.path.join(self.folder, e)))
            files.sort()
        return files

    def run(self):
        files = self._list_files()
        if not files:
            print("[frames] 未找到图片, 停止: %s" % self.folder)
            return
        print("[frames] 循环推帧 %d 张 @ %.1f fps（演示数据源, 无需 UVC）" % (len(files), 1.0 / self.interval))
        i = 0
        while True:
            try:
                with open(files[i % len(files)], "rb") as f:
                    data = f.read()
                with self.frame_lock:
                    self.last_jpeg, self.last_ts = data, int(time.time() * 1000)
                self.hub.broadcast_binary(data)
                if self.bridge:
                    self.bridge.feed_frame(data)
            except Exception as e:
                print("[frames] 读取失败 %s: %s" % (files[i % len(files)], e))
                time.sleep(1)
            i += 1
            time.sleep(self.interval)


# ═══════════════════════════ 全景→平面 重投影(photo_output.cpp 同算法) ═══════════════════════════
def reproject_rectilinear(pano, lon0, lat0, fov_x_rad, dw, dh):
    """等距柱状全景 → 以 (lon0, lat0) 为视线中心的常规平面照片(针孔模型)
       移植自 photo_output.cpp ReprojectRectilinear, 并补上俯仰旋转(C++ 版 lat0 未参与
       ev 计算, 有仰角时目标不会垂直居中; lat0=0 时两版逐像素一致):
       输出像素 → 相机系视线 → 俯仰 lat0 + 偏航 → 世界方向 → 经纬度 → 全景坐标 → 双线性采样
       约定(与 main.cpp 出图模块同源): lon0=(1-2*cxn)π, lat0=(0.5-cyn)π
       → 输出中心恰好落在全景 (cxn, cyn), 且画面右/上 = 全景右/上(无镜像)
       pano: BGR uint8 (ph, pw, 3); 返回 (dh, dw, 3) uint8"""
    import numpy as np
    ph, pw = pano.shape[:2]
    k_pi = 3.14159265
    f = (dw * 0.5) / math.tan(fov_x_rad * 0.5)

    # 输出像素 → 相机系视线方向(针孔模型; x 右, y 上, z 前)
    u = np.arange(dw, dtype=np.float32)[None, :]
    v = np.arange(dh, dtype=np.float32)[:, None]
    dx = np.broadcast_to(u - dw * 0.5, (dh, dw))       # (dh, dw)
    dy = np.broadcast_to(dh * 0.5 - v, (dh, dw))
    dz = np.full((dh, dw), f, dtype=np.float32)

    # 相机系 → 世界系: 先绕 x 轴俯仰 lat0, 再绕 y 轴偏航 theta0(=-lon0, 镜像约定)
    theta0 = -lon0
    cphi, sphi = math.cos(lat0), math.sin(lat0)
    cth, sth = math.cos(theta0), math.sin(theta0)
    ty = dy * cphi + dz * sphi                          # R_x(-lat0)
    tz = -dy * sphi + dz * cphi
    wx = dx * cth + tz * sth                            # R_y(theta0)
    wy = ty
    wz = -dx * sth + tz * cth

    # 世界方向 → 全景像素坐标(经度环形 wrap)
    theta = np.arctan2(wx, wz)
    wlen = np.sqrt(wx * wx + wy * wy + wz * wz)
    phi = np.arcsin(np.clip(wy / wlen, -1.0, 1.0))
    eu = (0.5 + theta / (2.0 * k_pi)) * pw
    ev = (0.5 - phi / k_pi) * ph

    # 双线性采样(经度环形 wrap, 纬度 clamp)
    x0 = np.floor(eu).astype(np.int64)
    y0 = np.floor(ev).astype(np.int64)
    fx = (eu - x0)[..., None]
    fy = (ev - y0)[..., None]
    xa, xb = x0 % pw, (x0 + 1) % pw
    ya = np.clip(y0, 0, ph - 1)
    yb = np.clip(y0 + 1, 0, ph - 1)
    p00 = pano[ya, xa].astype(np.float32)
    p01 = pano[ya, xb].astype(np.float32)
    p10 = pano[yb, xa].astype(np.float32)
    p11 = pano[yb, xb].astype(np.float32)
    out = (p00 * (1 - fx) * (1 - fy) + p01 * fx * (1 - fy) +
           p10 * (1 - fx) * fy + p11 * fx * fy)
    return np.clip(out, 0.0, 255.0).astype(np.uint8)


def _fmt_conf(c):
    if isinstance(c, (int, float)):
        return "%.2f" % c
    return str(c) if c is not None else "--"


# ═══════════════════════════ 人脸检测(YuNet) + 画质增强 ═══════════════════════════
class FaceScanner(object):
    """YuNet 人脸检测(OpenCV Zoo · face_detection_yunet_2023mar, 232KB):
       - detect() 返回 [{x,y,w,h,score}], 坐标为输入图像像素系
       - 线程安全(内部锁); 模型缺失/旧版 cv2(py3.6 的 4.5.4)时优雅降级 → 恒返回 []
       - setInputSize 按输入自适应, 小脸检出率随分辨率提升(全景 1920 宽时远优于 960)"""

    def __init__(self, score_thresh=0.55):
        self.lock = threading.Lock()
        self.det = None
        self.failed = False
        self.score_thresh = score_thresh
        self.model = os.path.join(
            os.path.dirname(os.path.abspath(__file__)),
            "models", "face_detection_yunet_2023mar.onnx")

    def available(self):
        return self._ensure() is not None

    def _ensure(self):
        """惰性创建检测器(首次调用线程); 失败只打一次日志, 之后不再尝试"""
        if self.det is not None or self.failed:
            return self.det
        with self.lock:
            if self.det is not None or self.failed:
                return self.det
            try:
                import cv2
                if not hasattr(cv2, "FaceDetectorYN"):
                    self.failed = True
                    print("[face] cv2 %s 无 FaceDetectorYN(py3.6 旧库), 人脸检测停用"
                          % getattr(cv2, "__version__", "?"))
                    return None
                if not os.path.isfile(self.model):
                    self.failed = True
                    print("[face] 模型缺失 %s, 人脸检测停用" % self.model)
                    return None
                self.det = cv2.FaceDetectorYN.create(self.model, "", (320, 320))
                print("[face] YuNet 人脸检测就绪(threshold %.2f)" % self.score_thresh)
            except Exception as e:
                self.failed = True
                print("[face] 检测器初始化失败(%s), 人脸检测停用" % e)
                return None
        return self.det

    def detect(self, bgr):
        """BGR 图 → 人脸列表(按面积降序); 任何异常都按无脸处理, 不影响主流程"""
        det = self._ensure()
        if det is None or bgr is None:
            return []
        h, w = bgr.shape[:2]
        if w < 16 or h < 16:
            return []
        with self.lock:
            try:
                det.setInputSize((w, h))
                det.setScoreThreshold(self.score_thresh)
                _rv, faces = det.detect(bgr)
            except Exception as e:
                print("[face] 检测异常: %s" % e)
                return []
        out = []
        if faces is not None:
            for f in faces:
                try:
                    fw, fh = float(f[2]), float(f[3])
                    if fw < 8 or fh < 8:      # 噪声过滤: 过小的人脸框直接丢弃
                        continue
                    out.append({"x": float(f[0]), "y": float(f[1]), "w": fw, "h": fh,
                                "score": float(f[14]) if len(f) > 14 else 0.5})
                except (TypeError, ValueError, IndexError):
                    continue
        out.sort(key=lambda d: d["w"] * d["h"], reverse=True)
        return out


def enhance_image(cv2, bgr, clahe=True, sharpen=True):
    """画质增强(取证照/人脸子图通用):
       ① CLAHE 自适应直方图均衡 → 暗部/逆光细节拉起
       ② 反锐化掩模(unsharp mask) → 边缘锐化, 提升主观清晰度"""
    out = bgr
    try:
        if clahe:
            lab = cv2.cvtColor(out, cv2.COLOR_BGR2LAB)
            ch = cv2.split(lab)
            ch[0] = cv2.createCLAHE(clipLimit=2.2, tileGridSize=(8, 8)).apply(ch[0])
            out = cv2.cvtColor(cv2.merge(ch), cv2.COLOR_LAB2BGR)
        if sharpen:
            blur = cv2.GaussianBlur(out, (0, 0), 2.0)
            out = cv2.addWeighted(out, 1.55, blur, -0.55, 0)
    except Exception:
        return bgr
    return out


def crop_face(cv2, bgr, face, out_size=256, margin=0.55):
    """人脸子图裁剪: face 框四周扩 margin 比例 → 放大到 out_size² → CLAHE+锐化增强"""
    h, w = bgr.shape[:2]
    mx, my = face["w"] * margin, face["h"] * margin
    x0 = max(0, int(face["x"] - mx))
    y0 = max(0, int(face["y"] - my - face["h"] * 0.15))   # 上方多留额头/头发
    x1 = min(w, int(face["x"] + face["w"] + mx))
    y1 = min(h, int(face["y"] + face["h"] + my))
    if x1 - x0 < 12 or y1 - y0 < 12:
        return None
    crop = bgr[y0:y1, x0:x1]
    crop = cv2.resize(crop, (out_size, out_size), interpolation=cv2.INTER_CUBIC)
    return enhance_image(cv2, crop)


class FaceMonitor(threading.Thread):
    """全景人脸巡检(约 1Hz, 独立线程): 对最新全景帧跑 YuNet,
       人数变化时广播理解流事件(前端 VIS 行 + 供指挥员判断)。
       全景直接检测的意义: 360° 画面里出现人脸即时报, 无需等取事件触发抽帧。"""

    def __init__(self, hub, frame_provider, scanner, interval=1.2):
        threading.Thread.__init__(self, daemon=True)
        self.hub = hub
        self.frame_provider = frame_provider
        self.faces = scanner
        self.interval = interval
        self.last_count = -1

    def run(self):
        import numpy as np
        import cv2
        while True:
            time.sleep(self.interval)
            if self.frame_provider is None or not self.faces.available():
                continue
            try:
                jpeg, _ts = self.frame_provider()
                if not jpeg:
                    continue
                pano = cv2.imdecode(np.frombuffer(jpeg, np.uint8), cv2.IMREAD_COLOR)
                if pano is None:
                    continue
                found = self.faces.detect(pano)
                n = len(found)
                if n == self.last_count:
                    continue
                first = self.last_count < 0
                self.last_count = n
                if first:
                    continue                      # 启动首帧不上报(避免无意义刷屏)
                if n > 0:
                    best = found[0]
                    self.hub.broadcast_json({
                        "type": "evt", "kind": "VIS", "faces": n,
                        "face_conf": round(float(best["score"]), 2),
                        "text": "全景人脸巡检 ×%d · 最优置信 %.2f · 全景 %dx%d"
                                % (n, best["score"], pano.shape[1], pano.shape[0]),
                        "conf": "%.2f" % best["score"]})
                else:
                    self.hub.broadcast_json({
                        "type": "evt", "kind": "VIS", "faces": 0,
                        "text": "全景人脸巡检: 当前画面无人脸", "conf": "--"})
            except Exception:
                continue


class EvidenceRecorder(object):
    """关键事件抽帧: 取最新全景帧(SDK 拼接流) → 按目标方位重投影为「常规平面照片」
       → CLAHE+锐化增强 → YuNet 人脸检测(标注框 + 人脸子图) → 存 evidence/ 目录
         + base64 推前端证据链(事件含 face/faces/face_conf 字段)。
       触发: person_enter(带 bbox, 目标居中) / audio 呼救(声源方向取景) / pose 意识判断。
       取景换算与 exe 出图模块同源(main.cpp): bbox→lon0/lat0/fov, 输出尺寸随全景分辨率自适应。"""

    def __init__(self, hub, frame_provider, out_dir="evidence",
                 dw=480, dh=640, cooldown=4.0, jpeg_q=88):
        self.hub = hub
        self.frame_provider = frame_provider   # () → (jpeg|None, ts_ms)
        self.out_dir = out_dir
        self.dw, self.dh = dw, dh
        self.cooldown = cooldown
        self.jpeg_q = jpeg_q
        self.lock = threading.Lock()
        self.last_shot = 0.0
        self.count = 0
        self.last_hint = None                  # 最近一次归一化 bbox(供 pose/audio 事件回看)
        self.faces = FaceScanner()             # YuNet 人脸检测(取证照人脸子图)
        self._cv2 = None
        self._np = None
        try:
            if not os.path.isdir(out_dir):
                os.makedirs(out_dir)
        except OSError as e:
            print("[evidence] 目录创建失败: %s" % e)

    # ---- 由 hub.tap 调用(YoloBridge/Mock 广播线程) ----
    def on_event(self, evt):
        if not isinstance(evt, dict):
            return
        t = str(evt.get("type", ""))
        if t in ("evidence", "evt", "stats", "slam", "risk", "person_leave"):
            return
        bbox = None
        if t == "person_enter":
            best = evt.get("best")
            bbox = (best.get("bbox") if isinstance(best, dict) else None) or evt.get("bbox")
            if not (isinstance(bbox, (list, tuple)) and len(bbox) == 4
                    and bbox[2] > 1 and bbox[3] > 1):
                return
            title = "发现被困人员 · 目标已锁定"
            meta = "视觉检测 person_enter · 置信度 %s" % _fmt_conf(
                best.get("score") if isinstance(best, dict) else evt.get("conf"))
        elif t == "audio":
            if str(evt.get("kw", "")) not in ("help", "呼救"):
                return
            title = "声纹呼救 · 声源方向取景"
            meta = "声学事件 · 方位 %s · 置信度 %s" % (evt.get("dir", "--"), _fmt_conf(evt.get("conf")))
        elif t == "pose":
            conf = evt.get("conf")
            if not (isinstance(conf, (int, float)) and conf >= 0.8):
                return
            title = "意识状态确认 · 现场取证"
            meta = "姿态/呼救回应判断 · 置信度 %s" % _fmt_conf(conf)
        else:
            return

        now = time.time()
        with self.lock:
            if now - self.last_shot < self.cooldown:   # 冷却, 防事件风暴刷屏
                return
            self.last_shot = now
        threading.Thread(target=self._capture,
                         args=(t, title, meta, bbox, evt), daemon=True).start()

    def _capture(self, reason, title, meta, bbox, evt):
        cv2, np = self._imports()
        if cv2 is None:
            return
        jpeg, _ts = self.frame_provider() if self.frame_provider else (None, 0)
        if not jpeg:
            print("[evidence] 暂无全景帧可抽(SDK 拼接流未连上?)")
            return
        pano = cv2.imdecode(np.frombuffer(jpeg, np.uint8), cv2.IMREAD_COLOR)
        if pano is None:
            print("[evidence] 全景帧解码失败")
            return
        ph, pw = pano.shape[:2]

        # ---- bbox(推流像素坐标) → 归一化 hint(main.cpp 同款) ----
        if bbox is not None:
            hint = (bbox[0] / pw, bbox[1] / ph, bbox[2] / pw, bbox[3] / ph)
            self.last_hint = hint
        else:
            hint = self.last_hint or (0.5, 0.5, 0.12, 0.24)
        hx, hy, hw, hh = hint

        # ---- 取景参数: 目标居中; 声学事件按声源方向取景 ----
        if reason == "audio":
            lon0 = self._dir_to_lon(evt.get("dir"))
            lat0, fov_y = 0.0, 1.0
        else:
            lon0 = (1.0 - 2.0 * (hx + hw * 0.5)) * math.pi
            lat0 = (0.5 - (hy + hh * 0.5)) * math.pi
            lat0 = min(1.4, max(-1.4, lat0))
            fov_y = (hh * math.pi) / 0.7
            fov_y = min(1.92, max(0.60, fov_y))
        fov_x = 2.0 * math.atan(math.tan(fov_y * 0.5) * 0.75)

        # ---- 输出尺寸随全景分辨率自适应: 高清全景(≥1600 宽)出 720x960, 避免低清源硬放大 ----
        if pw >= 1600:
            dw, dh = 720, 960
        elif pw >= 1280:
            dw, dh = 600, 800
        else:
            dw, dh = self.dw, self.dh

        view = reproject_rectilinear(pano, lon0, lat0, fov_x, dw, dh)
        view = enhance_image(cv2, view)            # CLAHE + 反锐化: 暗部拉起 + 边缘锐化

        # ---- YuNet 人脸检测(取证照为目标居中的近景, 人脸像素充足, 检出率高) ----
        faces = self.faces.detect(view)
        face_jpg = None
        face_best = None
        if faces:
            face_best = faces[0]                   # 最大的一张脸
            fcrop = crop_face(cv2, view, face_best, out_size=288)
            if fcrop is not None:
                ok2, buf2 = cv2.imencode(".jpg", fcrop,
                                         [int(cv2.IMWRITE_JPEG_QUALITY), 92])
                if ok2:
                    face_jpg = buf2.tobytes() if hasattr(buf2, "tobytes") else bytes(buf2)
            # 取证照上标注人脸框 + 置信度(取证语义: 检测结果可视化)
            for f in faces[:3]:
                cv2.rectangle(view, (int(f["x"]), int(f["y"])),
                              (int(f["x"] + f["w"]), int(f["y"] + f["h"])),
                              (0, 220, 220), 2)
                cv2.putText(view, "FACE %.2f" % f["score"],
                            (int(f["x"]), max(12, int(f["y"]) - 6)),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 220, 220), 1, cv2.LINE_AA)

        ok, buf = cv2.imencode(".jpg", view, [int(cv2.IMWRITE_JPEG_QUALITY), self.jpeg_q])
        if not ok:
            print("[evidence] 平面照片编码失败")
            return
        jpg = buf.tobytes() if hasattr(buf, "tobytes") else bytes(buf)

        with self.lock:
            self.count += 1
            n = self.count
        path = os.path.join(self.out_dir, "evidence_%03d_%s.jpg" % (n, reason))
        try:
            with open(path, "wb") as fp:
                fp.write(jpg)
        except OSError as e:
            print("[evidence] 写盘失败: %s" % e)
        # 循环覆盖: evidence 目录只保留最近 KEEP_EVIDENCE 张, 删更旧的(防止写满磁盘)
        self._prune_old()
        az, el = lon0 * 180.0 / math.pi, lat0 * 180.0 / math.pi
        msg = {"type": "evidence", "id": "EV-%03d" % n, "title": title,
               "meta": "%s · 方位 %.0f° 俯仰 %.0f° · 全景帧重投影 %dx%d · CLAHE+锐化增强"
                       % (meta, az, el, dw, dh),
               "az": round(az), "el": round(el), "ts": int(time.time() * 1000),
               "img": "data:image/jpeg;base64," + base64.b64encode(jpg).decode("ascii")}
        if face_jpg is not None and face_best is not None:
            msg["face"] = "data:image/jpeg;base64," + base64.b64encode(face_jpg).decode("ascii")
            msg["faces"] = len(faces)
            msg["face_conf"] = round(face_best["score"], 2)
            msg["meta"] += " · 人脸 ×%d(%.2f)" % (len(faces), face_best["score"])
        elif self.faces.available():
            msg["faces"] = 0
        self.hub.broadcast_json(msg)
        if face_jpg is not None:
            print("[evidence] %s %s (方位 %.0f° · 人脸 ×%d) → %s"
                  % (msg["id"], title, az, len(faces), path))
        else:
            print("[evidence] %s %s (方位 %.0f°) → %s" % (msg["id"], title, az, path))

    KEEP_EVIDENCE = 100          # evidence/ 循环覆盖: 只保留最近 100 张证据照片

    def _prune_old(self):
        """删除 out_dir 中最旧的证据照片, 使总数不超过 KEEP_EVIDENCE"""
        try:
            files = []
            for name in os.listdir(self.out_dir):
                if not name.endswith(".jpg"):
                    continue
                p = os.path.join(self.out_dir, name)
                try:
                    files.append((os.path.getmtime(p), name))
                except OSError:
                    pass
            if len(files) <= self.KEEP_EVIDENCE:
                return
            files.sort()
            for _, name in files[:len(files) - self.KEEP_EVIDENCE]:
                try:
                    os.remove(os.path.join(self.out_dir, name))
                except OSError:
                    pass
        except Exception:
            pass    # 清理失败不影响主流程

    def _imports(self):
        if self._cv2 is None:
            try:
                import cv2
                import numpy as np
                self._cv2, self._np = cv2, np
            except ImportError as e:
                print("[evidence] 缺少 cv2/numpy(%s), 抽帧重投影不可用" % e)
                return None, None
        return self._cv2, self._np

    @staticmethod
    def _dir_to_lon(d):
        """声源方位角(度, 如 "142°") → 全景经度(弧度), 虚拟机位转向声源方向"""
        try:
            deg = float("".join(ch for ch in str(d) if ch.isdigit() or ch == "."))
            return math.radians(deg)
        except ValueError:
            return 0.0


# ═══════════════════════════ 主入口 ═══════════════════════════
def main():
    ap = argparse.ArgumentParser(
        description="AIRSHOT RESCUE · X5 端 WebSocket 服务端（把感知数据推给前端大屏）")
    ap.add_argument("--port", type=int, default=9870, help="WebSocket 监听端口(前端连这个)")
    ap.add_argument("--mock", action="store_true", help="启用仿真事件源(无硬件演示)")
    ap.add_argument("--uvc", default=None, metavar="DEV",
                    help="UVC 设备(X4 网络摄像头模式): /dev/video0 | 索引 0 | auto(自动探测, X4 切入即热切换)")
    ap.add_argument("--uvc-fps", type=float, default=10.0, help="UVC 推流帧率(默认 10)")
    ap.add_argument("--uvc-width", type=int, default=None, help="请求宽度(如 3840)")
    ap.add_argument("--uvc-height", type=int, default=None,
                    help="请求高度(与 --uvc-width 配合, Windows 下自动启用 MJPG 协商高分辨率)")
    ap.add_argument("--jpeg-q", type=int, default=72, help="JPEG 质量 1-100(默认 72)")
    ap.add_argument("--yolo-bridge", action="store_true",
                    help="本地起 TCP 扮演 PC 角色, 桥接 yolo_pose_client.py(喂帧/收事件)")
    ap.add_argument("--bridge-port", type=int, default=9998,
                    help="yolo 桥接 TCP 端口(默认 9998; 9999 留给 exe 的 SDK 拼接流)")
    ap.add_argument("--sdk", default=None, metavar="HOST:PORT",
                    help="★官方 SDK 路径★: 以 RDK 角色连入 PC 端 x4_live_demo.exe 实时拼接流"
                         "(如 127.0.0.1:9999), 360° 全景转推前端 + 理解流事件 INEV 回传 exe")
    ap.add_argument("--no-evidence", action="store_true",
                    help="关闭关键事件抽帧(默认开启: person_enter/呼救/意识判断 → 全景重投影平面照片)")
    ap.add_argument("--evidence-dir", default="evidence",
                    help="抽帧平面照片存盘目录(默认 evidence/)")
    ap.add_argument("--frames", default=None, metavar="DIR",
                    help="循环推送本地 JPEG 目录作为实时帧(无 UVC 硬件的演示数据源)")
    ap.add_argument("--once", action="store_true", help="mock 只播一轮(调试用)")
    args = ap.parse_args()

    if not (args.mock or args.uvc or args.sdk or args.yolo_bridge or args.frames):
        args.mock = True   # 什么都不给 → 默认仿真, 保证开箱即有数据

    hub = Hub()
    bridge = None
    sdk = None
    frames_src = None
    if args.yolo_bridge:
        bridge = YoloBridge(hub, args.bridge_port)
        bridge.start()
    if args.sdk:
        sdk = SdkSource(args.sdk, hub, bridge)
        sdk.start()
    if args.uvc:
        uvc = UvcCapture(args.uvc, hub, bridge, fps=args.uvc_fps,
                         jpeg_q=args.jpeg_q, width=args.uvc_width, height=args.uvc_height)
        uvc.start()
    if args.frames:
        frames_src = FrameLoopSource(args.frames, hub, bridge, fps=args.uvc_fps)
        frames_src.start()
    if args.mock:
        MockSource(hub, loop=not args.once).start()

    # 关键事件抽帧: 取最新全景帧(SDK 拼接流优先, --frames 回放兜底) → 重投影 → 前端证据链
    evidence = None
    frame_src = sdk if sdk is not None else frames_src
    if frame_src is not None and not args.no_evidence:
        evidence = EvidenceRecorder(hub, frame_src.latest_frame, out_dir=args.evidence_dir)
        # 全景人脸巡检(约 1Hz): 与抽帧共用同一个 YuNet 检测器, 人数变化报理解流
        FaceMonitor(hub, frame_src.latest_frame, evidence.faces).start()

    fwd_types = ("person_enter", "person_leave", "pose", "audio")   # exe 关心的理解流事件

    def event_tap(evt):
        """所有下行事件的分接器: ① INEV 回传 exe(驱动其第三视角出图) ② 抽帧取证"""
        if sdk is not None and isinstance(evt, dict) \
                and str(evt.get("type", "")) in fwd_types:
            sdk.send_event(evt)
        if evidence is not None:
            evidence.on_event(evt)

    hub.tap = event_tap

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        srv.bind(("0.0.0.0", args.port))
    except socket.error as e:
        print("[main][错误] 端口 %d 绑定失败: %s" % (args.port, e))
        return 1
    srv.listen(8)
    mode = []
    if args.mock:
        mode.append("MOCK仿真")
    if args.uvc:
        mode.append("UVC:%s" % args.uvc)
    if args.frames:
        mode.append("帧回放:%s" % args.frames)
    if args.sdk:
        mode.append("SDK拼接:%s:%d" % (sdk.host, sdk.port))
    if args.yolo_bridge:
        mode.append("YOLO桥接:%d" % args.bridge_port)
    print("[main] ═══ AIRSHOT RESCUE X5 服务端 ═══")
    print("[main] WebSocket ws://<本机IP>:%d  模式: %s" % (args.port, " + ".join(mode)))
    print("[main] 前端打开 rescue-console/index.html → 右上「接入真实数据流」")
    if evidence is not None:
        print("[main] 关键事件抽帧: 开启 → 全景重投影平面照片存 %s/ + 推前端证据链"
              % args.evidence_dir)

    while True:
        try:
            conn, addr = srv.accept()
        except KeyboardInterrupt:
            print("[main] 退出")
            return 0
        try:
            if ws_handshake(conn):
                hub.add(conn)
                threading.Thread(target=client_loop, args=(hub, conn), daemon=True).start()
            else:
                conn.close()
        except socket.error:
            try:
                conn.close()
            except Exception:
                pass


if __name__ == "__main__":
    sys.exit(main())
