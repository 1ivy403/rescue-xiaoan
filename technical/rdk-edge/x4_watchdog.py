# -*- coding: utf-8 -*-
"""X4 取流看门狗: exe 的 SDK 会话偶发数分钟后冻结(视频/音频包计数不再增长, 画面定格),
本脚本每 10s 检查 exe_live.log 最新 [统计] 行的 拼接帧 计数, 连续 3 次(≈30s)无变化即重启 exe;
进程不在也直接拉起。连续拉起失败则 pnputil 重枚举 Insta360 USB 设备后再试。
ws_server(3s) 与 RDK yolo 客户端(5s) 会自动重连, 无需处理。
用法: python x4_watchdog.py   (后台常驻, 动作记录到 watchdog.log)"""
import os
import re
import subprocess
import time

HERE = os.path.dirname(os.path.abspath(__file__))
LOG = os.path.join(HERE, "exe_live.log")
EXE = os.path.join(HERE, "build", "Release", "x4_live_demo.exe")
EXE_NAME = "x4_live_demo.exe"
DEV_ID = r"USB\VID_2E1A&PID_0002\IBMLA2411FQPFV"   # Insta360 X4 (libusbK)
SDK_ROOT = os.path.join(os.path.dirname(HERE), "赛事SDK包（Windows+Linux）",
                        "Windows_CameraSDK-2.1.1_MediaSDK-3.1.3",
                        "Windows_CameraSDK-2.1.1_MediaSDK-3.1.3")
ARGS = ["--duration", "0", "--size", "2560x1280", "--rdk-stream",
        "--rdk-size", "2560x1280", "--rdk-fps", "10"]
POLL_S, FREEZE_POLLS, START_WAIT_S = 10, 3, 25

def log(msg):
    line = "%s %s" % (time.strftime("%m-%d %H:%M:%S"), msg)
    print(line, flush=True)
    try:
        with open(os.path.join(HERE, "watchdog.log"), "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except OSError:
        pass

def exe_running():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq %s" % EXE_NAME, "/FO", "CSV"],
                         capture_output=True).stdout.decode("gbk", "ignore")
    return EXE_NAME.lower() in out.lower()

def last_counter():
    """exe_live.log 最后一条 [统计] 行的 拼接帧 计数; 无则 -1"""
    try:
        with open(LOG, "rb") as f:
            f.seek(0, os.SEEK_END)
            f.seek(max(0, f.tell() - 65536))
            data = f.read().decode("utf-8", "ignore")
        hits = re.findall(r"拼接帧=(\d+)", data)
        return int(hits[-1]) if hits else -1
    except OSError:
        return -1

def start_exe():
    """直接启动 exe(run.ps1 在无控制台环境下 [Console]::OutputEncoding 会抛错),
    PATH 前置 SDK bin 注入运行时 DLL; 输出追加 exe_live.log"""
    env = os.environ.copy()
    bins = []
    for sub in (("MediaSDK-3.1.3-20260128-win64_1769600100370",
                 "MediaSDK-3.1.3-20260128-win64", "MediaSDK", "bin"),
                ("CameraSDK-20250812_192505-2.1.1-win64_1754998240815",
                 "CameraSDK-20250812_192505-2.1.1-win64", "bin")):
        p = os.path.join(SDK_ROOT, *sub)
        if os.path.isdir(p):
            bins.append(p)
    env["PATH"] = ";".join(bins + [env.get("PATH", "")])
    fh = open(LOG, "ab")
    subprocess.Popen([EXE] + ARGS, cwd=HERE, env=env, stdout=fh, stderr=subprocess.STDOUT,
                     creationflags=subprocess.CREATE_NO_WINDOW)
    fh.close()

def restart_usb():
    log("pnputil 重枚举 Insta360 USB 设备 ...")
    subprocess.run(["pnputil", "/restart-device", DEV_ID], capture_output=True)
    time.sleep(8)

def main():
    log("看门狗启动 (监测 %s)" % LOG)
    last_val, frozen, fail_run = -1, 0, 0
    while True:
        time.sleep(POLL_S)
        if not exe_running():
            fail_run += 1
            log("exe 未运行 (连续 %d 次) → 拉起" % fail_run)
            if fail_run >= 3:
                restart_usb()      # 反复拉不起 → 重枚举 USB 后再试
                fail_run = 0
            start_exe()
            time.sleep(START_WAIT_S)   # 留出 X4 同步时间再恢复监测
            if exe_running():
                fail_run = 0
                last_val, frozen = -1, 0
            continue
        fail_run = 0
        val = last_counter()
        frozen = frozen + 1 if val == last_val and val >= 0 else 0
        if last_val != val:
            last_val = val
        if frozen >= FREEZE_POLLS:
            log("计数 %d 连续 %d 次未变(≈%ds) → 判定 SDK 会话冻结, 重启 exe"
                % (val, frozen, frozen * POLL_S))
            subprocess.run(["taskkill", "/F", "/IM", EXE_NAME], capture_output=True)
            time.sleep(2)
            start_exe()
            time.sleep(START_WAIT_S)
            last_val, frozen = -1, 0

if __name__ == "__main__":
    main()
