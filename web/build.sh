#!/bin/sh
# Builds the browser patcher: web/ftl-core.js and web/ftl-core.wasm, from the desktop
# patcher's core (patcher/src) and web/patcher.cpp. Needs Emscripten's em++ in PATH.
set -eu
cd "$(dirname "$0")"

# A release's files, when they are here (a local release build): the web copy is committed
mkdir -p files
for f in prepatch.ftlt prepatch-hyperspace.ftlt ftlios.dylib; do
    if [ -f "../files/$f" ]; then cp "../files/$f" "files/$f"; fi
done

# miniz (the icon's PNG writer), the version the desktop build uses
MINIZ=3.1.0
MINIZ_DIR="${TMPDIR:-/tmp}/ftl-miniz-$MINIZ"
if [ ! -f "$MINIZ_DIR/miniz_tdef.c" ]; then
    mkdir -p "$MINIZ_DIR"
    curl -fsSL "https://github.com/richgel999/miniz/archive/refs/tags/$MINIZ.tar.gz" |
        tar -xz -C "$MINIZ_DIR" --strip-components=1
fi
# CMake generates it for the desktop build; the sources expect it
if [ ! -f "$MINIZ_DIR/miniz_export.h" ]; then
    cat > "$MINIZ_DIR/miniz_export.h" <<'EOF'
#ifndef MINIZ_EXPORT_H
#define MINIZ_EXPORT_H
#define MINIZ_EXPORT
#define MINIZ_NO_EXPORT
#define MINIZ_DEPRECATED __attribute__((__deprecated__))
#define MINIZ_DEPRECATED_EXPORT MINIZ_EXPORT MINIZ_DEPRECATED
#define MINIZ_DEPRECATED_NO_EXPORT MINIZ_NO_EXPORT MINIZ_DEPRECATED
#endif
EOF
fi

# miniz is C: build it separately, the C++ driver would warn
OBJ="$MINIZ_DIR/obj"
mkdir -p "$OBJ"
for f in miniz miniz_tdef; do
    emcc -O3 -I "$MINIZ_DIR" -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES -c "$MINIZ_DIR/$f.c" -o "$OBJ/$f.o"
done

em++ -std=c++17 -O3 -fwasm-exceptions \
    -I ../patcher/src -I ../src/third_party -I "$MINIZ_DIR" \
    -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES \
    ../patcher/src/util.cpp ../patcher/src/sha256.cpp ../patcher/src/macho.cpp \
    ../patcher/src/prepatch.cpp ../patcher/src/plist.cpp ../patcher/src/game.cpp \
    ../patcher/src/icon.cpp ../patcher/stb_impl.cpp \
    patcher.cpp \
    "$OBJ/miniz.o" "$OBJ/miniz_tdef.o" \
    -o ftl-core.js \
    -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createFtlCore \
    -sEXPORTED_FUNCTIONS='["_malloc","_free","_ftl_error","_ftl_free","_ftl_plist_string","_ftl_patch_info","_ftl_icons","_ftl_patch_exe"]' \
    -sEXPORTED_RUNTIME_METHODS='["HEAPU8","HEAPU32","UTF8ToString"]' \
    -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=2147483648 -sSTACK_SIZE=1048576 \
    -sENVIRONMENT=web,worker,node

echo "built web/ftl-core.js and web/ftl-core.wasm"
