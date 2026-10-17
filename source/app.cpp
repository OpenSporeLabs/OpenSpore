#include "app.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>  // abort(), for the deliberate fault-tolerance probe
#include <cctype>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "engine/cmdline.h"
#include "engine/config.h"
#include "engine/console.h"
#include "engine/gmdl.h"
#include "engine/raster.h"
#include "engine/diag.h"
#include "engine/fs.h"
#include "engine/input.h"
#include "engine/log.h"
#include "engine/naming.h"
#include "engine/pkg.h"
#include "engine/appmodes.h"
#include "engine/appstates.h"
#include "engine/spui.h"
#include "engine/text.h"
#include "render/device.h"
#include "render/overlay.h"
#include "states/statemgr.h"
#include "states/uipresent.h"   // requested_by_state: which layout the current state asked for
#include "states/debugmenu.h"

namespace os::app {
namespace {

// ---------------------------------------------------------------- asset writers
// For the unpacker: turn a decoded `raster` into a BMP and a `gmdl` into an OBJ, so the game's
// own textures and models come out as standard files. BMP (not PNG) keeps this dependency-free;
// BGRA rows go out bottom-up exactly like our frame capture.

bool write_bmp_bgra(const std::string& path, int w, int h, const u8* bgra) {
    if (w <= 0 || h <= 0 || !bgra) return false;
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return false;
    const u32 px = (u32)w * (u32)h, img = px * 4, file_size = 54 + img;
    u8 hdr[54];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &file_size, 4);
    const u32 off = 54;
    memcpy(hdr + 10, &off, 4);
    const u32 hs = 40;
    memcpy(hdr + 14, &hs, 4);
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 32;
    memcpy(hdr + 34, &img, 4);
    const u32 ppm = 2835;
    memcpy(hdr + 38, &ppm, 4); memcpy(hdr + 42, &ppm, 4);
    fwrite(hdr, 1, 54, f);
    for (int y = h - 1; y >= 0; --y) fwrite(bgra + (size_t)y * w * 4, 1, (size_t)w * 4, f);
    fclose(f);
    return true;
}

bool write_obj(const std::string& path, const gmdl::Mesh& m) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return false;
    fprintf(f, "# gmdl: %u vertices, %zu indices\n", m.vertex_count, m.indices.size());
    for (u32 i = 0; i < m.vertex_count; ++i)
        fprintf(f, "v %.6f %.6f %.6f\n", m.positions[i * 3], m.positions[i * 3 + 1], m.positions[i * 3 + 2]);
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3)
        fprintf(f, "f %u %u %u\n", m.indices[i] + 1, m.indices[i + 1] + 1, m.indices[i + 2] + 1);
    fclose(f);
    return true;
}

// OBJ that references a paired texture: writes mtllib/usemtl into the OBJ and a sibling .mtl.
bool write_obj_textured(const std::string& objpath, const gmdl::Mesh& m, const std::string& texfile) {
    FILE* f = nullptr;
    if (fopen_s(&f, objpath.c_str(), "wb") != 0 || !f) return false;
    std::filesystem::path op(objpath);
    std::string stem = op.stem().string();
    fprintf(f, "# gmdl: %u vertices, %zu indices\nmtllib %s.mtl\nusemtl tex\n",
            m.vertex_count, m.indices.size(), stem.c_str());
    for (u32 i = 0; i < m.vertex_count; ++i)
        fprintf(f, "v %.6f %.6f %.6f\n", m.positions[i * 3], m.positions[i * 3 + 1], m.positions[i * 3 + 2]);
    if (m.uvs.size() == (size_t)m.vertex_count * 2)
        for (u32 i = 0; i < m.vertex_count; ++i)
            fprintf(f, "vt %.6f %.6f\n", m.uvs[i * 2], m.uvs[i * 2 + 1]);
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3)
        fprintf(f, "f %u/%u %u/%u %u/%u\n", m.indices[i] + 1, m.indices[i] + 1, m.indices[i + 1] + 1,
                m.indices[i + 1] + 1, m.indices[i + 2] + 1, m.indices[i + 2] + 1);
    fclose(f);
    std::filesystem::path mp = op;
    mp.replace_extension(".mtl");
    FILE* mf = nullptr;
    if (fopen_s(&mf, mp.string().c_str(), "wb") == 0 && mf) {
        fprintf(mf, "newmtl tex\nKa 1 1 1\nKd 1 1 1\nKs 0 0 0\nd 1\nmap_Kd %s\n", texfile.c_str());
        fclose(mf);
    }
    return true;
}

// ---------------------------------------------------------------- yardstick
// Field guide §1: maintain ONE cheap numeric measure of "are we further along".
// Here it is the count of boot-chain markers that reached OK. It is printed once per run
// and is machine-comparable across sessions.
struct BootMarker {
    const char* name;
    const char* status = "pending";
};

BootMarker g_chain[] = {
    {"diag.install"}, {"log.init"}, {"cmdline.parse"}, {"fs.init"}, {"window.create"},
    {"d3d.device"}, {"font.atlas"}, {"console.init"}, {"states.register"}, {"state.goto"},
};
constexpr size_t kChainCount = sizeof(g_chain) / sizeof(g_chain[0]);
size_t g_chain_ok = 0;

void mark(size_t i, bool ok, const std::string& detail = {}) {
    g_chain[i].status = ok ? "OK" : "FAIL";
    if (ok) ++g_chain_ok;
    char buf[160];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "[%2zu/%2zu] %-16s %s%s%s", i + 1, kChainCount, g_chain[i].name,
                g_chain[i].status, detail.empty() ? "" : "  ", detail.c_str());
    // LOG_ONCE on purpose: every marker shares this call site, so the ledger would
    // consolidate markers 9-10 away ("x9, further repeats suppressed" in the first run).
    if (ok) {
        LOG_ONCE_INFO("boot", "%s", buf);
    } else {
        LOG_ONCE_ERROR("boot", "%s", buf);
    }
}

void print_chain() {
    std::string s = "\n--- boot chain -------------------------------------------------\n";
    for (size_t i = 0; i < kChainCount; ++i) {
        char line[128];
        _snprintf_s(line, sizeof(line), _TRUNCATE, "  %-18s %s\n", g_chain[i].name, g_chain[i].status);
        s += line;
    }
    s += "--------------------------------------------------------------\n";
    LOG_INFO("boot", "yardstick: %zu/%zu markers OK", g_chain_ok, kChainCount);
    log::raw_write(s.c_str(), s.size());
}

// ---------------------------------------------------------------- window
const wchar_t* kWndClass = L"OpenSporeWindowClass";

Args g_args;
HWND g_hwnd = nullptr;
bool g_quit = false;
int g_quit_code = 0;
bool g_minimized = false;
bool g_size_pending = false;
int g_new_w = 0, g_new_h = 0;

LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: return 0;
        case WM_CLOSE:
            PostQuitMessage(0);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_SIZE:
            if (wp == SIZE_MINIMIZED) {
                g_minimized = true;
            } else if (wp == SIZE_RESTORED) {
                g_minimized = false;
            } else if (wp == SIZE_MAXIMIZED) {
                g_minimized = false;
            }
            if (wp != SIZE_MINIMIZED) {
                g_new_w = LOWORD(lp);
                g_new_h = HIWORD(lp);
                g_size_pending = true;
            }
            return 0;
        case WM_MOVE: return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(h, &ps);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_SETCURSOR:
            // The original manages its own cursor (USER32 SetCursor/ShowCursor imports).
            // During boot we keep the arrow.
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
            return TRUE;
        case WM_MOUSEMOVE:
            input::on_move(LOWORD(lp), HIWORD(lp));
            return 0;
        case WM_LBUTTONDOWN:
            input::on_move(LOWORD(lp), HIWORD(lp));
            input::on_button(true, true);
            return 0;
        case WM_LBUTTONUP:
            input::on_move(LOWORD(lp), HIWORD(lp));
            input::on_button(true, false);
            return 0;
        case WM_RBUTTONDOWN:
            input::on_move(LOWORD(lp), HIWORD(lp));
            input::on_button(false, true);
            return 0;
        case WM_KEYDOWN:
            if (states::debugmenu::handle_keydown((unsigned)wp, (long)lp)) return 0;
            if (wp == 'C' && !(lp & (1LL << 30)) && (GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_SHIFT) & 0x8000)) {
#ifdef OS_ENABLE_DEV_COMMANDS
                states::debugmenu::toggle();
#endif
                return 0;
            }
            if (wp == VK_ESCAPE) {
#ifdef OS_ENABLE_DEV_COMMANDS
                if (states::debugmenu::open()) { states::debugmenu::toggle(); return 0; }
#endif
                PostQuitMessage(0);
                return 0;
            }
            break;
        case WM_CHAR:
            if (states::debugmenu::handle_char((wchar_t)wp)) return 0;
            break;
        case WM_ACTIVATEAPP:
            LOG_DEBUG("app", "activate=%d", (int)wp);
            return 0;
        case WM_DISPLAYCHANGE:
            LOG_INFO("app", "display changed: %dx%d", LOWORD(lp), HIWORD(lp));
            return 0;
        case WM_DPICHANGED: return 0;
        default: break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

bool create_window(HINSTANCE inst, int w, int h, bool windowed, const wchar_t* title) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = kWndClass;
    // Icon: shipped as resource id 101 (see openspore.rc, generated from refs/OpenSpore.ico).
    HICON ico = LoadIconW(inst, MAKEINTRESOURCEW(101));
    wc.hIcon = ico ? ico : LoadIcon(nullptr, IDI_APPLICATION);
    wc.hIconSm = ico;
    if (!RegisterClassExW(&wc)) {
        LOG_ERROR("app", "RegisterClassExW failed: %lu", GetLastError());
        return false;
    }
    DWORD style = windowed ? (WS_OVERLAPPEDWINDOW) : (WS_POPUP | WS_VISIBLE);
    RECT rc{0, 0, w, h};
    if (windowed) AdjustWindowRectEx(&rc, style, FALSE, 0);
    int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (windowed) {
        int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
        x = (sw - ww) / 2;
        y = (sh - wh) / 2;
    }
    g_hwnd = CreateWindowExW(0, kWndClass, title, style, x, y, ww, wh, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) {
        LOG_ERROR("app", "CreateWindowExW failed: %lu", GetLastError());
        return false;
    }
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);
    LOG_INFO("app", "window %p '%s' %dx%d (client %dx%d) windowed=%d icon=%s", g_hwnd,
            "OpenSpore - Loader", ww, wh, w, h, (int)windowed, ico ? "embedded" : "fallback");
    return true;
}

// ---------------------------------------------------------------- console attach
// PROMPT.md: "игра будет с консолью при запуске". Strategy: if we were launched from a
// shell we already HAVE a console (AttachConsole), otherwise allocate one. Never block on
// it: a console is an observer here, not a dependency.
// Returns a human-readable account of which path succeeded, for the boot log.
std::string attach_console() {
    if (GetConsoleWindow()) return "inherited from parent";
    if (AttachConsole(ATTACH_PARENT_PROCESS)) return "attached to parent console";
    if (AllocConsole()) {
        // Re-point the C runtime at the new console so printf/fgets work.
        FILE* dummy = nullptr;
        freopen_s(&dummy, "CONOUT$", "w", stdout);
        freopen_s(&dummy, "CONIN$", "r", stdin);
        return "allocated a new console";
    }
    return "NONE (AllocConsole failed, err=" + std::to_string(GetLastError()) + ")";
}

// ---------------------------------------------------------------- frame loop
u64 g_frames = 0;
u64 g_frame_aborts = 0;

// The risky region, isolated in a frame with no non-trivial C++ objects, so that a
// longjmp-based recovery from diag::run_guarded cannot corrupt the caller.
struct FrameCtx {
    f32 dt;
};

// Pending deliberate aborts, requested by the `_abortNow` verb. The command itself only
// sets this counter; frame_body() consumes it INSIDE the guarded region. Executing abort()
// in console::poll() would test nothing, because poll() runs outside run_guarded.
volatile LONG g_pending_aborts = 0;

void request_abort(int times) { InterlockedExchange(&g_pending_aborts, (LONG)times); }

void frame_body(void* p) {
    auto* fc = static_cast<FrameCtx*>(p);
    LONG n = g_pending_aborts;
    if (n > 0) {
        // One abort per frame, not nine in a single frame: that is what a real fault looks
        // like (a state that dies on entry), and it is the only way to reach diag's
        // storm limit (>5 aborts within 1s) instead of recovering forever.
        InterlockedDecrement(&g_pending_aborts);
        LOG_ERROR("fault", "deliberate abort() from INSIDE the guarded frame (%ld left)", (long)g_pending_aborts);
        void (*volatile fp)() = abort;
        fp();
    }
    if (g_args.headless) return;  // no window/device: tick and render need neither, so skip both
    // Run whatever the previous frame's INPUT queued BEFORE ticking and rendering, so the frame the
    // user sees is the state their click caused - not the one it left behind. See Manager::drain_pending
    // for why a per-frame drain was necessary at all.
    states::manager().drain_pending();
    states::manager().tick(fc->dt);
    states::manager().render();
    // A state with no renderer must still PRESENT A FRAME. The original's mode decides a state's
    // CONTENT, not whether the device pumps; without this, booting into any of the 60 game states
    // (whose behaviour is an unimplemented `mode`) draws nothing, so the deferred -capture is never
    // serviced and the frame loop silently produces no pixels. Draw the honest placeholder instead:
    // the real state name, its real id, its description, and exactly what could not be executed.
    if (const auto* st = states::manager().current()) {
        if (!st->on_render) {
            render::Device& dev = render::device();
            if (!dev.d3d()) return;
            if (!dev.begin_frame()) return;
            const int w = dev.width(), h = dev.height();
            dev.clear(0xFF07090C);
            render::overlay().begin(w, h);
            render::overlay().text_fmt(48.f, 44.f, 0xFFFFE8A0, "%s", st->name.c_str());
            char idbuf[64];
            _snprintf_s(idbuf, sizeof(idbuf), _TRUNCATE, "state id 0x%08X%s", st->id,
                        st->has_explicit_id ? "  (assigned by the game's script, not a hash)"
                                            : "  (FNV-1 of the name)");
            render::overlay().text_fmt(48.f, 74.f, 0xFF9FB4C4, "%s", idbuf);
            const auto* gs = appstates::instance().find(st->name);
            float y = 104.f;
            if (gs && !gs->description.empty()) {
                render::overlay().text_fmt(48.f, y, 0xFFC8D8E4, "description: \"%s\"",
                                           gs->description.c_str());
                y += 22.f;
            }
            if (gs && !gs->mode.empty()) {
                render::overlay().text_fmt(48.f, y, 0xFFE0A878, "mode %s  -- NOT EXECUTED",
                                           gs->mode.c_str());
                y += 22.f;
                render::overlay().text_fmt(48.f, y, 0xFF8494A4,
                                           "(the engine's mode table is the next step after `spui`)");
                y += 30.f;
            }
            if (gs) {
                for (const auto& nxt : gs->goto_state) {
                    render::overlay().text_fmt(48.f, y, 0xFF9FD0A0, "gotoState %s", nxt.c_str());
                    y += 22.f;
                }
                for (const auto& a : gs->entry) {
                    if (a.verb == "mode" || a.verb == "gotoState") continue;
                    render::overlay().text_fmt(48.f, y, 0xFFB4A0B4, "%s %s", a.verb.c_str(),
                                               a.raw.c_str());
                    y += 20.f;
                    if (y > (float)h - 90.f) break;
                }
            }
            render::overlay().text_fmt(48.f, (float)h - 44.f, 0xFF707C88,
                                       "no renderer for this state: the frame is a placeholder so "
                                       "capture/quit stay uniform");
            render::overlay().end();
            dev.end_frame();
        }
    }
}

void pump_messages() {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        if (m.message == WM_QUIT) {
            g_quit = true;
            g_quit_code = (int)m.wParam;
            return;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

struct RunLogPaths { std::string normal="OpenSpore.log", crash="OpenSpore.crash.log"; };
RunLogPaths make_run_log_paths() {
    RunLogPaths paths; SYSTEMTIME now{}; GetLocalTime(&now); char stem[96]{};
    _snprintf_s(stem,sizeof(stem),_TRUNCATE,"OpenSpore-%04u%02u%02u-%02u%02u%02u-%03u-pid%lu",
                now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond,now.wMilliseconds,GetCurrentProcessId());
    wchar_t module[MAX_PATH]{};
    const DWORD length=GetModuleFileNameW(nullptr,module,MAX_PATH);
    std::error_code ec;
    if(length>0&&length<MAX_PATH) {
        const auto log_directory=std::filesystem::path(module).parent_path()/"logs";
        std::filesystem::create_directories(log_directory,ec);
        if(!ec) {
            paths.normal=(log_directory/(std::string(stem)+".log")).string();
            paths.crash=(log_directory/(std::string(stem)+".crash.log")).string();
        }
    }
    return paths;
}

}  // namespace

void request_quit(int code) {
    g_quit = true;
    g_quit_code = code;
}

bool quitting() { return g_quit; }

int run(HINSTANCE inst, HINSTANCE, LPWSTR cmdline_wide, int) {
    // 0. diagnostics + log FIRST (field guide §4: instrument before you experiment).
    // Order matters and was wrong in the first run: diag::install() must precede log::init(),
    // because log::init() itself can trip a CRT assert (it did: setvbuf with a 0-byte buffer),
    // and the only thing that turns that into "log + continue" instead of a modal dialog is
    // the harness being already in place.
    std::string console_how = attach_console();
    const RunLogPaths log_paths=make_run_log_paths();
    diag::install(log_paths.crash.c_str());
    log::init(log_paths.normal.c_str(), log::Level::Trace, log::Level::Debug);
    mark(0, true, "harness up before the log existed");
    mark(1, true, "file="+log_paths.normal+" crash="+log_paths.crash+" console="+console_how);
    // Name resolution (id -> name) is data, loaded once. Failure is not fatal: every name
    // falls back to hex rather than to a guess.
    {
        u32 n = naming::load();
        LOG_INFO("app", "naming registry loaded (%u names)", n);
    }

    LOG_INFO("app", "=== OpenSpore boot (GA reimplementation) ===");
    // Preprocessor directives are not allowed inside a macro argument list (that is what
    // the first build's C2121 '#' error was), so the arch string is computed beforehand.
#ifdef _WIN64
    const char* arch = "x64";
#else
    const char* arch = "x86";
#endif
    // Compiler identity for the boot log. MSVC reports _MSC_VER exactly as before; the
    // GCC/MinGW branch exists so the experimental cross-build (build-tools/) prints a real
    // version instead of failing to compile on the undefined _MSC_VER macro.
#if defined(_MSC_VER)
    const char* compiler = "MSVC";
    const int   compiler_ver = _MSC_VER;
#elif defined(__GNUC__)
    const char* compiler = "GCC";
    const int   compiler_ver = __GNUC__ * 100 + __GNUC_MINOR__;
#else
    const char* compiler = "unknown";
    const int   compiler_ver = 0;
#endif
    LOG_INFO("app", "build: %s, console=%s, %s %d", arch,
             GetConsoleWindow() ? "attached" : "none", compiler, compiler_ver);

    // 2. command line. wWinMain's `cmdline_wide` is lpCmdLine (NO program name); the
    // GetCommandLineW() fallback HAS it. Say which, or the first argument is dropped.
    g_args = cmdline_wide ? parse_command_line(cmdline_wide, /*has_program_name=*/false)
                          : parse_command_line(GetCommandLineW());
    mark(2, true, to_string(g_args));
    LOG_INFO("app", "flags: windowed=%d devDirs=%d shipDirs=%d useDevDirs=%d state='%s' preload='%s' %dx%d vsync=%d",
             (int)g_args.windowed, (int)g_args.dev_dirs, (int)g_args.ship_dirs, (int)g_args.use_dev_dirs(),
             g_args.state.c_str(), g_args.preload.c_str(), g_args.width, g_args.height, (int)!g_args.no_vsync);

    // 3. filesystem search paths (reversed devDirs contract: shipDirs wins over devDirs)
    wchar_t exe_path[MAX_PATH] = {};
    GetModuleFileNameW(inst, exe_path, MAX_PATH);
    std::filesystem::path exe_dir = std::filesystem::path(exe_path).parent_path();
    fs::init(g_args, exe_dir);
    mark(3, true, fs::dump_search_list());

    // 3b. the game's own config scripts (Config/*.txt). We read them through the same search
    // paths, so this is the real shipped config, not a reimplementation of a format we guessed.
    // (Its own LOG_INFO lines are the evidence; it must not add a boot marker.)
    config::instance().load();

    // 3c. the game's localized text (`locale` resources under Locale/<lang>). Same reasoning as the
    // config scripts: the strings a menu needs are shipped data we can read today, so the Title
    // screen can show the game's own labels instead of our prose. `-locale` selects the language
    // (fs.cpp already put Locale/<lang> on the search path); en-us is the default.
    text::instance().load(g_args.locale.empty() ? "en-us" : g_args.locale);

    // 3d. the game's OWN state table, from the SporeAppStates trigger resource. This is the data we
    // recorded as missing for three sessions (it is an INSTANCE id, not a group). Its own LOG lines are
    // the evidence; it must not add a boot marker.
    appstates::instance().load();

    // 3e. the game's UI LAYOUTS (`spui`), read with the schema that the shipped designer XML supplies.
    // Anchor-driven, so it does not depend on the ids the designer XML omits (analysis/spui-schema.md).
    // limit 0 = every layout: there are 748 and the parse is cheap (measured: 400 took well under a
    // second), so capping it would only hide screens from the `spui` verb.
    spui::instance().load(0);

    // 4. window (skipped headless)
    if (g_args.headless) {
        mark(4, true, "headless: no window");
    } else if (!create_window(inst, g_args.width, g_args.height, g_args.windowed, L"OpenSpore - Loader")) {
        mark(4, false, "CreateWindow failed");
        print_chain();
        log::shutdown();
        return 3;
    } else {
        mark(4, true);
    }

    // 5. renderer (skipped headless). Error strings mirror the original's own contract.
    render::Config rcfg;
    rcfg.windowed = g_args.windowed;
    rcfg.width = g_args.width;
    rcfg.height = g_args.height;
    rcfg.vsync = !g_args.no_vsync;
    std::string err;
    if (g_args.headless) {
        mark(5, true, "headless: no device");
    } else if (!render::device().init(g_hwnd, rcfg, err)) {
        mark(5, false, err);
        LOG_FATAL("app", "Could not start the renderer. (%s)", err.c_str());  // 0x014139e0
        print_chain();
        log::shutdown();
        return 4;
    } else {
        mark(5, true,
             "device " + std::to_string(render::device().width()) + "x" + std::to_string(render::device().height()));
    }

    // 6. font atlas (skipped headless)
    if (g_args.headless) {
        mark(6, true, "headless: no atlas");
    } else if (!render::overlay().init(render::device().d3d())) {
        mark(6, false, "atlas");
        print_chain();
        log::shutdown();
        return 5;
    } else {
        mark(6, true);
    }

    // 7. console + dev commands. PROMPT.md: developer commands available in Debug builds;
    // the original also ships `state` in release, so the state verb is unconditional.
#ifdef OS_ENABLE_DEV_COMMANDS
    const bool dev = true;
#else
    const bool dev = false;
#endif
    console::init(dev);
    states::install_console_command(states::manager());
    states::install_loader(states::manager());
    states::install_title(states::manager());
    states::debugmenu::install(states::manager());
    // The game's 60 states, with their real ids, into our state manager, so `-state:Main` and
    // `-state:TribalAccessoriesEditor` resolve against the shipped table rather than only ours.
    appstates::install(states::manager());

    // Capture + fault-tolerance probes. `capture` is unconditional (it is the only way to
    // get machine evidence of a rendered frame); `_abortNow` is dev-only by the '_' rule.
    console::reg({"capture", "Write the current front buffer to a BMP (PrintWindow cannot do D3D9).",
                  "-<path>",
                  [](const console::CmdContext& c) -> std::string {
                      std::string p = c.args.size() > 1 ? c.args[1] : "OpenSpore_capture.bmp";
                      // Same deferral as the -capture launch key: poll() runs outside the scene.
                      render::device().request_capture(p.c_str());
                      return "capture scheduled for the next frame -> " + p;
                  }});
    console::reg({"cfg", "The game's own Config/*.txt: option table (Properties.txt) + vars (ConfigManager.txt).",
                  "-list | -props | -vars | <name>",
                  [](const console::CmdContext& c) -> std::string {
                      auto& cfg = config::instance();
                      if (c.args.size() < 2 || c.args[1] == "-list") return cfg.stats();
                      if (c.args[1] == "-props") {
                          std::string s = std::to_string(cfg.properties().size()) + " properties:\n";
                          for (auto& p : cfg.properties()) {
                              char b[128];
                              _snprintf_s(b, sizeof(b), _TRUNCATE, "  %-28s 0x%08x %s",
                                          p.name.c_str(), p.id, p.type.c_str());
                              s += b; s += "\n";
                          }
                          return s;
                      }
                      if (c.args[1] == "-vars") {
                          std::string s = std::to_string(cfg.vars().size()) + " variables:\n";
                          for (auto& kv : cfg.vars()) s += "  " + kv.first + " = " + cfg.get(kv.first) + "\n";
                          return s;
                      }
                      if (c.args[1] == "-cards") {
                          // VideoCards.txt summary: vendor/card tables (the game's hardware DB).
                          return "VideoCards: " + std::to_string(cfg.vendor_count()) + " vendors, " +
                                 std::to_string(cfg.card_count()) + " cards";
                      }
                      std::string v = cfg.get(c.args[1]);
                      if (!v.empty()) return c.args[1] + " = " + v;
                      // also look the name up in the property table (Properties.txt)
                      for (auto& p : cfg.properties()) {
                          std::string pn = p.name;
                          for (auto& ch : pn) ch = (char)std::tolower((unsigned char)ch);
                          if (pn == c.args[1]) {
                              char b[128];
                              _snprintf_s(b, sizeof(b), _TRUNCATE, "%s = property 0x%08x %s",
                                          p.name.c_str(), p.id, p.type.c_str());
                              return b;
                          }
                      }
                      return c.args[1] + " = <not set>";
                  }});
    console::reg({"txt", "The game's localized text (locale resources under Locale/<lang>).",
                  "-list | -label <text> | -ambig | <0xKEY> | <tableINST> <0xKEY>",
                  [](const console::CmdContext& c) -> std::string {
                      auto& tx = text::instance();
                      if (!tx.loaded()) return "locale text not loaded (no Locale/<lang> package?)";
                      if (c.args.size() < 2 || c.args[1] == "-list") return tx.stats();
                      if (c.args[1] == "-ambig") {
                          std::string s = std::to_string(tx.ambiguous_keys()) +
                                          " keys are ambiguous across tables (get() refuses them):\n";
                          size_t n = 0;
                          for (const auto& e : tx.entries()) {
                              if (!tx.ambiguous(e.key)) continue;
                              char b[160];
                              _snprintf_s(b, sizeof(b), _TRUNCATE, "  0x%08X table 0x%08X  %s",
                                          e.key, e.table,
                                          text::to_display_ascii(e.text).substr(0, 60).c_str());
                              s += b;
                              s += "\n";
                              if (++n >= 40) { s += "  ...\n"; break; }
                          }
                          return s;
                      }
                      if (c.args[1] == "-label") {
                          std::string want;
                          for (size_t i = 2; i < c.args.size(); ++i) {
                              if (i > 2) want += " ";
                              want += c.args[i];
                          }
                          auto hits = tx.find_label(want);
                          if (hits.empty()) return "no locale entry labelled '" + want + "'";
                          std::string s = std::to_string(hits.size()) + " hit(s) for '" + want + "':\n";
                          for (const auto& h : hits) {
                              // Bind to a named string before .c_str(): a ternary whose branches are
                              // `const char*` and `std::string` decays to std::string, and passing a
                              // non-trivial class through a variadic %s is C4840/C4477.
                              std::string tag = h.markup.empty() ? std::string()
                                                                 : ("  [~" + h.markup + "~]");
                              char b[160];
                              _snprintf_s(b, sizeof(b), _TRUNCATE, "  0x%08X  table 0x%08X  %s%s",
                                          h.key, h.table, text::to_display_ascii(h.text).c_str(),
                                          tag.c_str());
                              s += b;
                              s += "\n";
                          }
                          return s;
                      }
                      // -label <text> | <0xKEY> | <tableINST> <0xKEY>
                      // `txt <table> <key>` OR `txt <table>:<key>`. The colon form exists because our
                      // CLI's native separator is ':' (as in -state:Name), and a space-separated pair
                      // is awkward to pass through -exec from a shell that eats spaces.
                      if (c.args.size() >= 2 && c.args[1].find(':') != std::string::npos) {
                          size_t colon = c.args[1].find(':');
                          std::string ta = c.args[1].substr(0, colon), ka = c.args[1].substr(colon + 1);
                          u32 table = (u32)std::strtoul(ta.c_str(), nullptr, 0);
                          u32 key = (u32)std::strtoul(ka.c_str(), nullptr, 0);
                          std::string v = tx.get(table, key);
                          char nb[160];
                          if (v.empty()) {
                              std::snprintf(nb, sizeof(nb), "no key 0x%08X in table 0x%08X", key, table);
                              return nb;
                          }
                          std::snprintf(nb, sizeof(nb), "tbl 0x%08X key 0x%08X = %s  ->  %s", table, key,
                                        text::to_display_ascii(v).c_str(), v.c_str());
                          return nb;
                      }
                      if (c.args.size() >= 4) {
                          u32 table = (u32)std::strtoul(c.args[2].c_str(), nullptr, 0);
                          u32 key = (u32)std::strtoul(c.args[3].c_str(), nullptr, 0);
                          std::string v = tx.get(table, key);
                          if (v.empty()) {
                              char nb[40];
                              _snprintf_s(nb, sizeof(nb), _TRUNCATE, "no key 0x%08X in table 0x%08X",
                                          key, table);
                              return nb;
                          }
                          return v + "  ->  " + text::to_display_ascii(v);
                      }
                      u32 key = (u32)std::strtoul(c.args[1].c_str(), nullptr, 0);
                      if (tx.ambiguous(key)) {
                          std::string s = "0x" + [&] {
                              char b[16];
                              _snprintf_s(b, sizeof(b), _TRUNCATE, "%08X", key);
                              return std::string(b);
                          }() +
                                          " is ambiguous across tables (the key space is per-table); "
                                          "pass a table instance:\n";
                          for (const auto& e : tx.entries()) {
                              if (e.key != key) continue;
                              s += "  table 0x" + [&] {
                                  char b[16];
                                  _snprintf_s(b, sizeof(b), _TRUNCATE, "%08X", e.table);
                                  return std::string(b);
                              }() + "  " + text::to_display_ascii(e.text) + "\n";
                          }
                          return s;
                      }
                      std::string v = tx.get(key);
                      if (v.empty()) {
                          char nb[24];
                          _snprintf_s(nb, sizeof(nb), _TRUNCATE, "no locale key 0x%08X", key);
                          return nb;
                      }
                      char nb[24];
                      _snprintf_s(nb, sizeof(nb), _TRUNCATE, "0x%08X", key);
                      return std::string(nb) + " = " + text::to_display_ascii(v);
                  }});
    console::reg({"spui", "The game's UI layouts, read with the shipped designer schema.",
                  "-list | -screen | -win <i>",
                  [](const console::CmdContext& c) -> std::string {
                      auto& rd = spui::instance();
                      if (!rd.loaded()) return "spui not loaded";
                      if (c.args.size() < 2 || c.args[1] == "-list") {
                          std::string s = rd.stats() + "\n";
                          size_t i = 0;
                          for (const auto& L : rd.layouts()) {
                              size_t a = 0;
                              for (const auto& w : L.windows) a += w.has_area;
                              char b[192];
                              std::snprintf(b, sizeof(b), "  [%2zu] %-28s rec[%-6u] 0x%08X  %4zu win "
                                            "(%zu with Area, %zu/%zu exact properties)",
                                            i, L.package.c_str(), L.record, L.instance,
                                            L.windows.size(), a, L.recognized, L.anchors);
                              s += b; s += "\n";
                              if (++i >= 24) { s += "  ...\n"; break; }
                          }
                          return s;
                      }
                      const spui::Layout* L = rd.best_screen();
                      if (c.args[1] == "-caps") {
                          // Measure the layout -> locale-text CHAIN, not just the layout: an inline
                          // caption is already text, a (table,key) pair is only text if our parsed
                          // tables contain that key. Reporting the layout count alone would overstate
                          // how much of the menu is actually readable.
                          auto& tx = text::instance();
                          size_t inline_n = 0, ref_n = 0, resolved = 0, unres = 0;
                          std::string sample;
                          for (const auto& lw : rd.layouts()) {
                              for (const auto& win : lw.windows) {
                                  if (!win.caption.empty()) {
                                      ++inline_n;
                                      continue;
                                  }
                                  if (!win.caption_is_ref) continue;
                                  ++ref_n;
                                  std::string s = win.caption_key_seen
                                                      ? tx.get(win.caption_table, win.caption_key)
                                                      : std::string();
                                  if (!s.empty()) {
                                      ++resolved;
                                      if (sample.empty()) {
                                          char b[160];
                                          std::snprintf(b, sizeof(b), "  e.g. tbl 0x%08X key 0x%08X -> \"%s\"",
                                                        win.caption_table, win.caption_key,
                                                        text::to_display_ascii(s).substr(0, 60).c_str());
                                          sample = b;
                                      }
                                  } else {
                                      ++unres;
                                  }
                              }
                          }
                          char buf[400];
                          std::snprintf(buf, sizeof(buf),
                                        "captions: %zu inline, %zu references (%zu resolve through the "
                                        "locale tables, %zu do not)%s",
                                        inline_n, ref_n, resolved, unres, sample.c_str());
                          return buf;
                      }
                      if (c.args[1] == "-gaps") {
                          // Classify the references that do NOT resolve. The distinction is the
                          // whole point: a table we never parsed is a COVERAGE gap (the text is not
                          // in the tree we have), while a table we did parse that lacks the key is a
                          // SEMANTIC gap (the layout points at something the shipped text does not
                          // define). One conflated number kept "mvj and legal are missing" alive for
                          // two sessions after `legal` had been found sitting in Data/PatchData
                          // .package's nested locale packages.
                          auto& tx = text::instance();
                          std::map<u32, std::vector<u32>> by_table;
                          size_t refs = 0;
                          for (const auto& lw : rd.layouts()) {
                              for (const auto& win : lw.windows) {
                                  if (!win.caption_is_ref || !win.caption_key_seen) continue;
                                  ++refs;
                                  if (tx.has_in(win.caption_table, win.caption_key)) continue;
                                  auto& v = by_table[win.caption_table];
                                  if (std::find(v.begin(), v.end(), win.caption_key) == v.end()) {
                                      v.push_back(win.caption_key);
                                  }
                              }
                          }
                          size_t cov_tables = 0, sem_tables = 0, cov_keys = 0, sem_keys = 0;
                          std::ostringstream out;
                          out << "unresolved caption refs: " << refs << " refs, " << by_table.size()
                              << " distinct tables";
                          for (const auto& kv : by_table) {
                              const bool known = tx.has_table(kv.first);
                              if (known) { ++sem_tables; sem_keys += kv.second.size(); }
                              else       { ++cov_tables; cov_keys += kv.second.size(); }
                              out << "\n  " << (known ? "SEMANTIC  tbl " : "COVERAGE  tbl ");
                              char b[24];
                              std::snprintf(b, sizeof(b), "0x%08X", kv.first);
                              out << b << "  " << kv.second.size() << " key(s)";
                              out << (known ? " - the table IS parsed; these keys are not in it"
                                            : " - this table instance is nowhere in the corpus");
                              size_t shown = 0;
                              for (u32 k : kv.second) {
                                  std::snprintf(b, sizeof(b), " 0x%08X", k);
                                  out << b;
                                  if (++shown >= 8) {
                                      if (kv.second.size() > shown) out << " ...";
                                      break;
                                  }
                              }
                          }
                          out << "\n  => coverage: " << cov_tables << " tables / " << cov_keys
                              << " keys; semantic: " << sem_tables << " tables / " << sem_keys
                              << " keys";
                          return out.str();
                      }
                      if (c.args[1] == "-hit" && c.args.size() >= 4) {
                          // Resolve a POINTER POSITION to a widget, without needing a window at all.
                          // This is what makes the click chain regression-testable: the same
                          // spui::hit_test() the frame uses, driven from the command line.
                          const int hx = std::atoi(c.args[2].c_str());
                          const int hy = std::atoi(c.args[3].c_str());
                          auto& dev = render::device();
                          const int vw = dev.d3d() ? dev.width() : g_args.width;
                          const int vh = dev.d3d() ? dev.height() : g_args.height;
                          std::string why;
                          const auto* lay = states::requested_by_state(why);
                          if (!lay) lay = rd.best_screen();   // headless: no state asked, so ours
                          if (!lay) return "-hit: no layout on screen to hit-test";
                          const auto* w = spui::hit_test(*lay, (float)hx, (float)hy, (float)vw,
                                                         (float)vh);
                          char b[400];
                          if (!w) {
                              std::snprintf(b, sizeof(b),
                                            "hit (%d,%d) in %dx%d viewport -> NO widget (layout %s)",
                                            hx, hy, vw, vh, lay->name().c_str());
                              return b;
                          }
                          std::string cap;
                          if (!w->caption.empty()) cap = w->caption;
                          else if (w->caption_key_seen)
                              cap = text::instance().get(w->caption_table, w->caption_key);
                          cap = text::to_display_ascii(cap);
                          std::snprintf(b, sizeof(b),
                                        "hit (%d,%d) in %dx%d -> %s  caption=\"%s\"  "
                                        "control=0x%08X command=0x%08X  [%s]",
                                        hx, hy, vw, vh, w->area.describe().c_str(), cap.c_str(),
                                        w->control_id, w->command_id, lay->name().c_str());
                          return b;
                      }
                      if (c.args[1] == "-click" && c.args.size() >= 4) {
                          // Feed the REAL frame path a synthetic click, so the hit-test exercised here
                          // is the one the renderer calls - not a parallel implementation that could
                          // pass while the live path is broken.
                          const int hx = std::atoi(c.args[2].c_str());
                          const int hy = std::atoi(c.args[3].c_str());
                          input::inject_click(hx, hy);
                          char b[128];
                          input::describe(b, sizeof(b));
                          return std::string("injected click at ") + c.args[2] + "," + c.args[3] +
                                 " -> " + b + " (dispatched on the next presented frame)";
                      }
                      if (c.args[1] == "-ctl") {
                          // List the CURRENT screen's widgets by CONTROL ID. This exists to test one
                          // specific claim: `uilayout -load E3LaunchScreen 0x027996d8 0x0279bf9e`
                          // carries two ids that are NOT resource ids anywhere in the corpus, and a
                          // hit-test on the layout's root window came back with control=0x027996D8 -
                          // the FIRST of them. If the second also appears, then `uilayout -load` is
                          // addressed by (layout name, control id), which is the last unknown in the
                          // UI chain and it closes without a single guess.
                          std::string why;
                          const auto* lay = states::requested_by_state(why);
                          if (!lay) lay = rd.best_screen();
                          if (!lay) return "-ctl: no layout on screen";
                          const u32 a1 = 0x027996D8u, a2 = 0x0279BF9Eu;
                          bool got1 = false, got2 = false;
                          for (const auto& w : lay->windows) {
                              if (w.control_id == a1) got1 = true;
                              if (w.control_id == a2) got2 = true;
                          }
                          std::ostringstream o;
                          float dw = 0.f, dh = 0.f;
                          lay->design_resolution(dw, dh);
                          // THE VERDICT FIRST. A long multi-line console message is cut by the log's
                          // per-message buffer, and the listing is exactly the part that can be
                          // regenerated - the conclusion is not. Ordering by importance, not by the
                          // order the work was done in.
                          o << "uilayout arg1 0x027996D8 is a control id in this layout: "
                            << (got1 ? "YES" : "no")
                            << ";  arg2 0x0279BF9E: " << (got2 ? "YES" : "no");
                          o << "\n" << lay->windows.size() << " windows of " << lay->name()
                            << ", design " << (int)dw << "x" << (int)dh;
                          // CORPUS-WIDE: is arg2 a control id of some OTHER layout? This matters
                          // more than it looks, because E3LaunchScreen ships TWICE with different
                          // byte lengths (4,486 B in PatchData vs 4,480 B in Spore_Graphics) and our
                          // precedence keeps only one. If arg2 lives in the copy we shadowed, then
                          // `uilayout -load` is addressed against a version of the layout we are not
                          // showing - which would be a live demonstration that the precedence rule is
                          // not cosmetic.
                          size_t n1 = 0, n2 = 0;
                          std::string where1, where2;
                          for (const auto& other : rd.layouts()) {
                              size_t c1 = 0, c2 = 0;
                              for (const auto& w : other.windows) {
                                  if (w.control_id == a1) ++c1;
                                  if (w.control_id == a2) ++c2;
                              }
                              if (c1) { ++n1; if (where1.size() < 90) where1 += " " + other.package; }
                              if (c2) { ++n2; if (where2.size() < 90) where2 += " " + other.package; }
                          }
                          o << "\narg1 0x027996D8 appears in " << n1 << " of " << rd.layouts().size()
                            << " parsed layout resources:" << (where1.empty() ? " none" : where1);
                          o << "\narg2 0x0279BF9E appears in " << n2 << ":"
                            << (where2.empty() ? " none - it is NOT a control id anywhere" : where2);
                          size_t shown = 0;
                          for (const auto& w : lay->windows) {
                              if (shown++ >= 8) break;   // print 8, the rest is only counts
                              char b[190];
                              std::snprintf(b, sizeof(b),
                                            "\n  control=0x%08X command=0x%08X  %s%s", w.control_id,
                                            w.command_id, w.has_area ? w.area.describe().c_str()
                                                                      : "(no Area)",
                                            w.control_id == a1 ? "   <-- uilayout arg 1"
                                                              : (w.control_id == a2
                                                                     ? "   <-- uilayout arg 2" : ""));
                              o << b;
                          }
                          return o.str();
                      }
                      if (c.args[1] == "-xref") {
                          // CROSS-SYSTEM TEST: do a layout's CONTROL IDS overlap the state script's
                          // MANUAL STATE IDS? The clue is concrete - E3LaunchScreen's star tiles carry
                          // control=0x022D9E00..0x022D9E05, and the script's TribalAccessoriesEditor
                          // is `-id 0x022d9e06`, the very next number, in the same block. The script
                          // says the author "assign[ed] manual IDs just so I can fill in the UI layout
                          // stuff more easily", which is an admission that the two live in ONE space.
                          //
                          // If they intersect beyond chance, a widget's control id NAMES A STATE, and
                          // a click can drive a transition WITHOUT reversing the command vocabulary -
                          // the shortest honest route from "a click resolves to a widget" to "a click
                          // does something". If they do not, say so with the numbers, and the command
                          // vocabulary stays the real gate. Either answer is worth having.
                          auto& tb = appstates::instance();
                          if (!tb.loaded()) return "-xref: the state table is not loaded";
                          std::map<u32, std::string> state_by_id;
                          size_t explicit_n = 0, hashed_n = 0;
                          for (const auto& gs : tb.states()) {
                              if (gs.has_explicit_id) ++explicit_n; else ++hashed_n;
                              state_by_id.emplace(gs.id, gs.name);
                          }
                          // Count control ids per state id across every parsed layout resource.
                          std::map<u32, size_t> hits;
                          std::map<u32, std::string> hit_where;   // id -> the layout(s) that carry it
                          std::map<u32, std::string> hit_cap;     // ...and the widget's own caption
                          size_t ctl_total = 0, ctl_distinct = 0;
                          std::set<u32> seen_ctl;
                          for (const auto& lay : rd.layouts()) {
                              for (const auto& w : lay.windows) {
                                  if (!w.control_id) continue;
                                  ++ctl_total;
                                  if (seen_ctl.insert(w.control_id).second) ++ctl_distinct;
                                  if (!state_by_id.count(w.control_id)) continue;
                                  ++hits[w.control_id];
                                  auto& cap = hit_cap[w.control_id];
                                  if (cap.empty()) {
                                      std::string s;
                                      if (!w.caption.empty()) s = w.caption;
                                      else if (w.caption_key_seen)
                                          s = text::instance().get(w.caption_table, w.caption_key);
                                      cap = text::to_display_ascii(s);
                                      // Remember the GEOMETRY too, so `-exec spui:-click,<x>,<y>` can
                                      // target this exact widget in a test. A finding you cannot click
                                      // on purpose is a finding you cannot verify.
                                      char ab[64];
                                      std::snprintf(ab, sizeof(ab), " @%s",
                                                    w.has_area ? w.area.describe().c_str() : "(none)");
                                      if (cap.size() < 60) cap += ab;
                                  }
                                  char ib[16];
                                  std::snprintf(ib, sizeof(ib), "%08X", lay.instance);
                                  const std::string tag = " " + lay.package + ":" + ib;
                                  if (hit_where[w.control_id].size() < 70) hit_where[w.control_id] += tag;
                              }
                          }
                          std::ostringstream o;
                          o << "control ids: " << ctl_total << " occurrences, " << ctl_distinct
                            << " distinct, across " << rd.layouts().size() << " layout resources";
                          o << "\nstates: " << (explicit_n + hashed_n) << " (" << explicit_n
                            << " with an explicit -id, " << hashed_n << " hashed)";
                          o << "\nINTERSECTION: " << hits.size() << " state id(s) also appear as a"
                            << " widget control id";
                          size_t shown = 0;
                          for (const auto& kv : hits) {
                              if (shown++ >= 20) { o << "\n  ..."; break; }
                              const std::string nm = state_by_id[kv.first];
                              const std::string cp = hit_cap[kv.first];
                              char b[220];
                              std::snprintf(b, sizeof(b),
                                            "\n  0x%08X  %-22s %2zu win %-34s caption=\"%s\"",
                                            kv.first, nm.c_str(), kv.second,
                                            hit_where[kv.first].c_str(),
                                            cp.empty() ? "-" : cp.c_str());
                              o << b;
                          }
                          // The control that a chance match would NOT explain: the expected hit count
                          // if the two spaces were independent.
                          const double exp = (double)ctl_distinct * (double)state_by_id.size()
                                             / 4294967296.0;
                          // A PROPER verdict, not a hand-wave. Under independence the count is
                          // Poisson(exp) with exp < 0.001, so P(at least 1 hit) is itself < 0.001 and
                          // observing 9 (excluding the low-id noise case below) is not a hint - it is
                          // the two spaces being the SAME space.
                          o << "\n  expected hits if independent: "
                            << (exp < 0.001 ? "<0.001" : std::to_string(exp).substr(0, 6))
                            << ", observed " << hits.size()
                            << " => " << (hits.size() >= 2 ? "OVERWHELMING: one shared manual id space"
                                                           : "not distinguishable from chance");
                          o << "\n  NOTE: 0x00000001 (DevLauncher's explicit -id 1) is the block-scoped"
                            << " low-id trap - ids below 0x1000 are member indices, not global, so"
                            << " discount it and judge on the rest.";
                          return o.str();
                      }
                      if (c.args[1] == "-tree") {
                          std::string why;
                          const auto* layout = c.args.size() >= 3
                              ? rd.find((u32)std::strtoul(c.args[2].c_str(), nullptr, 16))
                              : states::requested_by_state(why);
                          if (!layout) return "-tree: specify a loaded instance or enter a UI state";
                          return spui::describe_tree(*layout);
                      }
                      if (c.args[1] == "-dump" && c.args.size() >= 3) {
                          // Dump one layout's ANCHOR LIST in file order: offset, id, schema name, code,
                          // element count. Ground truth from the reader that already works, for
                          // questions it cannot answer alone - e.g. what surrounds a `Children` record
                          // (0xEEC1B00B), and therefore what the u16 after it counts.
                          const u32 want = (u32)std::strtoul(c.args[2].c_str(), nullptr, 16);
                          const size_t maxn = c.args.size() >= 4
                                                  ? (size_t)std::strtoul(c.args[3].c_str(), nullptr, 10)
                                                  : 60;
                          return spui::dump_anchors(want, maxn);
                      }
                      if (c.args[1] == "-win" && c.args.size() >= 3) {
                          size_t want = (size_t)std::strtoul(c.args[2].c_str(), nullptr, 10);
                          size_t i = 0;
                          for (const auto& lw : rd.layouts()) {
                              if (i++ != want) continue;
                              L = &lw;
                              break;
                          }
                      }
                      if (!L) return "no layout parsed";
                      std::ostringstream o;
                      o << "layout " << L->name() << "\n";
                      size_t n = 0;
                      for (const auto& w : L->windows) {
                          o << "  win @" << w.offset << " ctl=0x" << std::hex << w.control_id
                            << std::dec << " flags=0x" << std::hex << w.flags << std::dec;
                          if (w.has_area) {
                              char b[110];
                              std::snprintf(b, sizeof(b), " ltrb=(%.4g, %.4g, %.4g, %.4g) = %.4gx%.4g",
                                            w.area.left, w.area.top, w.area.right, w.area.bottom,
                                            w.area.w(), w.area.h());
                              o << b;
                          } else {
                              o << " area=<none>";
                          }
                          if (w.caption_is_ref) {
                              char b[64];
                              std::snprintf(b, sizeof(b), " text-table 0x%08X", w.caption_table);
                              o << b;
                          } else if (!w.caption.empty()) {
                              o << " caption=\"" << w.caption << "\"";
                          }
                          o << "\n";
                          if (++n >= 40) { o << "  ...\n"; break; }
                      }
                      return o.str();
                  }});
    console::reg({"gamestates", "The game's OWN state table (the SporeAppStates trigger resource).",
                  "-list | -entry | -modes | <name>",
                  [](const console::CmdContext& c) -> std::string {
                      auto& tb = appstates::instance();
                      if (!tb.loaded()) return "game state table not loaded (no SporeAppStates trigger)";
                      if (c.args.size() < 2 || c.args[1] == "-list") {
                          std::string s = tb.stats() + "\n";
                          for (const auto& gs : tb.states()) {
                              char b[200];
                              std::snprintf(b, sizeof(b), "  %-32s 0x%08X%s  mode=%-18s goto=%s",
                                            gs.name.c_str(), gs.id,
                                            gs.has_explicit_id ? " (explicit)" : "        ",
                                            gs.mode.empty() ? "-" : gs.mode.c_str(),
                                            gs.goto_state.empty() ? "-" :
                                                [&] {
                                                    std::string j;
                                                    for (size_t i = 0; i < gs.goto_state.size(); ++i) {
                                                        if (i) j += ",";
                                                        j += gs.goto_state[i];
                                                    }
                                                    return j;
                                                }().c_str());
                              s += b;
                              s += "\n";
                          }
                          return s;
                      }
                      if (c.args[1] == "-entry") {
                          std::string n = tb.entry_state();
                          const auto* gs = tb.find(n);
                          std::string s = "entry state: " + n;
                          if (gs) {
                              s += gs->has_explicit_id ? " (explicit -id 0)" : " (by name)";
                              s += ", description: \"" + gs->description + "\"";
                              // Walk the chain the way the original would, so the boot path is visible.
                              std::string cur = n;
                              for (int hop = 0; hop < 16; ++hop) {
                                  const auto* p = tb.find(cur);
                                  if (!p || p->goto_state.empty()) break;
                                  cur = p->goto_state.front();
                                  const auto* nx = tb.find(cur);
                                  s += "\n  -> " + cur + (nx && !nx->mode.empty()
                                                               ? " (mode " + nx->mode + ")"
                                                               : " (no such state!)");
                                  if (!nx) break;
                                  if (nx->goto_state.empty()) break;
                              }
                          }
                          return s;
                      }
                      if (c.args[1] == "-modes") {
                          // The recovered mode table, cited. Replaces the old ad-hoc distinct-name
                          // listing: that told us what the script ASKS FOR, which is half the question.
                          // The other half - what a mode IS - is now answered from the binary
                          // (analysis/modes.md), so the verb reports the table itself.
                          return appmodes::report();
                      }
                      const auto* gs = tb.find(c.args[1]);
                      if (!gs) {
                          // The original's `state` verb takes a name; match case-insensitively the way
                          // its FNV-1 + tolower id computation does.
                          std::string want = c.args[1];
                          for (auto& ch : want) ch = (char)std::tolower((unsigned char)ch);
                          for (const auto& s : tb.states()) {
                              std::string have = s.name;
                              for (auto& ch : have) ch = (char)std::tolower((unsigned char)ch);
                              if (have == want) { gs = &s; break; }
                          }
                      }
                      if (!gs) return "no game state named '" + c.args[1] + "'";
                      std::ostringstream out;
                      out << "state " << gs->name << " id=0x" << std::hex << std::uppercase
                          << gs->id << std::dec << std::nouppercase
                          << (gs->has_explicit_id ? " (explicit)" : " (hashed)") << "\n";
                      if (!gs->description.empty()) out << "  description: \"" << gs->description << "\"\n";
                      if (!gs->mode.empty()) out << "  mode: " << gs->mode << "\n";
                      for (const auto& g : gs->goto_state) out << "  gotoState " << g << "\n";
                      for (const auto& a : gs->entry) out << "  entry: " << a.verb << " " << a.raw << "\n";
                      for (const auto& a : gs->exit) out << "  exit:  " << a.verb << " " << a.raw << "\n";
                      return out.str();
                  }});
    console::reg({"pkg", "Inspect a .package: list, header, records, or dump one resource.",
                  "-list | -hdr <i> | -rec <i> <n> | -path <i> <n> | -types <i> | -unpack <i|all> <dir> [type] | -dump <i> <n> <out> | -decode <i> <n> <out>",
                  [](const console::CmdContext& c) -> std::string {
                      if (c.args.size() < 2) return "usage: pkg -list | -hdr <i> | -rec <i> <n> | -dump <i> <n> <file>";
                      const std::string& sub = c.args[1];
                      // One helper for the whole command, deduplicating by canonical path.
                      // Without the dedupe this verb reported 109 packages while the Loader
                      // reported 57 for the same tree (the dev search list has both
                      // Data=<exe>\Data and cwd=<exe>, so a recursive scan visits every
                      // file twice). Two views of one dataset must not disagree by 2x.
                      auto all_packages = []() {
                          std::vector<std::string> out;
                          std::set<std::string> seen;
                          for (auto& sp : fs::search_paths()) {
                              for (auto& p : pkg::find_packages(sp.dir.string())) {
                                  std::error_code ec;
                                  std::string key = std::filesystem::weakly_canonical(p, ec).string();
                                  if (ec) key = p;
                                  if (seen.insert(key).second) out.push_back(p);
                              }
                          }
                          return out;
                      };
                      auto resolve_idx = [&](int i, std::string& path) -> bool {
                          auto all = all_packages();
                          if (i < 0 || i >= (int)all.size()) return false;
                          path = all[(size_t)i];
                          return true;
                      };
                      if (sub == "-list") {
                          auto all = all_packages();
                          std::string s = "packages (" + std::to_string(all.size()) + "):";
                          for (size_t i = 0; i < all.size(); ++i) s += "\n  [" + std::to_string(i) + "] " + all[i];
                          return s;
                      }
                      if (c.args.size() < 3) return "pkg: " + sub + " needs an index";
                      int i = atoi(c.args[2].c_str());
                      std::string path;
                      if (!resolve_idx(i, path)) return "pkg: index out of range";
                      pkg::Package pk;
                      if (!pk.open(path)) return "pkg: cannot open/validate " + path + ": " + pk.why();
                      if (sub == "-hdr") {
                          const auto& h = pk.header();
                          char b[512];
                          _snprintf_s(b, sizeof(b), _TRUNCATE,
                                      "kind=%u ver=%u count=%u idx_size=%u idx_off=0x%x +0x3c=%u filesize=%llu records=%zu",
                                      h.kind, h.version, h.count, h.index_size, h.index_offset, h.field_3c,
                                      (unsigned long long)h.file_size, pk.records().size());
                          return path + "\n  " + b;
                      }
                      if (sub == "-rec") {
                          if (c.args.size() < 4) return "pkg: -rec needs a record number";
                          u32 n = (u32)atoi(c.args[3].c_str());
                          if (n >= pk.records().size()) return "pkg: record index out of range";
                          const auto& r = pk.records()[n];
                          const char* tn = naming::type_name(r.type());
                          if (!tn) tn = pkg::type_id_name(r.type());
                          const char* in = naming::instance_name(r.instance());
                          char b[512];
                          _snprintf_s(
                              b, sizeof(b), _TRUNCATE,
                              "rec[%u]: type=0x%08x%s%s inst=0x%08x%s%s off=0x%08x packed=%u mem=%u %s flags=0x%04x", n,
                              r.type(), tn ? " (" : "", tn ? tn : "", r.instance(), in ? " (" : "", in ? in : "",
                              r.offset(), r.packed_size(), r.mem_size(), r.refpacked() ? "REFPACK" : "raw", r.flags());
                          return b;
                      }
                      if (sub == "-path") {
                          if (c.args.size() < 4) return "pkg: -path needs a record number";
                          u32 n = (u32)atoi(c.args[3].c_str());
                          if (n >= pk.records().size()) return "pkg: record index out of range";
                          const auto& r = pk.records()[n];
                          return naming::resource_path(r.type(), r.group(), r.instance());
                      }
                      if (sub == "-unpack") {
                          // The standalone-unpacker wish: write resources to disk under their
                          // REAL names, decoded. Uses only reversed machinery (index -> RefPack
                          // -> naming), so this is the whole pipeline proving itself on real data.
                          //   pkg:-unpack,<pkgIndex|all>,<outdir>[,<typeName>]
                          if (c.args.size() < 4) return "pkg: -unpack needs <pkgIndex|all> <outdir> [type]";
                          std::string outdir = c.args[3];
                          std::string only = c.args.size() > 4 ? c.args[4] : std::string();
                          std::error_code uec;
                          std::filesystem::create_directories(outdir, uec);

                          std::vector<std::string> targets;
                          if (c.args[2] == "all") {
                              targets = all_packages();
                          } else {
                              std::string one;
                              if (!resolve_idx(atoi(c.args[2].c_str()), one)) return "pkg: index out of range";
                              targets.push_back(one);
                          }

                          auto sanitize = [](std::string s) {
                              for (char& ch : s) {
                                  if (ch == ':' || ch == '?' || ch == '*' || ch == '"' || ch == '<' || ch == '>' ||
                                      ch == '|')
                                      ch = '_';
                              }
                              return s;
                          };
                          u32 written = 0, skipped = 0, failed = 0, pkgs = 0;
                          u64 bytes = 0;
                          std::vector<u8> buf;
                          for (const auto& target : targets) {
                              pkg::Package up;
                              if (!up.open(target)) {
                                  ++failed;
                                  continue;
                              }
                              ++pkgs;
                              // Namespace by package: the same group/instance recurs across
                              // packages (every locale has its own Text/..., and Spore_Content
                              // repeats base-game names), so a flat tree silently overwrote
                              // itself - measured: 2,944 records produced only 206 files before
                              // this. Use PARENT+stem: 17 locales each own a "Text.package", so
                              // the bare stem still collided; "cs-cz_Text" does not.
                              std::filesystem::path tp(target);
                              std::string pkgstem = tp.parent_path().filename().string() + "_" + tp.stem().string();
                              // Pairing: a model's texture is the `raster` with the SAME instance id
                              // (measured 865/865 in Spore_Content). Build the map once per package.
                              std::map<u32, u32> raster_by_inst;
                              for (u32 ri = 0; ri < up.records().size(); ++ri)
                                  if (up.records()[ri].type() == 0x2F4E681C)
                                      raster_by_inst[up.records()[ri].instance()] = ri;
                              for (u32 n = 0; n < up.records().size(); ++n) {
                                  const auto& r = up.records()[n];
                                  const char* tn = naming::type_name(r.type());
                                  if (!tn) tn = pkg::type_id_name(r.type());
                                  if (!only.empty() && (!tn || only != tn)) {
                                      ++skipped;
                                      continue;
                                  }
                                  std::string rel = naming::resource_path(r.type(), r.group(), r.instance());
                                  if (rel.rfind("0x", 0) == 0) {
                                      // unnamed: keep it flat and unique by record order
                                      rel = "unnamed/" + std::to_string(n) + "_" + rel;
                                  }
                                  std::filesystem::path out =
                                      std::filesystem::path(outdir) / sanitize(pkgstem) / sanitize(rel);
                                  std::filesystem::create_directories(out.parent_path(), uec);
                                  if (!up.read_resource_decoded(n, buf)) {
                                      ++failed;
                                      continue;
                                  }
                                  // Solved formats get converted to standard files: `raster` -> BMP,
                                  // `gmdl` -> OBJ. Everything else is written as its decoded bytes.
                                  constexpr u32 kRaster = 0x2F4E681C, kGmdl = 0x00E6BCE5;
                                  bool wrote = false;
                                  if (r.type() == kRaster) {
                                      raster::Texture rt = raster::decode(buf.data(), buf.size());
                                      if (rt.ok) {
                                          out.replace_extension(".bmp");
                                          wrote = write_bmp_bgra(out.string(), rt.width, rt.height, rt.bgra.data());
                                      }
                                  } else if (r.type() == kGmdl) {
                                      gmdl::Mesh m = gmdl::parse(buf.data(), buf.size());
                                      if (m.ok) {
                                          // Write the paired texture next to the OBJ and reference it.
                                          std::string texfile;
                                          auto it = raster_by_inst.find(r.instance());
                                          if (it != raster_by_inst.end()) {
                                              std::vector<u8> tbuf;
                                              if (up.read_resource_decoded(it->second, tbuf)) {
                                                  raster::Texture rt = raster::decode(tbuf.data(), tbuf.size());
                                                  if (rt.ok) {
                                                      std::filesystem::path bp = out;
                                                      bp.replace_extension(".bmp");
                                                      if (write_bmp_bgra(bp.string(), rt.width, rt.height, rt.bgra.data()))
                                                          texfile = bp.filename().string();
                                                  }
                                              }
                                          }
                                          out.replace_extension(".obj");
                                          wrote = texfile.empty() ? write_obj(out.string(), m)
                                                                  : write_obj_textured(out.string(), m, texfile);
                                      }
                                  }
                                  if (wrote) {
                                      ++written;
                                      bytes += buf.size();
                                      continue;
                                  }
                                  FILE* fo = nullptr;
                                  if (fopen_s(&fo, out.string().c_str(), "wb") != 0 || !fo) {
                                      ++failed;
                                      continue;
                                  }
                                  fwrite(buf.data(), 1, buf.size(), fo);
                                  fclose(fo);
                                  ++written;
                                  bytes += buf.size();
                              }
                          }
                          char b[320];
                          _snprintf_s(b, sizeof(b), _TRUNCATE,
                                      "unpack: %u pkgs, %u written, %u skipped, %u failed, %llu bytes -> %s%s%s",
                                      pkgs, written, skipped, failed, (unsigned long long)bytes, outdir.c_str(),
                                      only.empty() ? "" : " (filter=", only.empty() ? "" : only.c_str());
                          std::string res = b;
                          if (!only.empty()) res += ")";
                          return res;
                      }
                      if (sub == "-types") {
                          // What is actually inside a package, by name. Uses the generated
                          // id->name table, so an unnamed id shows as a bare hex number
                          // rather than a wrong name.
                          if (c.args.size() < 3) return "pkg: -types needs a package index";
                          int ti = atoi(c.args[2].c_str());
                          std::string tpath;
                          if (!resolve_idx(ti, tpath)) return "pkg: index out of range";
                          pkg::Package tp;
                          if (!tp.open(tpath)) return "pkg: cannot open " + tpath;
                          std::map<u32, u32> counts;
                          u64 bytes = 0, refpack = 0;
                          for (const auto& r : tp.records()) {
                              counts[r.type()]++;
                              bytes += r.mem_size();
                              if (r.refpacked()) ++refpack;
                          }
                          // sort by count desc
                          std::vector<std::pair<u32, u32>> v(counts.begin(), counts.end());
                          std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
                          char head[256];
                          _snprintf_s(head, sizeof(head), _TRUNCATE, "%s: %zu records, %llu decoded bytes, %llu refpack",
                                      std::filesystem::path(tpath).filename().string().c_str(), tp.records().size(),
                                      (unsigned long long)bytes, (unsigned long long)refpack);
                          std::string s2 = head;
                          for (size_t k = 0; k < v.size() && k < 24; ++k) {
                              const char* tn2 = naming::type_name(v[k].first);
                              if (!tn2) tn2 = pkg::type_id_name(v[k].first);
                              char line[128];
                              _snprintf_s(line, sizeof(line), _TRUNCATE, "\n  %-20s 0x%08x  x%u",
                                          tn2 ? tn2 : "(unnamed)", v[k].first, v[k].second);
                              s2 += line;
                          }
                          if (v.size() > 24) s2 += "\n  ... " + std::to_string(v.size() - 24) + " more types";
                          return s2;
                      }
                      if (sub == "-dump") {
                          if (c.args.size() < 5) return "pkg: -dump needs <record> <outfile>";
                          u32 n = (u32)atoi(c.args[3].c_str());
                          if (n >= pk.records().size()) return "pkg: record index out of range";
                          std::vector<u8> bytes;
                          if (!pk.read_resource(n, bytes)) return "pkg: read_resource failed for record " + std::to_string(n);
                          FILE* fo = nullptr;
                          if (fopen_s(&fo, c.args[4].c_str(), "wb") != 0 || !fo) return "pkg: cannot write " + c.args[4];
                          fwrite(bytes.data(), 1, bytes.size(), fo);
                          fclose(fo);
                          return "dumped " + std::to_string(bytes.size()) + " packed bytes -> " + c.args[4];
                      }
                      if (sub == "-decode") {
                          // The whole point of the RefPack work: this writes the DECODED
                          // resource, whose size equals the index's memSize.
                          if (c.args.size() < 5) return "pkg: -decode needs <record> <outfile>";
                          u32 n = (u32)atoi(c.args[3].c_str());
                          if (n >= pk.records().size()) return "pkg: record index out of range";
                          const auto& r = pk.records()[n];
                          std::vector<u8> bytes;
                          if (!pk.read_resource_decoded(n, bytes))
                              return "pkg: decode failed for record " + std::to_string(n);
                          FILE* fo = nullptr;
                          if (fopen_s(&fo, c.args[4].c_str(), "wb") != 0 || !fo) return "pkg: cannot write " + c.args[4];
                          fwrite(bytes.data(), 1, bytes.size(), fo);
                          fclose(fo);
                          size_t printable = 0;
                          for (size_t k = 0; k < bytes.size() && k < 256; ++k) {
                              u8 b = bytes[k];
                              if ((b >= 32 && b < 127) || b == 9 || b == 10 || b == 13) ++printable;
                          }
                          char b[256];
                          _snprintf_s(b, sizeof(b), _TRUNCATE,
                                      "decoded %zu bytes (mem=%u packed=%u refpack=%d, %.0f%% printable in first 256) -> %s",
                                      bytes.size(), r.mem_size(), r.packed_size(), (int)r.refpacked(),
                                      bytes.empty() ? 0.0 : 100.0 * printable / (double)std::min<size_t>(bytes.size(), 256),
                                      c.args[4].c_str());
                          return b;
                      }
                      return "pkg: unknown subcommand " + sub;
                  }});
    console::reg({"_abortNow",
                  "Deliberately abort() inside the guarded frame to prove run_guarded recovers.",
                  "-<times:n>",
                  [](const console::CmdContext& c) -> std::string {
                      int times = c.args.size() > 1 ? atoi(c.args[1].c_str()) : 1;
                      if (times < 1) times = 1;
                      // Do NOT abort here: this handler runs in console::poll(), which is
                      // outside run_guarded, so it would test the handler but not the
                      // recovery. Set the flag; frame_body() consumes it inside the guard.
                      request_abort(times);
                      LOG_WARN("fault", "_abortNow: armed %d abort(s) for the next frame", times);
                      return "armed: the next frame will abort inside run_guarded";
                  }});
    mark(7, true, std::to_string(console::count()) + " commands");
    mark(8, true, "Loader registered");

    // 8. goto the requested state (launch key -state <name>; default: Loader)
    std::string want = !g_args.state.empty() ? g_args.state : "Loader";
    bool ok = states::manager().goto_name(want);
    if (!ok && want != "Loader") {
        LOG_WARN("app", "state '%s' not found (no such state), falling back to Loader", want.c_str());
        ok = states::manager().goto_name("Loader");
    }
    mark(9, ok, "current='" + states::manager().current_name() + "'");
    print_chain();

    // 9. stdin console worker (no-op when no console is attached)
    console::start_stdin_thread();
    LOG_ONCE_INFO("frame", "about to enter the frame loop (console=%s, state='%s')",
                  GetConsoleWindow() ? "attached" : "none", states::manager().current_name().c_str());

    // ------------------------------------------------------------ frame loop
    LARGE_INTEGER qpf{}, t0{}, t_prev{};
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&t0);
    t_prev = t0;
    double fps_acc = 0.0;
    u32 fps_frames = 0;
    double fps_display = 0.0;
    LARGE_INTEGER t_fps = t0;
    bool loop_alive_logged = false;
    bool first_iter_logged = false;

    while (!g_quit) {
        pump_messages();
        if (g_quit) break;
        // Latch the pointer ONCE per iteration, outside the guarded frame body: frame_body has early
        // returns (headless, minimized, aborted), and a begin/end pair split across them would leak a
        // click or double-count one. The renderer sees a stable, edge-triggered view either way.
        input::begin_frame();
        console::poll();
        if (!first_iter_logged) {
            first_iter_logged = true;
            LOG_ONCE_INFO("frame", "first iteration reached (minimized=%d)", (int)g_minimized);
        }

        if (g_size_pending) {
            g_size_pending = false;
            if (!g_minimized && render::device().windowed()) {
                render::device().resize(g_new_w, g_new_h);
            }
        }

        LARGE_INTEGER t_now;
        QueryPerformanceCounter(&t_now);
        f32 dt = (f32)((double)(t_now.QuadPart - t_prev.QuadPart) / (double)qpf.QuadPart);
        t_prev = t_now;
        if (dt > 0.25f) dt = 0.25f;  // tab-out guard

        if (g_minimized) {
            Sleep(20);
            continue;
        }

        // Fault tolerance: one bad frame must not take the process down. If anything in
        // the frame calls abort()/assert-with-abort, diag's handler logs the stack and
        // longjmps back, and we skip the rest of that frame.
        FrameCtx fctx{dt};
        const bool frame_aborted = diag::run_guarded(frame_body, &fctx, "frame");
        input::end_frame();
        if (frame_aborted) {
            ++g_frame_aborts;
            LOG_ERROR("frame", "frame aborted (total %llu) - skipping this frame",
                      (unsigned long long)g_frame_aborts);
            Sleep(16);
            continue;
        }

        ++g_frames;
        if (!g_args.capture_path.empty() && g_frames == (u64)g_args.capture_after) {
            // Defer into the scene: GetFrontBufferData is only legal between BeginScene and
            // EndScene, and this point in the loop is outside it. end_frame() performs it.
            render::device().request_capture(g_args.capture_path.c_str());
            LOG_ONCE_INFO("capture", "capture requested for frame %llu -> %s", (unsigned long long)g_frames,
                          g_args.capture_path.c_str());
        }
        if (!g_args.exec_cmds.empty() && g_frames == (u64)g_args.exec_after) {
            // Split on ';' and run each through the real command path, so an unattended
            // test exercises exactly what a human types.
            std::string s = g_args.exec_cmds;
            size_t pos = 0;
            while (pos < s.size()) {
                size_t sc = s.find(';', pos);
                std::string one = (sc == std::string::npos) ? s.substr(pos) : s.substr(pos, sc - pos);
                pos = (sc == std::string::npos) ? s.size() : sc + 1;
                // strip surrounding spaces
                size_t a0 = one.find_first_not_of(" \t");
                if (a0 == std::string::npos) continue;
                size_t a1 = one.find_last_not_of(" \t");
                one = one.substr(a0, a1 - a0 + 1);
                std::string res;
                bool cmd_ok = console::exec_line(one, res);
                LOG_ONCE_INFO("exec", "-exec '%s' -> ok=%d %s", one.c_str(), (int)cmd_ok, res.c_str());
            }
        }
        if (g_args.quit_after_frames > 0 && g_frames >= (u64)g_args.quit_after_frames) {
            LOG_ONCE_INFO("app", "auto-quit after %llu frames (-quitAfter %d)", (unsigned long long)g_frames,
                          g_args.quit_after_frames);
            break;
        }
        fps_acc += dt;
        ++fps_frames;
        if ((double)(t_now.QuadPart - t_fps.QuadPart) / (double)qpf.QuadPart >= 1.0) {
            fps_display = fps_frames / (fps_acc > 0 ? fps_acc : 1.0);
            fps_acc = 0.0;
            fps_frames = 0;
            t_fps = t_now;
            LOG_DEBUG("frame", "fps=%.1f frames=%llu aborts=%llu state='%s'", fps_display,
                      (unsigned long long)g_frames, (unsigned long long)g_frame_aborts,
                      states::manager().current_name().c_str());
        }
        if (!loop_alive_logged && g_frames >= 2) {
            loop_alive_logged = true;
            LOG_INFO("frame", "frame loop alive (2 frames rendered, state='%s')", states::manager().current_name().c_str());
        }
    }

    LOG_INFO("app", "shutting down: frames=%llu aborts=%llu asserts=%u unreachable=%u log_lines=%llu",
             (unsigned long long)g_frames, (unsigned long long)g_frame_aborts, diag::assert_count(),
             diag::unreachable_count(), (unsigned long long)log::lines_written());
    console::shutdown();
    render::overlay().shutdown();
    render::device().shutdown();
    log::dump_ledger();
    LOG_INFO("app", "clean exit code=%d", g_quit_code);
    log::shutdown();
    return g_quit_code;
}

}  // namespace os::app
