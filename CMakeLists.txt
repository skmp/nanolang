cmake_minimum_required(VERSION 3.25)
project(attolang LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    find_program(CCACHE_PROGRAM ccache)
    if(CCACHE_PROGRAM)
        set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    endif()
endif()

option(ATTOLANG_BUILD_EDITOR "Build the attoflow visual editor (requires SDL3 + imgui)" ON)

# attolang: core language library (no UI dependencies)
add_library(attolang STATIC
    src/atto/types.cpp
    src/atto/args.cpp
    src/atto/expr.cpp
    src/atto/type_utils.cpp
    src/atto/serial.cpp
    src/atto/inference.cpp
    src/atto/graph_index.cpp
    src/atto/shadow.cpp
    src/atto/symbol_table.cpp
)
target_include_directories(attolang PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/atto
)

# attoc: standalone compiler
add_executable(attoc
    src/attoc/main.cpp
    src/attoc/codegen.cpp
)
target_include_directories(attoc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/attoc
)
target_link_libraries(attoc PRIVATE
    attolang
)

# test_inference: unit tests
add_executable(test_inference
    tests/test_inference.cpp
)
target_link_libraries(test_inference PRIVATE
    attolang
)

# attoflow: visual flow editor (optional, requires SDL3 + imgui)
if(ATTOLANG_BUILD_EDITOR)
    set(ATTO_NEEDS_IMGUI ON)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/AttoDeps.cmake)

    add_executable(attoflow
        src/attoflow/main.cpp
        src/attoflow/editor.cpp
    )
    target_include_directories(attoflow PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/attoflow
    )
    if(WIN32)
        target_link_libraries(attoflow PRIVATE attolang SDL3::SDL3 imgui::imgui)
    else()
        target_link_libraries(attoflow PRIVATE attolang SDL3::SDL3-static imgui_all)
    endif()
    add_dependencies(attoflow attoc)
endif()
