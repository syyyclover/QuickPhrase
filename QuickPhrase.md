# 口径生成器 —— 完整开发需求（C 语言原生版）

## 项目概述
开发一个 Windows 系统托盘小工具，用于管理和快速生成结构化的重复性文本（口径）。

技术栈：C 语言（C11）+ Win32 API + cJSON
数据存储：本地 JSON 文件（%APPDATA%\QuickPhrase\data.json）
编译工具链：MinGW-w64 (gcc) 或 MSVC，静态链接，最终产物为单个 .exe
目标：极简（<2MB，纯 C 无浏览器内核，通常可压到 500KB 以内）、零额外运行时依赖、快速唤起、原生 Windows 手感

> 说明：不再使用 Tauri/Electron/React 等任何浏览器内核方案。整个界面用原生 Win32 控件（Edit、ComboBox、TreeView、ListView、Dialog）构建，规则引擎用纯 C 实现，无 Web 运行时，因此体积小、内存占用低、启动快。

---

## 核心概念定义

### 模板（Template）
一条口径的结构定义，由若干「片段」按顺序组成。

### 片段类型（Segment）
1. **固定片段（Static）**：纯文字，输出时原样拼接
2. **变量片段（Variable）**：用户每次填写，有以下3种输入方式：
   - `text`：自由文本输入框
   - `select`：下拉单选（预设选项列表）
   - `multi-select`：下拉多选（选中项用分隔符拼接，分隔符可配置）

### 解析规则（ParseRule）
附加在 `text` 类型变量上，用于从输入值中自动提取信息，填充其他变量。
支持两种规则类型（可叠加，按顺序匹配，第一个命中的生效）：
- `split`：按指定分隔符切割字符串，取第 N 段（N 从1开始）
- `contains`：如果输入值包含关键词，则命中

规则命中后，可设置「目标变量」的值自动填充。

### 条件规则（ConditionRule）
控制片段内容的动态变化，格式为：
`如果 [变量A] [operator] [匹配值] → 则 [目标片段或变量] 的内容替换为 [结果值]`

operator 支持：
- `contains`：包含
- `equals`：等于
- `startsWith`：以...开头
- `in`：等于列表中的任意一个

条件规则挂载在固定片段或变量片段上，一个片段可有多条规则，按顺序匹配，命中即停止。
未命中任何规则时使用该片段的默认值。

---

## 真实场景示例（开发参考）

**原始口径：**
经核查由于存储服务器NFV-D-HNZJI-11A-1401-AD05-S-DBS-02一硬盘硬件异常引起告警，维护厂家跟进处理后告警消除，不影响业务。

**模板结构：**
[固定] "经核查由于"
[变量:设备类型] 类型=select, 默认="存储服务器"
└─ 条件规则来自[设备编号]的解析结果自动填充
[变量:设备编号] 类型=text
└─ 解析规则：按"-"分割，取第7段(DBS)
命中"DBS" → 设备类型="存储服务器"
命中"XXX" → 设备类型="XXX设备"（用户可自行配置）
[固定] "一"
[变量:故障部件] 类型=multi-select, 选项=["硬盘","内存","电源","网卡","主板"]
[变量:故障类型] 类型=multi-select, 选项=["硬件","软件","配置","网络"]
[固定] "异常引起告警，"
[变量:处理结果] 类型=select, 默认="维护厂家跟进处理后告警消除"
└─ 条件规则：如果[故障类型]包含"硬件" → 显示"维护厂家跟进处理后告警消除"
如果[故障类型]包含"软件" → 显示"重启服务后告警消除"
（用户可自行添加更多规则）
[固定] "，不影响业务。"
---

## 功能模块

### 1. 系统托盘
- Shell_NotifyIcon(NIM_ADD) 注册托盘图标（.ico 资源，LoadIcon 加载）
- 应用启动即最小化到托盘，不显示任务栏按钮（窗口加 WS_EX_TOOLWINDOW，且启动时不 ShowWindow）
- 托盘图标右键弹出菜单（CreatePopupMenu + TrackPopupMenu）：打开主界面 / 退出
- 全局快捷键 RegisterHotKey（默认 Alt+Q，即 MOD_ALT | 'Q'），WM_HOTKEY 唤起主界面；可在设置中修改
- 主界面点关闭按钮（WM_CLOSE）→ 隐藏窗口（ShowWindow(SW_HIDE)）而非退出程序

### 2. 主界面布局
- 主窗口 800×600，WS_THICKFRAME 可拖拽调整；WM_GETMINMAXINFO 设最小尺寸
- 记住上次位置/尺寸：WM_MOVE/WM_SIZE 时 GetWindowPlacement 保存到配置
- 左侧：TreeView（SysTreeView32）模板列表，支持分组/文件夹
- 右侧：当前模板的生成区域（动态创建子控件）
- 顶部：Edit 搜索框，EN_CHANGE 时过滤模板名称
- 所有子控件在 WM_SIZE 中用 MoveWindow 按比例重排

### 3. 口径生成界面（核心）
选中一个模板后，右侧动态创建控件：
- text → Edit（ES_AUTOHSCROLL）
- select → ComboBox（CBS_DROPDOWNLIST）
- multi-select → 弹出式复选框下拉（自绘：点击按钮弹出一个带 Checkbox 的 ListView 或子窗口，选中项用分隔符拼接）

事件触发：
- Edit 的 EN_CHANGE、ComboBox 的 CBN_SELCHANGE、多选弹窗的选中变化 → 实时触发解析规则和条件规则，相关字段自动更新
- 底部实时预览：只读多行 Edit（ES_READONLY | ES_MULTILINE），等宽字体 Consolas
- 「一键复制」按钮 → OpenClipboard / EmptyClipboard / SetClipboardData(CF_UNICODETEXT)，随后显示短暂"已复制"反馈（如按钮文字短暂变化 + 定时器恢复）
- Ctrl+Enter 加速键（子类化或 WM_KEYDOWN 处理）直接复制

### 4. 模板管理（增删改）
入口：TreeView 右键菜单 或 顶部菜单（CreateMenu / AppendMenu）

**新建/编辑模板界面（模态对话框，DialogBox 或运行时 CreateWindow 构建）：**
- 模板名称输入（Edit）
- 片段列表（ListView LVS_REPORT），支持拖拽排序（LVN_BEGINDRAG / LVN_ITEMCHANGING）
- 每个片段可选类型（固定/文本/单选/多选，ComboBox 选择）
- 固定片段：直接编辑文字内容（Edit）
- 变量片段配置：
  - 变量名称（用于在规则中引用）
  - 占位提示文字
  - 若为 select/multi-select：选项列表（ListBox 增删 + 上移/下移排序）
  - 多选分隔符配置（默认"、"）
  - 默认值设置
- 解析规则配置（仅 text 类型）：
  - 可添加多条规则（ListView）
  - 每条规则：规则类型（split/contains）+ 参数 + 目标变量 + 填充值
- 条件规则配置：
  - 可添加多条规则
  - 每条规则：触发变量 + operator + 匹配值 + 当前片段结果值
  - 支持拖拽调整规则优先级
- 底部实时预览（显示默认值下的口径效果）
- 保存 / 取消按钮

**删除模板：**右键 → 删除 → MessageBox 确认对话框

**模板分组：**TreeView 文件夹，支持新建文件夹、模板拖入文件夹、文件夹可折叠

### 5. 数据导入导出
- 导出全部模板为单个 JSON（GetSaveFileName 对话框）
- 导入 JSON（GetOpenFileName），合并/覆盖两种模式
- 用于备份和在多台电脑间同步

### 6. 设置
- 全局唤起快捷键配置
- 开机自启动开关（写注册表 HKCU\Software\Microsoft\Windows\CurrentVersion\Run）
- 主题：浅色 / 深色 / 跟随系统（深色可用系统深色主题 + SetWindowTheme，跟随系统读注册表 AppsUseLightTheme）
- 多选分隔符全局默认值

---

## 数据结构（C 语言定义，供参考，可调整）

```c
typedef enum { SEG_STATIC, SEG_TEXT, SEG_SELECT, SEG_MULTI_SELECT } SegmentType;
typedef enum { PARSE_SPLIT, PARSE_CONTAINS } ParseRuleType;
typedef enum { OP_CONTAINS, OP_EQUALS, OP_STARTS_WITH, OP_IN } CondOperator;

typedef struct ParseRule {
    char id[64];
    ParseRuleType type;
    char delimiter[16];        // split：分隔符
    int  segment_index;        // split：取第N段，从1开始
    char keyword[128];         // contains：关键词
    char target_segment_id[64];
    char fill_value[256];
    struct ParseRule *next;
} ParseRule;

typedef struct ConditionRule {
    char id[64];
    char trigger_segment_id[64];
    CondOperator op;
    char match_value[256];     // 单个匹配值
    char **match_values;       // op == OP_IN 时的列表
    int  match_value_count;
    char result_value[512];
    struct ConditionRule *next;
} ConditionRule;

typedef struct Segment {
    char id[64];
    SegmentType type;
    char label[128];           // 变量名（static 类型为空）
    char content[1024];        // static 类型的固定内容
    char placeholder[128];
    char **options;            // select/multi-select 的选项
    int  option_count;
    char separator[16];        // multi-select 拼接分隔符，默认 "、"
    char default_value[512];   // 多选默认值可存 "a、b" 形式
    ParseRule *parse_rules;
    ConditionRule *condition_rules;
    struct Segment *next;
} Segment;

typedef struct Template {
    char id[64];
    char name[256];
    char group_id[64];         // 空 = 未分组
    Segment *segments;
    char created_at[32];
    char updated_at[32];
    struct Template *next;
} Template;

typedef struct Group {
    char id[64];
    char name[256];
    int  order;
    struct Group *next;
} Group;
```

存储：cJSON 负责序列化/反序列化到 data.json（UTF-8）。内部字符串统一用 wchar_t（UTF-16），仅在读写文件时用 WideCharToMultiByte / MultiByteToWideChar 与 UTF-8 互转。

---

## 编译与工程结构

```
QuickPhrase/
├── main.c          # WinMain + 消息循环 + 托盘 + 全局热键
├── ui.c / ui.h     # 主窗口与控件布局、模板编辑对话框、设置对话框
├── engine.c / engine.h  # 解析规则 + 条件规则引擎（纯 C，与 UI 解耦，可独立单元测试）
├── storage.c / storage.h  # 模板/分组的 cJSON 读写
├── cJSON.c / cJSON.h  # 第三方单文件 JSON 库
├── resource.rc     # 图标、菜单、对话框资源
└── Makefile 或 CMakeLists.txt
```

编译命令（MinGW-w64 示例）：
```
x86_64-w64-mingw32-gcc -O2 -s -municode -mwindows \
  main.c ui.c engine.c storage.c cJSON.c resource.res \
  -o QuickPhrase.exe \
  -luser32 -lgdi32 -lcomctl32 -lshell32 -ladvapi32 -lcomdlg32
```

要点：
- `-mwindows` 避免弹出控制台窗口；`-s` 剥离符号；静态链接后单 exe 通常 <500KB
- 引擎层（engine.c）不依赖任何窗口 API，便于单元测试和后续移植
- 需要初始化公共控件：InitCommonControlsEx（TreeView/ListView 等需要 comctl32）

---

## UI 设计要求

- 整体风格：简约、专业，接近系统原生风格（直接使用系统控件，不额外美化）
- 主色调：蓝灰色系（可通过控件颜色或深色主题轻度调整）
- 无多余装饰，信息密度适中
- 动画：仅保留必要的反馈（如复制成功提示），不做花哨动效
- 字体：系统默认字体，口径预览区使用等宽字体（Consolas）

------

## 开发优先级

1. 数据结构 + cJSON 本地存储（data.json 读写 + 自测）
2. 主窗口骨架 + 消息循环 + 系统托盘 + 全局快捷键（先跑通最小可用程序）
3. 模板管理（增删改 + 分组）
4. 口径生成界面（动态控件 + 实时预览 + 复制）
5. 解析规则和条件规则引擎（纯 C，独立实现并自测）
6. 导入导出
7. 设置界面
