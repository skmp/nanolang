#pragma once
#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <cstdio>

// Wraps an SDL3 window + renderer + ImGui context.
// Each instance is an independent ImGui context, enabling multi-window apps.
struct SdlImGuiWindow {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    ImGuiContext* imgui_ctx = nullptr;
    bool open = false;

    float dpi_scale = 1.0f;

    bool init(const char* title, int w, int h) {
        window = SDL_CreateWindow(title, w, h,
            SDL_WINDOW_RESIZABLE | SDL_WINDOW_MAXIMIZED | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (!window) {
            fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
            return false;
        }
        renderer = SDL_CreateRenderer(window, nullptr);
        if (!renderer) {
            fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
            return false;
        }

        // Initial DPI: compute pixel-to-point ratio
        {
            int ww, wh, pw, ph;
            SDL_GetWindowSize(window, &ww, &wh);
            SDL_GetWindowSizeInPixels(window, &pw, &ph);
            dpi_scale = (ww > 0) ? (float)pw / (float)ww : 1.0f;
            if (dpi_scale < 1.0f) dpi_scale = 1.0f;
            SDL_SetRenderScale(renderer, dpi_scale, dpi_scale);
        }

        imgui_ctx = ImGui::CreateContext();
        ImGui::SetCurrentContext(imgui_ctx);
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImFontConfig font_cfg;
        font_cfg.SizePixels = 17.0f * dpi_scale;
        io.Fonts->AddFontDefault(&font_cfg);
        io.FontGlobalScale = 1.0f / dpi_scale;
        ImGui::StyleColorsDark();
        ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
        ImGui_ImplSDLRenderer3_Init(renderer);

        open = true;
        return true;
    }

    void update_dpi_scale() {
        int ww, wh, pw, ph;
        SDL_GetWindowSize(window, &ww, &wh);
        SDL_GetWindowSizeInPixels(window, &pw, &ph);
        float new_scale = (ww > 0) ? (float)pw / (float)ww : 1.0f;
        if (new_scale < 1.0f) new_scale = 1.0f;
        if (new_scale != dpi_scale) {
            dpi_scale = new_scale;
            SDL_SetRenderScale(renderer, dpi_scale, dpi_scale);
            // Rebuild font at new DPI
            ImGui::SetCurrentContext(imgui_ctx);
            ImGuiIO& io = ImGui::GetIO();
            io.Fonts->Clear();
            ImFontConfig font_cfg;
            font_cfg.SizePixels = 17.0f * dpi_scale;
            io.Fonts->AddFontDefault(&font_cfg);
            io.FontGlobalScale = 1.0f / dpi_scale;
            ImGui_ImplSDLRenderer3_DestroyFontsTexture();
            io.Fonts->Build();
            ImGui_ImplSDLRenderer3_CreateFontsTexture();
        }
    }

    void process_event(SDL_Event& e) {
        if (!open) return;
        ImGui::SetCurrentContext(imgui_ctx);
        ImGui_ImplSDL3_ProcessEvent(&e);
        if (e.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED ||
            e.type == SDL_EVENT_WINDOW_MOVED) {
            update_dpi_scale();
        }
    }

    void begin_frame() {
        if (!open) return;
        ImGui::SetCurrentContext(imgui_ctx);
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
    }

    void end_frame(uint8_t r = 26, uint8_t g = 26, uint8_t b = 46) {
        if (!open) return;
        ImGui::SetCurrentContext(imgui_ctx);
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, r, g, b, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    // End frame with custom rendering between clear and imgui overlay
    void end_frame_with(auto render_fn, uint8_t r = 26, uint8_t g = 26, uint8_t b = 46) {
        if (!open) return;
        ImGui::SetCurrentContext(imgui_ctx);
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, r, g, b, 255);
        SDL_RenderClear(renderer);
        render_fn(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    SDL_WindowID window_id() const {
        return window ? SDL_GetWindowID(window) : 0;
    }

    ImGuiIO& io() {
        ImGui::SetCurrentContext(imgui_ctx);
        return ImGui::GetIO();
    }

    void shutdown() {
        if (!imgui_ctx) return;
        ImGui::SetCurrentContext(imgui_ctx);
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext(imgui_ctx);
        imgui_ctx = nullptr;
        if (renderer) { SDL_DestroyRenderer(renderer); renderer = nullptr; }
        if (window) { SDL_DestroyWindow(window); window = nullptr; }
        open = false;
    }
};
