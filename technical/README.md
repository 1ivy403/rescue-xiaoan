# Technical Source

本目录保存能够证明 Rescue Xiaoan 技术链路的源码。评审不需要在本地复现完整硬件环境，但应该能够从代码和说明中确认真实的数据路径、模块职责与控制边界。

| 目录 | 职责 | 主要输入 | 主要输出 |
| --- | --- | --- | --- |
| `camera-stream/` | 360° 相机接入与媒体控制 | X4 视频流、相机事件 | 帧、媒体片段、相机状态 |
| `multimodal-ai/` | 连续视频与音频理解 | 图像帧、ASR 文本、规则 | 人员、风险、事件与建议 |
| `rdk-edge/` | 边缘设备运行和通信 | 设备数据、模型任务 | 推理结果、消息与设备状态 |
| `robot-control/` | 机器狗运动控制 | 路径、停止与绕行指令 | 底盘动作与执行反馈 |
| `system-integration/` | 端到端编排与协议 | 各模块输出 | 指挥中心结构化数据 |

接口与消息结构见[技术实现](../docs/technical-details.md)，提交规则见[CONTRIBUTING.md](../CONTRIBUTING.md)。
