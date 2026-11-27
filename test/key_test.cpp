// key_test.cpp - KeyHook.h in a real Windows process, against a stand-in client (key_game.cpp) whose input update
// carries the client's two keyboard read sites byte for byte. This exe is module A, and it holds the stand-in
// keyboard device.
//
//   key_test.exe main <layout_keyhook.txt>   the stand-in client loaded late: wait, then the patch, pass-through
//                                            with nobody driving, every press rule through the client's own
//                                            code, the heartbeat, a revision 2 module (key_mod.cpp) taking the
//                                            dispatch and handing it back from its DllMain, the status, the layout
//   key_test.exe nomatch                     a client whose cmps have another encoding: refused for good, the
//                                            bytes untouched, every key the game's
#include "../src/KeyHook/KeyHook.h"

#include "key_layout.hpp"

#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace {

int g_pass = 0, g_fail = 0;
void check(char const* name, bool ok, char const* detail = "") {
    if (ok) ++g_pass;
    else { ++g_fail; std::printf("FAIL %s %s\n", name, detail); }
}
bool has(char const* text, char const* part) { return std::strstr(text, part) != nullptr; }

// ---- the stand-in keyboard device: a COM object with GetDeviceState (+0x24) and GetDeviceData (+0x28) ----

struct Dev { void* const* vtbl; };
unsigned char g_down[256];
KeyEvent g_queue[256];
DWORD g_nq = 0, g_seq = 0;
LONG g_hr = 0;

LONG __stdcall dev_state(Dev*, DWORD cb, void* buf) {
    if (cb != 256 || !buf) return E_INVALIDARG;
    std::memcpy(buf, g_down, 256);
    return g_hr;
}
LONG __stdcall dev_data(Dev*, DWORD cb, KeyEvent* ev, DWORD* inout, DWORD flags) {
    if (cb != sizeof(KeyEvent) || !inout || flags != 0) return E_INVALIDARG;
    DWORD const n = g_nq < *inout ? g_nq : *inout;
    std::memcpy(ev, g_queue, n * sizeof(KeyEvent));
    std::memmove(g_queue, g_queue + n, (g_nq - n) * sizeof(KeyEvent));
    g_nq -= n;
    *inout = n;
    return g_hr;
}
void* g_vtbl[16] = {};
Dev g_dev {g_vtbl};

void press(unsigned k) { g_down[k] = 0x80; g_queue[g_nq++] = KeyEvent {k, 0x80, GetTickCount(), ++g_seq, 0}; }
void release(unsigned k) { g_down[k] = 0; g_queue[g_nq++] = KeyEvent {k, 0, GetTickCount(), ++g_seq, 0}; }

// ---- the stand-in client ----

typedef void(__cdecl* update_fn)(void*);
typedef DWORD const*(__cdecl* rec_fn)();
typedef void*(__cdecl* ptr_fn)();
typedef int(__cdecl* join_fn)();
typedef void(__cdecl* hide_fn)(int);

update_fn g_update = nullptr;
rec_fn g_rec = nullptr;
alignas(16) unsigned char g_input[0xDC4];

struct Seen {
    unsigned char state[256];
    KeyEvent ev[16];
    DWORD n;
    DWORD rec[4];
};
// one tick of the client's input update; what the game got
Seen tick() {
    std::memset(g_input + 0x98, 0, 256);
    g_update(g_input);
    Seen s {};
    std::memcpy(s.state, g_input + 0x98, 256);
    std::memcpy(&s.n, g_input + 0x8DC, 4);
    std::memcpy(s.ev, g_input + 0x79C, (s.n < 16 ? s.n : 16) * sizeof(KeyEvent));
    std::memcpy(s.rec, g_rec(), sizeof(s.rec));
    return s;
}
bool st(Seen const& s, unsigned k) { return (s.state[k] & 0x80) != 0; }
int ev_of(Seen const& s, unsigned k) {
    int n = 0;
    for (DWORD j = 0; j < s.n; ++j) if (s.ev[j].ofs == k) ++n;
    return n;
}
bool game_down(Seen const& s, unsigned k) {
    if (st(s, k)) return true;
    for (DWORD j = 0; j < s.n; ++j) if (s.ev[j].ofs == k && (s.ev[j].data & 0x80)) return true;
    return false;
}

template <typename T> T fn_of(HMODULE m, char const* name) {
    return m ? reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(m, name))) : nullptr;
}

bool load_game(char const* path) {
    HMODULE g = LoadLibraryA(path);
    g_update = fn_of<update_fn>(g, "game_update");
    g_rec = fn_of<rec_fn>(g, "game_rec");
    g_vtbl[9] = reinterpret_cast<void*>(&dev_state);
    g_vtbl[10] = reinterpret_cast<void*>(&dev_data);
    std::memset(g_input, 0, sizeof(g_input));
    Dev* d = &g_dev;
    std::memcpy(g_input + 0x88, &d, 4);
    return g_update && g_rec;
}

constexpr unsigned UP = 0xC8, DOWN = 0xD0, ENTER = 0x1C, W = 0x11, K = 0x25, ESC = 0x01;

void hide(KeyBus* bus, int slot, std::initializer_list<unsigned> keys) {
    unsigned long mask[KEYHOOK_WORDS] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (unsigned k : keys) mask[k >> 5] |= 1ul << (k & 31);
    keyhook_hide(bus, slot, mask);
}

// The client's own eax and flags after each cmp, for each HRESULT, with the patch in and someone driving.
void results(Seen const* base, int n, LONG const* hrs, char const* name) {
    bool ok = true;
    for (int i = 0; i < n; ++i) {
        g_hr = hrs[i];
        Seen const s = tick();
        ok = ok && s.rec[0] == base[i].rec[0] && (s.rec[1] & 0x8D5) == (base[i].rec[1] & 0x8D5) && s.rec[2] == base[i].rec[2]
            && (s.rec[3] & 0x8D5) == (base[i].rec[3] & 0x8D5);
    }
    g_hr = 0;
    check(name, ok);
}

int run_main(char const* layout) {
    KeyBus* bus = keyhook_attach();
    check("bus", bus != nullptr);
    if (!bus) return 1;
    // the client is not loaded yet: wait, not blocked
    int r = keyhook_join(bus, "a");
    check("WAIT: no client module: retry later", r == KEYHOOK_JOIN_RETRY && keyhook_state(bus) == KEYHOOK_WAIT, bus->status);
    check("WAIT: the status says why", has(bus->status, "not loaded"), bus->status);
    r = keyhook_join(bus, "a", true);
    check("WAIT: the simulated not-found path", r == KEYHOOK_JOIN_RETRY && has(bus->status, "simulated"), bus->status);
    if (!load_game("game\\FFXiMain.dll")) { check("the stand-in client loads", false); return 1; }
    // the client's own results, unpatched
    LONG const hrs[] = {0, 1, static_cast<LONG>(0x800700AA), static_cast<LONG>(0x8007001E), static_cast<LONG>(0x8007000C), 0x7FFFFFFF};
    int const nhr = sizeof(hrs) / sizeof(hrs[0]);
    Seen base[nhr];
    for (int i = 0; i < nhr; ++i) { g_hr = hrs[i]; base[i] = tick(); }
    g_hr = 0;
    press(UP);
    Seen s = tick();
    check("RAW: unpatched, the game gets Up", st(s, UP) && ev_of(s, UP) == 1);
    release(UP);
    tick();

    // the patch, nobody driving yet
    auto* code = static_cast<unsigned char*>(fn_of<ptr_fn>(GetModuleHandleA("FFXiMain.dll"), "game_code")());
    check("PATCH: installed", keyhook_ensure_hook(bus, false, "a") && keyhook_state(bus) == KEYHOOK_LIVE, bus->status);
    check("PATCH: the status names both sites", has(bus->status, "installed at FFXiMain's keyboard reads"), bus->status);
    auto* cmp_a = reinterpret_cast<unsigned char*>(bus->site_state);
    auto* cmp_b = reinterpret_cast<unsigned char*>(bus->site_events);
    check("PATCH: both cmps in the client's update, now calls", cmp_a > code && cmp_b > cmp_a && cmp_b - code < 0x100
        && cmp_a[0] == 0xE8 && cmp_b[0] == 0xE8 && bus->site_state % 8 == 1 && bus->site_events % 8 == 0);
    check("PATCH: nobody drives", *bus->dispatch_ptr == nullptr && bus->device_disp == 0x88);
    press(UP);
    s = tick();
    check("PASS: nobody drives: every key the game's", st(s, UP) && ev_of(s, UP) == 1 && bus->calls == 0);
    release(UP);
    tick();

    // module A joins and drives, while W is held from before
    press(W);
    tick();
    int const a = keyhook_register(bus, "a");
    check("JOIN: a slot", a >= 0 && bus->owner_slot == a && *bus->dispatch_ptr == &keyhook_detail::dispatch);
    hide(bus, a, {W});
    s = tick();
    check("KEY-15: W held when the driving started: still the game's", st(s, W));
    hide(bus, a, {});
    release(W);
    tick();
    results(base, nhr, hrs, "JOIN: driving: the client's eax and flags after each cmp");
    hide(bus, a, {UP, DOWN, ENTER});
    press(W);
    press(UP);
    s = tick();
    check("KEY-2: Up hidden from the state and the events", !game_down(s, UP), "");
    check("KEY-2: W the game's", st(s, W) && ev_of(s, W) == 1);
    check("KEY-2: the dispatcher saw the client's device", bus->keys.device == &g_dev && bus->calls >= 2);
    char dev[64];
    check("STATUS: the device's module is this exe", keyhook_device_module(bus, dev, sizeof(dev)) && has(dev, "key_test"), dev);
    hide(bus, a, {});
    s = tick();
    check("KEY-4: out of the mask, still held: still hidden", !st(s, UP));
    release(UP);
    release(W);
    s = tick();
    check("KEY-4: its release dropped, W's kept", ev_of(s, UP) == 0 && ev_of(s, W) == 1);
    // a press the game saw stays the game's
    press(DOWN);
    tick();
    hide(bus, a, {DOWN});
    s = tick();
    check("KEY-3: seen before the mask: still the game's", st(s, DOWN));
    release(DOWN);
    s = tick();
    check("KEY-3: its release reaches the game", ev_of(s, DOWN) == 1);
    // a tap inside one tick
    hide(bus, a, {ENTER});
    press(ENTER);
    release(ENTER);
    s = tick();
    check("KEY-5: a hidden tap: nothing reaches the game", !game_down(s, ENTER) && ev_of(s, ENTER) == 0);
    // the heartbeat
    hide(bus, a, {ESC});
    Sleep(KEYHOOK_HEARTBEAT_MS + 100);
    press(ESC);
    s = tick();
    check("KEY-13: lapsed: Escape reaches the game", game_down(s, ESC) && bus->lapses >= 1 && !keyhook_hiding(bus, a));
    release(ESC);
    tick();

    // a revision 2 module takes the dispatch, hides its own key, and hands the dispatch back from DllMain
    HMODULE m = LoadLibraryA("key_mod_r2.dll");
    auto mod_join = fn_of<join_fn>(m, "mod_join");
    auto mod_hide = fn_of<hide_fn>(m, "mod_hide");
    auto mod_dispatch = fn_of<ptr_fn>(m, "mod_dispatch");
    check("R2: loaded", mod_join && mod_hide && mod_dispatch);
    if (!mod_join) return 1;
    LONG const handovers = bus->handovers;
    int const b = mod_join();
    check("R2: joins and drives", b >= 0 && bus->owner_rev == 2 && *bus->dispatch_ptr == mod_dispatch() && bus->handovers == handovers + 1);
    hide(bus, a, {UP});
    mod_hide(K);
    press(UP);
    press(K);
    press(W);
    s = tick();
    check("R2: the union: Up (A's) and K (r2's) hidden, W not", !game_down(s, UP) && !game_down(s, K) && game_down(s, W));
    char line[256];
    keyhook_describe(bus, line, sizeof(line));
    check("R2: describe", has(line, "live at FFXiMain's keyboard reads") && has(line, "2 clients") && has(line, "r2 r2 (drives, hides 1)")
        && has(line, "a r1 (hides 1)"), line);
    FreeLibrary(m);
    check("R2: unloaded: the dispatch is back with A", bus->owner_slot == a && *bus->dispatch_ptr == &keyhook_detail::dispatch
        && keyhook_active_clients(bus) == 1);
    hide(bus, a, {UP});
    s = tick();
    check("R2: after it left: K stays hidden until released", !st(s, K) && !st(s, UP) && st(s, W));
    release(UP);
    release(K);
    release(W);
    s = tick();
    check("R2: the releases: only W's reaches the game", ev_of(s, W) == 1 && ev_of(s, K) == 0 && ev_of(s, UP) == 0);

    // A leaves: nobody drives, the thunks pass every key through
    keyhook_unregister(bus, a, true);
    check("LEAVE: nobody drives", *bus->dispatch_ptr == nullptr && bus->owner_slot == -1);
    LONG const calls = bus->calls;
    press(UP);
    s = tick();
    check("LEAVE: every key the game's again", st(s, UP) && ev_of(s, UP) == 1 && bus->calls == calls);
    release(UP);
    tick();
    results(base, nhr, hrs, "LEAVE: the client's eax and flags after each cmp");
    // joining again needs no second patch
    LONG const installs = bus->installs;
    r = keyhook_join(bus, "a");
    check("AGAIN: joins the patch that is there", r >= 0 && bus->installs == installs && cmp_a[0] == 0xE8);
    keyhook_unregister(bus, r, true);

    int const bad = keyhook_layout_compare(layout);
    check("LAYOUT: layout_keyhook.txt is the header's", bad == 0);
    return 0;
}

int run_nomatch() {
    if (!load_game("game_nomatch\\FFXiMain.dll")) { check("the stand-in client loads", false); return 1; }
    auto* code = static_cast<unsigned char*>(fn_of<ptr_fn>(GetModuleHandleA("FFXiMain.dll"), "game_code")());
    unsigned char before[0x100];
    std::memcpy(before, code, sizeof(before));
    KeyBus* bus = keyhook_attach();
    int const r = keyhook_join(bus, "a");
    check("NOMATCH: refused for good", r == KEYHOOK_JOIN_BLOCKED && keyhook_state(bus) == KEYHOOK_BLOCKED, bus->status);
    check("NOMATCH: the status says why", has(bus->status, "were not found"), bus->status);
    check("NOMATCH: the client's bytes untouched", std::memcmp(before, code, sizeof(before)) == 0);
    check("NOMATCH: never tried again", keyhook_join(bus, "a") == KEYHOOK_JOIN_BLOCKED && bus->installs == 0);
    char line[256];
    keyhook_describe(bus, line, sizeof(line));
    check("NOMATCH: describe", has(line, "blocked: FFXiMain's keyboard reads were not found"), line);
    press(UP);
    Seen const s = tick();
    check("NOMATCH: every key the game's", st(s, UP) && ev_of(s, UP) == 1);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    char const* mode = argc > 1 ? argv[1] : "main";
    if (std::strcmp(mode, "main") == 0) run_main(argc > 2 ? argv[2] : "layout_keyhook.txt");
    else if (std::strcmp(mode, "nomatch") == 0) run_nomatch();
    else { std::printf("unknown mode %s\n", mode); return 2; }
    std::printf("key_test %s: %d passed, %d failed\n", mode, g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
