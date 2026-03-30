#include <SDL3/SDL.h>
#include <cstdio>
#include "window.h"

int main(int argc, char* argv[]) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    FlowEditorWindow editor;
    const char* path = (argc > 1) ? argv[1] : nullptr;
    if (path) {
        editor.init(path);
    } else {
        editor.init();
    }

    bool running = true;
    Uint64 last_time = SDL_GetTicksNS();

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            editor.process_event(event);
            if (event.type == SDL_EVENT_QUIT) running = false;
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) running = false;
        }

        if (editor.is_open()) {
            editor.draw();
        } else {
            running = false;
        }

        // Cap at 60 FPS
        Uint64 now = SDL_GetTicksNS();
        Uint64 frame_ns = now - last_time;
        last_time = now;
        constexpr Uint64 target_ns = 1000000000ULL / 60;
        if (frame_ns < target_ns) {
            SDL_DelayNS(target_ns - frame_ns);
        }
    }

    editor.shutdown(); 
    SDL_Quit();
    return 0;
}
