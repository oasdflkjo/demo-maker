cmake_minimum_required(VERSION 3.25)

project(tiny_demo_studio VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenGL REQUIRED)
find_package(SDL2 CONFIG REQUIRED)

add_library(tiny_core
    src/effect.cpp
    src/project_io.cpp
    src/song.cpp
    src/studio_project.cpp
    src/synth.cpp
)

target_include_directories(tiny_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_options(tiny_core PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

add_executable(tiny_demo
    src/gl_renderer.cpp
    src/main.cpp
)

target_link_libraries(tiny_demo
    PRIVATE
        tiny_core
        SDL2::SDL2
        OpenGL::GL
)

target_compile_definitions(tiny_demo
    PRIVATE
        TINY_DEMO_SHADER_PATH="${CMAKE_CURRENT_SOURCE_DIR}/shaders/demo.frag"
)

target_compile_options(tiny_demo PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

option(TINY_BUILD_EDITOR "Build the graphical music editor" ON)

if(TINY_BUILD_EDITOR)
    find_package(Freetype REQUIRED)

    add_executable(tiny_editor
        src/editor_main.cpp
        src/editor_ui.cpp
    )

    target_link_libraries(tiny_editor
        PRIVATE
            tiny_core
            SDL2::SDL2
            Freetype::Freetype
    )

    target_compile_options(tiny_editor PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )

    add_executable(tiny_effect_editor
        src/effect_editor_main.cpp
        src/editor_ui.cpp
        src/gl_renderer.cpp
    )

    target_link_libraries(tiny_effect_editor
        PRIVATE
            tiny_core
            SDL2::SDL2
            Freetype::Freetype
            OpenGL::GL
    )

    target_compile_definitions(tiny_effect_editor
        PRIVATE
            TINY_DEMO_SHADER_PATH="${CMAKE_CURRENT_SOURCE_DIR}/shaders/demo.frag"
    )

    target_compile_options(tiny_effect_editor PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()

include(CTest)

if(BUILD_TESTING)
    add_executable(tiny_core_tests tests/core_tests.cpp)
    target_link_libraries(tiny_core_tests PRIVATE tiny_core)
    target_compile_options(tiny_core_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME tiny_core_tests COMMAND tiny_core_tests)
endif()
