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

if(DEFINED VITA_ELF_CREATE)
    # PlayStation Vita (vitasdk toolchain file).
    set(VITA TRUE)
    include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
    set(VITA_TITLEID "PIGI00001" CACHE STRING "Vita title id")
    set(VITA_VERSION "01.00" CACHE STRING "Vita app version")
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
    target_link_libraries(igi PRIVATE igi_recomp
        SDL2 vitaGL vitashark SceShaccCgExt mathneon taihen_stub
        SceShaccCg_stub SceKernelDmacMgr_stub SceCtrl_stub SceTouch_stub SceHid_stub SceMotion_stub
        SceAudio_stub SceAudioIn_stub SceDisplay_stub SceGxm_stub SceCommonDialog_stub SceSysmodule_stub
        ScePower_stub SceAppMgr_stub SceAppUtil_stub SceIme_stub SceLibKernel_stub SceProcessmgr_stub
        stdc++ pthread m c)
    vita_create_self(eboot.bin igi UNSAFE)
    vita_create_vpk(igi.vpk ${VITA_TITLEID} eboot.bin
        VERSION ${VITA_VERSION}
        NAME "Project IGI"
        FILE ${CMAKE_SOURCE_DIR}/platform/vita/sce_sys sce_sys)
else()
    find_package(SDL2 REQUIRED)
    find_package(OpenGL REQUIRED)
    target_link_libraries(igi PRIVATE igi_recomp SDL2::SDL2 OpenGL::GL m)
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
