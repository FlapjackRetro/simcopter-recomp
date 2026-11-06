/*
 * SimCopter - static recompilation host.
 *
 * Adapted from the Streets of SimCity recomp's host (sp00nznet/streetsofsimcity,
 * MIT): Streets runs on the SimCopter engine, so the CD, display, Smacker and
 * input handling carry over. Game-specific addresses are SimCopter's.
 *
 * A 32-bit host on pcrecomp's runtime/native32 (the native bridge, callbacks
 * and the machine lock; see its header). What is here is only what is
 * specific to this game: the guest image, where it finds its data, the
 * command line, scripted input and the fault report. The display is
 * ddraw.c. docs/architecture.md has the reasoning.
 *
 * Linked at /BASE:0x60000000 (CMakeLists.txt) so 0x00400000 (SimCopter.exe) is
 * free when main() maps the image.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "native32.h"
#include "recomp_trace.h"
#include "ddraw_host.h"
#include "present.h"
#include "oracle.h"

extern const uint32_t sco_entry_va;       /* recomp_dispatch.c */

#define SCO_BASE 0x00400000u

static DWORD g_watchdog_s;
static int   g_headless;
static char  g_game[MAX_PATH];           /* the game folder, with a trailing '\' */
static char  g_guest_exe[MAX_PATH], g_guest_cmdline[MAX_PATH + 3];

#define ARG(n) MEM32(g_esp + 4 + 4 * (n))
#define RET(v, nargs) do { g_eax = (uint32_t)(v); g_esp += 4 + 4 * (nargs); } while (0)
static const char* gstr(uint32_t va) { return va ? (const char*)(uintptr_t)va : "(null)"; }

/* ------------------------------------------------------------- the guest */

/* The guest is SimCopter.exe in the game folder, not this host: its hInstance
 * (icon, strings) is GetModuleHandleA(NULL). */
static void shim_GetModuleHandleA(void) {
    uint32_t name = ARG(0);
    RET(name ? (uint32_t)(uintptr_t)GetModuleHandleA(gstr(name)) : SCO_BASE, 1);
}

static void shim_GetModuleFileNameA(void) {
    uint32_t h = ARG(0), size = ARG(2);
    char* out = (char*)(uintptr_t)ARG(1);
    if (h == 0 || h == SCO_BASE) {
        uint32_t n = (uint32_t)strlen(g_guest_exe);
        if (size) {
            if (n >= size) n = size - 1;
            memcpy(out, g_guest_exe, n);
            out[n] = 0;
        }
        RET(n, 3);
    } else {
        RET(GetModuleFileNameA((HMODULE)(uintptr_t)h, out, size), 3);
    }
}

static void shim_GetCommandLineA(void) { RET((uintptr_t)g_guest_cmdline, 0); }

/* --headless: nothing reaches the screen (REPO_RULES section 13). The game's
 * own message boxes go to stderr. */
static void shim_MessageBoxA(void) {
    fprintf(stderr, "[messagebox] %s: %s\n", gstr(ARG(2)), gstr(ARG(1)));
    RET(IDOK, 4);
}

/* ------------------------------------------------------------ input */

/* --key NAME@MS[+HOLD] and --click X,Y@MS: scripted input, in milliseconds
 * from entry. The game reads keys through window messages (typing a name)
 * and by polling GetKeyState and GetAsyncKeyState (driving), so a scripted
 * key is posted to the focus window and also held for both; a click is
 * posted as a button down and up at a 640x480 position. Headless, the real
 * keyboard is not read at all. */
#define MAX_INPUT 64
static struct { int vk, x, y; DWORD at, hold; } g_in[MAX_INPUT];
static int g_nin;
static volatile LONG g_held[256];
static int vk_of(const char* s) {
    static const struct { const char* n; int vk; } names[] = {
        {"UP", VK_UP}, {"DOWN", VK_DOWN}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT},
        {"SPACE", VK_SPACE}, {"ENTER", VK_RETURN}, {"ESC", VK_ESCAPE}, {"SHIFT", VK_SHIFT},
        {"CTRL", VK_CONTROL}, {"TAB", VK_TAB}, {"LBUTTON", VK_LBUTTON}};
    for (int i = 0; i < (int)(sizeof names / sizeof names[0]); i++)
        if (!_stricmp(s, names[i].n)) return names[i].vk;
    if ((s[0] == 'F' || s[0] == 'f') && atoi(s + 1) >= 1 && atoi(s + 1) <= 12) return VK_F1 + atoi(s + 1) - 1;
    if (s[0] && !s[1]) return toupper((unsigned char)s[0]);
    return (int)strtol(s, NULL, 0);
}
static int parse_key(const char* arg) {
    char name[16];
    unsigned at = 0, hold = 100;
    if (g_nin >= MAX_INPUT || sscanf(arg, "%15[^@]@%u+%u", name, &at, &hold) < 2) return 0;
    g_in[g_nin].vk = vk_of(name) & 0xFF;
    g_in[g_nin].x = -1;
    g_in[g_nin].at = at;
    g_in[g_nin++].hold = hold;
    return 1;
}
static int parse_click(const char* arg) {
    int x, y;
    unsigned at = 0;
    if (g_nin >= MAX_INPUT || sscanf(arg, "%d,%d@%u", &x, &y, &at) != 3) return 0;
    g_in[g_nin].vk = VK_LBUTTON;
    g_in[g_nin].x = x;
    g_in[g_nin].y = y;
    g_in[g_nin].at = at;
    g_in[g_nin++].hold = 80;
    return 1;
}
static DWORD WINAPI input_thread(LPVOID p) {
    int i = (int)(intptr_t)p;
    DWORD t0 = GetTickCount();
    while (!ddh_window() || GetTickCount() - t0 < g_in[i].at) Sleep(5);
    /* A popup window up takes the input: clicks stay in main-window
     * coordinates and are mapped onto it. (The game's own dialogs are drawn
     * on its screen, not windows; this is for a system one.) */
    HWND w = ddh_window(), pop = GetLastActivePopup(w);
    int vk = g_in[i].vk;
    if (g_in[i].x >= 0) {
        int cx = g_in[i].x, cy = g_in[i].y;
        present_unmap(&cx, &cy);            /* the presenter maps it back to X,Y */
        /* To the deepest child window under the point, in its coordinates. */
        POINT pt = { cx, cy };
        if (pop != w) { MapWindowPoints(w, pop, &pt, 1); w = pop; }
        for (HWND c; (c = RealChildWindowFromPoint(w, pt)) && c != w; w = c)
            MapWindowPoints(w, c, &pt, 1);
        LPARAM xy = MAKELPARAM(pt.x, pt.y);
        fprintf(stderr, "[input] click %d,%d at %lu ms -> hwnd %p at %ld,%ld\n", g_in[i].x, g_in[i].y,
                GetTickCount() - t0, (void*)w, pt.x, pt.y);
        PostMessageA(w, WM_MOUSEMOVE, 0, xy);
        Sleep(30);
        InterlockedIncrement(&g_held[VK_LBUTTON]);
        PostMessageA(w, WM_LBUTTONDOWN, MK_LBUTTON, xy);
        Sleep(g_in[i].hold);
        InterlockedDecrement(&g_held[VK_LBUTTON]);
        PostMessageA(w, WM_LBUTTONUP, 0, xy);
        return 0;
    }
    /* Keys go where real ones would: the game thread's focus window, else
     * the popup. */
    GUITHREADINFO gi = { sizeof gi };
    w = GetGUIThreadInfo(GetWindowThreadProcessId(pop, NULL), &gi) && gi.hwndFocus ? gi.hwndFocus : pop;
    fprintf(stderr, "[input] key 0x%02X down at %lu ms -> hwnd %p\n", vk, GetTickCount() - t0, (void*)w);
    InterlockedIncrement(&g_held[vk]);
    PostMessageA(w, WM_KEYDOWN, vk, 1);
    Sleep(g_in[i].hold);
    InterlockedDecrement(&g_held[vk]);
    PostMessageA(w, WM_KEYUP, vk, 0xC0000001u);
    return 0;
}
static void shim_GetKeyState(void) {
    int vk = ARG(0) & 0xFF;
    SHORT s = g_held[vk] ? (SHORT)0x8000 : g_headless ? 0 : GetKeyState(vk);
    RET((uint16_t)s, 1);
}
/* Windowed, a real key counts only while the game's window has the focus:
 * GetAsyncKeyState sees the whole desktop's keyboard. */
static void shim_GetAsyncKeyState(void) {
    int vk = ARG(0) & 0xFF;
    SHORT s = g_held[vk] ? (SHORT)0x8000 : g_headless || !ddh_focused() ? 0 : GetAsyncKeyState(vk);
    RET((uint16_t)s, 1);
}

/* ------------------------------------------------------------ the CD */

/* The game wants its CD. sub_00435F20 walks GetLogicalDriveStringsA, takes
 * each drive GetDriveTypeA calls DRIVE_CDROM, appends "SimCopter\" and asks
 * sub_00436260 whether that is the disc: the drive must have no free space
 * (sub_00464BE0, GetDiskFreeSpaceA) and hold the game's files under
 * SimCopter\. Without one it asks "Please insert the SimCopter CD-ROM".
 *
 * The game folder is game\SIMCOPTER\, as on the disc, so its parent, game\,
 * is offered as the CD: a free drive letter mapped onto it for this logon
 * session (DefineDosDevice, what `subst` does, no admin needed), which
 * GetDriveTypeA reports as a CD-ROM. A letter a crashed run left mapped onto
 * the same folder is reused. docs/bringup.md 5. The game folder holds the disc's data,
 * so it is offered as that CD: a free drive letter mapped onto it for this
 * logon session (DefineDosDevice, what `subst` does, no admin needed), which
 * GetDriveTypeA reports as a CD-ROM. A letter a crashed run left mapped onto
 * the same folder is reused. docs/architecture.md. */
static char g_cd[4];                     /* "X:\", or "" when no letter was free */
static char g_cd_target[MAX_PATH + 8];   /* what it maps to, for the removal */

static void cd_unmount(void) {
    char dev[3] = { g_cd[0], ':', 0 };
    if (g_cd[0] && DefineDosDeviceA(DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE | DDD_RAW_TARGET_PATH,
                                    dev, g_cd_target))
        g_cd[0] = 0;
}

static void cd_mount(void) {
    char folder[MAX_PATH], raw[MAX_PATH + 8], have[MAX_PATH + 8];
    strcpy(folder, g_game);
    folder[strlen(folder) - 1] = 0;                   /* no trailing '\' */
    char* up = strrchr(folder, '\\');                /* the CD is the game folder's parent */
    if (up && up > folder + 2) *up = 0;
    _snprintf(raw, sizeof raw - 1, "\\??\\%s", folder);
    raw[sizeof raw - 1] = 0;
    for (char l = 'Z'; l >= 'D'; l--) {
        char dev[3] = { l, ':', 0 };
        int mine = QueryDosDeviceA(dev, have, sizeof have) && !_stricmp(have, raw);
        if (!mine && (QueryDosDeviceA(dev, have, sizeof have) || GetLastError() != ERROR_FILE_NOT_FOUND)) continue;
        if (mine || DefineDosDeviceA(DDD_RAW_TARGET_PATH, dev, raw)) {
            strcpy(g_cd_target, raw);
            g_cd[0] = l; g_cd[1] = ':'; g_cd[2] = '\\';
            fprintf(stderr, "[cd] %s is %s, as the SimCopter CD\n", g_cd, folder);
            return;
        }
    }
    fprintf(stderr, "[cd] no free drive letter: the game will ask for its CD\n");
}

static void shim_GetDriveTypeA(void) {
    const char* root = (const char*)(uintptr_t)ARG(0);
    if (g_cd[0] && root && toupper((unsigned char)root[0]) == g_cd[0] && root[1] == ':') {
        RET(DRIVE_CDROM, 1);
        return;
    }
    RET(GetDriveTypeA(root), 1);
}

/* ...and then wants the drive full, as a pressed disc is (sub_00464BE0 returns
 * the free bytes, and anything but 0 is not the disc):
 * no free clusters, the 2048-byte sectors of a CD, about 500 MB in all. */
static void shim_GetDiskFreeSpaceA(void) {
    const char* root = (const char*)(uintptr_t)ARG(0);
    DWORD* out[4] = { (DWORD*)(uintptr_t)ARG(1), (DWORD*)(uintptr_t)ARG(2),
                      (DWORD*)(uintptr_t)ARG(3), (DWORD*)(uintptr_t)ARG(4) };
    if (g_cd[0] && root && toupper((unsigned char)root[0]) == g_cd[0] && root[1] == ':') {
        DWORD v[4] = { 1, 2048, 0, 255500 };   /* sectors/cluster, bytes/sector, free, total */
        for (int i = 0; i < 4; i++) if (out[i]) *out[i] = v[i];
        RET(TRUE, 5);
        return;
    }
    RET(GetDiskFreeSpaceA(root, out[0], out[1], out[2], out[3]), 5);
}

/* ------------------------------------------------------------ the file dialog */

/* The game asks for a SimCity 2000 city with GetOpenFileNameA (*.sc2);
 * headless, that dialog is cloaked and waits forever. `--open FILE`
 * answers the next one with FILE, without showing it (as SimCity 2000's
 * recomp answers its own). One-shot: later dialogs are shown. */
static char g_open[MAX_PATH];

static void shim_GetOpenFileNameA(void) {
    OPENFILENAMEA* o = (OPENFILENAMEA*)(uintptr_t)ARG(0);
    if (!g_open[0]) { RET(GetOpenFileNameA(o), 1); return; }
    const char *name = strrchr(g_open, '\\'), *dot = strrchr(g_open, '.');
    lstrcpynA(o->lpstrFile, g_open, o->nMaxFile);
    o->nFileOffset = (WORD)(name ? name - g_open + 1 : 0);
    o->nFileExtension = (WORD)(dot && dot > name ? dot - g_open + 1 : 0);
    if (o->lpstrFileTitle) lstrcpynA(o->lpstrFileTitle, g_open + o->nFileOffset, o->nMaxFileTitle);
    fprintf(stderr, "[open] answered with %s\n", g_open);
    g_open[0] = 0;
    RET(TRUE, 1);
}

/* The game's window is 640x480 to the game. The presenter draws that picture
 * scaled into the real window, which is whatever size the player made it, but
 * the game centres things in its own client area: before each video the
 * Smacker player (sub_0044A030) takes GetClientRect of the window and centres
 * the movie in it, then SmackBufferBlit draws it there, in 640x480
 * coordinates on the host's screen. Given the real 1600-odd-pixel client area,
 * the intro landed down and right, mostly off the picture. Every other window
 * is answered as it is. */
static void shim_GetClientRect(void) {
    HWND h = (HWND)(uintptr_t)ARG(0);
    RECT* r = (RECT*)(uintptr_t)ARG(1);
    if (h && h == ddh_window() && r) {
        r->left = r->top = 0;
        r->right = 640;
        r->bottom = 480;
        RET(TRUE, 2);
        return;
    }
    RET(GetClientRect(h, r), 2);
}

/* ...and the game sizes and places that window itself, for a 640x480 desktop.
 * Its fullscreen setup (sub_004212D0) makes it a WS_POPUP and SetWindowPos's
 * it to 640x480; its windowed one (sub_004211D0, sub_004213A0) gives it a
 * caption and MoveWindow's it to the middle of GetSystemMetrics' screen.
 * Done to the real window, that shrank it to 640x480 and took it from the
 * presenter. So, for the game's window only, the world is a 640x480 screen
 * with the window filling it at 0,0: moves, resizes and style changes are
 * accepted and not done, its rectangle is 0,0-640,480 and client coordinates
 * are screen coordinates. (The presenter maps the mouse back to 640x480.) */
static int game_wnd(uint32_t h) { return h && (HWND)(uintptr_t)h == ddh_window(); }

static void shim_SetWindowPos(void) {
    if (game_wnd(ARG(0))) { RET(TRUE, 7); return; }
    RET(SetWindowPos((HWND)(uintptr_t)ARG(0), (HWND)(uintptr_t)ARG(1), (int)ARG(2), (int)ARG(3),
                     (int)ARG(4), (int)ARG(5), ARG(6)), 7);
}

static void shim_MoveWindow(void) {
    if (game_wnd(ARG(0))) { RET(TRUE, 6); return; }
    RET(MoveWindow((HWND)(uintptr_t)ARG(0), (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4), ARG(5)), 6);
}

/* Style changes only: the game (and the host's DirectDraw) subclass the
 * window through GWL_WNDPROC, which must still go through. */
static void shim_SetWindowLongA(void) {
    int idx = (int)ARG(1);
    if (game_wnd(ARG(0)) && (idx == GWL_STYLE || idx == GWL_EXSTYLE)) {
        RET(GetWindowLongA((HWND)(uintptr_t)ARG(0), idx), 3);
        return;
    }
    RET(SetWindowLongA((HWND)(uintptr_t)ARG(0), idx, (LONG)ARG(2)), 3);
}

static void shim_GetWindowRect(void) {
    RECT* r = (RECT*)(uintptr_t)ARG(1);
    if (game_wnd(ARG(0)) && r) {
        SetRect(r, 0, 0, 640, 480);
        RET(TRUE, 2);
        return;
    }
    RET(GetWindowRect((HWND)(uintptr_t)ARG(0), r), 2);
}

static void shim_ClientToScreen(void) {
    if (game_wnd(ARG(0))) { RET(TRUE, 2); return; }   /* the point is unchanged */
    RET(ClientToScreen((HWND)(uintptr_t)ARG(0), (POINT*)(uintptr_t)ARG(1)), 2);
}

static void shim_GetSystemMetrics(void) {
    int i = (int)ARG(0);
    if (i == SM_CXSCREEN || i == SM_CXFULLSCREEN) { RET(640, 1); return; }
    if (i == SM_CYSCREEN || i == SM_CYFULLSCREEN) { RET(480, 1); return; }
    RET(GetSystemMetrics(i), 1);
}

/* The frame pace. The game simulates once per pass of its main loop
 * (sub_004308B0: PeekMessageA until the queue is empty, then the frame), and
 * its clock is timeGetTime. On a 1997 machine that was 15-30 passes a second;
 * here it is thousands, and the helicopter, the traffic and the camera all
 * advance per pass: the game overspeeds, and the camera jitters on a clock
 * that moves in 1-16 ms steps under sub-millisecond frames. Flight sims of the
 * era were fixed the same way: hold the loop to a steady rate. So the
 * PeekMessageA that finds the queue empty, the end of one pass, waits for the
 * next tick of --fps (default 30; 0: unpaced, as shipped). Messages still go
 * through at once. Also the guest's timer resolution is 1 ms
 * (timeBeginPeriod). docs/bringup.md 9. */
static int g_fps = 30;
static void pace(void) {
    static LARGE_INTEGER freq, next;
    LARGE_INTEGER now;
    if (g_fps <= 0) return;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LONGLONG step = freq.QuadPart / g_fps;
    QueryPerformanceCounter(&now);
    if (!next.QuadPart || now.QuadPart - next.QuadPart > 4 * step) next = now;   /* a stall: start over */
    while (now.QuadPart < next.QuadPart) {
        LONGLONG ms = (next.QuadPart - now.QuadPart) * 1000 / freq.QuadPart;
        Sleep(ms > 1 ? (DWORD)(ms - 1) : 0);
        QueryPerformanceCounter(&now);
    }
    next.QuadPart += step;
}

static void shim_PeekMessageA(void) {
    MSG* m = (MSG*)(uintptr_t)ARG(0);
    BOOL r;
    mach_leave();                        /* the window procedure may run */
    r = PeekMessageA(m, (HWND)(uintptr_t)ARG(1), ARG(2), ARG(3), ARG(4));
    if (!r) pace();
    mach_enter();
    RET(r, 5);
}

/* After AddFontResourceA the game tells every window with
 * SendMessageA(HWND_BROADCAST, WM_FONTCHANGE) (Streets does; kept in case
 * SimCopter does too), which waits on
 * every top-level window of the desktop: one hung window anywhere (a suspended
 * Settings app did it) and the game never starts. A broadcast skips hung
 * windows and gives each a second; anything else goes straight through. */
static void shim_SendMessageA(void) {
    HWND h = (HWND)(uintptr_t)ARG(0);
    if (h == HWND_BROADCAST) {
        DWORD_PTR r = 0;
        SendMessageTimeoutA(h, ARG(1), ARG(2), ARG(3), SMTO_ABORTIFHUNG, 1000, &r);
        RET((uint32_t)r, 4);
        return;
    }
    RET((uint32_t)SendMessageA(h, ARG(1), ARG(2), ARG(3)), 4);
}

/* Set by a rasterizer edge routine that returns to its caller's caller
 * (run_lift.py, "returns to the caller's caller"); the call site returns too. */
uint32_t sco_unwind;
/* For guarded-divide patches (run_lift.py PATCHES), once one is found. */
uint32_t sco_div_ovf;

/* ------------------------------------------------------------ Glide */

/* SimCopter imports glide.dll (Glide 2.x, 28 functions) statically and probes
 * for a 3dfx board at start with grGlideInit + grSstQueryHardware. With no
 * Voodoo there is no glide.dll: these answer "no hardware", so it should pick
 * its software renderer, and the other 26 imports stay unbound (they are only
 * called once a board was found). Same idea as Streets' grSstQueryBoards. */
static void shim_grSstQueryHardware(void) {
    uint32_t hw = ARG(0);                /* GrHwConfiguration*: num_sst first */
    if (hw) MEM32(hw) = 0;
    RET(0, 1);                           /* FXFALSE: no 3dfx hardware */
}
static void shim_grGlideInit(void) { RET(0, 0); }
static void shim_grGlideShutdown(void) { RET(0, 0); }

/* ------------------------------------------------------------ exit */

static void shim_ExitProcess(void) {
    fprintf(stderr, "[exit] ExitProcess(%u) after %ld frames\n", ARG(0), ddh_frames());
    ddh_finish();
    cd_unmount();
    fflush(stderr);
    ExitProcess(ARG(0));
}

static native32_shim_t g_shims[] = {
    { "GetModuleHandleA", shim_GetModuleHandleA },
    { "GetModuleFileNameA", shim_GetModuleFileNameA },
    { "GetCommandLineA", shim_GetCommandLineA },
    { "GetKeyState", shim_GetKeyState },
    { "GetAsyncKeyState", shim_GetAsyncKeyState },
    { "ExitProcess", shim_ExitProcess },
    { "GetDriveTypeA", shim_GetDriveTypeA },
    { "GetDiskFreeSpaceA", shim_GetDiskFreeSpaceA },
    { "GetOpenFileNameA", shim_GetOpenFileNameA },
    { "SendMessageA", shim_SendMessageA },
    { "GetClientRect", shim_GetClientRect },
    { "SetWindowPos", shim_SetWindowPos },
    { "MoveWindow", shim_MoveWindow },
    { "SetWindowLongA", shim_SetWindowLongA },
    { "GetWindowRect", shim_GetWindowRect },
    { "ClientToScreen", shim_ClientToScreen },
    { "GetSystemMetrics", shim_GetSystemMetrics },
    { "PeekMessageA", shim_PeekMessageA },
    { "_grSstQueryHardware@4", shim_grSstQueryHardware },
    { "_grGlideInit@0", shim_grGlideInit },
    { "_grGlideShutdown@0", shim_grGlideShutdown },
    DDH_SHIMS,
    { "MessageBoxA", shim_MessageBoxA },          /* headless only: the last entry */
};

/* ------------------------------------------------------------ reports */

recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }

/* Added after native32's own handler, so callbacks are resolved first and
 * only real faults get here. */
static LONG CALLBACK crash(EXCEPTION_POINTERS* ep) {
    EXCEPTION_RECORD* r = ep->ExceptionRecord;
    if ((r->ExceptionCode & 0xF0000000u) != 0xC0000000u) return EXCEPTION_CONTINUE_SEARCH;
    fprintf(stderr, "\n=== fault 0x%08lX at 0x%p ===\n", r->ExceptionCode, r->ExceptionAddress);
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
        ULONG_PTR op = r->ExceptionInformation[0];
        uint32_t at = (uint32_t)r->ExceptionInformation[1];
        fprintf(stderr, "  %s of 0x%08X%s\n", op == 0 ? "read" : op == 1 ? "write" : "execute", at,
                native32_in_guest(at) ? " (inside the guest image)" : at < 0x10000 ? " (null/low)" : "");
    }
    recomp_trace_flush();
    fprintf(stderr, "  in lifted sub_%08X, last native call %s\n", g_cur_func, g_cur_import);
    fprintf(stderr, "  eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X\n",
            g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi);
    native32_dump_icalls(12);
    recomp_dump_trace("fault");
    fflush(stderr);
    cd_unmount();
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* --original: guarded divides. Streets of SimCity divides inside C++
 * try/catch(...) and relies on the CPU's divide fault reaching the catch;
 * under --original Windows will not run a handler outside a loaded image, so
 * Streets' host emulates its catch here. SimCopter's engine may do the same:
 * when one is found, handle it here by its eip (and in run_lift.py PATCHES).
 * Until then, a divide fault is reported and ends the run. */
static LONG CALLBACK guarded_divide(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == STATUS_INTEGER_OVERFLOW || code == STATUS_INTEGER_DIVIDE_BY_ZERO)
        fprintf(stderr, "[divide] fault at eip %08lX: a guarded divide? (docs/bringup.md)\n",
                ep->ContextRecord->Eip);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Where every other thread is: its eip and the host return addresses on its
 * stack, newest first. `py -3 tools/addr2line.py` names them. */
static void dump_threads(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = { sizeof te };
    uintptr_t lo = (uintptr_t)GetModuleHandleA(NULL), hi = lo + 0x4000000;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
        CONTEXT c = { 0 };
        c.ContextFlags = CONTEXT_CONTROL;
        if (t && SuspendThread(t) != (DWORD)-1) {
            if (GetThreadContext(t, &c)) {
                fprintf(stderr, "  thread %lu: eip=%08lX esp=%08lX host frames:", te.th32ThreadID, c.Eip, c.Esp);
                MEMORY_BASIC_INFORMATION mbi;
                int n = 0;
                if (VirtualQuery((void*)(uintptr_t)c.Esp, &mbi, sizeof mbi))
                    for (uint32_t* p = (uint32_t*)(uintptr_t)c.Esp;
                         (uint8_t*)p + 4 <= (uint8_t*)mbi.BaseAddress + mbi.RegionSize && n < 16; p++)
                        if (*p >= lo && *p < hi) { fprintf(stderr, " %08X", *p); n++; }
                fprintf(stderr, "\n");
            }
            ResumeThread(t);
        }
        if (t) CloseHandle(t);
    }
    CloseHandle(snap);
}

static DWORD WINAPI watchdog(LPVOID unused) {
    (void)unused;
    Sleep(g_watchdog_s * 1000);
    dump_threads();
    fprintf(stderr, "\n[watchdog] %lu s: in sub_%08X, last native call %s, %u indirect calls, %ld frames\n",
            g_watchdog_s, g_cur_func, g_cur_import, g_icall_count, ddh_frames());
    native32_dump_icalls(8);
    recomp_dump_trace("watchdog");          /* with -DSCO_TRACE=ON: the last function entries */
    ddh_finish();
    cd_unmount();
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 4);
    return 0;
}

int main(int argc, char** argv) {
    const char* game = "game\\SIMCOPTER";
    int run = 0, scale = 0, original = 0, nlift = 0;
    static uint32_t lift[256];
    ddh_opts_t o = { 0 };
    for (int i = 1; i < argc; i++) {
        int n = recomp_trace_arg(argc, argv, i);
        if (n) { i += n - 1; continue; }
        if (!strcmp(argv[i], "--run")) run = 1;
        else if (!strcmp(argv[i], "--headless")) g_headless = 1;
        else if (!strcmp(argv[i], "--key") && i + 1 < argc) {
            if (!parse_key(argv[++i])) { fprintf(stderr, "bad --key %s\n", argv[i]); return 1; }
        }
        else if (!strcmp(argv[i], "--click") && i + 1 < argc) {
            if (!parse_click(argv[++i])) { fprintf(stderr, "bad --click %s (want X,Y@MS)\n", argv[i]); return 1; }
        }
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) {
            /* Absolute now: --run moves into the game folder before ffmpeg starts. */
            static char rec[MAX_PATH];
            GetFullPathNameA(argv[++i], MAX_PATH, rec, NULL);
            o.record = rec;
        }
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) o.stop_after = atol(argv[++i]);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) {
            static char shot[MAX_PATH];
            char* colon = strchr(argv[++i], ':');
            o.shot_frame = atol(argv[i]);
            if (!colon || o.shot_frame < 1) { fprintf(stderr, "bad --shot %s (want N:file.bmp)\n", argv[i]); return 1; }
            GetFullPathNameA(colon + 1, MAX_PATH, shot, NULL);
            o.shot_path = shot;
        }
        else if (!strcmp(argv[i], "--diff") && i + 1 < argc) {
            if (sscanf(argv[++i], "%ld,%ld", &o.diff_a, &o.diff_b) != 2 || o.diff_b <= o.diff_a) {
                fprintf(stderr, "bad --diff %s (want A,B in ms, A < B)\n", argv[i]);
                return 1;
            }
        }
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc) g_fps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
        else if (!strcmp(argv[i], "--open") && i + 1 < argc) GetFullPathNameA(argv[++i], MAX_PATH, g_open, NULL);
        else if (!strcmp(argv[i], "--watchdog") && i + 1 < argc) g_watchdog_s = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--native-trace")) native32_trace_native = 1;
        else if (!strcmp(argv[i], "--callbacks")) native32_trace_callbacks = 1;
        else if (!strcmp(argv[i], "--ddraw-trace")) o.trace = 1;
        else if (!strcmp(argv[i], "--original")) original = 1;
        else if (!strcmp(argv[i], "--lifted") && i + 1 < argc)      /* with --original */
            for (char* t = strtok(argv[++i], ","); t && nlift < 256; t = strtok(NULL, ","))
                lift[nlift++] = strtoul(t, NULL, 16);
        else {
            printf("usage: sco [--run] [--headless] [--record out.mp4] [--frames N]\n"
                   "             [--shot N:file.bmp] [--diff A,B] [--key NAME@MS[+HOLD]] [--click X,Y@MS]\n"
                   "             [--scale N] [--fps N] [--game game] [--watchdog S] [--native-trace]\n"
                   "             [--callbacks] [--ddraw-trace] [--original [--lifted VA,...]]\n");
            recomp_trace_help();
            return argv[i][1] == 'h' || argv[i][2] == 'h' ? 0 : 1;
        }
    }
    GetFullPathNameA(game, MAX_PATH - 1, g_game, NULL);
    if (g_game[strlen(g_game) - 1] != '\\') strcat(g_game, "\\");
    _snprintf(g_guest_exe, sizeof g_guest_exe - 1, "%sSimCopter.exe", g_game);
    _snprintf(g_guest_cmdline, sizeof g_guest_cmdline - 1, "\"%s\"", g_guest_exe);

    timeBeginPeriod(1);                  /* the guest's timeGetTime, and pace()'s Sleep */
    if (g_fps > 0) printf("[pace] main loop held to %d fps (--fps N; 0 = unpaced)\n", g_fps);
    o.headless = g_headless;
    o.scale = scale;
    ddh_init(&o);
    native32_init();
    SetDllDirectoryA(g_game);            /* smackw32.dll sits in the game folder */
    printf("SimCopter recomp host\n  lifted functions in dispatch: %u\n", recomp_dispatch_count);
    int nshims = (int)(sizeof g_shims / sizeof g_shims[0]) - !g_headless;
    LoadLibraryA("smackw32.dll");        /* now, so its imports can be pointed at the screen */
    ddh_hook_module("smackw32.dll");
    if (original && run) {
        /* The shipping machine code under the same host, shims and input
         * (oracle.c): the reference a lifted run is compared against. */
        if (!SetCurrentDirectoryA(g_game)) { fprintf(stderr, "cannot enter %s\n", g_game); return 1; }
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);   /* a crash ends it, no dialog */
        AddVectoredExceptionHandler(1, guarded_divide);
        cd_mount();
        if (g_watchdog_s) CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
        for (int i = 0; i < g_nin; i++)
            CloseHandle(CreateThread(NULL, 0, input_thread, (LPVOID)(intptr_t)i, 0, NULL));
        oracle_lift(lift, nlift);
        return oracle_run(g_guest_exe, SCO_BASE, g_shims, nshims, NULL, 0);
    }
    /* Lifted runs only: the original handles its own exceptions with SEH,
     * and this would end it at the first. */
    AddVectoredExceptionHandler(0, crash);
    uint32_t span = native32_map(g_guest_exe, SCO_BASE);
    if (!span) { fprintf(stderr, "cannot map %s at 0x%08X\n", g_guest_exe, SCO_BASE); return 1; }
    printf("  mapped SimCopter.exe: 0x%08X-0x%08X\n", SCO_BASE, SCO_BASE + span);
    /* SimCopter.exe imports glide.dll statically, but calls it only when the
     * 3dfx renderer is picked; with no Voodoo there is no glide.dll, so its
     * other 25 slots stay 0 (the three it calls at start are shimmed: "Glide"
     * above) and the software renderer runs. */
    int unresolved = native32_bind(SCO_BASE, g_shims, nshims);
    if (unresolved && !GetModuleHandleA("glide.dll") && unresolved == 25)
        fprintf(stderr, "[bind] no glide.dll: the 3dfx renderer is unavailable\n");
    else if (unresolved) {
        fprintf(stderr, "[bind] %d imports unresolved (expected 25 Glide ones without glide.dll)\n", unresolved);
        return 1;
    }
    if (!run) {
        printf("\n(dry run: image mapped and bound; --run enters 0x%08X)\n", sco_entry_va);
        return 0;
    }
    /* The game opens its data by relative path (bmp\, smk\, geo\), from the current folder. */
    if (!SetCurrentDirectoryA(g_game)) { fprintf(stderr, "cannot enter %s\n", g_game); return 1; }
    cd_mount();
    if (g_watchdog_s) CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
    for (int i = 0; i < g_nin; i++)
        CloseHandle(CreateThread(NULL, 0, input_thread, (LPVOID)(intptr_t)i, 0, NULL));
    printf("  entering 0x%08X\n\n", sco_entry_va);
    fflush(stdout);
    native32_call_guest(sco_entry_va, 0, NULL);
    printf("\nentry returned eax=%08X\n", g_eax);
    ddh_finish();
    cd_unmount();
    return (int)g_eax;
}
