#!/bin/bash
# 探测 pyeasy_dnn 张量属性名 + 模型输入输出布局
python3 -u - <<'EOF' 2>&1
import numpy as np
from hobot_dnn import pyeasy_dnn as dnn

m = dnn.load("/root/models/yolov8n_pose_bayese_640x640_nv12.bin")[0]
print("=== model dir ===")
print([a for a in dir(m) if not a.startswith("_")])
print("=== input[0] dir ===")
print([a for a in dir(m.inputs[0]) if not a.startswith("_")])
if hasattr(m.inputs[0], "properties"):
    p = m.inputs[0].properties
    print("input properties:", dir(p), "shape:", getattr(p, "shape", "?"), "dtype:", getattr(p, "dtype", "?"))
for i, o in enumerate(m.outputs):
    if hasattr(o, "properties"):
        p = o.properties
        print("output[%d] shape=%s dtype=%s" % (i, getattr(p, "shape", "?"), getattr(p, "dtype", "?")))

# 试 forward：NV12 (H*1.5, W)
try:
    nv12 = np.zeros((960, 640), dtype=np.uint8)
    outs = m.forward(nv12)
    print("=== forward NV12 (960,640) OK ===")
    for i, o in enumerate(outs):
        arr = np.asarray(o.buffer) if hasattr(o, "buffer") else np.asarray(o)
        print(" out[%d] shape=%s dtype=%s" % (i, arr.shape, arr.dtype))
except Exception as e:
    print("forward NV12 failed:", type(e).__name__, e)
EOF
