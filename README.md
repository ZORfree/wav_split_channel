# WAV 音频通道分离工具 / WAV Audio Channel Splitter

[中文](#简介) | [English](#Introduction)

## 简介
专业的WAV/PCM音频文件通道分离工具，支持多线程处理和实时进度显示。通过可视化界面轻松实现音频文件的通道分离和格式转换。

全系列由AI完成

## 功能特性
✅ 支持WAV/PCM格式输入输出  
⚡ 多线程并行处理加速  
📥 拖放文件/文件夹快速导入  
📊 实时进度条和状态显示  
⚙️ 可配置采样率/位深度/通道数  

## 安装说明
1. 克隆仓库：
```bash
git clone https://github.com/ZORfree/wav_split_channel.git
```
2. 使用Visual Studio 2022打开解决方案文件
3. 编译并运行（需安装Windows SDK 10.0+）
4. 添加依赖项 comctl32.lib;shlwapi.lib

## 使用指南
1. 点击「导入」选择文件或拖放文件到窗口  
2. 设置输出参数（默认自动匹配源文件格式）  
3. 点击「开始处理」执行通道分离  
4. 处理完成后在源文件目录查看输出文件

## 配置选项
| 参数          | 选项                      |
|---------------|--------------------------|
| 采样率        | (必选)                    |
| 位深度        | (必选)                    |
| 输出格式      | (必选)                    |
| 线程数        | (必选)                    |

## 贡献指南
欢迎提交Issue或PR！请遵循以下规范：
- 使用C++17标准  
- 保持Windows API调用兼容性  
- 添加必要的单元测试  
- 提交前运行clang-format

---

# Introduction
Professional WAV/PCM audio channel splitting tool with multi-threading and real-time progress monitoring. Easily split audio channels and convert formats through GUI.

## Features
✅ WAV/PCM input/output support  
⚡ Multi-threaded processing  
📥 Drag-n-drop files/folders  
📊 Real-time progress tracking  
⚙️ Configurable sample rate/bit depth/channels  
📤 Mono/Stereo separation output

## Installation
1. Clone repo:
```bash
git clone https://github.com/ZORfree/wav_split_channel.git
```
2. Open solution in Visual Studio 2022
3. Build & Run (Requires Windows SDK 10.0+)

## Usage
1. Click "Import" or drag files to window  
2. Set output parameters (auto-match source format by default)  
3. Click "Start Processing"  
4. Find output files in source directory

## Configuration
| Parameter     | Options                  |
|---------------|--------------------------|
| Sample Rate   | 8000-192000 Hz           |
| Bit Depth     | 16bit/24bit/32bit        |
| Output Format | WAV/PCM                  |
| Thread Count  | 1-8 (Auto-suggest based on CPU cores)|

## Contributing
Issues and PRs are welcome! Please follow:
- Use C++17 standard  
- Maintain Windows API compatibility  
- Add unit tests  
- Run clang-format before commit

## License
MIT License