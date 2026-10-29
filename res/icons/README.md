# QuickPhrase 3.5.0 图标

使用内置 image 模型分别生成，透明 PNG 母图保留原始输出。

- `app-master.png`：深蓝灰铅笔粗线勾勒的对话气泡，内部三条短文本线，暖白填充。
- `setup-master.png`：同风格向下箭头进入浅托盘，安装和卸载共用。

提示词约束：单个居中图标、素描简笔画、轻微手绘纹理、粗线、小尺寸可辨识、透明背景；无文字、水印、投影、密集排线或额外装饰。

打包：`python3 tools/mkicon.py`，从 PNG 的 alpha 边界归一化留白，输出 16/20/24/32/40/48/64/128/256px ICO。普通构建直接使用 `res/app.ico` 和 `res/setup.ico`，不重新生成图像。

程序母图生成提示词：

> Use case: logo-brand. Asset type: Windows desktop application icon for QuickPhrase, master image to be downsampled to 16–256 pixels. Primary request: a single minimal hand-drawn pencil sketch speech bubble with three short horizontal text strokes inside. Thick clean dark blue-gray graphite outlines, subtly imperfect hand-drawn simplified sketch linework, professional and restrained. Centered square composition, bold compact silhouette, generous transparent margin, sparse details. Interior of bubble warm off-white for visibility on both light and dark backgrounds. Actual transparent background. No letters, no readable text, no watermark, no mockup, no shadows, no hatching, no decorative elements. Generate exactly one icon.

安装器母图生成提示词：

> Generate exactly one desktop installer icon. Minimal hand-drawn pencil sketch: a bold downward arrow entering a shallow rectangular download tray. Thick dark blue-gray graphite outline with subtly imperfect hand-drawn edges. Warm off-white fill inside arrow and tray for contrast. Centered square composition, compact readable silhouette, transparent background and generous transparent margins. Matching a professional minimalist pencil speech-bubble application icon. No words, no lettering, no logos, no watermark, no mockup, no shadows, no hatching, no extra objects. Optimized to remain recognizable at 16 and 32 pixels.
