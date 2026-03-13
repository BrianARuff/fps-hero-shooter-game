cmake_minimum_required(VERSION 3.28)

project(Accretion LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(Accretion WIN32
    src/main.cpp
)

target_include_directories(Accretion PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

target_compile_definitions(Accretion PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

if(MINGW)
    target_compile_options(Accretion PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wno-missing-field-initializers
        -Wno-unused-parameter
    )
    target_link_options(Accretion PRIVATE
        -municode
        -static
        -static-libgcc
        -static-libstdc++
    )
endif()

target_link_libraries(Accretion PRIVATE
    d3d11
    dxgi
    gdi32
    ole32
    user32
    shell32
    ws2_32
    winmm
    xaudio2_9
)
