#!/bin/bash
# 崩溃取证: 重启原因 / 温度 / 内核日志关键行
echo "=== uptime / last ==="
uptime
last reboot 2>/dev/null | head -5
echo "=== 温度 ==="
for z in /sys/class/thermal/thermal_zone*; do
  t=$(cat $z/type 2>/dev/null); v=$(cat $z/temp 2>/dev/null)
  echo "$z: $t = $v"
done
echo "=== dmesg 关键行(错误/崩溃/温度/复位) ==="
dmesg 2>/dev/null | grep -iE "error|crash|panic|thermal|reset|watchdog|oops|bpu|hobot|dnn|uvdv|vpm" | tail -30
echo "=== 上次关机记录 ==="
journalctl --list-boots 2>/dev/null | tail -5
last -x shutdown 2>/dev/null | head -3
