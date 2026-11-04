cmake_minimum_required(VERSION 3.20)
project(tekken8mvm LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>") # no VC++ redist needed
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
add_compile_options(/W3 /utf-8 /permissive- /Zc:__cplusplus)

set(IMGUI third_party/imgui)
set(IMGUI_CORE ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
    ${IMGUI}/backends/imgui_impl_win32.cpp)

set(MINHOOK third_party/minhook/src)
add_library(minhook STATIC ${MINHOOK}/buffer.c ${MINHOOK}/hook.c ${MINHOOK}/trampoline.c ${MINHOOK}/hde/hde64.c)
target_include_directories(minhook PUBLIC third_party/minhook/include)

# ----- the in-game mod: built as dinput8.dll so the game loads it from its own folder
add_library(tekken8mvm_mod SHARED
    src/mod/proxy.cpp src/mod/dllmain.cpp src/mod/scan.cpp src/mod/ue.cpp src/mod/game.cpp
    src/mod/camera.cpp src/mod/effects.cpp src/mod/guard.cpp src/mod/recorder.cpp src/mod/overlay.cpp src/mod/depth.cpp
    src/mod/menu.cpp src/mod/savetrace.cpp src/mod/dvars.cpp src/mod/postfx.cpp src/mod/lights.cpp src/mod/audio.cpp
    src/mod/presets.cpp src/mod/replay.cpp src/mod/sharing.cpp src/mod/testcmd.cpp src/common/config.cpp
    ${IMGUI_CORE} ${IMGUI}/backends/imgui_impl_dx12.cpp)
set_target_properties(tekken8mvm_mod PROPERTIES OUTPUT_NAME dinput8)
target_include_directories(tekken8mvm_mod PRIVATE src ${IMGUI} ${IMGUI}/backends)
target_link_libraries(tekken8mvm_mod PRIVATE minhook d3d12 dxgi iphlpapi ws2_32 mmdevapi ole32 shell32 bcrypt)

# ----- the launcher: installs the mod only for a cinematic session, and removes it afterwards
add_executable(tekken8mvm WIN32 src/launcher/main.cpp src/launcher/game_files.cpp src/common/config.cpp
    ${IMGUI_CORE} ${IMGUI}/backends/imgui_impl_dx11.cpp)
target_include_directories(tekken8mvm PRIVATE src ${IMGUI} ${IMGUI}/backends)
target_link_libraries(tekken8mvm PRIVATE d3d11 d3dcompiler dxgi shell32 ole32 dwmapi)

# ----- offline checks: no game needed
add_executable(tekken8mvm_tests tests/test_main.cpp src/mod/scan.cpp src/launcher/game_files.cpp src/common/config.cpp)
target_include_directories(tekken8mvm_tests PRIVATE src)
target_compile_definitions(tekken8mvm_tests PRIVATE TEKKEN8MVM_TESTS)
target_link_libraries(tekken8mvm_tests PRIVATE shell32 ole32)

# A tiny D3D12 app that stands in for the game when testing the overlay and the recorder.
add_executable(tekken8mvm_harness WIN32 tools/harness/harness.cpp)
set_target_properties(tekken8mvm_harness PROPERTIES LINK_FLAGS "/ENTRY:wmainCRTStartup")
target_link_libraries(tekken8mvm_harness PRIVATE d3d12 dxgi winmm)

# dist/ is the finished tool: run dist/tekken8mvm.exe.
set(DIST ${CMAKE_SOURCE_DIR}/dist)
add_custom_command(TARGET tekken8mvm POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DIST}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:tekken8mvm> ${DIST}/tekken8mvm.exe)
add_custom_command(TARGET tekken8mvm_mod POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DIST}/mod
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:tekken8mvm_mod> ${DIST}/mod/dinput8.dll)
# The harness with no arguments is the preview: the editor with no game.
add_custom_command(TARGET tekken8mvm_harness POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DIST}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:tekken8mvm_harness> ${DIST}/tekken8mvm_preview.exe)
