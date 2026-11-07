# FTL iOS Patcher: the graphical patcher, for Windows, macOS and Linux.
#   cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
# wxWidgets and miniz are downloaded and linked in statically.
cmake_minimum_required(VERSION 3.20)
project(ftl-ios-patcher VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # no VC++ runtime to install
set(BUILD_SHARED_LIBS OFF)

include(FetchContent)

# wxWidgets: the system's own controls (Cocoa, Windows, GTK 3), only its core
set(wxBUILD_SHARED OFF CACHE BOOL "" FORCE)
set(wxBUILD_MONOLITHIC OFF CACHE BOOL "" FORCE)
set(wxBUILD_SAMPLES OFF CACHE STRING "" FORCE)
set(wxBUILD_TESTS OFF CACHE STRING "" FORCE)
set(wxBUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(wxBUILD_INSTALL OFF CACHE BOOL "" FORCE)
foreach(opt wxUSE_WEBVIEW wxUSE_MEDIACTRL wxUSE_OPENGL wxUSE_GLCANVAS wxUSE_STC wxUSE_AUI wxUSE_RIBBON
        wxUSE_PROPGRID wxUSE_RICHTEXT wxUSE_XRC wxUSE_HTML wxUSE_WEBREQUEST wxUSE_SECRETSTORE wxUSE_LIBSDL
        wxUSE_SPELLCHECK wxUSE_LIBTIFF wxUSE_LIBJPEG wxUSE_LIBWEBP wxUSE_LIBLZMA wxUSE_LIBMSPACK wxUSE_SOUND
        wxUSE_NET wxUSE_SOCKETS wxUSE_URL wxUSE_PROTOCOL wxUSE_FS_INET wxUSE_XML wxUSE_XTEST wxUSE_LIBNOTIFY
        wxUSE_PRIVATE_FONTS wxUSE_DEBUGREPORT wxUSE_QA wxUSE_HELP wxUSE_WXHTML_HELP wxUSE_MS_HTML_HELP)
    set(${opt} OFF CACHE BOOL "" FORCE)
endforeach()
foreach(opt wxUSE_LIBPNG wxUSE_ZLIB wxUSE_EXPAT wxUSE_REGEX)
    set(${opt} builtin CACHE STRING "" FORCE)
endforeach()
FetchContent_Declare(wxwidgets
    GIT_REPOSITORY https://github.com/wxWidgets/wxWidgets.git
    GIT_TAG v3.3.2
    GIT_SHALLOW TRUE)

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(AMALGAMATE_SOURCES OFF CACHE BOOL "" FORCE)
set(INSTALL_PROJECT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG 3.1.0
    GIT_SHALLOW TRUE)

FetchContent_MakeAvailable(wxwidgets miniz)

set(CORE_SOURCES
    src/util.cpp src/sha256.cpp src/macho.cpp src/prepatch.cpp src/plist.cpp
    src/ftldat.cpp src/zipio.cpp src/icon.cpp src/patch.cpp stb_impl.cpp)

add_library(ftl-core STATIC ${CORE_SOURCES})
target_include_directories(ftl-core PUBLIC src ${CMAKE_CURRENT_SOURCE_DIR}/../src/third_party)
target_compile_definitions(ftl-core PUBLIC MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
target_link_libraries(ftl-core PUBLIC miniz)
if(NOT MSVC)
    target_compile_options(ftl-core PUBLIC -Wno-unused-function)  # miniz.h's zlib-style wrappers
endif()
if(WIN32)
    target_compile_definitions(ftl-core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()
if(MINGW)
    target_link_options(ftl-core INTERFACE -static -static-libgcc -static-libstdc++ $<$<CONFIG:Release>:-s>)
elseif(UNIX AND NOT APPLE)
    target_link_options(ftl-core INTERFACE -static-libgcc -static-libstdc++)  # runs without a newer libstdc++
endif()

set(APP_SOURCES src/main.cpp src/gui.cpp)
if(WIN32)
    enable_language(RC)
    list(APPEND APP_SOURCES src/app.rc)
endif()
add_executable(ftl-ios-patcher WIN32 MACOSX_BUNDLE ${APP_SOURCES})
target_link_libraries(ftl-ios-patcher PRIVATE ftl-core wx::core wx::base)
if(WIN32)
    target_link_libraries(ftl-ios-patcher PRIVATE shell32)
endif()

# The release tool (macOS with Xcode): builds ftlios.dylib, records the prepatches, zips the apps
if(APPLE)
    add_executable(ftl-ios-release src/release.cpp)
    target_link_libraries(ftl-ios-release PRIVATE ftl-core)
    target_compile_definitions(ftl-ios-release PRIVATE FTL_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/..")
endif()

if(WIN32)
    set_target_properties(ftl-ios-patcher PROPERTIES OUTPUT_NAME "FTL iOS Patcher")
endif()
if(APPLE)
    set_target_properties(ftl-ios-patcher PROPERTIES
        OUTPUT_NAME "FTL iOS Patcher"
        MACOSX_BUNDLE_BUNDLE_NAME "FTL iOS Patcher"
        MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.the-dumb-dino.ftl-ios-patcher"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
endif()
