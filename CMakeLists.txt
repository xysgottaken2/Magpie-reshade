cmake_minimum_required(VERSION 3.20)
project(MagpieReShadeInput LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "This add-on is Windows-only.")
endif()

if(NOT RESHADE_INCLUDE_DIR)
    message(FATAL_ERROR "Pass -DRESHADE_INCLUDE_DIR=C:/path/to/reshade/include")
endif()

add_library(MagpieReShadeInput SHARED
    src/MagpieReShadeInput.cpp
)

target_include_directories(MagpieReShadeInput PRIVATE
    "${RESHADE_INCLUDE_DIR}"
)

target_compile_definitions(MagpieReShadeInput PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(MagpieReShadeInput PRIVATE
    user32
)
