# C++ Real-time Whisper & Silero VAD POC Demo (耳语实时转录)

本工程是一个基于 C++ 实现的 Windows 实时录音与耳语转录 Proof of Concept (POC) 演示。物理耳语在声学结构上属于**无调清音（几乎无基频）**，能量集中在高频，传统的 WebRTC VAD 难以检测。本 POC 特别通过以下技术进行调优：

1. **Windows 原生 WASAPI 录音**：超低延迟音频采集，支持将原生多声道/高采样率音频线性插值降采样至 16kHz 单声道。
2. **DSP 算法调优**：
   - **高通滤波器 (High-Pass Filter)**：使用 2 阶 Butterworth 高通滤波器强行滤除 150Hz 以下的环境低频噪音，保留耳语的摩擦音。
   - **自动增益控制 (AGC)**：引入平滑系数的增益放大器，针对极其微弱的耳语摩擦音进行最大 30 倍 (30dB) 的动态平滑拉高，使其达到可听和可识别的能量级。
3. **基于深度学习的 Silero VAD**：使用 ONNX Runtime C++ 加载 Silero VAD 模型，并手动调低激活阈值至 `0.25`（默认通常为 0.5），对高频摩擦耳语实现精准的活动检测。
4. **whisper.cpp 本地转录**：集成高性能 C++ 版 Whisper，对 VAD 切分的语音块进行低延迟转录。

---

## 目录结构

```
cpp_whisper_poc/
├── CMakeLists.txt        # CMake 构建文件（自动拉取 whisper.cpp 和下载 ONNX Runtime）
├── README.md             # 本说明文档
└── src/
    ├── main.cpp          # 主程序入口（VAD状态机与转录循环）
    ├── dsp.hpp           # 2阶Butterworth高通滤波与微弱耳语AGC
    ├── vad.hpp           # Silero VAD ONNX Runtime C++ 封装及平滑状态跟踪
    └── recorder.hpp      # Windows WASAPI 录音与实时线性重采样器
```

---

## 依赖准备

在编译和运行程序前，您需要准备好以下两个模型文件并放置在编译输出的二进制目录中（或在运行时通过命令行参数指定它们的路径）：

### 1. Silero VAD 模型 (ONNX)
- 推荐使用 Silero VAD v4 版本。
- **下载地址**：[silero_vad.onnx (Github)](https://github.com/snakers4/silero-vad/raw/master/src/silero_vad/data/silero_vad.onnx)
- 下载后保存为 `silero_vad.onnx`。

### 2. OpenAI Whisper 语言模型 (GGML 格式)
- `whisper.cpp` 要求使用 GGML 格式的模型。
- **推荐模型**：`ggml-tiny.bin` 或 `ggml-base.bin`（对实时性要求高的 POC，建议优先使用 `tiny`）。
- **下载地址**：
  - `ggml-tiny.bin`: [HuggingFace 链接](https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.bin)
  - `ggml-base.bin`: [HuggingFace 链接](https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.bin)
- 下载后保存为 `ggml-tiny.bin` 或 `ggml-base.bin`。

---

## 编译指南

系统环境已装有 **MinGW-W64 (g++ 13.2.0)** 和 **CMake (3.29.2)**，您可以直接在 PowerShell 终端执行以下步骤进行编译：

```powershell
# 1. 进入工程目录
cd d:\work\WhisperPOC\cpp_whisper_poc

# 2. 创建并进入构建目录
mkdir build
cd build

# 3. 使用 CMake 配置工程
# CMAKE_BUILD_TYPE 可设为 Release 以获得最佳性能
cmake -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release ..

# 4. 执行编译
cmake --build . --config Release
```

> **提示**：在 CMake 配置阶段，CMake 将会自动：
> 1. 通过 `FetchContent` 自动拉取并配置 `whisper.cpp` 源码库。
> 2. 自动从 Microsoft 官方仓库下载 Windows x64 预编译好的 **ONNX Runtime 1.17.3 压缩包**并解压。
> 3. 编译完成后，自动将 `onnxruntime.dll` 拷贝至可执行文件所在目录（`build/bin/`）。

---

## 运行与测试

### 1. 放置模型文件
编译完成后，生成的可执行文件位于 `build/bin/whisper_poc.exe`。
为了运行方便，请将下载好的 `silero_vad.onnx` 和 `ggml-tiny.bin` 放到 `build/bin/` 目录下。

### 2. 运行程序
在 PowerShell 中运行：

```powershell
cd d:\work\WhisperPOC\cpp_whisper_poc\build\bin

# 运行（默认加载当前目录下的 ggml-tiny.bin 和 silero_vad.onnx，默认使用中文 zh）
.\whisper_poc.exe
```

您也可以手动指定模型路径和转录语言：
```powershell
# 命令格式：.\whisper_poc.exe <Whisper模型路径> <VAD模型路径> <转写语言>
.\whisper_poc.exe ggml-tiny.bin silero_vad.onnx zh
```

### 3. 操作说明
1. 启动程序后，控制台会输出您的默认录音设备的格式信息。
2. 当 VAD 准备就绪且开始监听时，控制台将显示 `>>> Started Listening! ... <<<`。
3. **开始测试耳语**：贴近麦克风进行物理耳语（Whispering / 摩擦音说话）。
4. **VAD 状态提醒**：
   - 当检测到您开始耳语，控制台会输出：`[VAD] Speech Detected. Recording...`
   - 当您停止耳语，控制台会输出：`[VAD] Silence Detected. Transcribing...`
   - 稍等片刻，您将看到转录出的文字结果：
     ```
     -------------------------------------------------------
       Result: "你好，这是一个耳语实时转录测试。"
       (Inference time: 120ms)
     -------------------------------------------------------
     ```
5. 想要退出程序时，在控制台窗口内按下 **ENTER（回车键）** 即可安全优雅地释放所有音频流、模型资源并退出。
