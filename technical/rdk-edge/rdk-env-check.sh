#!/bin/bash
# RDK 环境自检：依赖版本 + 模型目录
echo "=== python3 ==="
python3 --version
echo "=== cv2 ==="
python3 -c "import cv2; print(cv2.__version__)" 2>&1
echo "=== numpy ==="
python3 -c "import numpy; print(numpy.__version__)" 2>&1
echo "=== hobot_dnn ==="
python3 -c "from hobot_dnn import pyeasy_dnn; print('OK')" 2>&1
echo "=== /root/models ==="
ls -la /root/models/ 2>/dev/null || echo "NO_MODELS_DIR"
echo "=== 磁盘 ==="
df -h /root | tail -1
