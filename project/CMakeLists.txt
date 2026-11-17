cmake_minimum_required(VERSION 3.20)

# ============================================================================
# YOUR_GAME_NAME - Static Recompilation
#
# Copy this template to your game project root and customize:
#   1. Change the project name below
#   2. Set XBOXRECOMP_DIR to where you cloned xboxrecomp
#   3. Adjust source file lists as you add game-specific code
#   4. Add any game-specific compile definitions or link libraries
# ============================================================================

project(fatalframe C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# ── xboxrecomp toolkit ──────────────────────────────────────────
#
# Option A: xboxrecomp as a subdirectory (simplest)
#   Place xboxrecomp/ next to your game project and use a relative path.
#
# Option B: find_package (if you install xboxrecomp system-wide)
#   find_package(xboxrecomp REQUIRED)
#
# Where the toolkit lives. It is "toolkit" in this repository and in a
# release, and "xboxrecomp" in a plain clone of it beside the project, so
# look for both rather than making one layout the only one that builds.
# Override with -DXBOXRECOMP_DIR=... for anything else.
if(NOT XBOXRECOMP_DIR)
  foreach(_cand "${CMAKE_CURRENT_SOURCE_DIR}/../toolkit"
                "${CMAKE_CURRENT_SOURCE_DIR}/../xboxrecomp")
    if(EXISTS "${_cand}/CMakeLists.txt")
      set(XBOXRECOMP_DIR "${_cand}")
      break()
    endif()
  endforeach()
endif()
if(NOT XBOXRECOMP_DIR OR NOT EXISTS "${XBOXRECOMP_DIR}/CMakeLists.txt")
  message(FATAL_ERROR
    "Cannot find the xboxrecomp toolkit. Expected it beside this project as "
    "../toolkit or ../xboxrecomp; pass -DXBOXRECOMP_DIR=<path> if it is "
    "somewhere else.")
endif()
set(XBOXRECOMP_DIR "${XBOXRECOMP_DIR}" CACHE PATH
    "Path to the xboxrecomp toolkit root directory" FORCE)
message(STATUS "xboxrecomp toolkit: ${XBOXRECOMP_DIR}")

# Add xboxrecomp as a subdirectory -- this builds all 6 runtime libraries
# and provides the 'xboxrecomp' umbrella target to link against.
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# ── Recompiled game executable ──────────────────────────────────

# Collect generated recompiled code.
# The recomp pipeline outputs files into src/recomp/gen/.
# With --split 1000, you get gen_0000.c, gen_0001.c, etc.
# CONFIGURE_DEPENDS so re-running the recompiler picks up added/removed files
# without a manual re-configure. A plain GLOB is evaluated once at configure
# time, so a newly generated file (an extra chunk, recomp_stubs_unresolved.c)
# is silently left out of the build and shows up as unresolved externals.
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "src/recomp/gen/*.c")

# Core game sources
set(GAME_SOURCES
    src/main.c
    src/recomp_manual.c
    src/ff_launcher.c
    src/ff_gpu.c
    src/ff_watch.c
    src/ff_anim.c
    src/ff_cheats.c
    src/ff_tmdl.c
    # Direct3D 11 renderer for the NV2A pushbuffer executor (host GPU).
    ${XBOXRECOMP_DIR}/src/kernel/nv2a_d3d11.c
    # Add game-specific sources here, e.g.:
    # src/asset_loader.c
    # src/custom_renderer.c
)

# The recomp pipeline also generates a dispatch header and function declarations.
# These are included by the generated .c files:
#   src/recomp/gen/recomp_funcs.h    - extern declarations for all recompiled functions
#   src/recomp/recomp_dispatch.h     - binary search dispatch table
#   src/recomp/recomp_types.h        - register model, ICALL macros, MEM helpers

add_executable(${PROJECT_NAME}
    ${GAME_SOURCES}
    ${RECOMP_GEN_SOURCES}
)

# Link against the xboxrecomp umbrella target (all 6 runtime libraries)
target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

# Include paths for generated recomp headers
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${XBOXRECOMP_DIR}/include
    ${XBOXRECOMP_DIR}/src
    ${XBOXRECOMP_DIR}/src/kernel
)

# Windows SDK libraries needed at link time
target_link_libraries(${PROJECT_NAME} PRIVATE
    d3d11
    dxgi
    dxguid
    $<IF:$<BOOL:${MINGW}>,xinput1_4,xinput>   # see src/input/CMakeLists.txt
    winmm
    comdlg32
    d3dcompiler
    windowscodecs
    ole32
    uuid
    # Lets the fault handler resolve the host RIP back to the generated
    # function's symbol, i.e. to the guest function that faulted.
    dbghelp
)

# MinGW: link the compiler's own runtime into the executable.
#
# Otherwise the game depends on libgcc_s_seh-1.dll and libwinpthread-1.dll,
# which live beside the compiler -- and the compiler is a build-time thing
# the launcher stages and removes again. The game ran once, from the shell
# that still had the compiler on PATH, and then could never start again.
# Static here means the built exe stands on its own: copy it anywhere, run
# it from Explorer, no PATH and no DLLs beside it.
if(MINGW)
    target_link_options(${PROJECT_NAME} PRIVATE
        -static
        -static-libgcc
    )
endif()

# MSVC-specific: large address aware, increase section limit for big generated files
if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE
        /LARGEADDRESSAWARE
    )
    # The generated code files can be very large. Increase object file section limit.
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)
endif()

# ── Game-specific additions ─────────────────────────────────────
#
# Add any game-specific libraries, loaders, or custom renderers below.
# Examples:
#
# # Custom texture loader for your game's format
# target_sources(${PROJECT_NAME} PRIVATE src/texture_loader.c)
#
# # If your game uses RenderWare, you may need a RW bridge layer
# target_sources(${PROJECT_NAME} PRIVATE src/rw_bridge.c)
#
# # Additional compile definitions for debugging
# target_compile_definitions(${PROJECT_NAME} PRIVATE
#     ENABLE_ICALL_TRACE=1
#     VERBOSE_KERNEL_LOG=1
# )
