cmake_minimum_required(VERSION 3.20)
project(PixelPhysicsSim2D LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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

if(MSVC)
    target_compile_options(sim_core PRIVATE /W4 /O2)
else()
    target_compile_options(sim_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(pixel_sim
    src/app/main.cpp
    src/app/Win32App.cpp
)

target_link_libraries(pixel_sim PRIVATE sim_core)

if(WIN32)
    target_link_libraries(pixel_sim PRIVATE user32 gdi32)
    target_compile_definitions(pixel_sim PRIVATE UNICODE _UNICODE)
    if(NOT MSVC)
        target_link_options(pixel_sim PRIVATE -static)
    endif()
endif()

if(MSVC)
    target_compile_options(pixel_sim PRIVATE /W4 /O2)
else()
    target_compile_options(pixel_sim PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_executable(pixel_sim_tests
    tests/test_physics.cpp
)
target_link_libraries(pixel_sim_tests PRIVATE sim_core)
if(MSVC)
    target_compile_options(pixel_sim_tests PRIVATE /W4 /O2)
else()
    target_compile_options(pixel_sim_tests PRIVATE -Wall -Wextra -Wpedantic)
    if(WIN32)
        target_link_options(pixel_sim_tests PRIVATE -static)
    endif()
endif()

add_test(NAME PixelPhysicsUnitTests COMMAND pixel_sim_tests)
