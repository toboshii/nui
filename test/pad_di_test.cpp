// pad_di_test.cpp - PadHook's DirectInput source (revision 4) in a real process, against a stand-in client with the
// client's own read bytes. This exe is module A (revision 4).
//
//   pad_di_test.exe dinput    the stand-in client (game_di/FFXiMain.dll, pad_game.cpp with -DGAME_DI): both read sites
//                             byte for byte around a stand-in pad device in this exe. Waiting before the client loads;
//                             the client's own results before the patch; the join and the patch through the real PE
//                             sections and VirtualProtect; nobody driving; the device's maker; the press rules on both
//                             reads; the chord and a tap given back; a revision 5 module (pad_mod_r5.dll) taking the
//                             DirectInput dispatch and handing it back from its DllMain; a revision 2 module (frozen
//                             header) that never touches it but whose hold counts; 200 ticks balanced
//   pad_di_test.exe nomatch   a stand-in whose event read the signature does not match: refused with the reason, the
//                             bytes untouched, and the join still made through the XInput wrapper
#include "../src/PadHook/PadHook.h"

#include <cstdio>
#include <cstring>

namespace {

int g_pass = 0, g_fail = 0;
void check(char const* name, bool ok, char const* detail = "") {
    if (ok) ++g_pass;
    else { ++g_fail; std::printf("FAIL %s %s\n", name, detail); }
}

bool has(char const* text, char const* part) { return text && std::strstr(text, part) != nullptr; }

template <typename F>
F fn_of(HMODULE m, char const* name) { return reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(m, name))); }

typedef void(__cdecl* di_fn)(void*);
typedef void*(__cdecl* sites_fn)(int);
typedef volatile DWORD*(__cdecl* rec_fn)();
typedef void(__cdecl* onoff_fn)(int);
typedef int(__cdecl* join_fn)();
typedef void(__cdecl* cap_fn)(int);
typedef void*(__cdecl* ptr_fn)();

// ---- the stand-in pad device ----

struct FakeDev { void* const* vtbl; };
FakeDev g_dev {nullptr};
unsigned char g_js[PADHOOK_JS_BYTES];
PadDiEvent g_events[32];
DWORD g_nev = 0;
LONG g_hr_state = 0, g_hr_data = 0;
int g_state_calls = 0, g_data_calls = 0, g_polls = 0;
bool g_args_ok = true;

LONG __stdcall dev_state(FakeDev* self, DWORD cb, void* buf) {
    ++g_state_calls;
    g_args_ok = g_args_ok && self == &g_dev && cb == PADHOOK_JS_BYTES && buf;
    std::memcpy(buf, g_js, PADHOOK_JS_BYTES);
    return g_hr_state;
}
LONG __stdcall dev_data(FakeDev* self, DWORD cb, PadDiEvent* ev, DWORD* inout, DWORD flags) {
    ++g_data_calls;
    g_args_ok = g_args_ok && self == &g_dev && cb == 0x14 && flags == 0 && inout && *inout == 16;
    DWORD const n = g_nev < *inout ? g_nev : *inout;
    std::memcpy(ev, g_events, n * sizeof(PadDiEvent));
    *inout = n;
    return g_hr_data;
}
LONG __stdcall dev_poll(FakeDev*) { ++g_polls; return 0; }
LONG __stdcall dev_info(FakeDev*, unsigned char* inst) {
    DWORD size;
    std::memcpy(&size, inst, 4);
    if (size != 580) return static_cast<LONG>(0x80070057);
    DWORD const vidpid = 0x09CC054C;
    std::memcpy(inst + 20, &vidpid, 4);
    unsigned char const tail[12] = {0, 0, 0, 0, 0, 0, 'P', 'I', 'D', 'V', 'I', 'D'};
    std::memcpy(inst + 24, tail, 12);
    std::strcpy(reinterpret_cast<char*>(inst + 300), "Wireless Controller");
    return 0;
}
LONG __stdcall dev_other(FakeDev*) { return 0; }   // the method the no-match stand-in calls instead
void* g_vtbl[32] = {};

alignas(16) unsigned char g_input[0xDC4];
unsigned char* in_state() { return g_input + 0xA44; }
PadDiEvent* in_events() { return reinterpret_cast<PadDiEvent*>(g_input + 0xB54); }
DWORD in_count() { DWORD n; std::memcpy(&n, g_input + 0xC94, 4); return n; }

constexpr unsigned CROSS = 1, CIRCLE = 2, L1 = 4, L3 = 10, R3 = 11;

void set_pad(unsigned long down, unsigned long pressed, unsigned long released = 0, LONG lx = 0, DWORD pov = PADHOOK_POV_CENTRED) {
    std::memset(g_js, 0, sizeof(g_js));
    std::memcpy(g_js, &lx, 4);
    std::memcpy(g_js + PADHOOK_JS_POV, &pov, 4);
    for (unsigned b = 0; b < 32; ++b) if (down & (1ul << b)) g_js[PADHOOK_JS_BUTTONS + b] = 0x80;
    g_nev = 0;
    for (unsigned b = 0; b < 32; ++b) {
        if (released & (1ul << b)) g_events[g_nev++] = PadDiEvent {0x30 + b, 0, GetTickCount(), g_nev, 0};
        if (pressed & (1ul << b)) g_events[g_nev++] = PadDiEvent {0x30 + b, 0x80, GetTickCount(), g_nev, 0};
    }
}

// what the game got from one tick
struct Got {
    bool balanced;
    DWORD flags, eax, flag, failed;
    unsigned long held, press, release;
    LONG lx;
    DWORD pov, n;
};

di_fn g_update = nullptr;
rec_fn g_rec = nullptr;

}  // namespace

// Call the stand-in (cdecl, one argument) as the client calls its update. *delta = esp after less esp before, *regs = 1
// when ebx, esi, edi and ebp came back.
extern "C" {
DWORD g_sp = 0, g_frame = 0;
void run_checked(void const* fn, void* input, int* delta, int* regs);
}
asm(R"(
    .text
    .globl _run_checked
_run_checked:
    pushl %ebp
    movl %esp, %ebp
    pushl %ebx
    pushl %esi
    pushl %edi
    movl %ebp, _g_frame
    movl 12(%ebp), %eax
    movl 8(%ebp), %ecx
    movl $0x0B0B0B0B, %ebx
    movl $0x05050505, %esi
    movl $0x0D0D0D0D, %edi
    movl $0x0E0E0E0E, %ebp
    movl %esp, _g_sp
    pushl %eax
    call *%ecx
    addl $4, %esp
    movl %esp, %ecx
    subl _g_sp, %ecx
    xorl %eax, %eax
    cmpl $0x0B0B0B0B, %ebx
    jne 1f
    cmpl $0x05050505, %esi
    jne 1f
    cmpl $0x0D0D0D0D, %edi
    jne 1f
    cmpl $0x0E0E0E0E, %ebp
    jne 1f
    movl $1, %eax
1:  movl _g_sp, %esp
    movl _g_frame, %ebp
    movl 16(%ebp), %edx
    movl %ecx, (%edx)
    movl 20(%ebp), %edx
    movl %eax, (%edx)
    popl %edi
    popl %esi
    popl %ebx
    popl %ebp
    ret
)");

namespace {

Got tick() {
    volatile DWORD* r = g_rec();
    for (int i = 0; i < 6; ++i) r[i] = 0;
    int delta = -1, regs = 0;
    run_checked(reinterpret_cast<void const*>(g_update), g_input, &delta, &regs);
    Got g {};
    g.balanced = delta == 0 && regs == 1;
    g.flags = r[0] & 0x8D5;
    g.eax = r[1];
    g.flag = r[2];
    g.failed = r[3];
    for (unsigned b = 0; b < 32; ++b) if (in_state()[0x30 + b] & 0x80) g.held |= 1ul << b;
    std::memcpy(&g.lx, in_state(), 4);
    std::memcpy(&g.pov, in_state() + PADHOOK_JS_POV, 4);
    g.n = in_count();
    for (DWORD j = 0; j < g.n && j < 32; ++j) {
        PadDiEvent const& e = in_events()[j];
        if (e.ofs < 0x30 || e.ofs >= 0x50) continue;
        if (e.data & 0x80) g.press |= 1ul << (e.ofs - 0x30);
        else g.release |= 1ul << (e.ofs - 0x30);
    }
    return g;
}

LONG const kHrs[] = {0, 1, static_cast<LONG>(0x800700AA), static_cast<LONG>(0x8007001E), static_cast<LONG>(0x80070005)};
constexpr int kNHr = sizeof(kHrs) / sizeof(kHrs[0]);

void setup_device(bool other_read) {
    g_vtbl[9] = reinterpret_cast<void*>(&dev_state);
    g_vtbl[10] = reinterpret_cast<void*>(&dev_data);
    g_vtbl[11] = reinterpret_cast<void*>(&dev_other);
    g_vtbl[15] = reinterpret_cast<void*>(&dev_info);
    g_vtbl[25] = reinterpret_cast<void*>(&dev_poll);
    (void)other_read;
    g_dev.vtbl = g_vtbl;
    std::memset(g_input, 0, sizeof(g_input));
    FakeDev* devp = &g_dev;
    std::memcpy(g_input + 0x90, &devp, 4);
    g_input[0xC98] = 1;
}

int run_dinput() {
    PadBus* bus = padhook_attach();
    check("DIW-0: bus", bus != nullptr);
    if (!bus) return 1;
    // before the client loads: waiting, and why
    check("DIW-1: no client: waiting", !padhook_ensure_di(bus) && padhook_di_state(bus) == PADHOOK_WAIT
        && has(bus->di.status, "FFXiMain.dll is not loaded"), bus->di.status);
    HMODULE game = LoadLibraryA("game_di\\FFXiMain.dll");
    check("DIW-1: the stand-in client loads", game != nullptr);
    if (!game) return 1;
    g_update = fn_of<di_fn>(game, "game_di_fn") ? reinterpret_cast<di_fn>(fn_of<ptr_fn>(game, "game_di_fn")()) : nullptr;
    g_rec = fn_of<rec_fn>(game, "game_di_rec");
    auto sites = fn_of<sites_fn>(game, "game_di_sites");
    auto gamepad = fn_of<onoff_fn>(game, "game_di_gamepad");
    check("DIW-1: its exports", g_update && g_rec && sites && gamepad);
    if (!(g_update && g_rec && sites && gamepad)) return 1;
    auto* const site_a = static_cast<unsigned char*>(sites(0));
    auto* const site_b = static_cast<unsigned char*>(sites(1));
    setup_device(false);

    // the client's own results, before the patch
    Got base[kNHr][kNHr];
    bool ok = true;
    for (int i = 0; i < kNHr; ++i)
        for (int j = 0; j < kNHr; ++j) {
            g_hr_state = kHrs[i];
            g_hr_data = kHrs[j];
            set_pad(1ul << CROSS, 1ul << CROSS);
            base[i][j] = tick();
            Got const& g = base[i][j];
            ok = ok && g.balanced && g.eax == static_cast<DWORD>(kHrs[i])
                && (kHrs[j] == 0 ? (g.flag == 1 && !g.failed && g.n == 1) : (g.failed == 1 && g.n == 0));
        }
    check("DIW-2: unpatched: the stand-in runs the client's bytes", ok && g_args_ok && g_polls == kNHr * kNHr);

    // the join patches both sites
    int const slot = padhook_join(bus, "test");
    check("DIW-3: joined", slot >= 0, bus->status);
    check("DIW-3: the DirectInput sites are live", padhook_di_state(bus) == PADHOOK_LIVE
        && bus->di.site_state == reinterpret_cast<std::uintptr_t>(site_a) + 47
        && bus->di.site_events == reinterpret_cast<std::uintptr_t>(site_b) + 95, bus->di.status);
    check("DIW-3: the status", has(bus->di.status, "hook installed at FFXiMain's DirectInput pad reads")
        && has(bus->di.status, "by test (rev 4)"), bus->di.status);
    check("DIW-3: the cmp and the store are calls now", site_a[47] == 0xE8 && site_b[95] == 0xE8 && site_a[52] == 0x9C);
    check("DIW-3: this module drives it", bus->di.owner == slot + 1 && *bus->di.dispatch_ptr == &padhook_detail::di_dispatch);
    check("DIW-3: the XInput route too", padhook_state(bus) == PADHOOK_LIVE && bus->route == PADHOOK_ROUTE_WRAPPER, bus->status);
    char line[256];
    padhook_di_describe(bus, line, sizeof(line));
    check("DIW-3: described", has(line, "live at FFXiMain's DirectInput pad reads") && has(line, "driven by test r4"), line);

    // driving, nobody holding: the client's own results, and the game gets everything
    ok = true;
    for (int i = 0; i < kNHr; ++i)
        for (int j = 0; j < kNHr; ++j) {
            g_hr_state = kHrs[i];
            g_hr_data = kHrs[j];
            set_pad(1ul << CROSS, 1ul << CROSS);
            Got const g = tick();
            Got const& b = base[i][j];
            ok = ok && g.balanced && g.flags == b.flags && g.eax == b.eax && g.flag == b.flag && g.failed == b.failed;
            if (kHrs[i] == 0 && kHrs[j] == 0) ok = ok && (g.held & (1ul << CROSS)) && (g.press & (1ul << CROSS));
        }
    check("DIW-4: driving: the client's flags, eax and store, and nothing hidden", ok);
    check("DIW-4: the dispatcher ran on both reads", bus->di.calls >= kNHr * kNHr + kNHr);
    PadDiState d {};
    check("DIW-5: the device's maker and name", padhook_di_read(bus, &d) && d.vidpid == 0x09CC054C
        && std::strcmp(d.name, "Wireless Controller") == 0 && d.device == &g_dev);

    // held: a new press hidden from both reads, the sticks and the POV centred, L1 held from before the game's
    g_hr_state = g_hr_data = 0;
    set_pad(1ul << L1, 1ul << L1);
    tick();
    padhook_capture(bus, slot, true);
    set_pad((1ul << L1) | (1ul << CROSS), 1ul << CROSS, 0, 120, 9000);
    Got g = tick();
    check("DIW-6: held: Cross hidden from the state and the events", !(g.held & (1ul << CROSS)) && !(g.press & (1ul << CROSS)));
    check("DIW-6: held: L1 from before is the game's", (g.held & (1ul << L1)) != 0);
    check("DIW-6: held: the stick 0, the POV centred", g.lx == 0 && g.pov == PADHOOK_POV_CENTRED);
    check("DIW-6: held: the record is raw", padhook_di_read(bus, &d) && (d.buttons & (1ul << CROSS)) && d.axes[0] == 120 && d.pov == 9000);
    padhook_capture(bus, slot, false);
    set_pad((1ul << L1) | (1ul << CROSS), 0, 0, 120);
    g = tick();
    check("DIW-6: given back: Cross stays hidden until released, the stick back", !(g.held & (1ul << CROSS)) && g.lx == 120);
    set_pad(1ul << L1, 0, 1ul << CROSS);
    g = tick();
    check("DIW-6: its release dropped", !(g.release & (1ul << CROSS)) && g.n == 0);
    set_pad(0, 0, 1ul << L1);
    tick();

    // the chord: L3 + R3 never reaches the game; L3 tapped alone does, as a press and a release
    padhook_di_chord(bus, slot, (1ul << L3) | (1ul << R3));
    set_pad((1ul << L3) | (1ul << R3), (1ul << L3) | (1ul << R3));
    g = tick();
    check("DIW-7: the chord hidden", !(g.held & ((1ul << L3) | (1ul << R3))) && g.n == 0 && bus->di.press.chords == 1);
    set_pad(0, 0, (1ul << L3) | (1ul << R3));
    g = tick();
    check("DIW-7: its releases dropped", g.n == 0);
    set_pad(1ul << L3, 1ul << L3);
    g = tick();
    check("DIW-7: L3 alone waits", !(g.held & (1ul << L3)) && g.n == 0);
    set_pad(0, 0, 1ul << L3);
    g = tick();
    check("DIW-7: L3 tapped alone: the game gets a press and a release", (g.press & (1ul << L3)) && (g.release & (1ul << L3))
        && g.n == 2 && bus->di.press.injected == 1);

    // a revision 5 module takes the DirectInput dispatch, and hands it back from its DllMain
    HMODULE m5 = LoadLibraryA("pad_mod_r5.dll");
    check("DIW-8: module r5 loads", m5 != nullptr);
    if (m5) {
        auto j5 = fn_of<join_fn>(m5, "mod_join");
        auto c5 = fn_of<cap_fn>(m5, "mod_capture");
        auto d5 = fn_of<ptr_fn>(m5, "mod_di_dispatch");
        int const s5 = j5 ? j5() : -1;
        check("DIW-8: r5 joins, no second patch", s5 >= 0 && s5 != slot && bus->di.installs == 1);
        check("DIW-8: r5 drives DirectInput", d5 && bus->di.owner == s5 + 1
            && reinterpret_cast<void*>(*bus->di.dispatch_ptr) == d5() && bus->di.owner_rev == 5);
        c5(1);
        set_pad(1ul << CIRCLE, 1ul << CIRCLE);
        g = tick();
        check("DIW-8: r5's hold hides for everyone", !(g.held & (1ul << CIRCLE)) && g.n == 0);
        c5(0);
        FreeLibrary(m5);
        check("DIW-8: r5 left: the DirectInput dispatch is back with this module", bus->di.owner == slot + 1
            && *bus->di.dispatch_ptr == &padhook_detail::di_dispatch);
        set_pad(1ul << CIRCLE, 0);
        g = tick();
        check("DIW-8: after r5 unloaded: reads work, its hidden press stays hidden", g.balanced && !(g.held & (1ul << CIRCLE)));
        set_pad(0, 0, 1ul << CIRCLE);
        tick();
        set_pad(1ul << CIRCLE, 1ul << CIRCLE);
        g = tick();
        check("DIW-8: the next press is the game's", (g.held & (1ul << CIRCLE)) && (g.press & (1ul << CIRCLE)));
        set_pad(0, 0, 1ul << CIRCLE);
        tick();
    }

    // a revision 2 module (the frozen header): it never touches the DirectInput dispatch, but its hold counts
    HMODULE m2 = LoadLibraryA("pad_mod_r2.dll");
    check("DIW-9: module r2 loads", m2 != nullptr);
    if (m2) {
        auto j2 = fn_of<join_fn>(m2, "mod_join");
        auto c2 = fn_of<cap_fn>(m2, "mod_capture");
        int const s2 = j2 ? j2() : -1;
        check("DIW-9: r2 joins the XInput route", s2 >= 0, bus->status);
        check("DIW-9: r2 does not drive DirectInput", bus->di.owner == slot + 1);
        c2(1);
        set_pad(1ul << CROSS, 1ul << CROSS);
        g = tick();
        check("DIW-9: r2's hold hides the DirectInput pad too", !(g.held & (1ul << CROSS)) && g.n == 0);
        c2(0);
        FreeLibrary(m2);
        check("DIW-9: r2 left: the DirectInput dispatch unchanged", bus->di.owner == slot + 1);
        set_pad(0, 0, 1ul << CROSS);
        tick();
    }

    // the gamepad off in the game's settings: no event read, and the thunk on its branch never runs
    gamepad(0);
    LONG const calls = bus->di.calls;
    g = tick();
    check("DIW-10: the gamepad off: no event read, no store", g.balanced && g.flag == 0 && bus->di.calls == calls + 1);
    gamepad(1);
    ok = true;
    for (int i = 0; i < 200; ++i) {
        padhook_capture(bus, slot, ((i / 20) & 1) != 0);
        set_pad(1ul << (i & 31), 1ul << ((i * 7) & 31), 0, i);
        Got const t = tick();
        ok = ok && t.balanced && t.flag == 1;
    }
    padhook_capture(bus, slot, false);
    check("DIW-10: 200 more ticks balanced", ok);

    padhook_unregister(bus, slot, true);
    check("DIW-11: left: nobody drives", bus->di.owner == 0 && *bus->di.dispatch_ptr == nullptr);
    set_pad(1ul << CROSS, 1ul << CROSS);
    g = tick();
    check("DIW-11: nobody drives: the client's own reads", g.balanced && (g.held & (1ul << CROSS)) && g.flag == 1);
    return 0;
}

int run_nomatch() {
    PadBus* bus = padhook_attach();
    HMODULE game = LoadLibraryA("game_di_nomatch\\FFXiMain.dll");
    check("DIW-12: the no-match client loads", game != nullptr && bus);
    if (!game || !bus) return 1;
    auto sites = fn_of<sites_fn>(game, "game_di_sites");
    auto* const site_a = static_cast<unsigned char*>(sites(0));
    auto* const site_b = static_cast<unsigned char*>(sites(1));
    unsigned char before_a[64], before_b[104];
    std::memcpy(before_a, site_a, sizeof(before_a));
    std::memcpy(before_b, site_b, sizeof(before_b));
    int const slot = padhook_join(bus, "test");
    check("DIW-12: joined through the XInput wrapper", slot >= 0 && padhook_state(bus) == PADHOOK_LIVE, bus->status);
    check("DIW-12: DirectInput refused, and why", padhook_di_state(bus) == PADHOOK_BLOCKED
        && has(bus->di.status, "no GetDeviceData read of the pad"), bus->di.status);
    check("DIW-12: its bytes untouched", std::memcmp(before_a, site_a, sizeof(before_a)) == 0
        && std::memcmp(before_b, site_b, sizeof(before_b)) == 0);
    check("DIW-12: never tried again", !padhook_ensure_di(bus) && padhook_di_state(bus) == PADHOOK_BLOCKED);
    char line[256];
    padhook_di_describe(bus, line, sizeof(line));
    check("DIW-12: described", has(line, "blocked: FFXiMain's DirectInput pad reads were not found"), line);
    padhook_unregister(bus, slot, true);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    char const* mode = argc > 1 ? argv[1] : "dinput";
    int r = 1;
    if (std::strcmp(mode, "dinput") == 0) r = run_dinput();
    else if (std::strcmp(mode, "nomatch") == 0) r = run_nomatch();
    else std::printf("unknown mode %s\n", mode);
    std::printf("pad_di_test %s: %d passed, %d failed\n", mode, g_pass, g_fail);
    return (r == 0 && g_fail == 0) ? 0 : 1;
}
