# AttoDeps.cmake — Fetch SDL3 + imgui via FetchContent (non-Windows) or vcpkg (Windows)
#
# Provides:
#   SDL3::SDL3   — SDL3 target
#   imgui_all    — imgui core + SDL3 backends (only if ATTO_NEEDS_IMGUI is set)

if(WIN32)
    # On Windows, use vcpkg (user must set CMAKE_TOOLCHAIN_FILE)
    find_package(SDL3 CONFIG REQUIRED)
    if(ATTO_NEEDS_IMGUI)
        find_package(imgui CONFIG REQUIRED)
    endif()
else()
    # On Linux/macOS, use FetchContent
    include(FetchContent)

    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG        release-3.4.2
        GIT_SHALLOW    TRUE
    )
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(SDL3)

    if(ATTO_NEEDS_IMGUI)
        FetchContent_Declare(imgui
            GIT_REPOSITORY https://github.com/ocornut/imgui.git
            GIT_TAG        v1.92.6
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(imgui)

        # imgui doesn't ship a CMakeLists.txt — build it ourselves
        add_library(imgui_all STATIC
            ${imgui_SOURCE_DIR}/imgui.cpp
            ${imgui_SOURCE_DIR}/imgui_demo.cpp
            ${imgui_SOURCE_DIR}/imgui_draw.cpp
            ${imgui_SOURCE_DIR}/imgui_tables.cpp
            ${imgui_SOURCE_DIR}/imgui_widgets.cpp
            ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
            ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
        )
        target_include_directories(imgui_all PUBLIC
            ${imgui_SOURCE_DIR}
            ${imgui_SOURCE_DIR}/backends
        )
        target_link_libraries(imgui_all PUBLIC SDL3::SDL3-static)
    endif()
endif()
