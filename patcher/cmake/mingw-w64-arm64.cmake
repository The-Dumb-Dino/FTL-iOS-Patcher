# Cross-compiling the Windows ARM64 build with llvm-mingw (aarch64-w64-mingw32-*):
#   cmake -B build-windows-arm64 -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-arm64.cmake -DCMAKE_BUILD_TYPE=Release
# The toolchain's bin/ must be in PATH (https://github.com/mstorsjo/llvm-mingw/releases).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR arm64)
set(CMAKE_C_COMPILER aarch64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER aarch64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER aarch64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
