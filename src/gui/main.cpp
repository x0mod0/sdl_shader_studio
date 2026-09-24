// Entry point: SDL3 window + GPU device, Dear ImGui with the SDL GPU backend,
// then App::frame() once per frame.
#include <chrono>
#include <cstdio>
#include <string>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>

#include "app.h"

namespace {

// The device is asked for every format the machine might support; the preview
// only needs one of them to work.
constexpr SDL_GPUShaderFormat kWantedFormats =
    SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL |
    SDL_GPU_SHADERFORMAT_METALLIB;

void fatal(const char* what) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s: %s", what, SDL_GetError());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "SDL Shader Studio", what, nullptr);
}

}  // namespace

int main(int argc, char** argv) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fatal("SDL_Init failed");
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("SDL Shader Studio", 1600, 950,
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        fatal("could not create a window");
        SDL_Quit();
        return 1;
    }

    // Keep the window from being shrunk past the point where the docked panels
    // stop being usable: a fifth of the display it opened on.
    SDL_Rect display_bounds{};
    if (SDL_GetDisplayBounds(SDL_GetDisplayForWindow(window), &display_bounds)) {
        SDL_SetWindowMinimumSize(window, display_bounds.w / 5, display_bounds.h / 5);
    }

    SDL_GPUDevice* device = SDL_CreateGPUDevice(kWantedFormats, /*debug_mode=*/false, nullptr);
    if (!device) {
        fatal("could not create a GPU device");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    if (!SDL_ClaimWindowForGPUDevice(device, window)) {
        fatal("could not attach the GPU device to the window");
        SDL_DestroyGPUDevice(device);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_SetGPUSwapchainParameters(device, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                  SDL_GPU_PRESENTMODE_VSYNC);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    // The app keeps the layout itself, in the config directory beside its
    // settings (App::save_layout_now / App::init). ImGui's own imgui.ini would
    // land in whatever directory the app happened to be started from, and be a
    // second, competing copy of the same thing.
    io.IniFilename = nullptr;

    ImGui_ImplSDL3_InitForSDLGPU(window);
    ImGui_ImplSDLGPU3_InitInfo init_info = {};
    init_info.Device = device;
    init_info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(device, window);
    init_info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    ImGui_ImplSDLGPU3_Init(&init_info);

    ssstudio::gui::App app;
    if (!app.init(window, device)) {
        fatal("application init failed");
        return 1;
    }

    // Projects can be handed in on the command line, which is how the "open
    // with" association and `ssstudio --gui <project>` both work. Several are
    // accepted because several can be open at once: the window opens a tab for
    // each and leaves the last one in front.
    // An explicit path is a request for that project and nothing else; only a
    // bare launch reopens what was there last time.
    if (argc > 1) {
        for (int i = 1; i < argc; ++i) app.open_project(argv[i]);
    } else {
        app.restore_session();
    }

    auto previous = std::chrono::steady_clock::now();
    bool running = true;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            // Both go through the app rather than ending the loop outright, so
            // closing the window asks about unsaved projects exactly the way
            // File > Quit does.
            if (event.type == SDL_EVENT_QUIT) app.request_quit();
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                event.window.windowID == SDL_GetWindowID(window)) {
                app.request_quit();
            }
            // Any of the app's windows, not only the main one: with panels
            // dragged out into their own windows, coming back to the app often
            // means clicking one of those first.
            if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) app.window_focus_gained();
        }
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(10);
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        const float delta = std::chrono::duration<float>(now - previous).count();
        previous = now;

        // Fonts are swapped here, outside any frame, when a theme change
        // during the last one asked for different ones.
        app.before_frame();

        ImGui_ImplSDLGPU3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        app.frame(delta);
        if (app.wants_quit()) running = false;

        ImGui::Render();
        ImDrawData* draw_data = ImGui::GetDrawData();
        const bool minimized = draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f;

        SDL_GPUCommandBuffer* command_buffer = SDL_AcquireGPUCommandBuffer(device);
        SDL_GPUTexture* swapchain = nullptr;
        SDL_AcquireGPUSwapchainTexture(command_buffer, window, &swapchain, nullptr, nullptr);

        if (swapchain && !minimized) {
            ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, command_buffer);

            SDL_GPUColorTargetInfo target = {};
            target.texture = swapchain;
            target.clear_color = SDL_FColor{0.06f, 0.06f, 0.07f, 1.0f};
            target.load_op = SDL_GPU_LOADOP_CLEAR;
            target.store_op = SDL_GPU_STOREOP_STORE;

            SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(command_buffer, &target, 1, nullptr);
            ImGui_ImplSDLGPU3_RenderDrawData(draw_data, command_buffer, pass);
            SDL_EndGPURenderPass(pass);
        }

        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }
        SDL_SubmitGPUCommandBuffer(command_buffer);
    }

    SDL_WaitForGPUIdle(device);
    app.shutdown();

    ImGui_ImplSDL3_Shutdown();
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui::DestroyContext();

    SDL_ReleaseWindowFromGPUDevice(device, window);
    SDL_DestroyGPUDevice(device);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
