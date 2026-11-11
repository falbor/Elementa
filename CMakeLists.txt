cmake_minimum_required(VERSION 3.20)
project(PixelPhysicsSim2D LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ELEMENTA_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer on Clang/GCC" OFF)
option(ELEMENTA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(ELEMENTA_BUILD_TESTS "Build physics unit tests" ON)

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
if(WIN32)
    list(APPEND PIXEL_SIM_SOURCES src/app/Win32App.cpp)
endif()

add_executable(pixel_sim ${PIXEL_SIM_SOURCES})
target_link_libraries(pixel_sim PRIVATE sim_core)
elementa_configure_target(pixel_sim)

if(WIN32)
    target_link_libraries(pixel_sim PRIVATE user32 gdi32)
    target_compile_definitions(pixel_sim PRIVATE UNICODE _UNICODE)
    if(MINGW)
        target_link_options(pixel_sim PRIVATE -static)
    endif()
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
