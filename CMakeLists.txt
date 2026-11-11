cmake_minimum_required(VERSION 3.21)
project(PixelPhysicsSim2D LANGUAGES C CXX)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ELEMENTA_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer on Clang/GCC" OFF)
option(ELEMENTA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(ELEMENTA_BUILD_TESTS "Build physics unit tests" ON)
option(ELEMENTA_BUILD_GUI "Build SDL3 + Dear ImGui + OpenGL3 GUI into pixel_sim" ON)

find_package(Threads REQUIRED)

function(elementa_configure_target target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE
            /W4
            /utf-8
            $<$<NOT:$<CONFIG:Debug>>:/O2>
            $<$<BOOL:${ELEMENTA_WARNINGS_AS_ERRORS}>:/WX>
        )
    else()
        target_compile_options(${target_name} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            $<$<BOOL:${ELEMENTA_WARNINGS_AS_ERRORS}>:-Werror>
        )
        if(ELEMENTA_ENABLE_SANITIZERS)
            target_compile_options(${target_name} PRIVATE
                -fsanitize=address,undefined
                -fno-omit-frame-pointer
            )
            target_link_options(${target_name} PRIVATE
                -fsanitize=address,undefined
            )
        endif()
    endif()
endfunction()

add_library(sim_core STATIC
    src/engine/Material.cpp
    src/engine/SimWorld.cpp
    src/engine/FluidSolver.cpp
    src/engine/ThermoElectric.cpp
    src/engine/Reactions.cpp
    src/engine/ParticlePhysics.cpp
    src/engine/Renderer.cpp
)

target_include_directories(sim_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(sim_core PUBLIC Threads::Threads)
elementa_configure_target(sim_core)

set(PIXEL_SIM_SOURCES
    src/app/main.cpp
)
if(ELEMENTA_BUILD_GUI)
    include(FetchContent)

    if(NOT DEFINED OpenGL_GL_PREFERENCE)
        set(OpenGL_GL_PREFERENCE GLVND)
    endif()

    # Keep SDL3 minimal and statically linked: video, events, clipboard, OpenGL only
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)

    set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
    set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
    set(SDL_JOYSTICK OFF CACHE BOOL "" FORCE)
    set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
    set(SDL_HIDAPI OFF CACHE BOOL "" FORCE)
    set(SDL_POWER OFF CACHE BOOL "" FORCE)
    set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
    set(SDL_DIALOG OFF CACHE BOOL "" FORCE)
    set(SDL_TRAY OFF CACHE BOOL "" FORCE)
    set(SDL_GPU OFF CACHE BOOL "" FORCE)
    set(SDL_RENDER OFF CACHE BOOL "" FORCE)
    set(SDL_VULKAN OFF CACHE BOOL "" FORCE)
    set(SDL_DIRECTX OFF CACHE BOOL "" FORCE)
    set(SDL_OPENGLES OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        SDL3
        URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/SDL3-3.4.20.tar.gz
        URL_HASH SHA256=F3BB06F55BED9557227A700AF2EA62E49497EBDF58037732A731A5FB3EF8581F
    )
    FetchContent_Declare(
        imgui
        URL https://github.com/ocornut/imgui/archive/refs/tags/v1.93.0-docking.tar.gz
        URL_HASH SHA256=746C265540BD3AA109570F29492EF4D8F5B421DD9FD0049C35CA35F21496F1A5
    )
    FetchContent_MakeAvailable(SDL3 imgui)

    if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.25")
        if(TARGET SDL3_Headers)
            set_target_properties(SDL3_Headers PROPERTIES SYSTEM ON)
        endif()
        if(TARGET SDL3-static)
            set_target_properties(SDL3-static PROPERTIES SYSTEM ON)
        endif()
    endif()

    find_package(OpenGL REQUIRED)

    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    )
    target_include_directories(imgui SYSTEM PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
    )
    target_link_libraries(imgui PUBLIC SDL3::SDL3 OpenGL::GL)

    list(APPEND PIXEL_SIM_SOURCES
        src/app/SdlApp.cpp
        src/app/GuiOverlay.cpp
        src/app/GlShaderPipeline.cpp
    )
endif()

add_executable(pixel_sim ${PIXEL_SIM_SOURCES})
target_link_libraries(pixel_sim PRIVATE sim_core)
elementa_configure_target(pixel_sim)

if(ELEMENTA_BUILD_GUI)
    target_compile_definitions(pixel_sim PRIVATE ELEMENTA_WITH_GUI=1)
    if(APPLE)
        target_compile_definitions(pixel_sim PRIVATE GL_SILENCE_DEPRECATION)
    endif()
    target_link_libraries(pixel_sim PRIVATE imgui SDL3::SDL3 OpenGL::GL)
endif()

if(MINGW)
    target_link_options(pixel_sim PRIVATE -static)
endif()

if(ELEMENTA_BUILD_TESTS)
    enable_testing()
    add_executable(pixel_sim_tests
        tests/test_physics.cpp
    )
    target_link_libraries(pixel_sim_tests PRIVATE sim_core)
    elementa_configure_target(pixel_sim_tests)
    if(MINGW)
        target_link_options(pixel_sim_tests PRIVATE -static)
    endif()
    add_test(NAME PixelPhysicsUnitTests COMMAND pixel_sim_tests)
endif()

install(TARGETS pixel_sim RUNTIME DESTINATION bin)
if(ELEMENTA_BUILD_TESTS)
    install(TARGETS pixel_sim_tests RUNTIME DESTINATION bin)
endif()
install(FILES README.md DESTINATION .)
