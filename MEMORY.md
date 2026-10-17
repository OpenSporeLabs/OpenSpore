# MEMORY — durable facts and traps

Reviewed **2026-10-15**. This short file replaces the mixed-session notebook as the current entry
point. [Full historical notebook](docs/archive/MEMORY-2026-10-15.md) is retained, not erased.

## Working rules

- Follow Main → boot/load → UI → gameplay; reverse only what advances that path.
- Disassembly is primary. Use persistent Ghidra / REA and retain actual instructions plus target
  identity; data corroborates branches, names do not prove behavior.
- GA-unpacked is the address oracle. Beta EXE/PDB and other builds are references, never GA addresses.
- Debug **x86**, zero compiler warnings, console/consolidated log, no blocking modal asserts.
  Test with `-w -devDirs`; unattended runs need `-quitAfter` (frames).
- Commit our changes; user pushes in GitHub Desktop. Preserve unfamiliar user work.

## Current stable facts

| Topic | Fact / authority |
|---|---|
| Oracle | `refs/bins/ReleaseGA/SporeApp.exe.unpacked.exe`; SHA-256 `086f299216f6a70d2c23be0fb04b4b7c570d3bbd94ca0b26bcad53681b3e9fe7` |
| Main chain | entry `0x11E11E1` → WinMain `0xF489E0` → app_main `0xF48810` → run `0xF47E90`; main loop `0x8482F0` |
| Hash | FNV-1, multiply then xor, lowercase mode; Loader `0xD7D04C04`. Type IDs are registry-assigned |
| DBPF | header 0x60, shared-field mask; prefix `4+4*popcount(K)`, stride `32-4*popcount(K)` |
| Compression | record tail is compression:u16 + flags:u16; RefPack sizes are big-endian; detector masks 0x1FFF |
| Resource corpus | 57 packages / 150,945 records; counts belong to the tested data set |
| SPUI | sequential object/property stream; code 19 = count u16 references, FFFF=null; code 20 = nested structs |
| SPUI identity | object index, **not ControlID**; root flag 0x8000; marker 0x5FF5; 748 exact EOF parses |
| Geometry | Area = left/top/right/bottom; ancestor visibility/enabled apply; current placement/Z is a host policy |
| Clip | parent flag 0x400 clips children, not itself; shared `inherited_clip` drives pointer picking and per-window D3D9 scissor; pixel regression tests pass |
| Drawable chain | Window → typed drawable → Image (`0x01BE6B15`) → external PNG, atlas UV/logical size |
| Supported drawables | ImageDrawable None branch + non-tileable Standard/Radio; scale/alignment and state slices tested |
| Locale | key space is per table; use `(u64(table)<<32)|key`, never narrow it to u32 |
| Game states / modes | 60 script states / 17 mode names, 10 recovered binary registrations; drivers are not implemented |

Deep evidence: [binary SPUI](analysis/spui-binary.md), [drawables](analysis/spui-drawables.md),
[locale](analysis/text-locale.md), [formats](analysis/formats.md), [current VERIFY](VERIFY.md).

## Experimental Linux -> Windows MinGW cross-build (2026-10-17)

- **Not officially supported.** Supported config stays MSVC / Visual Studio 2022 / Win32 / Debug.
  Recipe: [analysis/mingw-cross-build.md](analysis/mingw-cross-build.md); toolchain
  `build-tools/mingw-w64-i686.cmake`; output `build-mingw32/` (gitignored).
- GCC 16.2.0 `i686-w64-mingw32-g++`; the `OpenSpore` target and the selected verification targets
  in the recipe cross-build and link. Two `if(MINGW)` deltas in `CMakeLists.txt`: `-municode` (our
  `wWinMain` needs the Unicode CRT startup; the default startup wants ANSI `WinMain`) and `gdiplus`
  (MSVC's `#pragma comment(lib, ...)` is invisible to GCC). `app.cpp` printed `_MSC_VER`
  unconditionally; now a portable compiler string with identical MSVC output.
- **Compilation is not runtime parity.** Smoke test only: Wine 11.19 and Proton 9.0 (DXVK) both
  reached 13 boot markers and loaded all 60 states from the EP1 package. No native Linux port and no
  runtime-parity claim.
- **MSVC was not built on the Linux host** (no `cl.exe`/`msbuild`); MSVC stays the supported path.

## Do not repeat these mistakes

1. **A plausible log line is not a consequence.** `queued goto` once never executed. Verify the
   destination state and frame, not merely the request.
2. **A hidden widget is not a successful visible UI test.** `(942,261)` hit hidden GGE. Current
   regression uses visible `(311,550)` → CreatureEditor; it does not implement that editor.
3. **Property aliases are not classes.** `0xEEC1B007` is overridden under several window classes.
   Calling it `DialogDrawable` did not establish a wrapper object.
4. **PNG dimensions are not logical Image dimensions.** Retain atlas UV and logical size, then
   select the standard/radio state inside the Image rectangle.
5. **FILE ownership matters.** Temporary `Package` objects without destructors exhausted CRT streams.
   RAII + a test exceeding 512 opens fixes the cause; a cache alone only delays it.
6. **No name match does not prove absent code.** Missing plaintext strings, Ghidra-uncovered vtable
   functions and corpus-filtered searches all have bounded negative coverage.
7. **A typo fix does not prove override precedence.** Document host policies until GA establishes them.
8. **Do not kill an analysis JVM by process name.** Identify its exact command line/project owner.
9. **Counters are not the product.** Feature index ≠ playable game %. Manual function catalogue ≠
   exhaustive reversed-code coverage. Generated evidence is not automatically a current claim.
10. **Captures are in-engine.** PrintWindow can return black D3D9 content; use `-capture`.

## Navigation

[RESUME](RESUME.md) = next actions · [PROGRESS](PROGRESS.md) = capabilities/tests ·
[HISTORY](HISTORY.md) = chronology · [analysis index](analysis/README.md) = focused evidence.
