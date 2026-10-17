cmake_minimum_required(VERSION 3.25)
project(OpenSpore LANGUAGES CXX)

# Keep every executable (main app and explicit read-only utilities) together in the
# canonical review/runtime directory. Multi-config generators expand $<CONFIG>.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")

# ---------------------------------------------------------------------------
# Target shape, decided by evidence, not taste:
#   * The original (refs/bins/ReleaseGA/SporeApp.exe) is 32-bit (machine 0x14c),
#     subsystem Windows GUI, image base 0x00400000, ASLR-capable. PROMPT.md asks for
#     x86 first, x64 later -> so x86 is the default and the file is written to allow
#     a 64-bit pass without restructuring.
#   * Debug is the shipped configuration of this project (developer commands must be
#     present), so the default build type is Debug rather than Release.
# ---------------------------------------------------------------------------

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

set(OS_ARCH "x86" CACHE STRING "x86 or x64 (x86 first, per PROMPT.md)")
set_property(CACHE OS_ARCH PROPERTY STRINGS x86 x64)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(OS_IS_X64 TRUE)
else()
  set(OS_IS_X64 FALSE)
endif()
if(OS_ARCH STREQUAL "x64" AND NOT OS_IS_X64)
  message(FATAL_ERROR "OS_ARCH=x64 but the generator is 32-bit. Use -A x64.")
endif()
if(OS_ARCH STREQUAL "x86" AND OS_IS_X64)
  message(FATAL_ERROR "OS_ARCH=x86 but the generator is 64-bit. Use -A Win32.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Keep the icon out of any glob: it is generated into the build dir, never into source/.
set(OS_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${OS_GENERATED_DIR})

# ---------------------------------------------------------------------------
# Icon. The tracked copy is assets/OpenSpore.ico; refs/ is gitignored (it holds the
# original binaries), so it is only a fallback for the very first checkout.
# ---------------------------------------------------------------------------
set(OS_ICON_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/assets/OpenSpore.ico)
if(NOT EXISTS ${OS_ICON_SOURCE})
  set(OS_ICON_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/refs/OpenSpore.ico)
endif()
set(OS_ICON_BUILD  ${OS_GENERATED_DIR}/openspore.ico)
set(OS_RC_FILE     ${OS_GENERATED_DIR}/openspore.rc)
set(OS_HAS_ICON FALSE)
if(EXISTS ${OS_ICON_SOURCE})
  configure_file(${OS_ICON_SOURCE} ${OS_ICON_BUILD} COPYONLY)
  set(OS_HAS_ICON TRUE)
  message(STATUS "OpenSpore: icon from ${OS_ICON_SOURCE}")
endif()
if(OS_HAS_ICON)
  file(WRITE ${OS_RC_FILE}
"#include <windows.h>
#define OS_ICON_NAME 101
OS_ICON_NAME ICON \"openspore.ico\"
VS_VERSION_INFO VERSIONINFO
 FILEVERSION 3,1,0,29
 PRODUCTVERSION 3,1,0,29
 FILEFLAGSMASK 0x3fL
#ifdef _DEBUG
 FILEFLAGS 0x1L
#else
 FILEFLAGS 0x0L
#endif
 FILEOS 0x40004L
 FILETYPE 0x1L
 FILESUBTYPE 0x0L
BEGIN
    BLOCK \"StringFileInfo\"
    BEGIN
        BLOCK \"040904b0\"
        BEGIN
            VALUE \"CompanyName\", \"OpenSpore\"
            VALUE \"FileDescription\", \"OpenSpore Loader\"
            VALUE \"FileVersion\", \"3.1.0.29\"
            VALUE \"InternalName\", \"OpenSpore\"
            VALUE \"LegalCopyright\", \"Reimplementation; no EA content\"
            VALUE \"OriginalFilename\", \"OpenSpore.exe\"
            VALUE \"ProductName\", \"OpenSpore\"
            VALUE \"ProductVersion\", \"3.1.0.29\"
        END
    END
    BLOCK \"VarFileInfo\"
    BEGIN
        VALUE \"Translation\", 0x409, 1200
    END
END
")
else()
  message(WARNING "no OpenSpore.ico in assets/ or refs/ - building WITHOUT icon. "
                  "This is not a failure, but the taskbar/exe icon will be the default. "
                  "Tracked copy belongs in assets/ (refs/ is gitignored).")
  file(WRITE ${OS_RC_FILE} "// no icon: neither assets/OpenSpore.ico nor refs/OpenSpore.ico existed\n")
endif()

enable_language(RC)

# ---------------------------------------------------------------------------
# Sources. Explicit list, no globs (field guide §2: a glob once pulled a vendored
# library's .hlsl into the target and broke the build).
# ---------------------------------------------------------------------------
set(OS_SOURCES
  source/main.cpp
  source/app.cpp
  source/engine/diag.cpp
  source/engine/log.cpp
  source/engine/cmdline.cpp
  source/engine/fs.cpp
  source/engine/input.cpp
  source/engine/appmodes.cpp
  source/engine/mode_ui.cpp
  source/engine/gge_intro.cpp
  source/engine/gge_dbcheck.cpp
  source/engine/gge_catalog.cpp
  source/engine/gge_catalog_snapshot.cpp
  source/engine/serialized_frame.cpp
  source/engine/starsdb.cpp
  source/engine/starsdb_links.cpp
  source/engine/starsdb_context.cpp
  source/engine/starsdb_dbpf.cpp
  source/engine/save_header.cpp
  source/engine/planet_record.cpp
  source/engine/gge_saved_adapter.cpp
  source/engine/gge_star_present.cpp
  source/engine/gge_star_instance.cpp
  source/engine/gge_star_phase1.cpp
  source/engine/gge_star_transition.cpp
  source/engine/gge_image_provider.cpp
  source/engine/gge_star_lifecycle.cpp
  source/engine/gge_star_scene.cpp
  source/engine/console.cpp
  source/engine/pkg.cpp
  source/engine/refpack.cpp
  source/engine/naming.cpp
  source/engine/gmdl.cpp
  source/engine/raster.cpp
  source/engine/config.cpp
  source/engine/appstates.cpp
  source/engine/spui.cpp
  source/engine/spui_binary.cpp
  source/engine/spui_geometry.cpp
  source/engine/spui_drawable.cpp
  source/engine/text.cpp
  source/render/device.cpp
  source/render/overlay.cpp
  source/render/imgfile.cpp
  source/render/mesh.cpp
  source/states/statemgr.cpp
  source/states/loader.cpp
  source/states/title.cpp
  source/states/uipresent.cpp
  source/states/ggehostpreview.cpp source/states/debugmenu.cpp
)

add_executable(OpenSpore WIN32 ${OS_SOURCES} ${OS_RC_FILE})
target_include_directories(OpenSpore PRIVATE source ${OS_GENERATED_DIR})

# Offline parser verification against decoded fixtures from tools/spui_objects.py --extract.
add_executable(spui_verify EXCLUDE_FROM_ALL tools/spui_verify.cpp
  source/engine/spui_binary.cpp source/engine/spui_geometry.cpp source/engine/spui_drawable.cpp)
target_include_directories(spui_verify PRIVATE source)
add_executable(mode_ui_verify EXCLUDE_FROM_ALL tools/mode_ui_verify.cpp source/engine/mode_ui.cpp)
target_include_directories(mode_ui_verify PRIVATE source)
add_executable(gge_intro_verify EXCLUDE_FROM_ALL tools/gge_intro_verify.cpp source/engine/gge_intro.cpp)
target_include_directories(gge_intro_verify PRIVATE source)
add_executable(gge_dbcheck_verify EXCLUDE_FROM_ALL tools/gge_dbcheck_verify.cpp
  source/engine/gge_dbcheck.cpp source/engine/gge_intro.cpp source/engine/spui_binary.cpp
  source/engine/spui_geometry.cpp)
target_include_directories(gge_dbcheck_verify PRIVATE source)
add_executable(gge_catalog_verify EXCLUDE_FROM_ALL tools/gge_catalog_verify.cpp source/engine/gge_catalog.cpp)
target_include_directories(gge_catalog_verify PRIVATE source)
add_executable(starsdb_verify EXCLUDE_FROM_ALL tools/starsdb_verify.cpp source/engine/serialized_frame.cpp source/engine/starsdb.cpp source/engine/starsdb_links.cpp)
target_include_directories(starsdb_verify PRIVATE source)
add_executable(starsdb_links_verify EXCLUDE_FROM_ALL tools/starsdb_links_verify.cpp source/engine/serialized_frame.cpp source/engine/starsdb.cpp source/engine/starsdb_links.cpp)
target_include_directories(starsdb_links_verify PRIVATE source)
add_executable(starsdb_context_verify EXCLUDE_FROM_ALL tools/starsdb_context_verify.cpp source/engine/serialized_frame.cpp source/engine/starsdb.cpp source/engine/starsdb_links.cpp source/engine/starsdb_context.cpp)
target_include_directories(starsdb_context_verify PRIVATE source)
add_executable(starsdb_context_inspect EXCLUDE_FROM_ALL tools/starsdb_context_inspect.cpp source/engine/serialized_frame.cpp source/engine/starsdb.cpp source/engine/starsdb_links.cpp source/engine/starsdb_context.cpp)
target_include_directories(starsdb_context_inspect PRIVATE source)
add_executable(starsdb_dbpf_inspect EXCLUDE_FROM_ALL tools/starsdb_dbpf_inspect.cpp source/engine/pkg.cpp source/engine/refpack.cpp source/engine/log.cpp source/engine/serialized_frame.cpp source/engine/starsdb.cpp source/engine/starsdb_links.cpp source/engine/starsdb_context.cpp source/engine/starsdb_dbpf.cpp)
target_include_directories(starsdb_dbpf_inspect PRIVATE source)
add_executable(dbpf_inventory_inspect EXCLUDE_FROM_ALL tools/dbpf_inventory_inspect.cpp source/engine/pkg.cpp source/engine/refpack.cpp source/engine/log.cpp)
target_include_directories(dbpf_inventory_inspect PRIVATE source)
add_executable(dbpf_resource_inspect EXCLUDE_FROM_ALL tools/dbpf_resource_inspect.cpp source/engine/pkg.cpp source/engine/refpack.cpp source/engine/log.cpp)
target_include_directories(dbpf_resource_inspect PRIVATE source)
add_executable(dbpf_serialized_frame_inspect EXCLUDE_FROM_ALL tools/dbpf_serialized_frame_inspect.cpp source/engine/pkg.cpp source/engine/refpack.cpp source/engine/log.cpp source/engine/serialized_frame.cpp)
target_include_directories(dbpf_serialized_frame_inspect PRIVATE source)
add_executable(starsdb_dbpf_verify EXCLUDE_FROM_ALL tools/starsdb_dbpf_verify.cpp source/engine/pkg.cpp source/engine/refpack.cpp source/engine/log.cpp source/engine/serialized_frame.cpp source/engine/starsdb.cpp source/engine/starsdb_links.cpp source/engine/starsdb_context.cpp source/engine/starsdb_dbpf.cpp)
target_include_directories(starsdb_dbpf_verify PRIVATE source)
add_executable(starsdb_postload_verify EXCLUDE_FROM_ALL tools/starsdb_postload_verify.cpp source/engine/serialized_frame.cpp source/engine/starsdb.cpp)
target_include_directories(starsdb_postload_verify PRIVATE source)
set(OS_SAVE_HEADER_SOURCES source/engine/save_header.cpp source/engine/starsdb_dbpf.cpp
  source/engine/starsdb_context.cpp source/engine/starsdb_links.cpp source/engine/starsdb.cpp
  source/engine/serialized_frame.cpp source/engine/pkg.cpp source/engine/refpack.cpp source/engine/log.cpp)
add_executable(starsdb_save_corpus_inspect EXCLUDE_FROM_ALL tools/starsdb_save_corpus_inspect.cpp source/engine/planet_record.cpp ${OS_SAVE_HEADER_SOURCES})
target_include_directories(starsdb_save_corpus_inspect PRIVATE source)
add_executable(save_header_verify EXCLUDE_FROM_ALL tools/save_header_verify.cpp source/engine/gge_catalog.cpp ${OS_SAVE_HEADER_SOURCES})
target_include_directories(save_header_verify PRIVATE source)
add_executable(save_header_inspect EXCLUDE_FROM_ALL tools/save_header_inspect.cpp ${OS_SAVE_HEADER_SOURCES})
target_include_directories(save_header_inspect PRIVATE source)
add_executable(planet_record_verify EXCLUDE_FROM_ALL tools/planet_record_verify.cpp source/engine/planet_record.cpp
  source/engine/gge_saved_adapter.cpp source/engine/gge_catalog.cpp ${OS_SAVE_HEADER_SOURCES})
target_include_directories(planet_record_verify PRIVATE source)
add_executable(planet_record_inspect EXCLUDE_FROM_ALL tools/planet_record_inspect.cpp source/engine/planet_record.cpp ${OS_SAVE_HEADER_SOURCES})
target_include_directories(planet_record_inspect PRIVATE source)
add_executable(starsdb_gge_capture EXCLUDE_FROM_ALL tools/starsdb_gge_capture.cpp source/engine/planet_record.cpp ${OS_SAVE_HEADER_SOURCES})
target_include_directories(starsdb_gge_capture PRIVATE source)
add_executable(gge_star_present_verify EXCLUDE_FROM_ALL tools/gge_star_present_verify.cpp source/engine/gge_star_present.cpp
  source/engine/spui_binary.cpp source/engine/spui_geometry.cpp)
target_include_directories(gge_star_present_verify PRIVATE source)
add_executable(gge_star_instance_verify EXCLUDE_FROM_ALL tools/gge_star_instance_verify.cpp source/engine/gge_star_instance.cpp
  source/engine/spui_binary.cpp source/engine/spui_geometry.cpp source/engine/spui_drawable.cpp)
target_include_directories(gge_star_instance_verify PRIVATE source)
add_executable(gge_star_phase1_verify EXCLUDE_FROM_ALL tools/gge_star_phase1_verify.cpp
  source/engine/gge_star_phase1.cpp source/engine/gge_star_instance.cpp
  source/engine/spui_binary.cpp source/engine/spui_geometry.cpp source/engine/spui_drawable.cpp)
target_include_directories(gge_star_phase1_verify PRIVATE source)
add_executable(gge_image_verify EXCLUDE_FROM_ALL tools/gge_image_verify.cpp source/engine/gge_image_provider.cpp
  source/engine/gge_star_instance.cpp source/engine/gge_star_phase1.cpp source/engine/spui_drawable.cpp
  source/engine/spui_binary.cpp source/engine/spui_geometry.cpp ${OS_SAVE_HEADER_SOURCES}
  source/render/imgfile.cpp source/render/device.cpp source/render/overlay.cpp)
target_include_directories(gge_image_verify PRIVATE source)
target_compile_definitions(gge_image_verify PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(gge_image_verify PRIVATE d3d9 user32 gdi32 ole32)
add_executable(gge_animated_verify EXCLUDE_FROM_ALL tools/gge_animated_verify.cpp source/engine/gge_image_provider.cpp
  source/engine/gge_star_instance.cpp source/engine/gge_star_phase1.cpp source/engine/gge_star_present.cpp source/engine/spui_drawable.cpp
  source/engine/spui_binary.cpp source/engine/spui_geometry.cpp ${OS_SAVE_HEADER_SOURCES}
  source/render/imgfile.cpp source/render/device.cpp source/render/overlay.cpp)
target_include_directories(gge_animated_verify PRIVATE source)
target_compile_definitions(gge_animated_verify PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(gge_animated_verify PRIVATE d3d9 user32 gdi32 ole32)
add_executable(gge_star_lifecycle_verify EXCLUDE_FROM_ALL tools/gge_star_lifecycle_verify.cpp source/engine/gge_star_lifecycle.cpp source/engine/gge_star_transition.cpp
  source/engine/gge_image_provider.cpp source/engine/gge_star_instance.cpp source/engine/gge_star_phase1.cpp
  source/engine/gge_star_present.cpp source/engine/spui_drawable.cpp source/engine/spui_binary.cpp source/engine/spui_geometry.cpp
  ${OS_SAVE_HEADER_SOURCES} source/render/imgfile.cpp)
target_include_directories(gge_star_lifecycle_verify PRIVATE source)
target_compile_definitions(gge_star_lifecycle_verify PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(gge_star_lifecycle_verify PRIVATE user32 gdi32 ole32)
add_executable(gge_streaming_image_verify EXCLUDE_FROM_ALL tools/gge_streaming_image_verify.cpp
  source/engine/gge_image_provider.cpp source/engine/gge_star_instance.cpp source/engine/gge_star_phase1.cpp
  source/engine/spui_drawable.cpp source/engine/spui_binary.cpp source/engine/spui_geometry.cpp
  ${OS_SAVE_HEADER_SOURCES} source/render/imgfile.cpp)
target_include_directories(gge_streaming_image_verify PRIVATE source)
target_compile_definitions(gge_streaming_image_verify PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(gge_streaming_image_verify PRIVATE user32 gdi32 ole32)
add_executable(gge_star_scene_verify EXCLUDE_FROM_ALL tools/gge_star_scene_verify.cpp source/engine/gge_star_scene.cpp source/engine/gge_star_transition.cpp
  source/engine/gge_star_lifecycle.cpp source/engine/gge_image_provider.cpp source/engine/gge_star_instance.cpp
  source/engine/gge_star_phase1.cpp source/engine/gge_star_present.cpp source/engine/gge_catalog.cpp
  source/engine/spui_drawable.cpp source/engine/spui_binary.cpp source/engine/spui_geometry.cpp
  ${OS_SAVE_HEADER_SOURCES} source/render/imgfile.cpp)
target_include_directories(gge_star_scene_verify PRIVATE source)
target_compile_definitions(gge_star_scene_verify PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(gge_star_scene_verify PRIVATE user32 gdi32 ole32)
add_executable(gge_star_transition_verify EXCLUDE_FROM_ALL tools/gge_star_transition_verify.cpp source/engine/gge_star_transition.cpp
  source/engine/gge_star_scene.cpp source/engine/gge_star_lifecycle.cpp source/engine/gge_image_provider.cpp
  source/engine/gge_star_instance.cpp source/engine/gge_star_phase1.cpp source/engine/gge_star_present.cpp source/engine/gge_catalog.cpp
  source/engine/spui_drawable.cpp source/engine/spui_binary.cpp source/engine/spui_geometry.cpp
  ${OS_SAVE_HEADER_SOURCES} source/render/imgfile.cpp)
target_include_directories(gge_star_transition_verify PRIVATE source)
target_compile_definitions(gge_star_transition_verify PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(gge_star_transition_verify PRIVATE user32 gdi32 ole32)
add_executable(gge_star_input_verify EXCLUDE_FROM_ALL tools/gge_star_input_verify.cpp source/engine/gge_star_scene.cpp
  source/engine/gge_star_transition.cpp source/engine/gge_star_lifecycle.cpp source/engine/gge_image_provider.cpp
  source/engine/gge_star_instance.cpp source/engine/gge_star_phase1.cpp source/engine/gge_star_present.cpp source/engine/gge_catalog.cpp
  source/engine/spui_drawable.cpp source/engine/spui_binary.cpp source/engine/spui_geometry.cpp ${OS_SAVE_HEADER_SOURCES} source/render/imgfile.cpp)
target_include_directories(gge_star_input_verify PRIVATE source)
target_compile_definitions(gge_star_input_verify PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(gge_star_input_verify PRIVATE user32 gdi32 ole32)
add_executable(gge_catalog_snapshot_verify EXCLUDE_FROM_ALL tools/gge_catalog_snapshot_verify.cpp
  source/engine/gge_catalog_snapshot.cpp source/engine/gge_catalog.cpp)
target_include_directories(gge_catalog_snapshot_verify PRIVATE source)
add_executable(gge_catalog_snapshot_inspect EXCLUDE_FROM_ALL tools/gge_catalog_snapshot_inspect.cpp
  source/engine/gge_catalog_snapshot.cpp source/engine/gge_catalog.cpp)
target_include_directories(gge_catalog_snapshot_inspect PRIVATE source)
add_executable(starsdb_inspect EXCLUDE_FROM_ALL tools/starsdb_inspect.cpp source/engine/serialized_frame.cpp source/engine/starsdb.cpp source/engine/starsdb_links.cpp)
target_include_directories(starsdb_inspect PRIVATE source)
add_executable(pkg_lifetime_verify EXCLUDE_FROM_ALL tools/pkg_lifetime_verify.cpp
  source/engine/pkg.cpp source/engine/refpack.cpp source/engine/log.cpp)
target_include_directories(pkg_lifetime_verify PRIVATE source)
add_executable(overlay_clip_verify EXCLUDE_FROM_ALL tools/overlay_clip_verify.cpp
  source/render/device.cpp source/render/overlay.cpp source/engine/log.cpp)
target_include_directories(overlay_clip_verify PRIVATE source)
target_compile_definitions(overlay_clip_verify PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(overlay_clip_verify PRIVATE d3d9 user32 gdi32)
if(MSVC)
  target_compile_options(spui_verify PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_compile_options(mode_ui_verify PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_compile_options(gge_intro_verify PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_compile_options(gge_dbcheck_verify PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_compile_options(gge_catalog_verify PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_compile_options(starsdb_verify PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_compile_options(starsdb_links_verify PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_compile_options(starsdb_context_verify PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_compile_options(starsdb_context_inspect PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_compile_options(starsdb_dbpf_inspect PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_compile_options(starsdb_dbpf_verify PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_compile_options(starsdb_postload_verify PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_compile_options(save_header_verify PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_compile_options(save_header_inspect PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_compile_options(planet_record_verify PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_compile_options(planet_record_inspect PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_compile_options(starsdb_gge_capture PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_compile_options(gge_star_present_verify PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_compile_options(gge_star_instance_verify PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_compile_options(gge_star_phase1_verify PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_compile_options(gge_image_verify PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_compile_options(gge_animated_verify PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_compile_options(gge_star_lifecycle_verify PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_compile_options(gge_streaming_image_verify PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_compile_options(gge_star_scene_verify PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_compile_options(gge_star_transition_verify PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_compile_options(gge_star_input_verify PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_compile_options(gge_catalog_snapshot_verify PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_compile_options(gge_catalog_snapshot_inspect PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_compile_options(starsdb_inspect PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_compile_options(pkg_lifetime_verify PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_compile_options(overlay_clip_verify PRIVATE /W4 /permissive- /utf-8 /EHsc)
endif()

set(OS_DEV_COMMANDS OFF)
if(CMAKE_BUILD_TYPE MATCHES "^[Dd]ebug" OR CMAKE_CONFIGURATION_TYPES)
  set(OS_DEV_COMMANDS ON)
endif()

target_compile_definitions(OpenSpore PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  UNICODE
  _UNICODE
  $<$<BOOL:${OS_DEV_COMMANDS}>:OS_ENABLE_DEV_COMMANDS=1>
  $<$<CONFIG:Debug>:_ITERATOR_DEBUG_LEVEL=0>   # keep Debug fast enough to tick a renderer
)

if(MSVC)
  target_compile_options(OpenSpore PRIVATE
    /W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc /Zi /Gy-
    $<$<CONFIG:Debug>:/Od>
    /fp:precise          # the original is a 2008-era link; do not let fast-math vary results
    /Zc:inline-
  )
  target_link_options(OpenSpore PRIVATE /DEBUG /INCREMENTAL:NO /OPT:NOREF /OPT:NOICF)
  # /SAFESEH cannot be used with /INCREMENTAL and is irrelevant for a 32-bit GUI exe here.
  if(OS_ARCH STREQUAL "x86")
    target_link_options(OpenSpore PRIVATE /SAFESEH:NO)
  endif()
endif()

# ---------------------------------------------------------------------------
# EXPERIMENTAL Linux -> Windows x86 cross-build (build-tools/mingw-w64-i686.cmake).
# The officially supported configuration stays Visual Studio 2022 / Win32 / Debug
# under MSVC; this block is inert for that compiler, so the MSVC link line, entry
# point and flags are byte-for-byte unchanged. Compilation here does NOT imply
# Proton runtime parity - see RESUME.md / MEMORY.md for the tested command.
# ---------------------------------------------------------------------------
if(MINGW)
  # main.cpp defines wWinMain. MinGW's default GUI startup (crtexewin.o) expects the
  # ANSI WinMain, so linking fails with "undefined reference to `WinMain@16'"; the
  # Unicode startup object (expecting _wWinMain@16) is selected by -municode.
  target_link_options(OpenSpore PRIVATE -municode)
  # render/imgfile.cpp relies on MSVC's `#pragma comment(lib, "gdiplus.lib")`, which
  # GCC ignores. Link GDI+ explicitly for this toolchain only.
  target_link_libraries(OpenSpore PRIVATE gdiplus)
endif()

# d3d9 + the Win32 surface the original itself imports (see analysis/recon.md §Imports).
# NOTE: no d3dx9 - the June 2010 DX SDK is not installed here and we call no D3DX function.
# The original imports 9 d3dx9_27 functions; that dependency returns with the RW bridge.
target_link_libraries(OpenSpore PRIVATE
  d3d9
  user32 gdi32 kernel32 advapi32 shell32 ole32
  dbghelp
  winmm
)

set_target_properties(OpenSpore PROPERTIES
  OUTPUT_NAME "OpenSpore"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)

message(STATUS "OpenSpore: arch=${OS_ARCH} build=${CMAKE_BUILD_TYPE} dev_commands=${OS_DEV_COMMANDS} icon=${OS_HAS_ICON}")
