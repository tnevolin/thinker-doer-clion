cmake_minimum_required(VERSION 3.10)

# Check if we are using the correct compiler architecture
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "This project requires a 32-bit (i686) toolchain.
Your current toolchain uses a 64-bit compiler: ${CMAKE_CXX_COMPILER}
The bundled CLion MinGW (GCC 13+) is 64-bit only and does NOT support 32-bit builds.

To fix this:
1. Go to File | Settings | Build, Execution, Deployment | Toolchains.
2. Create a new MinGW toolchain.
3. Set 'Environment' to 'C:/mingw32' (the standalone 32-bit MinGW).
4. Go to Settings | Build, Execution, Deployment | CMake.
5. Ensure your 'Debug' profile uses this new 32-bit toolchain.")
endif()

project(thinker)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Global compiler options from CBP
add_compile_options(
    -march=i686
    -mtune=generic
    -pedantic
    -Wall
    -Wextra
    -Wshadow
    -Wundef
    -Wpointer-arith
    -Wfloat-conversion
    -Wno-shadow
)

# C++ specific flags
add_definitions(-D_X86_)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wuseless-cast")

if(MSVC)
    # Mapping GCC-style flags to MSVC if necessary, but project uses MinGW/GCC
else()
    # Add target specific flags later
endif()

# thinker.dll target
set(THINKER_SOURCES
    src/base.cpp
    src/base.h
    src/build.cpp
    src/build.h
    src/config.cpp
    src/config.h
    src/debug.cpp
    src/debug.h
    src/engine.cpp
    src/engine.h
    src/engine_base.h
    src/engine_enums.h
    src/engine_types.h
    src/engine_veh.h
    src/engine_win.h
    src/faction.cpp
    src/faction.h
    src/game.cpp
    src/game.h
    src/goal.cpp
    src/goal.h
    src/gui.cpp
    src/gui.h
    src/gui_dialog.cpp
    src/gui_dialog.h
    src/lib/FastNoiseLite.h
    src/lib/ini.c
    src/lib/ini.h
    src/lib/tree.hh
    src/main.cpp
    src/main.h
    src/map.cpp
    src/map.h
    src/mapgen.cpp
    src/mapgen.h
    src/move.cpp
    src/move.h
    src/net.cpp
    src/net.h
    src/patch.cpp
    src/patch.h
    src/patchdata.h
    src/patchveh.h
    src/path.cpp
    src/path.h
    src/plan.cpp
    src/plan.h
    src/probe.cpp
    src/probe.h
    src/random.cpp
    src/random.h
    src/strings.cpp
    src/strings.h
    src/tech.cpp
    src/tech.h
    src/test.cpp
    src/test.h
    src/veh.cpp
    src/veh.h
    src/veh_combat.cpp
    src/veh_combat.h
    src/veh_turn.cpp
    src/veh_turn.h
    src/wtp_ai.cpp
    src/wtp_ai.h
    src/wtp_ai_game.cpp
    src/wtp_ai_game.h
    src/wtp_aiHurry.cpp
    src/wtp_aiHurry.h
    src/wtp_aiMove.cpp
    src/wtp_aiMove.h
    src/wtp_aiMoveArtifact.cpp
    src/wtp_aiMoveArtifact.h
    src/wtp_aiMoveColony.cpp
    src/wtp_aiMoveColony.h
    src/wtp_aiMoveCombat.cpp
    src/wtp_aiMoveCombat.h
    src/wtp_aiMoveCrawler.cpp
    src/wtp_aiMoveCrawler.h
#        src/wtp_aiMoveFormer_backup.cpp
#        src/wtp_aiMoveFormer_backup.h
    src/wtp_aiMoveTransport.cpp
    src/wtp_aiMoveTransport.h
    src/wtp_aiProduction.cpp
    src/wtp_aiProduction.h
    src/wtp_aiRoute.cpp
    src/wtp_aiRoute.h
    src/wtp_aiTask.cpp
    src/wtp_aiTask.h
    src/wtp_base.cpp
    src/wtp_base.h
    src/wtp_game.cpp
    src/wtp_game.h
    src/wtp_mod.cpp
    src/wtp_mod.h
    src/wtp_patch.cpp
    src/wtp_patch.h
    src/wtp_terranx.cpp
    src/wtp_terranx.h
    src/wtp_terranx_enum.h
        src/wtp_aiMoveFormer.cpp
        src/wtp_aiMoveFormer.h
        src/wtp_aiMoveProbe.cpp
        src/wtp_aiMoveProbe.h
)

add_library(thinker SHARED ${THINKER_SOURCES})
set_target_properties(thinker PROPERTIES PREFIX "" OUTPUT_NAME "thinker")
target_compile_definitions(thinker PRIVATE BUILD_DLL)

# Set ini.c as C source specifically if needed
set_source_files_properties(src/lib/ini.c PROPERTIES LANGUAGE C)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(thinker PRIVATE BUILD_DEBUG)
    target_compile_options(thinker PRIVATE -g -O0)
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_definitions(thinker PRIVATE BUILD_REL)
    target_compile_options(thinker PRIVATE -O2 -fno-common -fno-strict-overflow -fno-strict-aliasing -fno-delete-null-pointer-checks -fno-optimize-sibling-calls)
    target_link_libraries(thinker PRIVATE -s -static)
endif()

target_link_libraries(thinker PRIVATE user32 winmm gdi32 psapi)

# thinkerlaunch EXE
set(LAUNCH_SOURCES
    src/launch.cpp
    src/launch.h
    src/launch.rc
)

add_executable(thinkerlaunch ${LAUNCH_SOURCES})
set_target_properties(thinkerlaunch PROPERTIES OUTPUT_NAME "thinker")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(thinkerlaunch PRIVATE BUILD_DEBUG)
    target_compile_options(thinkerlaunch PRIVATE -g -O0)
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_definitions(thinkerlaunch PRIVATE BUILD_REL)
    target_compile_options(thinkerlaunch PRIVATE -O2 -flto -fno-rtti -fno-strict-aliasing -fno-delete-null-pointer-checks)
    target_link_libraries(thinkerlaunch PRIVATE -s -static)
endif()

target_link_libraries(thinkerlaunch PRIVATE user32)

# Custom commands for copying to patch/ directory
add_custom_command(TARGET thinker POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:thinker> ${CMAKE_CURRENT_SOURCE_DIR}/patch/
    COMMENT "Copying thinker DLL to patch directory"
)

# Debug specific deployment
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_custom_command(TARGET thinker POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/resources/configuration ${CMAKE_CURRENT_SOURCE_DIR}/patch/
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/resources/factions ${CMAKE_CURRENT_SOURCE_DIR}/patch/
        COMMENT "Copying Debug resources to patch directory"
    )
endif()

# Release specific deployment
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(RELEASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/releases/TheWillToPower")
    add_custom_command(TARGET thinker POST_BUILD
        # Documentation
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/README.md ${RELEASE_DIR}/
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/changelog.md ${RELEASE_DIR}/
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/installation.md ${RELEASE_DIR}/
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/strategy.md ${RELEASE_DIR}/
        # Core binaries and config
        COMMAND ${CMAKE_COMMAND} -E make_directory ${RELEASE_DIR}/core
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/resources/exe/terranx_mod.exe ${RELEASE_DIR}/core/
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:thinkerlaunch> ${RELEASE_DIR}/core/
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:thinker> ${RELEASE_DIR}/core/
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/resources/configuration ${RELEASE_DIR}/core/
        # Basenames
        COMMAND ${CMAKE_COMMAND} -E make_directory ${RELEASE_DIR}/core/basenames
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/resources/configuration/basenames ${RELEASE_DIR}/core/basenames/
        # Factions
        COMMAND ${CMAKE_COMMAND} -E make_directory ${RELEASE_DIR}/factions
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/resources/factions ${RELEASE_DIR}/factions/
        COMMENT "Copying Release artifacts to releases directory"
    )
endif()
