# MoonlightRecorder

`MoonlightRecorder` 是一个面向“录制”而不是“本地播放”的实验性分支项目，基于 [moonlight-stream/moonlight-qt](https://github.com/moonlight-stream/moonlight-qt) 改造而来。

和原版 Moonlight Qt 不同，这个分支的目标不是在本地解码并播放串流画面，而是保存串流文件到本地

## 当前状态

本仓库目前属于 `alpha / experimental` 阶段。

第一阶段已经实现的核心能力：

- 基于 GUI 的录制工作流
- 经过 Moonlight 重排序后的源码流录制
- 可选本地封装容器
- 基于客户端配置向主机协商 `AV1 / HEVC / H.264`
- 同步录制音频
- Windows 安装包、MSI、便携版打包

后续计划加入：

- 推流 / 转推能力
- 面向直播场景的输出链路

## AI / Vibe Coding 声明

这个 fork 是一个明显带有 `vibe coding` 特征的实验项目。

说得更直白一点：这个 fork 里“录制器相关的胶水层、集成层和不少新增逻辑”主要是通过 AI 辅助开发产出的，而不是由人类逐行手写完成。上游 Moonlight 自身的原始代码仍然归上游维护者和贡献者所有；这个说明主要针对本 fork 额外叠加出来的录制部分。

## 上游来源与许可证

本项目直接基于以下上游项目演化而来：

- [moonlight-stream/moonlight-qt](https://github.com/moonlight-stream/moonlight-qt)
- [moonlight-stream/moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c)

仓库继续沿用上游的 `GPL-3.0` 许可证体系，详见 [LICENSE](LICENSE)。

源码树中保留上游的名称、版权声明、归属信息和历史痕迹是有意为之。本项目不是官方 Moonlight 发行版。

## Windows 构建说明

当前实测通过的 Windows 工具链是：

- `Qt 6.8.3`
- `MSVC 2022 64-bit`
- `Visual Studio 2022 Build Tools`
- `Windows 10/11 SDK`

构建脚本已经改成默认把产物输出到：

- `C:\Users\Administrator\Desktop\MoonlightRecorder-build`

### Windows 构建依赖

- Qt 6.7 或更高版本
- [Visual Studio 2022](https://visualstudio.microsoft.com/downloads/)
- 安装 Qt 时选择 `MSVC`
- 如果你要继续自定义安装包，可选装 [7-Zip](https://www.7-zip.org/)
- 如果要跑 debug 图形调试，可选安装 Graphics Tools

### Windows 构建步骤

1. 打开 Visual Studio Developer Command Prompt，或者确保本机已经有可用的 MSVC 构建环境。
2. 在仓库根目录运行：

```bat
qmake moonlight-qt.pro
jom release
```

3. 如果需要打包 Windows 发行产物，继续运行：

```bat
scripts\build-arch.bat release
scripts\generate-bundle.bat release
```

默认情况下，这些脚本会把产物输出到桌面外置构建目录。

## 当前 Windows 产物

当前打包链会生成：

- `MoonlightRecorderSetup-<version>.exe`
- `MoonlightRecorder.msi`
- `MoonlightRecorderPortable-x64-<version>.zip`

## 致谢

原始串流客户端架构和大部分基础能力，功劳都属于 Moonlight 上游维护者和贡献者。

另外也要感谢：

- [Sunshine](https://github.com/LizardByte/Sunshine)，本项目测试时使用的主机端串流实现
- Moonlight Qt 与 Moonlight Common C 的原始贡献者们

## 贡献说明

如果你想参与贡献，请把这个仓库视为一个实验性 fork，而不是官方上游仓库。

如果你想参与官方 Moonlight 的开发、提 issue 或提交 PR，请前往：

- [moonlight-stream/moonlight-qt](https://github.com/moonlight-stream/moonlight-qt)
