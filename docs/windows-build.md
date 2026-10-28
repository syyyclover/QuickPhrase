# Windows 原生构建与运行文件核对

目前不能断定黑闪来自缓存或 Linux 交叉构建。已确认的错版风险是旧交付文件与最新文件混放，以及旧进程仍在时，全局单实例锁使新 EXE 退出并唤起旧进程。构建成功／最终画面正确也不代表偶发黑闪已消失。

## 构建环境

安装 [MSYS2](https://www.msys2.org/)，打开 **MSYS2 UCRT64** 终端。UCRT64 的工具目录和包前缀见 [官方环境说明](https://www.msys2.org/docs/environments/)。先更新 MSYS2；如提示关闭终端，重新打开同一 UCRT64 终端继续更新。

```bash
pacman -Syu
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-make mingw-w64-ucrt-x86_64-python
```

将**完整最新源码工作区**复制到例如 `C:\QuickPhrase`，不要只取旧 Git 提交；当前改动尚未提交，`src/core`、`src/ui`、`src/platform`、`third_party`、资源及安装器都必须包含。使用没有空格的源码路径，现有 Makefile 没有完整支持含空格的路径。

## 全量构建

在 UCRT64 终端执行：

```bash
cd /c/QuickPhrase
mingw32-make -B -j4 SHELL=sh CC=gcc WINDRES=windres BUILD=build/windows-native DIST=dist setup
```

这里使用原生 `mingw32-make`，并指定 MSYS2 的 sh 执行 Makefile 中的 POSIX 命令。`CC=gcc`、`WINDRES=windres` 覆盖仓库默认的 Linux 交叉工具名称。`-B` 强制重编译，`BUILD=build/windows-native` 隔离旧对象；无需运行会删除已有产物的 `make clean`。MSYS2 与原生程序的路径处理差异见 [官方路径说明](https://www.msys2.org/docs/filesystem-paths/)。

只需要主程序时：

```bash
mingw32-make -B -j4 SHELL=sh CC=gcc WINDRES=windres BUILD=build/windows-native app
```

对应主程序是 `build/windows-native/QuickPhrase.exe`；完整 setup 目标还会生成 `dist/QuickPhrase.exe` 和版本化安装包。普通构建使用已入库 ICO，不需要 Pillow，也不会生成图像。

**这些命令已依据当前 Makefile 和官方工具包说明核对；本环境没有执行 Windows 原生构建，不宣称该平台构建或视觉验收已通过。**

## 避免仍运行旧版本

1. 保存当前输入和模板修改，从托盘菜单选择“退出”。关闭窗口的 × 只是隐藏到托盘，旧进程仍存在。
2. 用 PowerShell 核对是否仍有旧进程及其路径：

```powershell
Get-Process QuickPhrase -ErrorAction SilentlyContinue | Select-Object Id, Path
```

3. 明确启动刚构建的文件，不通过旧快捷方式：

```powershell
Start-Process 'C:\QuickPhrase\build\windows-native\QuickPhrase.exe'
Get-Process QuickPhrase -ErrorAction SilentlyContinue | Select-Object Id, Path
Get-FileHash 'C:\QuickPhrase\build\windows-native\QuickPhrase.exe' -Algorithm SHA256
```

不同编译器、CRT 和平台的 SHA-256 通常不同，不要求 Windows 自编译文件与 Linux 交付文件字节相同。哈希用于区分你自己的构建与旧文件。

## 连续切换验收

连续点击最大化／还原／最小化，再从真正的任务栏按钮恢复，记录 Windows 版本、主题、缩放、触发轮数以及黑闪区域。与托盘恢复对照。

可在没有 QuickPhrase 配置、数据或窗口的独立测试环境构建和运行 Win32 测试；不要移动真实数据来绕过测试保护。

```bash
mingw32-make -B -j4 SHELL=sh CC=gcc WINDRES=windres BUILD=build/windows-native build/windows-native/test_win32.exe
./build/windows-native/test_win32.exe --artifacts build/windows-native/win32-artifacts
```

不要在 Windows 使用 `win32-test` 目标，它是 Linux 的 Wine/Xvfb 启动器。压力测试包含 60 轮切换和指定 DC 绘制检查，仍不能替代动画期间的人工视觉检查。
