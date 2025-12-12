## Simple SDL3 + Dear ImGui + Vulkan boilerplate build
## Uses the local ImGui sources under ./references/imgui

.DEFAULT_GOAL := all

CXX       ?= g++
EXE       = utf8-art-editor
IMGUI_DIR = references/imgui
BUILD_DIR = build

SOURCES  = main.cpp \
           canvas.cpp \
           colour_picker.cpp
SOURCES += $(IMGUI_DIR)/imgui.cpp \
           $(IMGUI_DIR)/imgui_demo.cpp \
           $(IMGUI_DIR)/imgui_draw.cpp \
           $(IMGUI_DIR)/imgui_tables.cpp \
           $(IMGUI_DIR)/imgui_widgets.cpp
SOURCES += $(IMGUI_DIR)/backends/imgui_impl_sdl3.cpp \
           $(IMGUI_DIR)/backends/imgui_impl_vulkan.cpp

OBJS     = $(addprefix $(BUILD_DIR)/, $(addsuffix .o, $(basename $(notdir $(SOURCES)))))

CXXFLAGS ?= -std=c++20
CXXFLAGS += -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends
# Enable 32-bit ImWchar in Dear ImGui so Unscii glyphs above U+FFFF render correctly.
CXXFLAGS += -DIMGUI_USE_WCHAR32
CXXFLAGS += -g -Wall -Wextra -Wformat

# SDL3 + Vulkan + Chafa + nlohmann_json flags provided by the Nix dev shell (see flake.nix)
CXXFLAGS += $(shell pkg-config --cflags sdl3 vulkan chafa nlohmann_json)
LIBS     = $(shell pkg-config --libs sdl3 vulkan chafa) -ldl

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: %.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/%.o: $(IMGUI_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/%.o: $(IMGUI_DIR)/backends/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

all: $(EXE)

$(EXE): $(OBJS)
	$(CXX) -o $@ $^ $(LIBS)

.PHONY: clean
clean:
	rm -f $(EXE)
	rm -rf $(BUILD_DIR)

