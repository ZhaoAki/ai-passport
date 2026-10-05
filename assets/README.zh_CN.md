<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 资源目录（Assets）

本目录集中存放可复用的资源（字库、图片、音乐等），按资源类型分子目录管理。每个资源放在其类型对应的子目录，并记录放置路径、命名方式、集成方式与来源/许可。二进制资源（字体、图片、音频）不属于纯 markdown 文档，请勿与文档混放。涉及版权/授权的资源需注明来源与许可。

## 字库（fonts）

可复用的字库文件与生成的字库源码放在 `fonts/`。

- 命名要能反映字族、字重、字级与格式。
- 记录来源、许可、字符范围、转换命令与目标放置路径。
- 添加字库前评估 Flash 与内部 RAM 影响；ESP32-C3 无 PSRAM。
- 不提交许可不允许分发的字库。

## 图片（images）

可复用的源图与生成的显示资产放在 `images/`。

| 文件 | 尺寸与格式 | 用途与来源 |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160，JPEG | 嵌入中英文项目 README 的产品主图，突出 AI Passport 产品形象与开放、人人可创作的理念。 |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724，PNG RGBA | 保留为可选技术参考图，不再用于首页主视觉。于 2026-09-17 使用内置图像生成工具为本仓库生成；已根据文档中的硬件能力契约核对图中的六项标签与参数。 |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336，PNG RGBA | 从仓库原始 `images/logo.png` 中精确裁切并去除背景的黑色字标；用于中英文项目 README 的浅色主题。 |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336，PNG RGBA | 提取字标的白色版本；README 使用 `<picture>` 在 GitHub 深色主题下显示。 |
| [`images/lanlan_sprites.c`](images/lanlan_sprites.c)、[`images/lanlan_sprites.h`](images/lanlan_sprites.h) | 6 帧 96 × 96 RGB565，每帧 18,432 字节 | 懒懒角色第二版，含待机两帧、眨眼、歪头、开心和叫声表情。内置 imagegen 沿用原设计并参考用户提供的 GIF 动作。原图和转换校验见 `images/lanlan-v2/`。 |

- 使用描述性命名，并记录尺寸、像素格式、转换步骤与目标路径。
- 优先采用适合 240 × 320 RGB565 显示的格式，并纳入 Flash 与内部 RAM 考量。
- 许可允许时保留可编辑源文件，并记录来源与许可。
- 图片中不得包含设备二维码秘密、凭证或个人数据。

## 音乐与音效（music）

可复用的音乐与音效源码放在 `music/`。

- 记录来源、许可、采样率、位深、声道、转换命令与目标路径。
- 与当前 BSP 音频路径匹配时优先采用 16 kHz、16 位单声道 PCM。
- 嵌入音频前评估 Flash 与内部 RAM 成本；长录音应流式或分块。
- 无再分发许可不提交媒体文件。

## 懒懒应用素材

应用设计见[懒懒应用设计](../docs/applications/cyber-lanlan.zh_CN.md)。以下全部由
`python3 tools/generate_lanlan_assets.py` 生成；脚本可复现，重复运行产生逐字节相同的输出。
`main/lanlan/strings.json` 是固定文案的唯一来源。

- `fonts/NotoSansCJKsc-Regular.otf` 和 `fonts/NotoSansCJK-OFL.txt`：来自 [Noto CJK](https://github.com/notofonts/noto-cjk)，采用 SIL OFL 许可。作为子集的源字体，OTF 本身不编入固件。
- `fonts/lanlan_symbols.txt`：精确的字符清单（ASCII 加上 `main/lanlan/strings.json` 以及 `main/lanlan_record.c`、`main/lanlan_model.c` 中固定标签的全部全角字符）。
- `fonts/lanlan_font_16.c`、`fonts/lanlan_font_24.c`：2 bpp、无压缩的 LVGL 子集，字体名分别为 `lanlan_font_16` 与 `lanlan_font_24`，使用固定版本的 `lv_font_conv` 1.5.3 生成。生成的每个文件头部都记录了确切命令行、转换器与源字体；生成脚本还会逐个校验所需字符确实存在。
- `images/lanlan_sprites.c` 与 `images/lanlan_sprites.h`：按参考形象生成的伙伴动画帧，见上方图片表。
- `music/lanlan_sfx_16k.pcm`：16 kHz、16 位有符号单声道 PCM，包含三段自制短音效（吠叫、欢快提示音、柔和提醒音）。不使用任何第三方音频，音调由生成脚本用正弦波与确定性合成噪声生成。
- `music/lanlan_sfx_manifest.json`：音频段名称、字节偏移、字节长度、每段 SHA-256 以及整包的 SHA-256。`main/lanlan_sfx_data.h` 与 `main/lanlan_sfx_data.c` 向固件暴露同一份偏移。
- 主人手写的备注属于服务端数据，不在本清单内；设备只保留有上限的备注预览，界面会明确标注备注已被截断。

重新生成命令（在仓库根目录执行，转换器与 node 路径可覆盖）：

```bash
python3 tools/generate_lanlan_assets.py \
  --converter /path/to/lv_font_conv/lv_font_conv.js \
  --node /path/to/node
```

`tools/validate.sh` 会运行 `tests/test_lanlan_assets.py`：新增固定文案却未重新生成字体、动画或音频清单，
PCM 与清单不一致，或精灵数据与转换清单不一致时，该测试都会失败。

## 第二版角色素材

`images/lanlan-v2/atlas.png` 保留内置 imagegen 生成的动作图，提示词见 `prompt.txt`。角色沿用此前设计，动作参考用户提供的 12 张透明 GIF；工程不包含原始家庭照片。这是项目专用的生成素材，外观仍待所有者确认。

运行 `python3 tools/import_lanlan_sprites.py` 需要 Pillow。脚本按固定网格切帧，导出网页形象，将设备帧缩小到 96 × 96，并按白色卡片背景转换为 RGB565，写入校验信息。之后运行 `python3 tools/generate_lanlan_assets.py --skip-fonts`，即可由固定素材重建 C 数组，无需再次生成图像。

六帧占用 110,592 字节 Flash，运行时仍只用一个 18,432 字节画布。GIF 中的文字与原始文件不进入应用。

## v0.3 互动声音

PCM 音效包现在有四段，共 47,678 字节：三种原创合成小狗短叫（`bark`、保留旧名的双声 `chirp`、`bark_soft`）和原有提醒铃声。这些是合成声音，不是真实狗狗录音。详见[互动说明](../docs/applications/cyber-lanlan-interaction.zh_CN.md)。
