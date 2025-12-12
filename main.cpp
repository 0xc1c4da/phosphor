// Minimal SDL3 + Dear ImGui boilerplate.
// This is intentionally short and focused on structure.
// For a full SDL3 + Vulkan + ImGui example, see:
//   references/imgui/examples/example_sdl3_vulkan/main.cpp

#include "imgui.h"

#include <SDL3/SDL.h>
#include <cstdio>

int main(int, char**)
{
    // Initialize SDL (video + optional gamepad input)
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
    {
        std::printf("Error: SDL_Init(): %s\n", SDL_GetError());
        return 1;
    }

    // Create a resizable high-DPI window.
    float main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    SDL_WindowFlags window_flags =
        (SDL_WindowFlags)(SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN | SDL_WINDOW_VULKAN);

    SDL_Window* window = SDL_CreateWindow(
        "utf8-art-editor",
        (int)(1280 * main_scale),
        (int)(720  * main_scale),
        window_flags);

    if (window == nullptr)
    {
        std::printf("Error: SDL_CreateWindow(): %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window);

    // Dear ImGui core context (no backends wired yet).
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    (void)io;

    // Basic dark style; scale it to match the window DPI.
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;

    bool done = false;
    while (!done)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            if (event.type == SDL_EVENT_QUIT)
                done = true;
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                event.window.windowID == SDL_GetWindowID(window))
                done = true;
        }

        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)
        {
            SDL_Delay(10);
            continue;
        }

        // TODO:
        //  - Hook up ImGui SDL3 + Vulkan backends (imgui_impl_sdl3 / imgui_impl_vulkan).
        //  - Create a Vulkan device + swapchain, and render ImGui each frame.
        //  - Use the full example in references/imgui/examples/example_sdl3_vulkan/main.cpp as a guide.

        SDL_Delay(10); // Simple idle to avoid spinning the CPU.
    }

    ImGui::DestroyContext();
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}

