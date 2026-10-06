cmake_minimum_required(VERSION 3.20)
project(igi-port C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# The recompiled code is generated from the user's own copy of the game.
set(IGI_EXE "${CMAKE_SOURCE_DIR}/gamedata/pc/IGI.exe" CACHE FILEPATH "Path to the retail IGI.exe")
if(NOT EXISTS "${IGI_EXE}")
    message(FATAL_ERROR "IGI.exe not found at ${IGI_EXE}; pass -DIGI_EXE=/path/to/IGI.exe")
endif()

# The port's version: the one place it is set. Vita releases are tagged v<this> on
# IGI_UPDATE_REPO (docs/VITA_RELEASE.md); the update check installs a release only when
# its version is higher, so raise it for every release. major.minor.patch, each 0-9 for
# minor and patch (the Vita's APP_VER is NN.NN: 0.1.0 -> 00.10, 1.2.3 -> 01.23).
set(IGI_PORT_VERSION "0.3.0")
set(IGI_UPDATE_REPO "HenryKun55/igi-vita" CACHE STRING "GitHub owner/repo whose releases the Vita update check reads")
if(NOT IGI_PORT_VERSION MATCHES "^([0-9]+)\\.([0-9])\\.([0-9])$")
    message(FATAL_ERROR "IGI_PORT_VERSION must be major.minor.patch with one-digit minor and patch")
endif()
math(EXPR IGI_VERSION_MAJOR "${CMAKE_MATCH_1}")
if(IGI_VERSION_MAJOR LESS 10)
    set(IGI_VERSION_MAJOR "0${IGI_VERSION_MAJOR}")
endif()
set(IGI_APP_VER "${IGI_VERSION_MAJOR}.${CMAKE_MATCH_2}${CMAKE_MATCH_3}")

if(DEFINED VITA_ELF_CREATE)
    # PlayStation Vita (vitasdk toolchain file).
    set(VITA TRUE)
    include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
    set(VITA_TITLEID "PIGI00001" CACHE STRING "Vita title id")
    set(VITA_VERSION "${IGI_APP_VER}")   # param.sfo APP_VER, from IGI_PORT_VERSION
    add_compile_definitions(__vita__)
    add_compile_options(-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -fno-short-enums
                        -ffunction-sections -fdata-sections -fno-pic -fno-pie)
    add_link_options(-Wl,-q -Wl,--gc-sections -static -fno-short-enums -Wl,--no-warn-mismatch -Wl,-z,noexecstack)
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(GEN_DIR "${CMAKE_BINARY_DIR}/gen")
file(GLOB RECOMP_TOOLS "${CMAKE_SOURCE_DIR}/tools/recomp/*.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${RECOMP_TOOLS} "${IGI_EXE}")
# Diagnostic entry probes (hex guest addresses), e.g. -DIGI_PROBES="497620 4E7960".
set(IGI_PROBES "" CACHE STRING "Guest functions to instrument with rt_probe")

message(STATUS "Recompiling ${IGI_EXE}")
execute_process(
    COMMAND ${CMAKE_COMMAND} -E env "IGI_PROBES=${IGI_PROBES}"
            "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/recomp/gen.py" "${IGI_EXE}" "${GEN_DIR}"
    RESULT_VARIABLE GEN_RESULT
    OUTPUT_VARIABLE GEN_OUTPUT)
if(NOT GEN_RESULT EQUAL 0)
    message(FATAL_ERROR "recompiler failed:\n${GEN_OUTPUT}")
endif()
message(STATUS "${GEN_OUTPUT}")

file(STRINGS "${GEN_DIR}/files.txt" GEN_FILES)
list(TRANSFORM GEN_FILES PREPEND "${GEN_DIR}/")

add_library(igi_recomp STATIC ${GEN_FILES})
target_include_directories(igi_recomp PUBLIC runtime/include "${GEN_DIR}")
# Guest code relies on x86 semantics: wrapping signed arithmetic, type punning
# and signed char. Without these, GCC (Vita) and clang (macOS) optimize the
# recompiled code differently.
target_compile_options(igi_recomp PRIVATE -Wall -Wextra -O2 -fwrapv -fno-strict-aliasing -fsigned-char)
# Generated-code variants (see runtime/include/recomp.h). IGI_X87_FLOAT trades x87
# exactness for speed; IGI_X87_CHECK verifies the static x87 stack analysis.
option(IGI_X87_FLOAT "Single-precision x87 registers in generated code (faster, inexact)" OFF)
option(IGI_X87_CHECK "Check statically derived x87 stack effects at run time" OFF)
if(IGI_X87_FLOAT)
    target_compile_definitions(igi_recomp PRIVATE IGI_X87_FLOAT)
endif()
if(IGI_X87_CHECK)
    target_compile_definitions(igi_recomp PRIVATE IGI_X87_CHECK)
endif()

file(GLOB RUNTIME_SOURCES CONFIGURE_DEPENDS runtime/src/*.c)
file(GLOB SHIM_SOURCES CONFIGURE_DEPENDS shim/src/*.c)

add_executable(igi ${RUNTIME_SOURCES} ${SHIM_SOURCES})
target_include_directories(igi PRIVATE runtime/include shim/include "${GEN_DIR}")
target_compile_options(igi PRIVATE -Wall -Wextra -fwrapv -fno-strict-aliasing -fsigned-char)
target_compile_definitions(igi PRIVATE IGI_PORT_VERSION="${IGI_PORT_VERSION}")
# Sampling profiler (runtime/src/profile.c): hooks in the generated code and a
# sampling thread; writes profile.txt / igi_profile.txt. No cost when OFF.
option(IGI_PROFILE "Build with the sampling profiler" OFF)
if(IGI_PROFILE)
    target_compile_definitions(igi_recomp PUBLIC IGI_PROFILE)
    target_compile_definitions(igi PRIVATE IGI_PROFILE)
endif()

if(VITA)
    target_include_directories(igi PRIVATE "$ENV{VITASDK}/arm-vita-eabi/include/SDL2")
    # Cutscene video: minimal FFmpeg from tools/build_ffmpeg_vita.sh, when built.
    if(EXISTS "${CMAKE_SOURCE_DIR}/deps/vita/lib/libavcodec.a")
        target_include_directories(igi PRIVATE "${CMAKE_SOURCE_DIR}/deps/vita/include")
        target_compile_definitions(igi PRIVATE IGI_HAVE_FFMPEG)
        target_link_libraries(igi PRIVATE "${CMAKE_SOURCE_DIR}/deps/vita/lib/libavcodec.a"
                                          "${CMAKE_SOURCE_DIR}/deps/vita/lib/libavutil.a")
    else()
        message(STATUS "deps/vita FFmpeg not built: AVI cutscenes will be skipped (tools/build_ffmpeg_vita.sh)")
    endif()
    # vitaGL: the private build from tools/build_vitagl_vita.sh (pinned commit plus
    # tools/vitagl/*.patch) when present, else the SDK's.
    set(IGI_VITAGL_DIR "${CMAKE_SOURCE_DIR}/deps/vita" CACHE PATH "Prefix of the private vitaGL build")
    if(EXISTS "${IGI_VITAGL_DIR}/lib/libvitaGL.a")
        message(STATUS "Using the private vitaGL in ${IGI_VITAGL_DIR}")
        target_include_directories(igi BEFORE PRIVATE "${IGI_VITAGL_DIR}/include")
        set(IGI_VITAGL_LIB "${IGI_VITAGL_DIR}/lib/libvitaGL.a")
    else()
        message(STATUS "Private vitaGL not built: using the SDK's (tools/build_vitagl_vita.sh)")
        set(IGI_VITAGL_LIB vitaGL)
    endif()
    target_link_libraries(igi PRIVATE igi_recomp
        SDL2 ${IGI_VITAGL_LIB} vitashark SceShaccCgExt mathneon taihen_stub
        SceShaccCg_stub SceKernelDmacMgr_stub SceCtrl_stub SceTouch_stub SceHid_stub SceMotion_stub
        SceAudio_stub SceAudioIn_stub SceDisplay_stub SceGxm_stub SceCommonDialog_stub SceSysmodule_stub
        ScePower_stub SceAppMgr_stub SceAppUtil_stub SceIme_stub SceLibKernel_stub SceProcessmgr_stub
        SceNet_stub SceNetCtl_stub SceIofilemgr_stub
        curl ssl crypto z zstd stdc++ pthread m c)
    # The update check (shim/src/vita_update.c) and its start-up screen (vita_bootui.c).
    target_compile_definitions(igi PRIVATE IGI_UPDATE_REPO="${IGI_UPDATE_REPO}")
    # The LiveArea pictures come from the game's own art (the loading screen), so
    # they are made here from the game data instead of being kept in the repo.
    get_filename_component(IGI_PC_DIR "${IGI_EXE}" DIRECTORY)
    set(IGI_SCE_SYS "${CMAKE_BINARY_DIR}/sce_sys")
    file(COPY "${CMAKE_SOURCE_DIR}/platform/vita/sce_sys/" DESTINATION "${IGI_SCE_SYS}")
    execute_process(
        # No version on the LiveArea: an update from the game doesn't replace sce_sys,
        # so it would go stale (the boot screen shows the running version).
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/make_vita_livearea.py" "${IGI_PC_DIR}" "${IGI_SCE_SYS}"
        RESULT_VARIABLE IGI_LIVEAREA_RESULT)
    if(NOT IGI_LIVEAREA_RESULT EQUAL 0)
        message(FATAL_ERROR "tools/make_vita_livearea.py failed (it needs Pillow: pip install Pillow)")
    endif()
    # The start-up screen's fonts (Oswald, OFL).
    set(IGI_BOOT_FONTS "${CMAKE_BINARY_DIR}/vita_font")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/make_vita_boot_font.py" "${IGI_BOOT_FONTS}"
        RESULT_VARIABLE IGI_FONT_RESULT)
    if(NOT IGI_FONT_RESULT EQUAL 0)
        message(FATAL_ERROR "tools/make_vita_boot_font.py failed (it needs Pillow: pip install Pillow)")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tools/make_vita_livearea.py"
        "${CMAKE_SOURCE_DIR}/tools/make_vita_boot_font.py" "${CMAKE_SOURCE_DIR}/platform/vita/fonts/Oswald.ttf"
        "${IGI_PC_DIR}/menusystem/loadingscreen.res")

    # updater.bin: puts a downloaded release in place of the game's files
    # (platform/vita/updater). A separate program, so the game is closed while its files
    # are replaced; unsafe, as only unsafe homebrew may write to ux0:app.
    set(IGI_UPDATER_DIR "${CMAKE_SOURCE_DIR}/platform/vita/updater")
    add_executable(igi_updater "${IGI_UPDATER_DIR}/updater.c" "${IGI_UPDATER_DIR}/debugScreen.c")
    target_compile_options(igi_updater PRIVATE -Wall -Wextra)
    target_link_libraries(igi_updater PRIVATE png z m
        SceDisplay_stub SceCtrl_stub SceAppMgr_stub ScePower_stub SceLibKernel_stub
        SceKernelThreadMgr_stub SceIofilemgr_stub SceProcessmgr_stub)
    vita_create_self(updater.bin igi_updater UNSAFE)

    vita_create_self(eboot.bin igi UNSAFE)
    vita_create_vpk(igi.vpk ${VITA_TITLEID} eboot.bin
        VERSION ${VITA_VERSION}
        NAME "Project IGI"
        FILE ${IGI_SCE_SYS} sce_sys
             "${CMAKE_BINARY_DIR}/updater.bin.out" updater.bin
             # CA certificates for the update check's HTTPS (curl.se/ca, MPL 2.0)
             "${IGI_UPDATER_DIR}/cacert.pem" cacert.pem
             "${IGI_BOOT_FONTS}/boot.font" font/boot.font
             "${IGI_BOOT_FONTS}/boot_small.font" font/boot_small.font
             "${CMAKE_SOURCE_DIR}/platform/vita/fonts/OFL.txt" font/Oswald-OFL.txt)
else()
    find_package(SDL2 REQUIRED)
    find_package(OpenGL REQUIRED)
    target_link_libraries(igi PRIVATE igi_recomp SDL2::SDL2 OpenGL::GL m)
    if(APPLE)
        # The render thread's main-thread servicing (platform_service_main).
        target_link_libraries(igi PRIVATE "-framework CoreFoundation")
    else()
        target_link_libraries(igi PRIVATE pthread)
    endif()
    # Cutscene video (Indeo 5) through libavcodec when available.
    find_package(PkgConfig)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(AVCODEC IMPORTED_TARGET libavcodec libavutil)
    endif()
    if(AVCODEC_FOUND)
        target_compile_definitions(igi PRIVATE IGI_HAVE_FFMPEG)
        target_link_libraries(igi PRIVATE PkgConfig::AVCODEC)
    else()
        message(STATUS "libavcodec not found: AVI cutscenes will be skipped")
    endif()
endif()
