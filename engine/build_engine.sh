#!/bin/bash
# AI engine (libtorch, MSVC). Output: engine/build/Release/haline_engine.dll + torch DLLs.
set -e
cd "$(dirname "$0")"
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
