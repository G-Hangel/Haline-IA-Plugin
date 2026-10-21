cmake_minimum_required(VERSION 3.18 FATAL_ERROR)

if(POLICY CMP0144)
    cmake_policy(SET CMP0144 NEW)
endif()

project(haline_engine LANGUAGES CXX)

# Native engine for the Krita plugin: plain C ABI DLL over libtorch (TorchScript).
# It does not link Python or use pybind11, so it needs neither python313.lib nor
# interpreter ABI matching. The plugin loads it through its C ABI.
#
# Configure (VS 2026/2022 x64, from the engine folder):
#   cmake -S . -B build -G "Visual Studio 18 2026" -A x64
#   cmake --build build --config Release
# Ninja alternative:  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Torch root (pip CPU wheel or libtorch): override with -DHALINE_TORCH_ROOT=<root>/site-packages/torch
set(HALINE_TORCH_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../Treinamento/.venv/Lib/site-packages/torch"
    CACHE PATH "Torch installation root")
set(Torch_DIR "${HALINE_TORCH_ROOT}/share/cmake/Torch"
    CACHE PATH "Directory containing TorchConfig.cmake" FORCE)

# The CPU torch wheel ships no kineto/fbgemm and TorchConfig warns for each missing lib.
# The log level is raised to ERROR only during find_package; real errors are still shown.
set(CMAKE_MESSAGE_LOG_LEVEL ERROR)
find_package(Torch CONFIG REQUIRED)
unset(CMAKE_MESSAGE_LOG_LEVEL)

add_library(haline_engine SHARED haline_engine.cpp)
target_include_directories(haline_engine PRIVATE ${TORCH_INCLUDE_DIRS})
target_link_libraries(haline_engine PRIVATE ${TORCH_LIBRARIES})
set_property(TARGET haline_engine PROPERTY CXX_STANDARD 20)

if(MSVC)
    # libtorch release on Windows is /MD; /MDd causes a CRT mismatch and the import fails.
    # /wd4267: C4267 (size_t -> int) comes from libtorch header instantiations, not this file.
    target_compile_options(haline_engine PRIVATE /utf-8 /wd4251 /wd4275 /wd4267)
    set_target_properties(haline_engine PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# Copy the torch DLLs next to the engine so Krita does not need libtorch/lib on
# PATH nor an add_dll_directory call.
file(GLOB TORCH_DLLS "${HALINE_TORCH_ROOT}/lib/*.dll")
add_custom_command(TARGET haline_engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${TORCH_DLLS} "$<TARGET_FILE_DIR:haline_engine>")
