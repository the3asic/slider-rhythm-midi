# Slider Rhythm MIDI

[English](README.md) · 由 **Slider Rhythm Lab** 维护

Slider Rhythm MIDI 是一款面向现场演奏的 MIDI 鼓音序器。用推子切换节奏型，实时调整分频、摇摆和力度，再把 MIDI 送给鼓乐器发声。插件本身只生成 MIDI。

- 1–16 条独立轨道，默认提供底鼓、军鼓、闭镲和开镲。
- 每轨 12 个节奏型，包含 3 个过门，每个节奏型可编辑 1–16 步。
- 支持 ⅓×、½×、1×、2×、3× 分频，以及 Shuffle/Swing、调制、力度、静音和独奏。
- 支持 MIDI Learn。点击 `MIDI LEARN`，选中界面控件，再移动硬件控制器。
- 跟随宿主速度，随工程保存状态。提供 GM、XLN Addictive Drums 2 Standard、XLN XO 和自定义映射起点。

## 下载并安装预编译插件

[下载 Apple Silicon 预览版 ZIP](https://github.com/zengyug-web/slider-rhythm-midi/releases/download/v0.1.0-alpha.1/Slider-Rhythm-MIDI-v0.1.0-alpha.1-macOS-arm64.zip)。退出宿主并解压，在 Finder 中选择「前往」→「前往文件夹」，按宿主复制对应插件：

- **Logic Pro（AU MIDI FX）**：将 `.component` 放入 `~/Library/Audio/Plug-Ins/Components/`。
- **VST3 宿主**：将 `.vst3` 放入 `~/Library/Audio/Plug-Ins/VST3/`。

目标文件夹不存在时可以手动创建。重启宿主，必要时重新扫描插件。此包适用于 Apple Silicon，使用临时签名且未经 Apple 公证，下载后可能被 macOS 或宿主阻止加载。

## 下载源码

打开 [v0.1.0-alpha.1 发布页](https://github.com/zengyug-web/slider-rhythm-midi/releases/tag/v0.1.0-alpha.1)，在 **Assets** 中点击 **Source code (zip)** 并解压。如需自行构建，也可以用终端下载同一版本：

```sh
git clone --branch v0.1.0-alpha.1 --depth 1 https://github.com/zengyug-web/slider-rhythm-midi.git
cd slider-rhythm-midi
```

## 构建与安装

需要 macOS、已完成首次启动设置的 Xcode、Git 和 CMake 3.22 或更新版本。首次构建需要联网获取 JUCE 9.0.2。

在终端进入源码目录（包含 `CMakeLists.txt`），运行：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

构建完成后，退出宿主，再在同一个终端窗口运行对应的安装命令：

**Logic Pro（AU MIDI FX）**

```sh
mkdir -p "$HOME/Library/Audio/Plug-Ins/Components"
ditto "build/SliderRhythmMidi_artefacts/Release/AU/Slider Rhythm MIDI.component" \
  "$HOME/Library/Audio/Plug-Ins/Components/Slider Rhythm MIDI.component"
```

重新打开 Logic，在软件乐器轨的 **MIDI FX** 插槽查找 Slider Rhythm MIDI。如果找不到，可在插件管理器重新扫描，或在终端运行 `auval -v aumi SlrM Slrh` 检查 AU 是否通过验证。

**支持插件 MIDI 输出的宿主（VST3）**

```sh
mkdir -p "$HOME/Library/Audio/Plug-Ins/VST3"
ditto "build/SliderRhythmMidi_artefacts/Release/VST3/Slider Rhythm MIDI.vst3" \
  "$HOME/Library/Audio/Plug-Ins/VST3/Slider Rhythm MIDI.vst3"
```

重新打开宿主并扫描 VST3。将 Slider Rhythm MIDI 的 MIDI 输出路由到鼓乐器轨。Ableton Live 的路由仍待实测。

## Logic Pro 快速上手

1. 创建软件乐器轨，在乐器插槽加载 Ultrabeat 或其他鼓乐器。
2. 在鼓乐器上方的 **MIDI FX** 插槽加载 Slider Rhythm MIDI。
3. 切到 `LIVE GENERATE` 并启动播放。默认输出 MIDI 通道 10，音符 36、38、42、46；可按鼓组调整。
4. 要录下生成的音符，从 MIDI FX 插槽菜单选择 `Record MIDI to Track Here`，启用轨道录音，再开始录制。
5. 回放录好的 MIDI 片段前，切到 `PLAY RECORDED MIDI`，避免音序器和片段同时触发鼓声。

## 预览版状态

AU 已在装有 Logic Pro 13 的 Apple Silicon Mac 上通过格式验证，也已确认 Logic 启用 `Record MIDI to Track Here` 后可录到 MIDI。Intel Mac 兼容性、Ableton Live 路由和 XLN XO 映射仍待实测。

项目采用 **AGPL-3.0-only** 许可证。反馈问题时请附上 macOS、宿主和插件版本，以及复现步骤。参见 [许可证](LICENSE) 和 [贡献指南](CONTRIBUTING.md)。
