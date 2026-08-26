# ==============================================================
#  口径生成器 QuickPhrase —— 构建脚本
#
#  make            构建全部：程序 + 卸载器 + 安装包
#  make app        只构建 build/QuickPhrase.exe
#  make setup      构建安装包 dist/QuickPhrase-2.2.0-setup.exe
#  make test       在本机（Linux/任意平台）编译并运行引擎/存储单元测试
#  make size       显示产物体积
#  make clean
#
#  交叉编译工具链：MinGW-w64（x86_64-w64-mingw32-gcc）
# ==============================================================

VERSION   := 2.2.0
CC        := x86_64-w64-mingw32-gcc
WINDRES   := x86_64-w64-mingw32-windres
STRIP     := x86_64-w64-mingw32-strip
HOSTCC    := cc

BUILD     := build
DIST      := dist
SETUP     := $(DIST)/QuickPhrase-$(VERSION)-setup.exe

# C11 但保留 MinGW 扩展：_snwprintf / _wtoi 在 __STRICT_ANSI__ 下会被隐藏
CFLAGS    := -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter \
             -municode -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN \
             -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 -D_WIN32_IE=0x0600 -Isrc
LDFLAGS   := -mwindows -s -static -Wl,--gc-sections -ffunction-sections -fdata-sections
LIBS      := -luser32 -lgdi32 -lcomctl32 -lshell32 -ladvapi32 -lcomdlg32 \
             -luxtheme -lole32 -loleaut32 -luuid -ldwmapi

APP_SRC   := src/main.c src/ui.c src/ui_dialogs.c src/engine.c src/storage.c src/cJSON.c
APP_HDR   := src/app.h src/ui.h src/engine.h src/storage.h src/resource.h
APP_EXE   := $(BUILD)/QuickPhrase.exe
UNI_EXE   := $(BUILD)/uninstall.exe

ICONS     := res/app.ico res/setup.ico

.PHONY: all app setup test size objsize clean dirs icons

all: setup

dirs:
	@mkdir -p $(BUILD) $(DIST)

# ---------------- 图标（PIL 生成，已生成则跳过） ----------------
icons: $(ICONS)

$(ICONS): tools/mkicon.py
	@mkdir -p res
	python3 tools/mkicon.py

# ---------------- 主程序 ----------------
$(BUILD)/resource.res: resource.rc res/app.manifest res/app.ico | dirs
	$(WINDRES) -c 65001 -I. -Isrc -Ires resource.rc -O coff -o $@

app: $(APP_EXE)

$(APP_EXE): $(APP_SRC) $(APP_HDR) $(BUILD)/resource.res | dirs
	$(CC) $(CFLAGS) $(APP_SRC) $(BUILD)/resource.res -o $@ $(LDFLAGS) $(LIBS)

# ---------------- 卸载程序 ----------------
$(BUILD)/uninstall.res: installer/uninstall.rc installer/setup.manifest res/setup.ico | dirs
	$(WINDRES) -c 65001 -I. -Iinstaller -Ires installer/uninstall.rc -O coff -o $@

$(UNI_EXE): installer/uninstall.c installer/setup_res.h $(BUILD)/uninstall.res | dirs
	$(CC) $(CFLAGS) installer/uninstall.c $(BUILD)/uninstall.res -o $@ \
	    $(LDFLAGS) -luser32 -lshell32 -lole32 -ladvapi32

# ---------------- 安装程序（内嵌上面两个 exe） ----------------
$(BUILD)/setup.res: installer/setup.rc installer/setup.manifest res/setup.ico \
                    $(APP_EXE) $(UNI_EXE) | dirs
	$(WINDRES) -c 65001 -I. -Iinstaller -Ires installer/setup.rc -O coff -o $@

setup: $(SETUP)

$(SETUP): installer/installer.c installer/setup_res.h $(BUILD)/setup.res | dirs
	$(CC) $(CFLAGS) installer/installer.c $(BUILD)/setup.res -o $@ \
	    $(LDFLAGS) -luser32 -lgdi32 -lcomctl32 -lshell32 -ladvapi32 -lole32 -luuid
	@cp $(APP_EXE) $(DIST)/QuickPhrase.exe
	@$(MAKE) --no-print-directory size

# ---------------- 单元测试（纯 C，可在本机跑） ----------------
test: dirs
	$(HOSTCC) -std=gnu11 -O1 -g -Wall -Isrc -o $(BUILD)/test_engine \
	    tests/test_engine.c src/engine.c
	$(HOSTCC) -std=gnu11 -O1 -g -Wall -Isrc -o $(BUILD)/test_storage \
	    tests/test_storage.c src/engine.c src/storage.c src/cJSON.c
	@$(BUILD)/test_engine
	@$(BUILD)/test_storage

size:
	@echo "--------------------------------------------"
	@ls -l $(APP_EXE) $(UNI_EXE) $(SETUP) 2>/dev/null | \
	    awk '{printf "  %-46s %8.1f KB\n", $$9, $$5/1024}'
	@echo "--------------------------------------------"

# 各编译单元的目标文件体积（看代码量都堆在哪儿，不产出可运行程序）
objsize: dirs
	@for f in main ui ui_dialogs engine storage cJSON; do \
	    $(CC) $(CFLAGS) -ffunction-sections -fdata-sections \
	        -c src/$$f.c -o $(BUILD)/objsize_$$f.o; done
	@ls -l $(BUILD)/objsize_*.o | \
	    awk '{printf "  %-40s %8.1f KB\n", $$9, $$5/1024}' | sort -k2 -rn

clean:
	rm -rf $(BUILD) $(DIST)
