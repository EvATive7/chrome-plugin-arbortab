cmake_minimum_required(VERSION 3.21)
project(arbortab_native LANGUAGES C CXX)

if(NOT MSVC)
  message(FATAL_ERROR "ArborTab's native module targets Windows/MSVC only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# MinHook (BSD-2-Clause), vendored under third_party/minhook, built as a static library.
add_library(minhook STATIC
  third_party/minhook/src/buffer.c
  third_party/minhook/src/hook.c
  third_party/minhook/src/trampoline.c
  third_party/minhook/src/hde/hde32.c
  third_party/minhook/src/hde/hde64.c
)
target_include_directories(minhook PUBLIC third_party/minhook/include)
target_compile_definitions(minhook PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
if(MSVC)
  target_compile_options(minhook PRIVATE /W3 /O2)
endif()

set(ARBORTAB_SOURCES
  src/main.cpp
  src/core/chrome_api.cpp
  src/core/cig.cpp
  src/core/hooks.cpp
  src/core/log.cpp
  src/core/offsets.cpp
  src/core/paths.cpp
  src/core/state.cpp
  src/core/ui_thread.cpp
  src/features/anim.cpp
  src/features/decor.cpp
  src/features/drag.cpp
  src/features/layout.cpp
  src/features/mouse.cpp
  src/features/paint.cpp
  src/link/ws_server.cpp
  src/proxy/exports.cpp
)

add_library(version SHARED ${ARBORTAB_SOURCES} version.def)
target_include_directories(version PRIVATE src third_party third_party/json)
# version.dll itself is never linked: every forwarded call goes through GetProcAddress.
target_link_libraries(version PRIVATE minhook winhttp shlwapi shell32)
target_compile_definitions(version PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
# Build time written to native.log (core/hooks.cpp), so scripts/dev.py can tell which DLL a
# running browser loaded. Taken at configure time; scripts/build-native.ps1 reconfigures on
# every build.
string(TIMESTAMP ARBORTAB_BUILD_STAMP "%m-%d %H:%M")
set_source_files_properties(src/core/hooks.cpp PROPERTIES
  COMPILE_DEFINITIONS "ARBORTAB_BUILD_STAMP=\"${ARBORTAB_BUILD_STAMP}\"")
if(MSVC)
  target_compile_options(version PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc)
  # Keep a PDB so crash addresses inside this DLL can be symbolised during development.
  target_compile_options(version PRIVATE $<$<CONFIG:Release>:/Zi>)
  target_link_options(version PRIVATE /DEBUG /OPT:REF /OPT:ICF)
endif()

# No import library or debug naming tricks: the artifact must be exactly version.dll.
set_target_properties(version PROPERTIES
  PREFIX ""
  OUTPUT_NAME "version"
  SUFFIX ".dll"
  LINK_FLAGS "/INCREMENTAL:NO"
)
