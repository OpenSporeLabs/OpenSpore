# Experimental Linux -> Windows x86 (MinGW-w64) cross-build

**Status: EXPERIMENTAL. Not an officially supported configuration.** The supported build stays
**Visual Studio 2022, `-A Win32`, Debug** under MSVC (see `CMakeLists.txt`, `README.md#проверки`).
Nothing here changes that path: every source/CMake delta is either inert for MSVC or keeps its MSVC
output identical.

This is **Linux-hosted cross-compilation of a Windows target**, not a native Linux port. The output is
a Windows PE32/i386 executable and the Win32/D3D9 dependency is unchanged. Running it under
Wine/Proton is *optional smoke-test evidence only*. **Compilation success does not imply runtime
parity** — see "Runtime smoke test" below.

Established 2026-10-17.

## Toolchain (measured)

| Tool | Version |
|---|---|
| `i686-w64-mingw32-g++` | GCC 16.2.0 (posix threads, dwarf2 exceptions, shared libgcc/libstdc++, UCRT) |
| `i686-w64-mingw32-windres` | GNU windres (binutils) 2.47.20260726 |
| CMake | 4.4.4 |
| Ninja | 1.13.2 |

## Exact commands (from the repository root)

```bash
cmake -S . -B build-mingw32 -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=build-tools/mingw-w64-i686.cmake \
      -DCMAKE_BUILD_TYPE=Debug
cmake --build build-mingw32 --target OpenSpore
# output: build-mingw32/bin/Debug/OpenSpore.exe
```

A **selected** set of the repository's offline `*_verify` targets also cross-compiles (console exes
land in `build-mingw32/`, not `bin/Debug/`, because only `OpenSpore` sets
`RUNTIME_OUTPUT_DIRECTORY`). This is a deliberate subset, not the whole suite — the goal is to show
the toolchain compiles and links real targets, not to certify every existing verifier:

```bash
cmake --build build-mingw32 --target \
  spui_verify mode_ui_verify starsdb_verify starsdb_links_verify starsdb_context_verify \
  starsdb_dbpf_verify starsdb_postload_verify save_header_verify planet_record_verify \
  pkg_lifetime_verify overlay_clip_verify
```

## Source deltas required (and why)

Two files are modified and one is new; none can alter the MSVC build:

1. **`build-tools/mingw-w64-i686.cmake`** — the cross-toolchain file (new). Sets
   `CMAKE_SYSTEM_NAME Windows`, `CMAKE_SYSTEM_PROCESSOR i686`, the GCC/windres compilers and
   `CMAKE_FIND_ROOT_PATH /usr/i686-w64-mingw32` (the compiler's own sysroot).
2. **`CMakeLists.txt`** — one `if(MINGW)` block (after the existing `if(MSVC)` block):
   - `target_link_options(OpenSpore PRIVATE -municode)` — `main.cpp` defines `wWinMain`. MinGW's
     default GUI startup (`crtexewin.o`) references `_WinMain@16`, so the link fails with
     ``undefined reference to `WinMain@16'``. `-municode` selects the Unicode startup object
     (references `_wWinMain@16`). Confirmed against the toolchain's own `libmingw32.a`.
   - `target_link_libraries(OpenSpore PRIVATE gdiplus)` — `render/imgfile.cpp` relies on MSVC's
     `#pragma comment(lib, "gdiplus.lib")`, which GCC ignores, so GDI+ symbols
     (`GdiplusStartup`, `GdipCreateBitmapFromStream`, …) were undefined.
3. **`source/app.cpp`** — the boot line printed `_MSC_VER` unconditionally, which does not exist
   under GCC. Replaced with a portable compiler identity (`_MSC_VER` branch unchanged for MSVC, new
   `__GNUC__` branch). MSVC output is identical (`MSVC <ver>`).

No compiler flags were weakened, no diagnostics/crash-handling/stack-walk/runtime checks were
removed, and no global flag was changed to accommodate GCC. `OS_ARCH=x86` is untouched.

## Compilation evidence

- Configure: **succeeds** (`arch=x86 build=Debug dev_commands=ON icon=TRUE`).
- `OpenSpore` target: **compiles and links** all 52 TUs + the generated `.rc` (icon embedded).
- The **11 selected `*_verify` targets** above: **compile and link**.
- PE: `PE32 executable for MS Windows 4.00 (GUI), Intel i386`, image base `0x400000`, entry
  `0x401450` = `_WinMainCRTStartup` -> `_wWinMain@16` (confirmed by `nm`/`objdump`).
- Non-system runtime DLLs imported (must accompany the exe):
  `libgcc_s_dw2-1.dll`, `libstdc++-6.dll`, `libwinpthread-1.dll`
  (source: `/usr/i686-w64-mingw32/bin/`). System imports: d3d9, gdiplus, dbghelp, KERNEL32, USER32,
  GDI32, plus the UCRT `api-ms-win-crt-*` set.

**MSVC compatibility was not built or run on the Linux host.** No `cl.exe`/`msbuild` is available
here, so the "inert for MSVC" property is argued from the diff (a guarded `if(MINGW)` block plus a
`#if defined(_MSC_VER)` branch that preserves the exact MSVC output), not demonstrated locally. MSVC
remains the supported configuration.

## Runtime smoke test (optional, separate from compilation)

The cross-built exe was run under Wine and Proton purely as a **smoke test** of startup and resource
loading. This is not a native Linux port and does not certify runtime parity.

Test tree (outside Git; `Data`/`DataEP1` are **hardlinks** to the user's install):

```text
OpenSpore-test/
  OpenSpore.exe  + libgcc_s_dw2-1.dll libstdc++-6.dll libwinpthread-1.dll
  Data/          (hardlink of <install>/Data, incl. Config/ and Locale/en-us/)
  DataEP1/       (hardlink of <install>/DataEP1)
  UserData/      (real, writable)
```

Command (frames, not seconds; startup/resource smoke test only):

```text
OpenSpore.exe -w -devDirs -state:DevLauncher -quitAfter 30
```

| Host | Startup | D3D9 device | Package/locale load | Exit |
|---|---|---|---|---|
| Wine 11.19 (fresh prefix under /tmp) | PASS (13 boot markers) | PASS | PASS (60 states from the EP1 package; drawable PNGs from `Spore_Graphics.package`) | 0 |
| Proton 9.0-203 (`proton run`, fresh `STEAM_COMPAT_DATA_PATH`) | PASS (13 boot markers) | PASS (via DXVK) | PASS (same 60 states) | 0 |

The selected `*_verify` targets were also run under Wine with the cross-built exes. Ten of the eleven
PASS: `mode_ui_verify`, `starsdb_verify`, `starsdb_links_verify`, `starsdb_context_verify`,
`starsdb_dbpf_verify`, `starsdb_postload_verify`, `save_header_verify`, `planet_record_verify`
(no arguments); `overlay_clip_verify` (needs an output-BMP path); `pkg_lifetime_verify <package>`
(544 scoped open/read/destruct cycles, CRT limit 512). `spui_verify` needs an extracted fixture
directory and was **not** exercised here. This is the subset named in the build command above, not
the entire `*_verify` suite.

## Known gaps / limits

- `_DEBUG` is not defined by the MinGW Debug config, so `diag.cpp`'s `#ifdef _DEBUG` branch (routing
  MSVC CRT reports via `crtdbg.h`) is inactive in the cross-build. The rest of the crash harness (SEH
  filter, dbghelp stack walks, non-fatal asserts, abort recovery) is active.
- The exe is **dynamically linked** (UCRT + MinGW runtimes); it is not self-contained. Ship the three
  MinGW runtime DLLs next to it.
- Not every pre-existing `*_verify` target links under MinGW yet: targets that compile
  `render/imgfile.cpp` pull GDI+ symbols through the MSVC `#pragma comment(lib, "gdiplus.lib")`
  (ignored by GCC), and one includes `Windows.h` with case-sensitive casing. This change adds the
  GDI+ link for `OpenSpore` only; those targets are separate work and are not part of this
  contribution.
- **MSVC build not verified on this host** (no `cl.exe`/`msbuild`); MSVC stays the supported path.
- **No licence is declared in this repository.** Contribution terms need maintainer clarification
  before merge; nothing here should be read as a licensing grant.

## Do not repeat

Raw **Unix** symlinks for `Data`/`DataEP1` do **not** work under Wine's directory enumeration: the
app resolved 0 packages. Use hardlinks (same filesystem) or a real directory. `/tmp` is tmpfs here,
so the test tree lives on the same filesystem as the game data.
