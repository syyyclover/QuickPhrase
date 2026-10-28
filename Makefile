# QuickPhrase — C11 / Win32. Run make test, make sanitize, make setup.
VERSION := $(shell sed -n 's/^\#define QP_VERSION_STR "\([^"]*\)"/\1/p' src/platform/version.h)
CC := x86_64-w64-mingw32-gcc
WINDRES := x86_64-w64-mingw32-windres
HOSTCC ?= cc
BUILD ?= build
DIST ?= dist
CORE_OPT ?= -O2
UI_OPT ?= -Os
LTO ?= 0
INCLUDES := -Isrc/core -Isrc/ui -Isrc/platform -Ithird_party/cjson
CFLAGS := -std=gnu11 -Wall -Wextra -Wno-unused-parameter -municode \
          -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0601 \
          -DWINVER=0x0601 -D_WIN32_IE=0x0600 -DCJSON_HIDE_SYMBOLS \
          -ffunction-sections -fdata-sections $(INCLUDES)
HOSTCFLAGS ?= -std=gnu11 -O1 -g -Wall
BENCH_OPT ?= $(CORE_OPT)
ifeq ($(LTO),1)
CFLAGS += -flto
LDFLAGS_EXTRA := -flto
BENCH_OPT += -flto
endif
CONFIG_ID := $(shell printf '%s\n' '$(CFLAGS)|$(CORE_OPT)|$(UI_OPT)|$(LTO)|$(CC)|$(HOSTCC)|$(HOSTCFLAGS)' | sha256sum | cut -c1-16)
LDFLAGS := -municode -mwindows -s -static -Wl,--gc-sections $(LDFLAGS_EXTRA)
LIBS := -luser32 -lgdi32 -lcomctl32 -lshell32 -ladvapi32 -lcomdlg32 \
        -luxtheme -lole32 -loleaut32 -luuid -ldwmapi
CORE_SRC := $(wildcard src/core/*.c) third_party/cjson/cJSON.c
APP_SRC := $(CORE_SRC) $(wildcard src/ui/*.c) $(wildcard src/platform/*.c)
APP_OBJ := $(patsubst %.c,$(BUILD)/app/$(CONFIG_ID)/%.o,$(APP_SRC))
INSTALL_OBJ := $(patsubst %.c,$(BUILD)/app/$(CONFIG_ID)/%.o,installer/installer.c installer/uninstall.c)
APP_HDR := $(wildcard src/core/*.h src/ui/*.h src/platform/*.h third_party/cjson/*.h)
HOST_SRC := $(CORE_SRC) src/platform/launch.c
HOST_OBJ := $(patsubst %.c,$(BUILD)/host/$(CONFIG_ID)/%.o,$(HOST_SRC))
TEST_NAMES := textview regex engine table lookup storage launch final_review encoding rule_layout model_failures json_failures rule_transfer editor_layout table_keys table_scope table_key_failures render_status eval_workspace table_match allocator_instrumentation dialog_viewport read_failures blank_marks paragraph_join choice
TEST_NAMES += portable_storage portable_failures
TEST_BINS := $(addprefix $(BUILD)/test_,$(TEST_NAMES))
.SECONDARY: $(HOST_OBJ) $(APP_OBJ) $(INSTALL_OBJ)
PY_TESTS := regex_diff preview_mode preview_geometry editor_geometry editor_interaction \
            lookup_geometry lookup_interaction preview_table_width table_dialog_repaint storage_atomic layout_fallback input_limits ui_data_transactions preview_allocations manifest_validation icon_resources rule_transactions condition_transactions session_transactions field_command_lifetime settings_transactions autostart_snapshot theme_transactions layout_damage paint_failures static_paint_failures window_workarea table_cache_race copy_status ui_copy_audit preview_refresh editor_preview_context session_actions modal_viewport popup_limits editor_commit installer_extract startup_palette preview_publication lookup_columns resizable_viewport window_transitions table_insert window_print preview_clip editor_time_selection custom_choice_interaction custom_choice_dispatch custom_choice_sync popup_cursor host_erase_repaint window_erase_repaint crash_report choice_realign
PY_TESTS += editor_wheel window_show restore_publication editor_resize editor_paint editor_resize_cost column_resize_paint
APP_EXE := $(BUILD)/QuickPhrase.exe
UNI_EXE := $(BUILD)/uninstall.exe
SETUP := $(DIST)/QuickPhrase-$(VERSION)-setup.exe
ICON_CHECK := $(DIST)/QuickPhrase-icon-check.exe
ACTIVE_CONFIG := $(BUILD)/active-config

.PHONY: all app setup dirs test sanitize icons icon-check size check-size clean objsize check-manifests win32-test bench FORCE
all: setup
dirs:
	@mkdir -p $(BUILD) $(DIST)
$(ACTIVE_CONFIG): FORCE | dirs
	@qp_config_tmp='$@.tmp.'$$$$; printf '%s\n' '$(CONFIG_ID)' > "$$qp_config_tmp"; \
	if ! cmp -s "$$qp_config_tmp" '$@'; then mv "$$qp_config_tmp" '$@'; else rm -f "$$qp_config_tmp"; fi

# AI masters are committed; ordinary builds never regenerate artwork.
icons:
	python3 tools/mkicon.py

check-manifests:
	python3 tests/manifest_validation.py
$(BUILD)/resource.res: resource.rc res/app.manifest res/app.ico src/ui/resource.h src/platform/version.h | dirs check-manifests
	$(WINDRES) -c 65001 -I. $(INCLUDES) resource.rc -O coff -o $@
app: $(APP_EXE)
$(BUILD)/app/$(CONFIG_ID)/%.o: %.c $(APP_HDR) Makefile | dirs
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(if $(filter src/core/% third_party/%,$<),$(CORE_OPT),$(UI_OPT)) -MMD -MP -c $< -o $@
-include $(APP_OBJ:.o=.d) $(INSTALL_OBJ:.o=.d)
$(APP_EXE): $(APP_OBJ) $(BUILD)/resource.res $(ACTIVE_CONFIG) | dirs
	$(CC) $(APP_OBJ) $(BUILD)/resource.res -o $@ $(LDFLAGS) $(LIBS)
	python3 tests/manifest_validation.py $(APP_EXE)
	python3 tests/icon_resources.py $(APP_EXE)

$(BUILD)/uninstall.res: installer/uninstall.rc installer/setup.manifest res/setup.ico installer/setup_res.h src/platform/version.h | dirs check-manifests
	$(WINDRES) -c 65001 -I. -Iinstaller $(INCLUDES) installer/uninstall.rc -O coff -o $@
$(UNI_EXE): $(BUILD)/app/$(CONFIG_ID)/installer/uninstall.o installer/setup_res.h $(BUILD)/uninstall.res $(ACTIVE_CONFIG) | dirs
	$(CC) $< $(BUILD)/uninstall.res -o $@ $(LDFLAGS) -municode -luser32 -lshell32 -lole32 -ladvapi32
$(BUILD)/setup.res: installer/setup.rc installer/setup.manifest res/setup.ico $(APP_EXE) $(UNI_EXE) src/platform/version.h | dirs check-manifests
	$(WINDRES) -c 65001 -I. -Iinstaller $(INCLUDES) -DQP_EMBED_APP=\\\"$(abspath $(APP_EXE))\\\" -DQP_EMBED_UNINSTALL=\\\"$(abspath $(UNI_EXE))\\\" installer/setup.rc -O coff -o $@
setup: $(SETUP)
$(SETUP): $(BUILD)/app/$(CONFIG_ID)/installer/installer.o installer/setup_res.h $(BUILD)/setup.res $(ACTIVE_CONFIG) | dirs
	$(CC) $< $(BUILD)/setup.res -o $@ $(LDFLAGS) -municode -luser32 -lgdi32 -lcomctl32 -lshell32 -ladvapi32 -lole32 -luuid
	python3 tests/manifest_validation.py $(APP_EXE) $(UNI_EXE) $(SETUP)
	python3 tests/icon_resources.py $(APP_EXE) $(UNI_EXE) $(SETUP) --embedded-app $(APP_EXE) --embedded-uninstall $(UNI_EXE)
	python3 tests/size_budget.py --app $(APP_EXE) --setup $(SETUP)
	@cp $(APP_EXE) $(DIST)/QuickPhrase.exe
	@$(MAKE) --no-print-directory size

$(ICON_CHECK): tools/windows_icon_check.c | dirs
	$(CC) $(CFLAGS) $(UI_OPT) tools/windows_icon_check.c -o $@ -static -luser32 -lgdi32 -lshell32 -ladvapi32 -lole32
icon-check: $(ICON_CHECK)

$(BUILD)/host/$(CONFIG_ID)/%.o: %.c $(APP_HDR) Makefile
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOSTCFLAGS) $(INCLUDES) -c $< -o $@
$(BUILD)/test_%: tests/test_%.c $(HOST_OBJ) $(APP_HDR) $(wildcard tests/*.h) $(ACTIVE_CONFIG) | dirs
	$(HOSTCC) $(HOSTCFLAGS) $(HOST_TEST_CFLAGS) $(INCLUDES) $< $(HOST_OBJ) -o $@ $(HOST_TEST_LDFLAGS)
FAULT_TEST_BINS := $(addprefix $(BUILD)/test_,model_failures json_failures rule_transfer table_key_failures render_status eval_workspace table_match allocator_instrumentation)
$(BUILD)/test_read_failures: HOST_TEST_LDFLAGS += -Wl,--wrap=fread,--wrap=ferror,--wrap=fseek,--wrap=fclose
FAULT_TEST_BINS += $(BUILD)/test_portable_failures
$(BUILD)/test_portable_failures: HOST_TEST_LDFLAGS += -Wl,--wrap=write,--wrap=fsync,--wrap=close
$(FAULT_TEST_BINS): HOST_TEST_LDFLAGS += -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
$(FAULT_TEST_BINS): HOST_TEST_CFLAGS += -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free
test: $(TEST_BINS)
	@set -e; for name in $(TEST_NAMES); do $(BUILD)/test_$$name; done
	@set -e; for name in $(PY_TESTS); do python3 tests/$$name.py; done
	python3 tools/audit_ui_copy.py --check
	python3 tests/build_configuration.py
	python3 tests/size_budget.py --self-test
sanitize:
	$(MAKE) BUILD=build/sanitize HOSTCC=clang HOSTCFLAGS='-std=gnu11 -O1 -g -Wall -fsanitize=address,undefined -fno-omit-frame-pointer' test
$(BUILD)/bench_core: tests/bench_core.c $(CORE_SRC) $(APP_HDR) Makefile $(ACTIVE_CONFIG) | dirs
	$(HOSTCC) -std=gnu11 $(BENCH_OPT) $(INCLUDES) tests/bench_core.c $(CORE_SRC) -o $@
bench: $(BUILD)/bench_core
	python3 tools/bench_compare.py --baseline $(BUILD)/bench_core --candidate $(BUILD)/bench_core
$(BUILD)/test_win32.exe: tests/test_win32.c $(APP_SRC) $(APP_HDR) $(BUILD)/resource.res | dirs
	$(CC) $(CFLAGS) $(CORE_OPT) tests/test_win32.c $(APP_SRC) $(BUILD)/resource.res -o $@ -static $(LIBS)
	python3 tests/manifest_validation.py $(BUILD)/test_win32.exe
win32-test: $(BUILD)/test_win32.exe
	python3 tests/run_win32.py
$(BUILD)/test_restore_frame_win32.exe: tests/test_restore_frame_win32.c $(APP_OBJ) $(BUILD)/resource.res | dirs
	$(CC) $(CFLAGS) $(UI_OPT) $< $(APP_OBJ) $(BUILD)/resource.res -o $@ -static $(LIBS)
	python3 tests/manifest_validation.py $@
$(BUILD)/test_restore_visibility_win32.exe: tests/test_restore_visibility_win32.c tests/win32_desktop_capture.h $(APP_OBJ) $(BUILD)/resource.res | dirs
	$(CC) $(CFLAGS) $(UI_OPT) $< $(APP_OBJ) $(BUILD)/resource.res -o $@ -static -Wl,--wrap=settings_save,--wrap=storage_save $(LIBS) -ld3d11 -ldxgi
	python3 tests/manifest_validation.py $@
$(BUILD)/test_editor_resize_win32.exe: tests/test_editor_resize_win32.c tests/win32_desktop_capture.h $(APP_OBJ) $(BUILD)/resource.res | dirs
	$(CC) $(CFLAGS) $(UI_OPT) $< $(filter-out %/src/ui/dialog_editor.o,$(APP_OBJ)) $(BUILD)/resource.res -o $@ -static -Wl,--wrap=settings_save,--wrap=storage_save $(LIBS) -ld3d11 -ldxgi
	python3 tests/manifest_validation.py $@
size:
	@ls -lh $(APP_EXE) $(UNI_EXE) $(SETUP)
check-size: setup
	python3 tests/size_budget.py --app $(APP_EXE) --setup $(SETUP)
objsize: dirs
	@set -e; for src in $(APP_SRC); do name=$$(basename $$src .c); $(CC) $(CFLAGS) -c $$src -o $(BUILD)/objsize_$$name.o; done
	@ls -lh $(BUILD)/objsize_*.o
clean:
	rm -rf build dist
