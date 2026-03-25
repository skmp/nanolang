#include "attoruntime.h"
#include <SDL3/SDL.h>
#include <cstdio>
#include <cmath>
#include <vector>
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

// Audio state
static std::function<void()>* s_audio_tick = nullptr;
static int s_channels = 2;

static void SDLCALL audio_callback(void* /*userdata*/, SDL_AudioStream* stream,
                                    int additional_amount, int /*total_amount*/) {
    if (additional_amount <= 0 || !s_audio_tick) return;
    int ch = s_channels;
    int samples = additional_amount / (sizeof(f32) * ch);
    std::vector<f32> buf(samples * ch);

    for (int i = 0; i < samples; i++) {
        (*s_audio_tick)();
        f32 sample = atto_consume_mix();
        if (sample > 1.0f) sample = 1.0f;
        if (sample < -1.0f) sample = -1.0f;
        for (int c = 0; c < ch; c++)
            buf[i * ch + c] = sample;
    }
    SDL_PutAudioStreamData(stream, buf.data(), samples * ch * sizeof(f32));
}

void av_create_window(std::string title,
                      std::function<void()>& audio_tick,
                      s32 sample_rate,
                      s32 channels,
                      std::function<void()>& video_tick,
                      s32 width,
                      s32 height,
                      std::function<void()>& on_close) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return;
    }

    SDL_Window* window = SDL_CreateWindow(title.c_str(), width, height, SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        return;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    // High DPI scaling: use actual pixel-to-point ratio from the window's backing store
    int ww, wh, pw, ph;
    SDL_GetWindowSize(window, &ww, &wh);
    SDL_GetWindowSizeInPixels(window, &pw, &ph);
    float dpi_scale = (ww > 0) ? (float)pw / (float)ww : 1.0f;
    if (dpi_scale < 1.0f) dpi_scale = 1.0f;
    SDL_SetRenderScale(renderer, dpi_scale, dpi_scale);
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig font_cfg;
    font_cfg.SizePixels = 13.0f * dpi_scale;
    io.Fonts->AddFontDefault(&font_cfg);
    io.FontGlobalScale = 1.0f / dpi_scale;

    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    // Audio setup
    SDL_AudioSpec spec = {};
    spec.freq = sample_rate;
    spec.format = SDL_AUDIO_F32;
    spec.channels = channels;

    s_audio_tick = &audio_tick;
    s_channels = channels;
    SDL_AudioStream* audio_stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_callback, nullptr);
    if (audio_stream) {
        SDL_ResumeAudioStreamDevice(audio_stream);
    }

    // Event loop
    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) running = false;
            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                if (event.key.scancode == SDL_SCANCODE_ESCAPE) { running = false; break; }
            }
            if (event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED ||
                event.type == SDL_EVENT_WINDOW_MOVED) {
                int nww, nwh, npw, nph;
                SDL_GetWindowSize(window, &nww, &nwh);
                SDL_GetWindowSizeInPixels(window, &npw, &nph);
                float new_scale = (nww > 0) ? (float)npw / (float)nww : 1.0f;
                if (new_scale < 1.0f) new_scale = 1.0f;
                if (new_scale != dpi_scale) {
                    dpi_scale = new_scale;
                    SDL_SetRenderScale(renderer, dpi_scale, dpi_scale);
                    ImGuiIO& dio = ImGui::GetIO();
                    dio.Fonts->Clear();
                    ImFontConfig fc;
                    fc.SizePixels = 13.0f * dpi_scale;
                    dio.Fonts->AddFontDefault(&fc);
                    dio.FontGlobalScale = 1.0f / dpi_scale;
                    ImGui_ImplSDLRenderer3_DestroyFontsTexture();
                    dio.Fonts->Build();
                    ImGui_ImplSDLRenderer3_CreateFontsTexture();
                }
            }
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        video_tick();

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    // Cleanup
    s_audio_tick = nullptr;
    on_close();

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    if (audio_stream) SDL_DestroyAudioStream(audio_stream);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}
