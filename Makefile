## Simple SDL3 + Dear ImGui + Vulkan boilerplate build
## Uses the local ImGui sources under ./references/imgui

.DEFAULT_GOAL := all

CXX       ?= g++
EXE       = utf8-art-editor
IMGUI_DIR = references/imgui
BUILD_DIR = build

SOURCES  = main.cpp \
           canvas.cpp \
           preview_window.cpp \
           ansi_importer.cpp \
           colour_picker.cpp \
           image_to_chafa_dialog.cpp \
           xterm256_palette.cpp \
           character_picker.cpp \
           character_palette.cpp \
           layer_manager.cpp \
           ansl_editor.cpp \
           ansl_script_engine.cpp \
           ansl_luajit.cpp \
           ansl_sort.cpp \
           ansl_params_ui.cpp \
           tool_palette.cpp \
           io_manager.cpp
SOURCES += $(IMGUI_DIR)/imgui.cpp \
           $(IMGUI_DIR)/imgui_demo.cpp \
           $(IMGUI_DIR)/imgui_draw.cpp \
           $(IMGUI_DIR)/imgui_tables.cpp \
           $(IMGUI_DIR)/imgui_widgets.cpp
SOURCES += $(IMGUI_DIR)/misc/cpp/imgui_stdlib.cpp
SOURCES += $(IMGUI_DIR)/backends/imgui_impl_sdl3.cpp \
           $(IMGUI_DIR)/backends/imgui_impl_vulkan.cpp

OBJS     = $(addprefix $(BUILD_DIR)/, $(addsuffix .o, $(basename $(notdir $(SOURCES)))))

CXXFLAGS ?= -std=c++20 -O3
CXXFLAGS += -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends
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

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: %.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c -o $@ $<

$(BUILD_DIR)/%.o: $(IMGUI_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c -o $@ $<

$(BUILD_DIR)/%.o: $(IMGUI_DIR)/backends/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c -o $@ $<

# imgui_stdlib lives under misc/cpp, but OBJS uses notdir(), so we need an explicit rule.
$(BUILD_DIR)/imgui_stdlib.o: $(IMGUI_DIR)/misc/cpp/imgui_stdlib.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c -o $@ $<

all: $(EXE)

$(EXE): $(OBJS)
	$(CXX) -o $@ $^ $(LIBS)

# Include generated dependency files if they exist.
-include $(DEPS)

.PHONY: clean
clean:
	rm -f $(EXE)
	rm -rf $(BUILD_DIR)

