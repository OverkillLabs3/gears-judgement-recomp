# Gears of War: Judgment, Windows build.
#
# The recompiled game code in generated/ is produced from your own default.xex by
# `rexglue codegen` and is not part of this repository. Run codegen first (see BUILDING.md);
# scripts/build.ps1 does the whole sequence.

cmake_minimum_required(VERSION 3.25)
project(gowj LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Written by `rexglue codegen`.
include(generated/rexglue.cmake)

set(GOWJ_SOURCES
    src/main.cpp
    src/gowj_kbm_input.cpp
    src/gowj_utf8_guard.cpp
    src/gowj_content_probe.cpp
    src/gowj_save_io_probe.cpp
    src/gowj_view_accel.cpp
    src/gowj_native_input.cpp
    src/gowj_checkpoint_probe.cpp
    src/gowj_blur_kill.cpp
    src/gowj_game_patches.cpp
    src/gowj_fsr.cpp
    src/gowj_pso_library.cpp
    src/gowj_fps_overlay.cpp
    src/gowj_quality.cpp
    src/gowj_coalesced.cpp
    src/gowj_fps_unlock.cpp
    src/gowj_system_tuning.cpp
    src/gowj_xam_input_guard.cpp
    src/gowj_motion_blur.cpp
    src/gowj_perf_watch.cpp
    src/gowj_crash_watch.cpp
    src/gowj_render_scale.cpp
)

# Keep the build machine's folder names out of the binaries (they would otherwise end up in the
# log messages' __FILE__ strings).
add_compile_options(-fmacro-prefix-map=${CMAKE_SOURCE_DIR}=. -ffile-prefix-map=${CMAKE_SOURCE_DIR}=.)
foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
    file(TO_CMAKE_PATH "${_prefix}" _prefix)
    add_compile_options(-fmacro-prefix-map=${_prefix}=sdk -ffile-prefix-map=${_prefix}=sdk)
endforeach()

add_executable(gowj WIN32 ${GOWJ_SOURCES})

# Development hooks (remote pad for unattended test runs, overlay without focus). Off in releases.
option(GOWJ_DEV_TOOLS "Development-only test hooks" OFF)
if(GOWJ_DEV_TOOLS)
    target_compile_definitions(gowj PRIVATE GOWJ_DEV_TOOLS=1)
    target_sources(gowj PRIVATE src/gowj_profiler.cpp)
    target_link_libraries(gowj PRIVATE pdh)
endif()
target_sources(gowj PRIVATE src/gowj_pgo.cpp)

# PGO (profile-guided optimisation) of the generated guest code, ~100 MB of straight-line recompiled
# PPC where block layout, inlining and hot/cold splitting matter most.
#   GOWJ_PGO=gen  instrumented build (a training build, slower than normal); src/gowj_pgo.cpp flushes the
#                 profile every 20 s. Run with LLVM_PROFILE_FILE=<dir>/gowj_%m.profraw
#   GOWJ_PGO=use  rebuild with the merged profile: llvm-profdata merge -o pgo/gowj.profdata *.profraw
# The profile only matches the exact generated sources it was collected from.
set(GOWJ_PGO "off" CACHE STRING "PGO mode for the generated guest code: off | gen | use")
set(GOWJ_PGO_PROFDATA "${CMAKE_CURRENT_SOURCE_DIR}/pgo/gowj.profdata" CACHE FILEPATH "Merged profile for GOWJ_PGO=use")
if(TARGET gowj_recomp AND GOWJ_PGO STREQUAL "gen")
    get_filename_component(_llvm_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    file(GLOB _prof_libs "${_llvm_bin}/../lib/clang/*/lib/windows/clang_rt.profile-x86_64.lib")
    list(GET _prof_libs 0 _prof_lib)
    if(NOT _prof_lib)
        message(FATAL_ERROR "GOWJ_PGO=gen: clang_rt.profile-x86_64.lib not found under ${_llvm_bin}/../lib/clang")
    endif()
    target_compile_options(gowj_recomp PRIVATE -fprofile-instr-generate)
    target_link_libraries(gowj PRIVATE "${_prof_lib}")
    target_compile_definitions(gowj PRIVATE GOWJ_PGO_GEN=1)
    message(STATUS "gowj: PGO instrumentation ON (profile runtime ${_prof_lib})")
elseif(TARGET gowj_recomp AND GOWJ_PGO STREQUAL "use")
    if(NOT EXISTS "${GOWJ_PGO_PROFDATA}")
        message(FATAL_ERROR "GOWJ_PGO=use but ${GOWJ_PGO_PROFDATA} does not exist")
    endif()
    target_compile_options(gowj_recomp PRIVATE
        -fprofile-instr-use=${GOWJ_PGO_PROFDATA}
        -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date -Wno-backend-plugin)
    message(STATUS "gowj: PGO profile ${GOWJ_PGO_PROFDATA} applied to the generated guest code")
endif()

# GPU_PLUGINS xenos stages rexgpu-xenos.dll next to the executable.
rexglue_setup_target(gowj GPU_PLUGINS xenos)

# The generated guest code contains no SEH scopes and no C++ try/catch, so the asynchronous
# exception mode that generated/rexglue.cmake turns on only stops the compiler from optimising
# across memory accesses and calls. It is removed here (this file survives re-running codegen,
# rexglue.cmake does not). The target options and the per-source options must change together or
# clang rejects the precompiled header. Turn it back on with -DGOWJ_ASYNC_EH=ON.
option(GOWJ_ASYNC_EH "Compile the generated guest code with async exception handling" OFF)
if(TARGET gowj_recomp AND NOT GOWJ_ASYNC_EH)
    set(_fast_recomp_opts ${REXGLUE_RECOMP_OPTIONS})
    list(REMOVE_ITEM _fast_recomp_opts -fasync-exceptions /EHa)
    get_target_property(_recomp_target_opts gowj_recomp COMPILE_OPTIONS)
    if(_recomp_target_opts)
        list(REMOVE_ITEM _recomp_target_opts -fasync-exceptions /EHa)
        set_target_properties(gowj_recomp PROPERTIES COMPILE_OPTIONS "${_recomp_target_opts}")
    endif()
    set_source_files_properties(${REXGLUE_ENTRYPOINT_GENERATED_SOURCES}
        PROPERTIES COMPILE_OPTIONS "${_fast_recomp_opts}")
endif()

# config/gowj.default.toml is copied to config/gowj.toml on first run; the default file is
# refreshed on every build, the user's file never is.
add_custom_command(TARGET gowj POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/config/gowj.default.toml
            $<TARGET_FILE_DIR:gowj>/config/gowj.default.toml
    COMMENT "Installing config/gowj.default.toml beside gowj.exe")

# SDL's gamepad mapping database; the input driver looks for it in the working directory.
add_custom_command(TARGET gowj POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/config/gamecontrollerdb.txt
            $<TARGET_FILE_DIR:gowj>/gamecontrollerdb.txt
    COMMENT "Installing gamecontrollerdb.txt beside gowj.exe")

# Post-codegen patchers. Each rewrites the generated sources before they are compiled, and they
# run in this order (each stamp depends on the previous one) because they edit the same files.
#   switchtrap     jump tables codegen under-counted: dispatch through the function table
#   renderscale    the guest's hardcoded 720p render size becomes a host setting
#   mousesmooth    drops the guest's thumbstick low-pass filter (raw mouse look)
#   rawlook        injects the mouse angle delta into the guest's look axes
#   motionblur     forces the guest's radial motion blur off
#   gamepatch      community game patches (film grain, FXAA, resolution-scaling fix)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(GOWJ_GEN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/generated/default)
set(_prev_stamp ${GOWJ_GEN_DIR}/codegen.build.stamp)
macro(gowj_patch name script description)
    set(_stamp ${GOWJ_GEN_DIR}/${name}.build.stamp)
    add_custom_command(OUTPUT ${_stamp}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/${script}
                ${GOWJ_GEN_DIR}/*.cpp
        COMMAND ${CMAKE_COMMAND} -E touch ${_stamp}
        DEPENDS ${_prev_stamp}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "${description}"
        VERBATIM)
    add_custom_target(gowj_${name}_patch DEPENDS ${_stamp})
    add_dependencies(gowj_recomp gowj_${name}_patch)
    add_dependencies(gowj gowj_${name}_patch)
    set(_prev_stamp ${_stamp})
endmacro()

gowj_patch(switchtrap patch_switch_traps.py "Patching switch traps in generated code")
gowj_patch(renderscale patch_render_resolution.py "Patching the guest's hardcoded 720p render resolution")
gowj_patch(mousesmooth patch_mouse_smoothing.py "Removing the guest's thumbstick low-pass filter")
gowj_patch(rawlook patch_raw_look.py "Injecting the mouse angle delta into the guest's look axes")
gowj_patch(motionblur patch_motion_blur.py "Forcing the guest's radial motion blur off")
gowj_patch(gamepatch patch_game_patches.py "Applying the game patches")

# The GPU plugin with the upscaler support (scripts/build-gpu.ps1 builds it into out/gpu). When it is
# there, it replaces the stock rexruntime.dll / rexgpu-xenos.dll that the SDK stages next to the exe.
set(GOWJ_GPU_DIR "${CMAKE_CURRENT_SOURCE_DIR}/out/gpu" CACHE PATH "Folder with the patched rexruntime.dll and rexgpu-xenos.dll")
if(EXISTS "${GOWJ_GPU_DIR}/rexgpu-xenos.dll" AND EXISTS "${GOWJ_GPU_DIR}/rexruntime.dll")
    add_custom_command(TARGET gowj POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${GOWJ_GPU_DIR}/rexgpu-xenos.dll" $<TARGET_FILE_DIR:gowj>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${GOWJ_GPU_DIR}/rexruntime.dll" $<TARGET_FILE_DIR:gowj>
        COMMENT "Using the GPU plugin with DLSS / FSR support from ${GOWJ_GPU_DIR}"
        VERBATIM)
    message(STATUS "GPU plugin with upscaler support: ${GOWJ_GPU_DIR}")
else()
    message(STATUS "GPU plugin with upscaler support: not built (run scripts/build-gpu.ps1); the stock SDK plugin is used")
endif()

# A patcher that rewrites an already-compiled file after ninja planned its work would link a
# stale object and still report success. This check fails the build in that case; build again.
add_custom_command(TARGET gowj POST_BUILD
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_objects_fresh.py
            ${GOWJ_GEN_DIR}
            ${CMAKE_BINARY_DIR}/CMakeFiles/gowj_recomp.dir/generated/default
    COMMENT "Checking that no patched source is newer than its object"
    VERBATIM)

# ---------------------------------------------------------------------------------------------
# Single-file release.
#   gowj_stub      small launcher; the packer appends the game and its runtime files to it
#   gowj_pack      build tool that does the appending (Windows LZMS compression)
#   gowj_settings  the settings window that sits next to the game exe
# All three use the static C runtime and only Windows components, so the result needs nothing
# installed. `cmake --build <dir> --target gowj_package` writes <dir>/package/.
# ---------------------------------------------------------------------------------------------
option(GOWJ_PACKAGE "Build the single-file release and the settings launcher" ON)
set(GOWJ_VCRT_DIR "" CACHE PATH "Folder with msvcp140.dll, vcruntime140.dll ... (Microsoft.VC143.CRT) to embed")
if(GOWJ_PACKAGE AND WIN32)
    file(READ config/gowj.default.toml GOWJ_DEFAULT_CONFIG)
    configure_file(launcher/default_config.h.in ${CMAKE_BINARY_DIR}/launcher_gen/default_config.h @ONLY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS config/gowj.default.toml)

    add_executable(gowj_stub WIN32 launcher/stub.cpp)
    target_link_libraries(gowj_stub PRIVATE cabinet user32 gdi32)
    add_executable(gowj_pack launcher/pack.cpp)
    target_link_libraries(gowj_pack PRIVATE cabinet)
    add_executable(gowj_settings WIN32 launcher/settings.cpp)
    target_include_directories(gowj_settings PRIVATE ${CMAKE_BINARY_DIR}/launcher_gen)
    target_link_libraries(gowj_settings PRIVATE gdiplus user32 gdi32 dwmapi shell32)
    set_target_properties(gowj_stub gowj_pack gowj_settings PROPERTIES MSVC_RUNTIME_LIBRARY MultiThreaded)

    if(NOT GOWJ_VCRT_DIR AND DEFINED ENV{VCToolsRedistDir})
        file(TO_CMAKE_PATH "$ENV{VCToolsRedistDir}" _redist)
        file(GLOB _crt_dirs "${_redist}/x64/Microsoft.VC*.CRT")
        list(GET _crt_dirs 0 GOWJ_VCRT_DIR)
    endif()
    if(NOT GOWJ_VCRT_DIR OR NOT EXISTS "${GOWJ_VCRT_DIR}/vcruntime140.dll")
        message(WARNING "gowj_package: the Visual C++ runtime DLLs were not found. Run from a Visual Studio "
                        "developer prompt or pass -DGOWJ_VCRT_DIR=<...>/Microsoft.VC143.CRT.")
    endif()

    set(_pkg ${CMAKE_BINARY_DIR}/package)
    add_custom_target(gowj_package
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_pkg}
        COMMAND gowj_pack ${_pkg}/GearsOfWarJudgment.exe $<TARGET_FILE:gowj_stub>
                GearsOfWarJudgment.exe=$<TARGET_FILE:gowj>
                rexruntime.dll=$<TARGET_FILE_DIR:gowj>/rexruntime.dll
                rexgpu-xenos.dll=$<TARGET_FILE_DIR:gowj>/rexgpu-xenos.dll
                msvcp140.dll=${GOWJ_VCRT_DIR}/msvcp140.dll
                msvcp140_atomic_wait.dll=${GOWJ_VCRT_DIR}/msvcp140_atomic_wait.dll
                vcruntime140.dll=${GOWJ_VCRT_DIR}/vcruntime140.dll
                vcruntime140_1.dll=${GOWJ_VCRT_DIR}/vcruntime140_1.dll
                gamecontrollerdb.txt=${CMAKE_CURRENT_SOURCE_DIR}/config/gamecontrollerdb.txt
                config/gowj.default.toml=${CMAKE_CURRENT_SOURCE_DIR}/config/gowj.default.toml
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:gowj_settings> "${_pkg}/Graphics Settings.exe"
        DEPENDS gowj gowj_stub gowj_pack gowj_settings
        COMMENT "Packing the single-file release"
        VERBATIM)
endif()
