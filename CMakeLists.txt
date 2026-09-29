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

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(GEN_DIR "${CMAKE_BINARY_DIR}/gen")
file(GLOB RECOMP_TOOLS "${CMAKE_SOURCE_DIR}/tools/recomp/*.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${RECOMP_TOOLS} "${IGI_EXE}")

message(STATUS "Recompiling ${IGI_EXE}")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/recomp/gen.py" "${IGI_EXE}" "${GEN_DIR}"
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
target_compile_options(igi_recomp PRIVATE -Wall -Wextra -O2)

file(GLOB RUNTIME_SOURCES runtime/src/*.c)
file(GLOB SHIM_SOURCES shim/src/*.c)

add_executable(igi ${RUNTIME_SOURCES} ${SHIM_SOURCES})
target_include_directories(igi PRIVATE runtime/include shim/include "${GEN_DIR}")
target_compile_options(igi PRIVATE -Wall -Wextra)
target_link_libraries(igi PRIVATE igi_recomp m)
