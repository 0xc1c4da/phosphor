.DEFAULT_GOAL := all

CXX       ?= g++
OBJCOPY   ?= objcopy
EXE       = phosphor
# Allow overriding ImGui location (e.g. from `nix develop` via env var).
# If you build outside of Nix, set IMGUI_DIR to a checkout of ocornut/imgui.
IMGUI_DIR ?= vendor/imgui
BUILD_DIR = build
LDEPNG_DIR ?=

# ---------------------------------------------------------------------------
# Version string
# ---------------------------------------------------------------------------
# Base version is tracked in a top-level VERSION file (SemVer, no leading "v").
# Local builds prefer `git describe` for a richer string:
#   v0.7.2-4-g1a2b3c4-dirty  ->  0.7.2-4-g1a2b3c4+dirty
# Nix builds (flake.nix) pass PHOSPHOR_VERSION_STR explicitly for reproducibility.
PHOSPHOR_BASE_VERSION := $(strip $(shell cat VERSION 2>/dev/null))
ifeq ($(PHOSPHOR_BASE_VERSION),)
PHOSPHOR_BASE_VERSION := 0.0.0
endif

PHOSPHOR_GIT_DESCRIBE_RAW := $(strip $(shell git describe --tags --match 'v[0-9]*' --dirty --always 2>/dev/null))
# If git describes a SemVer-ish tag, use it (strip leading 'v').
# If git falls back to a raw hash (no matching tags), prefix with base version.
PHOSPHOR_VERSION_STR ?= $(shell \
  raw='$(PHOSPHOR_GIT_DESCRIBE_RAW)'; \
  base='$(PHOSPHOR_BASE_VERSION)'; \
  if [ -z "$$raw" ]; then \
    printf '%s' "$$base"; \
  else \
    raw="$${raw#v}"; \
    case "$$raw" in \
      ([0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]|[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]-dirty) \
        if [ "$${raw#*-}" = "dirty" ]; then \
          h="$${raw%-dirty}"; \
          printf '%s-g%s+dirty' "$$base" "$$h"; \
        else \
          printf '%s-g%s' "$$base" "$$raw"; \
        fi \
        ;; \
      (*) \
        printf '%s' "$$raw" | sed 's/-dirty$$/+dirty/'; \
        ;; \
    esac; \
  fi)

# Make it obvious when vendor/ is removed and IMGUI_DIR isn't set.
ifeq ($(wildcard $(IMGUI_DIR)/imgui.h),)
$(error IMGUI_DIR '$(IMGUI_DIR)' does not contain imgui.h. Set IMGUI_DIR=/path/to/imgui (or use `nix develop` which sets it automatically).)
endif

SOURCES  = \
           src/app/main.cpp \
           src/app/vulkan_state.cpp \
           src/app/app_ui.cpp \
           src/app/clipboard_utils.cpp \
           src/app/workspace_persist.cpp \
           src/app/run_frame.cpp \
           src/app/canvas_preview_texture.cpp \
           src/app/bitmap_glyph_atlas_texture.cpp \
           src/io/convert/chafa_convert.cpp \
           src/core/canvas/canvas_core.cpp \
           src/core/canvas/canvas_undo.cpp \
           src/core/canvas/canvas_input.cpp \
           src/core/canvas/canvas_selection.cpp \
           src/core/canvas/canvas_layers.cpp \
           src/core/canvas/canvas_render.cpp \
           src/core/canvas/canvas_project.cpp \
           src/core/encodings.cpp \
           src/core/fonts.cpp \
           src/fonts/textmode_font.cpp \
           src/fonts/textmode_font_registry.cpp \
           src/core/canvas_rasterizer.cpp \
           src/core/deform/deform_engine.cpp \
           src/core/deform/glyph_mask_cache.cpp \
           src/core/embedded_assets.cpp \
           src/core/key_bindings.cpp \
           src/core/paths.cpp \
           src/core/color_system.cpp \
           src/core/color_ops.cpp \
           src/core/xterm256_palette.cpp \
           src/core/palette/palette.cpp \
           src/core/lut/lut_cache.cpp \
           src/ansl/ansl_script_engine.cpp \
           src/ansl/ansl_luajit.cpp \
           src/ansl/ansl_sort.cpp \
           src/ui/ImGuiDatePicker.cpp \
           src/ui/ansl_editor.cpp \
           src/ui/ansl_params_ui.cpp \
           src/ui/brush_palette_window.cpp \
           src/ui/character_palette.cpp \
           src/ui/character_picker.cpp \
           src/ui/character_set.cpp \
           src/ui/glyph_preview.cpp \
           src/ui/colour_picker.cpp \
           src/ui/colour_palette.cpp \
           src/ui/imgui_window_chrome.cpp \
           src/ui/image_window.cpp \
           src/ui/image_to_chafa_dialog.cpp \
           src/ui/markdown_to_ansi_dialog.cpp \
           src/ui/layer_manager.cpp \
           src/ui/minimap_window.cpp \
           src/ui/skin.cpp \
           src/ui/settings.cpp \
           src/ui/tool_params.cpp \
           src/ui/tool_parameters_window.cpp \
           src/ui/export_dialog.cpp \
           src/ui/sauce_editor_dialog.cpp \
           src/ui/tool_palette.cpp \
           src/ui/sixteen_colors_browser.cpp \
           src/io/http_client.cpp \
           src/io/formats/ansi.cpp \
           src/io/formats/figlet.cpp \
           src/io/formats/gpl.cpp \
           src/io/formats/image.cpp \
           src/io/formats/markdown.cpp \
           src/io/formats/plaintext.cpp \
           src/io/formats/sauce.cpp \
           src/io/formats/tdf.cpp \
           src/io/formats/xbin.cpp \
           src/io/binary_codec.cpp \
           src/io/image_loader.cpp \
           src/io/image_writer.cpp \
           src/io/lodepng_unit.cpp \
           src/io/io_manager.cpp \
           src/io/project_file.cpp \
           src/io/sdl_file_dialog_queue.cpp \
           src/io/session/imgui_persistence.cpp \
           src/io/session/open_canvas_cache.cpp \
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
ifneq ($(strip $(LDEPNG_DIR)),)
CXXFLAGS += -I$(LDEPNG_DIR)
endif
# Enable 32-bit ImWchar in Dear ImGui so Unscii glyphs above U+FFFF render correctly.
CXXFLAGS += -DIMGUI_USE_WCHAR32
CXXFLAGS += -DPHOSPHOR_VERSION_STR=\"$(PHOSPHOR_VERSION_STR)\"
CXXFLAGS += -g -Wall -Wextra -Wformat

# Auto-generate header dependency files so changes to .h trigger rebuilds.
# This prevents ODR/layout mismatches (e.g. when LayerManager gains std::string members).
DEPFLAGS = -MMD -MP
DEPS     = $(OBJS:.o=.d)

# SDL3 + Vulkan + Chafa + nlohmann_json + zstd flags provided by the Nix dev shell (see flake.nix).
# LuaJIT and ICU67 headers and libraries are also made available via the dev shell.
# TODO: add libsixel
CXXFLAGS += $(shell pkg-config --cflags sdl3 vulkan chafa nlohmann_json luajit libzstd libcurl md4c libblake3)
LIBS     = $(shell pkg-config --libs sdl3 vulkan chafa icu-uc icu-i18n luajit libzstd libcurl md4c libblake3) -ldl

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

# ---------------------------------------------------------------------------
# Font sanity checker: scans assets/fonts/* and renders "test"
# ---------------------------------------------------------------------------
FONT_SANITY_EXE = font_sanity
FONT_SANITY_SRCS = \
           src/tools/font_sanity.cpp \
           src/fonts/textmode_font.cpp \
           src/fonts/textmode_font_registry.cpp \
           src/core/fonts.cpp \
           src/core/xterm256_palette.cpp

$(FONT_SANITY_EXE): $(FONT_SANITY_SRCS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^

.PHONY: font-sanity
font-sanity: $(FONT_SANITY_EXE)

# Include generated dependency files if they exist.
-include $(DEPS)

.PHONY: clean
clean:
	rm -f $(EXE)
	rm -f $(FONT_SANITY_EXE)
	rm -rf $(BUILD_DIR)

# ---------------------------------------------------------------------------
# Release guardrails
# ---------------------------------------------------------------------------
.PHONY: release-check
release-check:
	@set -eu; \
	base="$$(tr -d '\r\n' < VERSION 2>/dev/null || true)"; \
	if [ -z "$$base" ]; then \
	  echo "release-check: VERSION file is empty/missing" >&2; \
	  exit 1; \
	fi; \
	# Require a clean tree for a release.
	if ! git diff --quiet || ! git diff --cached --quiet; then \
	  echo "release-check: working tree is dirty (commit or stash changes before tagging a release)" >&2; \
	  exit 1; \
	fi; \
	# Require an exact SemVer tag match at HEAD (vX.Y.Z).
	tag="$$(git describe --tags --exact-match --match 'v[0-9]*' 2>/dev/null || true)"; \
	if [ -z "$$tag" ]; then \
	  echo "release-check: HEAD is not exactly tagged (expected something like v$$base)" >&2; \
	  exit 1; \
	fi; \
	if [ "$$tag" != "v$$base" ]; then \
	  echo "release-check: VERSION ('$$base') does not match tag ('$$tag')" >&2; \
	  echo "release-check: fix by setting VERSION=$${tag#v} (or retag appropriately)" >&2; \
	  exit 1; \
	fi; \
	echo "release-check: OK (VERSION=$$base tag=$$tag)"

.PHONY: release
release:
	@./scripts/release.sh
	@$(MAKE) release-check

