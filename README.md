# 口径生成器 QuickPhrase

把重复劳动交给模板：选几个下拉、填一两个空，一段结构固定、措辞统一的文案就生成好了，
按 `Ctrl+Enter` 复制走。常驻系统托盘，全局热键（默认 `Alt+Q`）随时唤出。

纯 **C11 + Win32 API + cJSON** 实现，无 Electron / 无浏览器内核 / 无运行时依赖，
静态链接单文件 exe，几百 KB。

---

## 1. 交付物

| 文件 | 说明 |
| --- | --- |
| `dist/QuickPhrase-2.1.0-setup.exe` | **安装包**（自带界面向导，内嵌主程序与卸载器） |
| `dist/QuickPhrase.exe` | 免安装绿色版主程序（可直接双击运行） |

安装包只写 `HKEY_CURRENT_USER` 与用户目录，**不需要管理员权限、不弹 UAC**，
默认安装到 `%LOCALAPPDATA%\Programs\QuickPhrase`。
安装向导可选：桌面快捷方式 / 开始菜单 / 开机自动启动 / 安装后立即运行。
卸载可从「设置 → 应用」或安装目录里的 `uninstall.exe` 进行，卸载时会询问是否保留模板数据。

命令行开关：

```
QuickPhrase.exe            启动后驻留托盘（首次运行会显示主界面）
QuickPhrase.exe --show     启动并显示主界面（快捷方式用的就是这个）
QuickPhrase.exe --tray     强制静默驻留托盘（开机启动项用的就是这个）

QuickPhrase-2.1.0-setup.exe /S    静默安装到默认位置
uninstall.exe /S [/PURGE]         静默卸载（/PURGE 连数据一起删）
```

## 2. 数据位置

| 路径 | 内容 |
| --- | --- |
| `%APPDATA%\QuickPhrase\data.json` | 全部分组与模板（UTF-8，可直接编辑、可版本管理） |
| `%APPDATA%\QuickPhrase\settings.json` | 热键、主题、窗口位置、多选分隔符等 |

写入采用「先写 `.tmp` 再 `MoveFileEx` 覆盖」的原子替换，中途断电不会留下半个文件。
主界面「文件 → 导入 / 导出」可把所有模板打包成一个 JSON 交换，导入支持**合并**（同 id 覆盖、新 id 追加）
与**整体替换**两种模式。

## 3. 使用速览

- 左侧是模板树，支持分组（文件夹）、拖拽模板进分组、搜索框实时过滤（模板名 / 片段标签 / 固定内容都会被搜）。
- 右侧是生成区：模板里定义的每个片段会变成一行控件——
  文本片段 → 输入框（可勾「多行文本」给大文本框），单选片段 → 下拉框，
  多选片段 → 勾选弹层（结果按分隔符拼接），时间片段 → 输入框 + 「现在」按钮
  （打开时自动填当下时间，格式可配）。勾了「隐藏」的变量不在这里出现，只由规则填。
- 同一个模板可以同时开最多 9 份互不干扰的填写：生成区上方的标签条，`Ctrl+T` 新开、
  `Ctrl+1~9` / `Ctrl+Tab` 切换、双击改名、右键可复制这一份或换颜色。
  长时间的两地并行管控不用来回重填（当前只在内存里保存，退出程序不保留）。
- 下方预览区是等宽字体（Consolas）的只读文本，任何输入变化都会立刻重算；
  和左侧模板树之间那两条分隔条都可以拖（预览高度会记住），双击恢复默认。
- `一键复制` 或 `Ctrl+Enter` 把预览内容以 Unicode 文本写入剪贴板，按钮会短暂变成「✓ 已复制」。
- `Ctrl+F` 聚焦搜索，`Ctrl+N` 新建模板，`Ctrl+E` 编辑当前模板，`Ctrl+R` 重置输入，`Esc` 收回托盘。
- 关闭窗口 = 隐藏到托盘；真正退出请用托盘右键菜单「退出」。

### 模板里的三种自动填充

**解析规则**（挂在文本片段上）：从用户输入里抽取信息，自动填进别的变量。

- *按分隔符取段*：输入 `SN-A12-BJ-DBS-01`，用 `-` 分割取第 4 段 → 得到 `DBS`，写入「机型」变量。
- *包含关键词*：输入里出现 `DBS` → 把「机型」直接填成 `DBS 系列`。

**条件规则**（挂在任意片段上）：根据别的变量的值改写本片段的输出。

- 一条规则 = 若干条件项（`[变量] [关系] [匹配值]`）+ 组合方式（全部满足 / 任意满足）+ 结果值。
- 关系支持 包含 / 等于 / 开头是 / 结尾是 / 属于其中之一 / 为空，每项还可勾「不满足」取反，
  于是「不包含」「不为空」之类不需要额外的关系类型。多条规则按列表顺序优先，命中即止。

**引用**（挂在任意变量上）：本变量的值直接取自另一个变量，整体或其中一部分。

- 电话号码填在前面，后文要复述尾号 → 引用它、取「第 8 个字起 4 个」，不用手填两遍。
- 取法有 整体 / 按分隔符取第 N 段 / 从第 N 个字起取 M 个 三种；引用的目标始终跟着来源走，
  在填写界面是只读的。

三者会反复求值直到稳定（最多 8 轮），所以可以串联。同一个变量有多个来源时，
优先级是 **条件规则 > 解析规则 > 引用 > 手动填写 > 默认值**——变量匹配的结果压过手滑
点错的那一下；规则这一轮不再命中时，手填的内容会重新露出来。

## 4. 从源码构建

需要 MinGW-w64 交叉编译器（Linux 上 `dnf install mingw64-gcc` 或 `apt install gcc-mingw-w64-x86-64`）
与 Python 3 + Pillow（只用于生成 `.ico`，`res/*.ico` 已随源码提供）。

```bash
make            # 主程序 + 卸载器 + 安装包，产物在 dist/
make app        # 只出 build/QuickPhrase.exe
make test       # 在本机编译并运行引擎/存储单元测试（不需要 Windows）
make size       # 看产物体积
make clean
```

手工编译（等价于 `make app`）：

```bash
x86_64-w64-mingw32-windres -c 65001 -I. -Isrc resource.rc -O coff -o build/resource.res
x86_64-w64-mingw32-gcc -std=gnu11 -O2 -municode -mwindows -s -static -Isrc \
    src/main.c src/ui.c src/ui_dialogs.c src/engine.c src/storage.c src/cJSON.c \
    build/resource.res -o build/QuickPhrase.exe \
    -luser32 -lgdi32 -lcomctl32 -lshell32 -ladvapi32 -lcomdlg32 -luxtheme -ldwmapi
```

## 5. 源码结构

```
src/engine.c   .h    数据模型 + 规则引擎，纯 C，不含任何窗口 API → 可独立单测
src/storage.c  .h    cJSON 读写 data.json / settings.json、UTF-8↔UTF-16、导入导出
src/main.c           WinMain、单实例、托盘、全局热键、主题、开机启动
src/ui.c             主窗口：模板树、动态字段区、实时预览、复制、深色模式
src/ui_dialogs.c     模板编辑器、解析/条件规则对话框、设置、关于、输入框
src/cJSON.c    .h    第三方 JSON 库（MIT）
installer/           安装程序与卸载程序（同样是原生 Win32 C）
tests/               引擎与存储的自检用例（297 条断言）
```

界面全部用原生控件（Edit / ComboBox / TreeView / ListView / Button）手工布局，
按 DPI 缩放；深色模式走 uxtheme 的 `DarkMode_Explorer` 主题与 `DwmSetWindowAttribute`，
在不支持的旧系统上自动退回浅色，不会报错。

## 6. 许可

本项目代码以 MIT 许可发布；`src/cJSON.*` 版权归 Dave Gamble 及 cJSON 贡献者所有（MIT）。
