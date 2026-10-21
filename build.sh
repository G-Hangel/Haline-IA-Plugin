#!/usr/bin/env bash
# Usage (Git Bash):  ./build.sh            -> configure (first run) and build the plugin
#                    ./build.sh install    -> copy dist/ into the installed Krita (quick test)
#                    ./build.sh setup      -> build release/HalineAI-<version>-Setup.exe from dist/
# Every build regenerates dist/ with the same folder layout as Krita.
set -euo pipefail

# Virtual drive without spaces: pkg-config/Boost break on "Haline AI/Krita build" (junctions resolve back).
[[ -d /k/src/krita ]] || subst K: "$(cygpath -w "$(cd "$(dirname "$0")" && pwd)")"
ROOT="/k"
REAL_ROOT="$(cygpath -w "$(cd "$(dirname "$0")" && pwd)")"
SRC="$ROOT/src/krita"
DEPS="$SRC/_install"
BUILD="$ROOT/build"
DIST="$ROOT/dist"
ENGINE="$ROOT/engine"

export PATH="$ROOT/tools/llvm-mingw-20251216-ucrt-x86_64/bin:$ROOT/tools:$DEPS/bin:$PATH"
export CC=clang CXX=clang++

if [[ "${1:-}" == "install" ]]; then
    if tasklist //FI "IMAGENAME eq krita.exe" 2>/dev/null | grep -qi krita; then
        echo "Close Krita before installing (the DLL is in use)."
        exit 1
    fi
    powershell -NoProfile -ExecutionPolicy Bypass -File "$REAL_ROOT\installer\install.ps1"
    exit 0
fi

if [[ "${1:-}" == "setup" ]]; then
    [[ -f "$DIST/lib/kritaplugins/kritatoolhaline.dll" ]] || { echo "Run ./build.sh first (dist/ is empty)."; exit 1; }
    "$ROOT/tools/innosetup/ISCC.exe" //Qp "$REAL_ROOT\installer\haline.iss"
    echo "Installer ready at: $REAL_ROOT\release"
    exit 0
fi

# Cache from another project (e.g. engine configured into build/) -> reconfigure.
if [[ -f "$BUILD/CMakeCache.txt" ]] && ! grep -q "CMAKE_HOME_DIRECTORY:INTERNAL=K:/src/krita" "$BUILD/CMakeCache.txt"; then
    echo "build/ had a cache from another project; reconfiguring."
    rm -rf "$BUILD/CMakeCache.txt" "$BUILD/CMakeFiles"
fi
if [[ ! -f "$BUILD/CMakeCache.txt" ]]; then
    cmake -S "$SRC" -B "$BUILD" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="$DEPS" \
        -DCMAKE_INSTALL_PREFIX="$ROOT/install" \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DBUILD_TESTING=OFF \
        -DKRITA_ENABLE_PCH=OFF \
        -DFOUNDATION_BUILD=ON
fi

cmake --build "$BUILD" --target kritatoolhaline

# --- Package for testing/distribution ---
PLUG="$DIST/lib/kritaplugins"
rm -rf "$DIST"; mkdir -p "$PLUG/haline/models"
cp "$BUILD/bin/kritatoolhaline.dll" "$PLUG/"
if [[ -f "$ENGINE/build/Release/haline_engine.dll" ]]; then
    find "$ENGINE/build/Release" -maxdepth 1 -name "*.dll" ! -name "torch_python.dll" -exec cp {} "$PLUG/haline/" \;
    cp "$ENGINE"/models/*_ts.pt "$PLUG/haline/models/"
else
    echo "WARNING: engine not built (run engine/build_engine.sh); package has no AI."
fi
echo "Package ready at: $REAL_ROOT\dist"
