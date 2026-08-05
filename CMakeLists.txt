cmake_minimum_required(VERSION 3.16)
project(gwemu VERSION 1.1 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(GWEMU_STATIC "Link SDL and the runtime in, for a self-contained binary" OFF)
option(GWEMU_NATIVE_ARCH "Tune for the building machine (not for redistribution)" OFF)

# windres preprocesses through a shell, so every -I it is handed has to survive
# one, and this repository's path contains '&'. The resource needs no include
# path of its own. Set after project(), because the platform module installs
# its own rule while project() runs.
if(MINGW)
    set(CMAKE_RC_COMPILE_OBJECT "<CMAKE_RC_COMPILER> -O coff -o <OBJECT> <SOURCE>")
endif()

# --------------------------------------------------------------------
# SDL2 - the only dependency
# --------------------------------------------------------------------
find_package(SDL2 QUIET CONFIG)
if(SDL2_FOUND)
    if(GWEMU_STATIC AND TARGET SDL2::SDL2-static)
        set(GWEMU_SDL SDL2::SDL2-static)
    else()
        set(GWEMU_SDL SDL2::SDL2)
    endif()
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    set(GWEMU_SDL PkgConfig::SDL2)
endif()

# --------------------------------------------------------------------
# emulator core - portable C, no host dependencies
# --------------------------------------------------------------------
add_library(gwcore STATIC
    src/core/cpu.c
    src/core/bus.c
    src/core/periph.c
    src/core/octospi.c
    src/core/aes.c
    src/core/cryp.c
    src/core/state.c
    src/core/emu.c
)
target_include_directories(gwcore PUBLIC src/core)

# The interpreter is the whole cost of this program, so it is optimised even in
# a debug build; the frontend is what anyone actually steps through.
target_compile_options(gwcore PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-O3 -Wall -Wextra -Wno-unused-parameter>
)
if(GWEMU_NATIVE_ARCH)
    target_compile_options(gwcore PRIVATE -march=native)
endif()

# --------------------------------------------------------------------
# Dear ImGui - vendored, so there is no toolkit to find on any platform
# --------------------------------------------------------------------
set(IMGUI ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
add_library(imgui STATIC
    ${IMGUI}/imgui.cpp
    ${IMGUI}/imgui_draw.cpp
    ${IMGUI}/imgui_tables.cpp
    ${IMGUI}/imgui_widgets.cpp
    ${IMGUI}/backends/imgui_impl_sdl2.cpp
    ${IMGUI}/backends/imgui_impl_sdlrenderer2.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI} ${IMGUI}/backends)
target_link_libraries(imgui PUBLIC ${GWEMU_SDL})

# --------------------------------------------------------------------
# frontend
# --------------------------------------------------------------------
add_executable(gwemu
    src/frontend/main.c
    src/frontend/app.cpp
    src/frontend/config.c
    src/frontend/session.c
    src/frontend/savestate.c
    src/frontend/paths.c
    src/frontend/audio.c
    src/frontend/options.c
    src/frontend/cli.c
)
target_include_directories(gwemu PRIVATE src)
target_link_libraries(gwemu PRIVATE gwcore imgui ${GWEMU_SDL})

if(NOT MSVC)
    target_compile_options(gwemu PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

find_library(MATH_LIBRARY m)
if(MATH_LIBRARY)
    target_link_libraries(gwemu PRIVATE ${MATH_LIBRARY})
endif()

# Discard what is never referenced, and carry no debug symbols in a release.
if(NOT MSVC AND CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(gwemu PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(gwemu PRIVATE -Wl,--gc-sections -s)
endif()

# A self-contained binary: SDL and the compiler runtime linked in, so there is
# nothing beside the executable and nothing to install. SDL still opens X11,
# Wayland and the audio servers at runtime rather than linking them, so this
# stays portable across desktops.
if(GWEMU_STATIC)
    target_link_options(gwemu PRIVATE -static-libgcc -static-libstdc++)
    if(WIN32)
        target_link_options(gwemu PRIVATE -static)
    endif()
endif()

if(WIN32)
    # SDL renames main() to SDL_main on Windows and supplies the WinMain that
    # calls it, which lives in SDL2main.
    if(TARGET SDL2::SDL2main)
        target_link_libraries(gwemu PRIVATE SDL2::SDL2main)
    endif()

    # No console flash when double-clicked; main() attaches to the parent
    # console so --headless and --help still print from a command prompt.
    set_target_properties(gwemu PROPERTIES WIN32_EXECUTABLE TRUE)
    target_sources(gwemu PRIVATE src/platform/windows/gwemu.rc)
endif()

if(UNIX AND NOT APPLE)
    include(GNUInstallDirs)
    install(TARGETS gwemu RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES src/platform/linux/gwemu.desktop
            DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
    install(FILES src/platform/linux/gwemu.png
            DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/256x256/apps)
endif()
