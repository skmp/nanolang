cmake_minimum_required(VERSION 3.25)
project(nanolang LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    find_program(CCACHE_PROGRAM ccache)
    if(CCACHE_PROGRAM)
        set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    endif()
endif()

option(NANOLANG_BUILD_EDITOR "Build the nanoflow visual editor (requires SDL3 + imgui)" ON)

# nanolang: core language library (no UI dependencies)
add_library(nanolang STATIC
    src/nano/types.cpp
    src/nano/args.cpp
    src/nano/expr.cpp
    src/nano/type_utils.cpp
    src/nano/serial.cpp
    src/nano/inference.cpp
    src/nano/graph_index.cpp
    src/nano/shadow.cpp
)
target_include_directories(nanolang PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nano
)

# nanoc: standalone compiler
add_executable(nanoc
    src/nanoc/main.cpp
    src/nanoc/codegen.cpp
)
target_include_directories(nanoc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nanoc
)
target_link_libraries(nanoc PRIVATE
    nanolang
)

# test_inference: unit tests
add_executable(test_inference
    tests/test_inference.cpp
)
target_link_libraries(test_inference PRIVATE
    nanolang
)

# nanoflow: visual flow editor (optional, requires SDL3 + imgui)
if(NANOLANG_BUILD_EDITOR)
    set(NANO_NEEDS_IMGUI ON)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/NanoDeps.cmake)

    add_executable(nanoflow
        src/nanoflow/main.cpp
        src/nanoflow/editor.cpp
    )
    target_include_directories(nanoflow PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/nanoflow
    )
    if(WIN32)
        target_link_libraries(nanoflow PRIVATE nanolang SDL3::SDL3 imgui::imgui)
    else()
        target_link_libraries(nanoflow PRIVATE nanolang SDL3::SDL3-static imgui_all)
    endif()
    add_dependencies(nanoflow nanoc)
endif()
