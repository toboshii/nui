// pad_test.cpp - PadHook.h in a real process, against stand-ins with the exact shapes of the real code. This
// exe is module A.
//
//   pad_test.exe main <layout_padhook.txt>   route 0 on pad_fake_xinput.dll (the real hot-patchable entry
//                                            shape), with no client module loaded: the patch bytes,
//                                            pass-through, recording, the mask rules, renumbering, the
//                                            heartbeat, the chord, a revision 4 module (pad_mod.cpp) taking
//                                            the dispatch and handing it back from its DllMain, the layout
//   pad_test.exe nomatch                     a client module whose wrapper the signature does not match:
//                                            route 0 on pad_fake_xinput.dll, and the status says why
//   pad_test.exe both                        both routes available: route 1 is taken, the export untouched
//   pad_test.exe r2first                     a revision 2 module patches route 0 first, the client's wrapper
//                                            there too: revision 3 joins that patch and adds no second one
//   pad_test.exe foreign                     another tool's hot-patch already on the entry: refused
//   pad_test_nohot.exe nohot                 an entry that is not hot-patchable, with the no-match client
//                                            module: refused, bytes untouched, both reasons given
//   pad_test_nohot.exe wrapper               the field case: route 1 on the stand-in client module
//                                            (game/FFXiMain.dll, pad_game.cpp) and its shim (xinputdll.dll,
//                                            pad_shim.cpp), which calls pad_fake_nohot.dll. A revision 2
//                                            module refuses route 0 first and route 1 clears that. Every
//                                            check of `main` runs through the client's wrapper, then the
//                                            client's own life: the shim loaded late, pinned, unloaded and
//                                            loaded again, F rewritten, and the revision 2 module driving.
#include "../src/PadHook/PadHook.h"

#include "pad_layout.hpp"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_pass = 0, g_fail = 0;
void check(char const* name, bool ok, char const* detail = "") {
    if (ok) ++g_pass;
    else { ++g_fail; std::printf("FAIL %s %s\n", name, detail); }
}

typedef void(__cdecl* set_fn)(int, int, int, int, int, int, int);
typedef int(__cdecl* join_fn)();
typedef void(__cdecl* cap_fn)(int);
typedef void*(__cdecl* ptr_fn)();
typedef DWORD(__cdecl* game_read_fn)(DWORD, void*);
typedef int(__cdecl* game_setup_fn)();
typedef void(__cdecl* game_void_fn)();
typedef void(__cdecl* game_point_fn)(int);

padhook_get_fn g_get = nullptr;          // the fake's export, read directly (route 0)
game_read_fn g_game_read = nullptr;      // the stand-in client's call into its wrapper (route 1)
set_fn g_set = nullptr;

constexpr unsigned A = 0x1000, B = 0x2000, X = 0x4000, Y = 0x8000, START = 0x0010;

PadXState read(DWORD index = 0, DWORD* r = nullptr) {
    PadXState s {};
    DWORD const rr = g_game_read ? g_game_read(index, &s) : g_get(index, &s);
    if (r) *r = rr;
    return s;
}
PadXState press(unsigned buttons, int lt = 0, int rt = 0, int lx = 0) {
    g_set(static_cast<int>(buttons), lt, rt, lx, 0, 0, 0);
    return read();
}

bool load_fake(char const* dll) {
    HMODULE fx = LoadLibraryA(dll);
    if (!fx) return false;
    g_get = reinterpret_cast<padhook_get_fn>(reinterpret_cast<void*>(GetProcAddress(fx, "XInputGetState")));
    g_set = reinterpret_cast<set_fn>(reinterpret_cast<void*>(GetProcAddress(fx, "fake_set")));
    return g_get && g_set;
}

template <typename T> T fn_of(HMODULE m, char const* name) {
    return m ? reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(m, name))) : nullptr;
}

bool has(char const* text, char const* part) { return std::strstr(text, part) != nullptr; }

// Everything that does not depend on the route: pass-through, recording, the mask, renumbering, the
// heartbeat, the chord, a revision 4 module taking the dispatch and handing it back. Reads go through
// read(), which is the export (route 0) or the client's wrapper (route 1).
void shared_checks(PadBus* bus, int slot) {
    // pass-through and recording
    LONG const calls0 = bus->calls;
    PadXState s = press(A, 0, 0, 1234);
    check("pass-through: buttons", s.buttons == A);
    check("pass-through: stick", s.lx == 1234);
    check("through the dispatcher", bus->calls == calls0 + 1);
    PadState ps {};
    check("read back", padhook_read(bus, 0, &ps));
    check("recorded buttons", ps.buttons == A && ps.connected == 1 && ps.source == PADHOOK_SRC_XINPUT && ps.lx == 1234);
    check("last index", bus->last_index == 0);
    unsigned long const seq0 = ps.seq;
    read();
    padhook_read(bus, 0, &ps);
    check("same state: same seq", ps.seq == seq0);
    press(A | B);
    padhook_read(bus, 0, &ps);
    check("new state: new seq", ps.seq != seq0 && ps.buttons == (A | B));
    DWORD r = 0;
    read(1, &r);
    check("index 1 not connected", r == ERROR_DEVICE_NOT_CONNECTED);
    padhook_read(bus, 1, &ps);
    check("index 1 recorded as not connected", ps.connected == 0);
    check("last index stays the connected one", bus->last_index == 0);
    press(0);
    PadXState const trig = press(0, 200, 0);
    padhook_read(bus, 0, &ps);
    check("a trigger is a held bit", (ps.buttons & PADHOOK_LT) && trig.lt == 200);
    press(0);

    // capture: B held before it reaches the game until released; new presses are hidden
    press(B);
    padhook_capture(bus, slot, true);
    check("holding", padhook_holding(bus, slot));
    s = press(B | A, 200, 0, 20000);
    check("held before: still the game's", (s.buttons & B) != 0);
    check("new press: hidden", (s.buttons & A) == 0);
    check("stick centred", s.lx == 0);
    check("trigger released", s.lt == 0);
    padhook_read(bus, 0, &ps);
    check("raw still recorded", ps.buttons == (A | B | PADHOOK_LT) && ps.lx == 20000);
    s = press(A);
    check("B released", s.buttons == 0);
    s = press(A | B);
    check("B pressed again while held: hidden", s.buttons == 0);
    // renumbering: what the game sees did not change, so neither does its packet
    DWORD const p1 = s.packet;
    s = press(A | B | X);
    check("hidden change: same packet", s.packet == p1 && s.buttons == 0);
    // give the pad back with A, B, X held: hidden until each is released
    padhook_capture(bus, slot, false);
    s = press(A | B | X);
    check("given back: held presses stay hidden", s.buttons == 0);
    s = press(A | 0, 0, 0, 500);
    check("given back: sticks are the game's again", s.lx == 500);
    check("given back: still hidden until released", s.buttons == 0);
    s = press(0);
    s = press(A);
    check("released and pressed again: the game's", s.buttons == A);
    check("shown again: new packet", s.packet != p1);
    press(0);

    // the heartbeat: a capture not renewed lapses by itself
    padhook_capture(bus, slot, true);
    s = press(Y);
    check("captured: Y hidden", s.buttons == 0);
    press(0);
    Sleep(PADHOOK_HEARTBEAT_MS + 150);
    s = press(A);
    check("lapsed: A reaches the game", s.buttons == A);
    check("lapse counted", bus->lapses >= 1);
    check("lapsed: not holding", !padhook_holding(bus, slot));
    press(0);
    padhook_capture(bus, slot, true);
    for (int i = 0; i < 4; ++i) { Sleep(PADHOOK_HEARTBEAT_MS / 2); padhook_alive(bus, slot); }
    s = press(A);
    check("renewed: still holding", s.buttons == 0 && padhook_holding(bus, slot));
    padhook_capture(bus, slot, false);
    press(0);

    // the chord (rev 2): L3 + R3 never reaches the game; L3 alone does, late or replayed
    {
        constexpr unsigned L3 = 0x40, R3 = 0x80;
        padhook_chord(bus, slot, L3 | R3);
        press(0);
        check("chord: on the bus", bus->chord.buttons == (L3 | R3));
        s = press(L3);
        check("chord: L3 held back", (s.buttons & L3) == 0);
        s = press(L3 | R3);
        check("chord: never seen", (s.buttons & (L3 | R3)) == 0);
        s = press(R3);
        check("chord: hidden until released", (s.buttons & R3) == 0);
        press(0);
        press(L3);
        s = press(0);
        check("chord: a tap alone is replayed", (s.buttons & L3) != 0);
        press(0);
        press(0);
        s = press(L3);
        for (int i = 0; i < 20 && !(s.buttons & L3); ++i) { Sleep(20); s = read(); }
        check("chord: held alone, the game gets it", (s.buttons & L3) != 0);
        s = press(L3 | R3);
        check("chord: after that, R3 is the game's too", (s.buttons & (L3 | R3)) == (L3 | R3));
        press(0);
        padhook_chord(bus, slot, 0);
        s = press(L3);
        check("chord: off, L3 at once", (s.buttons & L3) != 0);
        press(0);
    }

    // a second module at revision 4 takes the dispatch; its DllMain hands it back
    HMODULE m2 = LoadLibraryA("pad_mod_r4.dll");
    check("module r4 loads", m2 != nullptr);
    if (m2) {
        auto mjoin = fn_of<join_fn>(m2, "mod_join");
        auto mcap = fn_of<cap_fn>(m2, "mod_capture");
        auto mdis = fn_of<ptr_fn>(m2, "mod_dispatch");
        int const s2 = mjoin ? mjoin() : -1;
        check("r4 joins", s2 >= 0 && s2 != slot);
        check("r4 drives", bus->owner_slot == s2 && reinterpret_cast<void*>(*bus->dispatch_ptr) == mdis());
        check("no second install", bus->installs == 1);
        s = press(A);
        check("r4 dispatches", s.buttons == A);
        mcap(1);
        s = press(A | B);
        check("r4's capture hides for everyone", (s.buttons & B) == 0 && (s.buttons & A) != 0);
        press(0);
        FreeLibrary(m2);
        check("r4 left: the dispatch is back with A", bus->owner_slot == slot && *bus->dispatch_ptr == &padhook_detail::dispatch);
        s = press(A);
        check("after r4 unloaded: reads still work", s.buttons == A, "");
        check("r4's capture went with it", s.buttons == A);
        press(0);
    }
}

// leaving: a pass-through, then a rejoin without a second install
void leave_and_rejoin(PadBus* bus, int slot) {
    padhook_unregister(bus, slot, true);
    check("left: nobody drives", *bus->dispatch_ptr == nullptr && bus->owner_slot == -1);
    LONG const c0 = bus->calls;
    PadXState const s = press(X, 0, 0, 77);
    check("pass-through with nobody", s.buttons == X && s.lx == 77 && bus->calls == c0);
    int const again = padhook_join(bus, "test");
    check("rejoin", again >= 0 && bus->installs == 1);
    padhook_unregister(bus, again, true);
}

void layout_check(char const* layout_path) {
    char text[8192];
    padhook_layout_text(text, sizeof(text));
    std::string want;
    if (FILE* f = std::fopen(layout_path, "rb")) {
        char buf[4096];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) want.append(buf, n);
        std::fclose(f);
    }
    std::string got;
    for (std::size_t at = 0; at < want.size();) {       // the file's non-comment lines
        std::size_t const end = want.find('\n', at);
        std::string line = want.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line[0] != '#') got += line + "\n";
        at = end == std::string::npos ? want.size() : end + 1;
    }
    check("layout_padhook.txt", got == text, "");
    if (got != text) std::printf("--- header layout:\n%s", text);
}

int run_main(char const* layout_path) {
    PadBus* bus = padhook_attach();
    check("attach", bus != nullptr);
    if (!bus) return 1;

    // no client module, and the DLL not loaded yet: retry, and nothing installed
    check("not loaded: retry", padhook_join(bus, "test") == PADHOOK_JOIN_RETRY, bus->status);
    check("not loaded: says so", has(bus->status, "is not loaded (the game reads"), bus->status);
    check("no client module: says so", has(bus->status, "FFXiMain.dll is not loaded"), bus->status);
    check("simulated missing: retry", padhook_join(bus, "test", true) == PADHOOK_JOIN_RETRY);
    check("simulated missing: both routes", has(bus->status, "wrapper was not found (simulated)") && has(bus->status, "not loaded (simulated)"),
          bus->status);
    check("not loaded: waiting", padhook_state(bus) == PADHOOK_WAIT);

    check("fake loads", load_fake(PADHOOK_DLL));
    if (!g_get) return 1;
    auto const* site = reinterpret_cast<unsigned char const*>(reinterpret_cast<void*>(g_get));
    check("fake is hot-patchable", site[0] == 0x8B && site[1] == 0xFF && site[-1] == 0x90);
    PadXState raw = press(A);
    check("unhooked read", raw.buttons == A);

    int const slot = padhook_join(bus, "test");
    check("join", slot >= 0, bus->status);
    check("route 0", bus->route == PADHOOK_ROUTE_EXPORT);
    check("patched entry EB F9", site[0] == 0xEB && site[1] == 0xF9);
    check("patched padding E9", site[-5] == 0xE9);
    check("live", padhook_state(bus) == PADHOOK_LIVE);
    check("drives", bus->owner_slot == slot && *bus->dispatch_ptr == &padhook_detail::dispatch);
    check("one install", bus->installs == 1);
    check("status: route 0 by name", has(bus->status, "!XInputGetState") && has(bus->status, "(rev 3)"), bus->status);
    char line[256];
    padhook_describe(bus, line, sizeof(line));
    check("describe: route 0", has(line, "live at " PADHOOK_DLL "!XInputGetState") && has(line, "1 client: test r3 (drives)"), line);
    check("pin: nothing to pin on route 0", !padhook_pin(bus) && !padhook_shim_loaded(bus));

    shared_checks(bus, slot);
    leave_and_rejoin(bus, slot);
    check("patch kept", site[0] == 0xEB && site[1] == 0xF9);
    layout_check(layout_path);
    return 0;
}

// a client module with no matching wrapper (a client update): route 0 takes over
int run_nomatch() {
    HMODULE game = LoadLibraryA("game_nomatch\\FFXiMain.dll");
    check("no-match client loads", game != nullptr);
    if (!game) return 1;
    auto const* wrapper = static_cast<unsigned char const*>(fn_of<ptr_fn>(game, "game_wrapper")());
    unsigned char before[48];
    std::memcpy(before, wrapper, sizeof(before));
    check("fake loads", load_fake(PADHOOK_DLL));
    PadBus* bus = padhook_attach();
    int const slot = padhook_join(bus, "test");
    check("join on route 0", slot >= 0 && bus->route == PADHOOK_ROUTE_EXPORT, bus->status);
    check("status: why not route 1", has(bus->status, "FFXiMain's XInput wrapper was not found"), bus->status);
    check("the client's wrapper untouched", std::memcmp(before, wrapper, sizeof(before)) == 0);
    PadXState const s = press(A);
    check("reads through route 0", s.buttons == A && bus->calls >= 1);
    padhook_unregister(bus, slot, true);
    return 0;
}

int run_foreign() {
    check("no-match client loads", LoadLibraryA("game_nomatch\\FFXiMain.dll") != nullptr);
    check("fake loads", load_fake(PADHOOK_DLL));
    if (!g_get) return 1;
    // another tool hot-patched it first
    auto* site = reinterpret_cast<unsigned char*>(reinterpret_cast<void*>(g_get));
    DWORD old = 0;
    VirtualProtect(site - 5, 7, PAGE_EXECUTE_READWRITE, &old);
    unsigned char const pad[5] = {0xE9, 0, 0, 0, 0};       // jmp +0: falls into the entry, a harmless detour
    std::memcpy(site - 5, pad, 5);
    VirtualProtect(site - 5, 7, old, &old);
    PadBus* bus = padhook_attach();
    check("foreign: blocked", padhook_join(bus, "test") == PADHOOK_JOIN_BLOCKED, bus->status);
    check("foreign: says so", has(bus->status, "already hooked"), bus->status);
    check("foreign: entry untouched", site[0] == 0x8B && site[1] == 0xFF);
    check("foreign: never retried", padhook_join(bus, "test") == PADHOOK_JOIN_BLOCKED);
    return 0;
}

int run_nohot() {
    check("no-match client loads", LoadLibraryA("game_nomatch\\FFXiMain.dll") != nullptr);
    check("fake loads", load_fake(PADHOOK_DLL));
    if (!g_get) return 1;
    auto const* site = reinterpret_cast<unsigned char const*>(reinterpret_cast<void*>(g_get));
    unsigned char before[7];
    std::memcpy(before, site - 5, 7);
    PadBus* bus = padhook_attach();
    check("nohot: blocked", padhook_join(bus, "test") == PADHOOK_JOIN_BLOCKED, bus->status);
    check("nohot: says so", has(bus->status, "not hot-patchable"), bus->status);
    check("nohot: and why not route 1", has(bus->status, "FFXiMain's XInput wrapper was not found"), bus->status);
    check("nohot: bytes untouched", std::memcmp(before, site - 5, 7) == 0);
    check("nohot: state blocked", padhook_state(bus) == PADHOOK_BLOCKED);
    char line[256];
    padhook_describe(bus, line, sizeof(line));
    check("nohot: describe", has(line, "blocked: FFXiMain's XInput wrapper was not found") && has(line, "not hot-patchable"), line);
    PadXState const s = press(A);
    check("nohot: the game still reads", s.buttons == A);
    padhook_join(bus, "test", true);
    check("nohot: nosig keeps route 0's real reason", has(bus->status, "not hot-patchable") && !has(bus->status, "simulated"),
          bus->status);
    return 0;
}

// both routes available: route 1 first, and the export is left alone
int run_both() {
    HMODULE game = LoadLibraryA("game\\FFXiMain.dll");
    check("client loads", game != nullptr);
    if (!game) return 1;
    check("fake loads", load_fake(PADHOOK_DLL));
    check("client setup", fn_of<game_setup_fn>(game, "game_setup")() == 1);
    auto const* entry = reinterpret_cast<unsigned char const*>(reinterpret_cast<void*>(g_get));
    auto const* wrapper = static_cast<unsigned char const*>(fn_of<ptr_fn>(game, "game_wrapper")());
    PadBus* bus = padhook_attach();
    int const slot = padhook_join(bus, "test");
    check("both: route 1 taken", slot >= 0 && bus->route == PADHOOK_ROUTE_WRAPPER && wrapper[9] == 0xB8, bus->status);
    check("both: the export untouched", entry[0] == 0x8B && entry[1] == 0xFF && entry[-5] == 0x90);
    check("both: pinned at install", padhook_pin(bus));
    g_game_read = fn_of<game_read_fn>(game, "game_read");
    LONG const c0 = bus->calls;
    PadXState s = press(A);
    check("both: the client's read, through the dispatcher once", s.buttons == A && bus->calls == c0 + 1);
    g_game_read = nullptr;
    s = press(B);
    check("both: a direct export read is not hooked", s.buttons == B && bus->calls == c0 + 1);
    padhook_unregister(bus, slot, true);
    return 0;
}

// a revision 2 module patched route 0 first: revision 3 joins it, and the wrapper stays as the client has it
int run_r2first() {
    HMODULE game = LoadLibraryA("game\\FFXiMain.dll");
    check("client loads", game != nullptr);
    if (!game) return 1;
    check("fake loads", load_fake(PADHOOK_DLL));
    check("client setup", fn_of<game_setup_fn>(game, "game_setup")() == 1);
    auto const* entry = reinterpret_cast<unsigned char const*>(reinterpret_cast<void*>(g_get));
    auto const* wrapper = static_cast<unsigned char const*>(fn_of<ptr_fn>(game, "game_wrapper")());
    HMODULE r2 = LoadLibraryA("pad_mod_r2x.dll");
    check("module r2 loads", r2 != nullptr);
    if (!r2) return 1;
    PadBus* bus = padhook_attach();
    int const s2 = fn_of<join_fn>(r2, "mod_join")();
    check("r2: route 0", s2 >= 0 && entry[0] == 0xEB && entry[1] == 0xF9 && bus->route == PADHOOK_ROUTE_EXPORT, bus->status);
    int const slot = padhook_join(bus, "test");
    check("r3 joins the same patch", slot >= 0 && bus->installs == 1 && bus->route == PADHOOK_ROUTE_EXPORT);
    check("the wrapper untouched", wrapper[9] == 0xA1);
    check("r3 drives", *bus->dispatch_ptr == &padhook_detail::dispatch);
    g_game_read = fn_of<game_read_fn>(game, "game_read");
    LONG const c0 = bus->calls;
    PadXState const s = press(A);
    check("the client's read: through the dispatcher once", s.buttons == A && bus->calls == c0 + 1);
    padhook_unregister(bus, slot, true);
    FreeLibrary(r2);
    return 0;
}

// the field case: route 0 refuses, route 1 holds the pad through the client's own wrapper
int run_wrapper() {
    PadBus* bus = padhook_attach();
    check("fake loads", load_fake(PADHOOK_DLL));
    if (!g_get) return 1;
    auto const* entry = reinterpret_cast<unsigned char const*>(reinterpret_cast<void*>(g_get));
    unsigned char entry_before[7];
    std::memcpy(entry_before, entry - 5, 7);

    // a revision 2 module comes first: route 0 refuses, and it blocks the bus
    HMODULE r2 = LoadLibraryA("pad_mod_r2.dll");
    check("module r2 loads", r2 != nullptr);
    if (!r2) return 1;
    auto r2join = fn_of<join_fn>(r2, "mod_join");
    auto r2cap = fn_of<cap_fn>(r2, "mod_capture");
    auto r2dis = fn_of<ptr_fn>(r2, "mod_dispatch");
    check("r2: route 0 refused", r2join() == PADHOOK_JOIN_BLOCKED && bus->blocked == 1, bus->status);
    // before the client module is loaded, revision 3 has no wrapper either. Route 1 can still work, so it waits
    check("no client yet: retry", padhook_join(bus, "test") == PADHOOK_JOIN_RETRY, bus->status);
    check("no client yet: waiting", padhook_state(bus) == PADHOOK_WAIT && bus->blocked == 1);
    check("no client yet: both reasons", has(bus->status, "FFXiMain.dll is not loaded") && has(bus->status, "not hot-patchable"), bus->status);

    // the client module, before it has loaded its shim (Enable XInput off, or input init not done)
    HMODULE game = LoadLibraryA("game\\FFXiMain.dll");
    check("client loads", game != nullptr);
    if (!game) return 1;
    g_game_read = fn_of<game_read_fn>(game, "game_read");
    auto game_setup = fn_of<game_setup_fn>(game, "game_setup");
    auto game_unload = fn_of<game_void_fn>(game, "game_unload");
    auto game_point = fn_of<game_point_fn>(game, "game_point");
    auto* wrapper = static_cast<unsigned char const*>(fn_of<ptr_fn>(game, "game_wrapper")());
    auto const* slots = reinterpret_cast<volatile DWORD const*>(fn_of<ptr_fn>(game, "game_slots")());
    // F holds another function than the shim's XInputGetState_: not route 1 now, but tried again
    check("client setup", game_setup() == 1);
    game_point(1);
    check("slots mismatch: retry", padhook_join(bus, "test") == PADHOOK_JOIN_RETRY, bus->status);
    check("slots mismatch: says so", has(bus->status, "function slot does not hold"), bus->status);
    check("slots mismatch: not patched", wrapper[9] == 0xA1);
    game_point(0);
    game_unload();
    DWORD r = 0;
    read(0, &r);
    check("no shim: the wrapper's own failure", r == padhook_detail::kWrapperFail);

    int const slot = padhook_join(bus, "test");
    check("join: route 1", slot >= 0 && bus->route == PADHOOK_ROUTE_WRAPPER, bus->status);
    check("route 1 clears blocked", bus->blocked == 0 && padhook_state(bus) == PADHOOK_LIVE);
    check("site is the GetState wrapper", bus->site == reinterpret_cast<std::uintptr_t>(wrapper));
    auto const block = reinterpret_cast<std::uintptr_t>(bus->block);
    check("patched: mov eax, thunk", wrapper[9] == 0xB8 && padhook_detail::le32(wrapper + 10) == block + padhook_detail::kThunkAt);
    check("patched: H kept", padhook_detail::le32(wrapper + 1) == reinterpret_cast<std::uintptr_t>(&slots[0]));
    check("original is the adapter", reinterpret_cast<std::uintptr_t>(bus->original) == block + padhook_detail::kAdapterAt);
    check("the export untouched", std::memcmp(entry_before, entry - 5, 7) == 0);
    check("status", has(bus->status, "hook installed at FFXiMain's XInput wrapper") && has(bus->status, "(rev 3)"), bus->status);
    char line[256];
    padhook_describe(bus, line, sizeof(line));
    check("describe: not loaded yet", has(line, "live at FFXiMain's XInput wrapper") && has(line, "has not loaded xinputdll.dll yet"), line);
    check("pin: nothing to pin yet", !padhook_pin(bus) && !padhook_shim_loaded(bus));
    LONG const c0 = bus->calls;
    read(0, &r);
    check("no shim: still the wrapper's failure, no dispatch", r == padhook_detail::kWrapperFail && bus->calls == c0);

    // input init: the client loads the shim and writes the slots
    check("client setup", game_setup() == 1 && slots[0] != 0);
    HMODULE const shim = GetModuleHandleA("xinputdll.dll");
    check("the shim is the handle", reinterpret_cast<DWORD>(shim) == slots[0]);
    check("pin", padhook_pin(bus) && padhook_shim_loaded(bus));
    padhook_describe(bus, line, sizeof(line));
    check("describe: loaded", has(line, "live at FFXiMain's XInput wrapper") && !has(line, "not loaded") && has(line, "1 client: test r3 (drives)"),
          line);

    shared_checks(bus, slot);

    // a re-enumeration rewrites all four slots with the same values: nothing changes
    game_setup();
    PadXState s = press(A);
    check("rewritten slots: reads work", s.buttons == A && wrapper[9] == 0xB8);
    press(0);

    // the client moves F (a reload at another address): the next read uses the new function
    game_point(1);
    LONG const c1 = bus->calls;
    s = press(A);
    check("F moved: the new function, through the dispatcher", s.buttons == (A | START) && bus->calls == c1 + 1);
    game_point(0);
    s = press(A);
    check("F back", s.buttons == A);
    press(0);

    // the unloader: frees the shim and zeros H. Pinned, the shim stays mapped
    game_unload();
    check("unloaded: H zero", slots[0] == 0);
    check("unloaded: the shim stays mapped (pinned)", GetModuleHandleA("xinputdll.dll") == shim);
    LONG const c2 = bus->calls;
    read(0, &r);
    check("unloaded: the wrapper's own failure, no dispatch", r == padhook_detail::kWrapperFail && bus->calls == c2);
    check("unloaded: describe says so", (padhook_describe(bus, line, sizeof(line)), has(line, "has not loaded xinputdll.dll yet")), line);
    check("reloaded", game_setup() == 1 && slots[0] == reinterpret_cast<DWORD>(shim));
    s = press(B);
    check("reloaded: reads through the dispatcher", s.buttons == B && bus->calls == c2 + 1);
    press(0);

    // the revision 2 module joins the route 1 bus: it registers, and drives once module A leaves
    int const s2 = r2join();
    check("r2 joins route 1", s2 >= 0 && s2 != slot && bus->installs == 1);
    check("r2 does not drive (A is r3)", bus->owner_slot == slot);
    padhook_unregister(bus, slot, true);
    check("A left: r2 drives", reinterpret_cast<void*>(*bus->dispatch_ptr) == r2dis());
    LONG const c3 = bus->calls;
    s = press(A);
    check("r2's dispatcher through the thunk and the adapter", s.buttons == A && bus->calls == c3 + 1);
    r2cap(1);
    s = press(A | X);
    check("r2's capture hides", (s.buttons & X) == 0 && (s.buttons & A) != 0);
    r2cap(0);
    press(0);
    int const back = padhook_join(bus, "test");
    check("A again: drives (r3 over r2)", back >= 0 && *bus->dispatch_ptr == &padhook_detail::dispatch);
    FreeLibrary(r2);
    check("r2 gone: A still drives", bus->owner_slot == back);
    s = press(Y);
    check("after r2 unloaded: reads work", s.buttons == Y);
    press(0);
    leave_and_rejoin(bus, back);
    check("patch kept", wrapper[9] == 0xB8);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string const mode = argc > 1 ? argv[1] : "main";
    if (mode == "main") run_main(argc > 2 ? argv[2] : "layout_padhook.txt");
    else if (mode == "nomatch") run_nomatch();
    else if (mode == "both") run_both();
    else if (mode == "r2first") run_r2first();
    else if (mode == "foreign") run_foreign();
    else if (mode == "nohot") run_nohot();
    else if (mode == "wrapper") run_wrapper();
    else { std::printf("unknown mode %s\n", mode.c_str()); return 2; }
    std::printf("pad_test %s: %d passed, %d failed\n", mode.c_str(), g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
