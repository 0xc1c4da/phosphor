.DEFAULT_GOAL := all

CXX       ?= g++
OBJCOPY   ?= objcopy
EXE       = phosphor
# Allow overriding ImGui location (e.g. from `nix develop` via env var).
# If you build outside of Nix, set IMGUI_DIR to a checkout of ocornut/imgui.
IMGUI_DIR ?= vendor/imgui
BUILD_DIR = build

# Make it obvious when vendor/ is removed and IMGUI_DIR isn't set.
ifeq ($(wildcard $(IMGUI_DIR)/imgui.h),)
$(error IMGUI_DIR '$(IMGUI_DIR)' does not contain imgui.h. Set IMGUI_DIR=/path/to/imgui (or use `nix develop` which sets it automatically).)
endif

SOURCES  = \
           src/app/main.cpp \
           src/core/canvas.cpp \
           src/core/embedded_assets.cpp \
           src/core/key_bindings.cpp \
           src/core/paths.cpp \
           src/core/xterm256_palette.cpp \
           src/ansl/ansl_script_engine.cpp \
           src/ansl/ansl_luajit.cpp \
           src/ansl/ansl_sort.cpp \
           src/ui/ImGuiDatePicker.cpp \
           src/ui/ansl_editor.cpp \
           src/ui/ansl_params_ui.cpp \
           src/ui/character_palette.cpp \
           src/ui/character_picker.cpp \
           src/ui/character_set.cpp \
           src/ui/colour_picker.cpp \
           src/ui/colour_palette.cpp \
           src/ui/imgui_window_chrome.cpp \
           src/ui/image_window.cpp \
           src/ui/image_to_chafa_dialog.cpp \
           src/ui/layer_manager.cpp \
           src/ui/preview_window.cpp \
           src/ui/skin.cpp \
           src/ui/settings.cpp \
           src/ui/sauce_editor_dialog.cpp \
           src/ui/tool_palette.cpp \
           src/io/ansi_importer.cpp \
           src/io/sauce.cpp \
           src/io/binary_codec.cpp \
           src/io/image_loader.cpp \
           src/io/io_manager.cpp \
           src/io/sdl_file_dialog_queue.cpp \
           src/io/session/imgui_persistence.cpp \
           src/io/session/open_canvas_codec.cpp \
           src/io/session/project_state_json.cpp \
           src/io/session/session_state.cpp
SOURCES += $(IMGUI_DIR)/imgui.cpp \
           $(IMGUI_DIR)/imgui_demo.cpp \
           $(IMGUI_DIR)/imgui_draw.cpp \
           $(IMGUI_DIR)/imgui_tables.cpp \
           $(IMGUI_DIR)/imgui_widgets.cpp
SOURCES += $(IMGUI_DIR)/misc/cpp/imgui_stdlib.cpp
SOURCES += $(IMGUI_DIR)/backends/imgui_impl_sdl3.cpp \
           $(IMGUI_DIR)/backends/imgui_impl_vulkan.cpp

OBJS     = $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(SOURCES))

# ---------------------------------------------------------------------------
# Embedded assets (assets/ -> tar -> zstd -> linked into the binary)
# ---------------------------------------------------------------------------
ASSETS_ARCHIVE = $(BUILD_DIR)/phosphor_assets.tar.zst
ASSETS_OBJ     = $(BUILD_DIR)/phosphor_assets_blob.o

OBJS += $(ASSETS_OBJ)

CXXFLAGS ?= -std=c++20 -O3
CXXFLAGS += -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends -Isrc
# Enable 32-bit ImWchar in Dear ImGui so Unscii glyphs above U+FFFF render correctly.
CXXFLAGS += -DIMGUI_USE_WCHAR32
CXXFLAGS += -g -Wall -Wextra -Wformat

# Auto-generate header dependency files so changes to .h trigger rebuilds.
# This prevents ODR/layout mismatches (e.g. when LayerManager gains std::string members).
DEPFLAGS = -MMD -MP
DEPS     = $(OBJS:.o=.d)

# SDL3 + Vulkan + Chafa + nlohmann_json + zstd flags provided by the Nix dev shell (see flake.nix).
# LuaJIT and ICU67 headers and libraries are also made available via the dev shell.
CXXFLAGS += $(shell pkg-config --cflags sdl3 vulkan chafa nlohmann_json luajit libzstd)
LIBS     = $(shell pkg-config --libs sdl3 vulkan chafa icu-uc icu-i18n luajit libzstd) -ldl

# Optional, provided by the Nix dev shell (see flake.nix).
# libnoise in nixpkgs is static-only, so we link it explicitly via these env vars.
CXXFLAGS += $(LIBNOISE_CFLAGS)
LIBS     += $(LIBNOISE_LIBS)

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c -o $@ $<

$(ASSETS_ARCHIVE): $(shell find assets -type f)
	@mkdir -p $(dir $@)
	@tmp="$@.tmp"; \
	rm -f "$$tmp"; \
	tar --format=ustar -C assets -cf - . | zstd -q -19 -o "$$tmp"; \
	mv -f "$$tmp" "$@"

$(ASSETS_OBJ): $(ASSETS_ARCHIVE)
	@mkdir -p $(dir $@)
	ld -r -b binary -o $@ $<
	$(OBJCOPY) --add-section .note.GNU-stack=/dev/null --set-section-flags .note.GNU-stack=contents $@

all: $(EXE)

$(EXE): $(OBJS)
	$(CXX) -o $@ $^ $(LIBS)

# Include generated dependency files if they exist.
-include $(DEPS)

.PHONY: clean
clean:
	rm -f $(EXE)
	rm -rf $(BUILD_DIR)

