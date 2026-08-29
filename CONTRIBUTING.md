# Contributing

这个仓库用于黑客松评审和后续技术协作。提交内容应帮助评审理解真实实现，不应暴露凭证、保密 SDK 或无法授权分发的文件。

## 技术代码归属

- `technical/camera-stream/`：Insta360 取流、相机控制、抽帧与媒体获取。
- `technical/multimodal-ai/`：视频与音频理解、Prompt、事件规则和结构化输出。
- `technical/rdk-edge/`：RDK 运行环境、设备接入、端侧任务和部署脚本。
- `technical/robot-control/`：当前手动遥控方式、操作协议，以及未来可接入的机器狗控制接口。
- `technical/system-integration/`：模块编排、消息协议、配置和端到端入口。

每个模块需要自己的 `README.md`，至少说明用途、输入、输出、依赖和运行方式。不要把所有代码直接堆到 `technical/` 根目录。

## 提交前检查

```bash
node --test tests/repository-structure.test.mjs
```

提交前同时确认：

- 不包含 API Key、账号信息、私钥或 `.env`。
- 不包含受 NDA 约束的 Insta360 SDK 文件；只提交你们自行编写的适配代码。
- 不包含本地模型权重、虚拟环境、缓存、原始录屏和剪辑中间文件。
- 单个文件小于 100 MB；较大成片使用压缩版本、Git LFS 或 GitHub Release。
- AI 推断和生命迹象描述不写成医疗诊断结论。

## Git 工作流

```bash
git clone https://github.com/1ivy403/rescue-xiaoan.git
cd rescue-xiaoan
git pull --rebase origin main
```

工程师取得仓库写入权限后，可以在自己的功能分支提交，再合并至 `main`；紧急黑客松交付也可以在确认无冲突后直接更新 `main`。
