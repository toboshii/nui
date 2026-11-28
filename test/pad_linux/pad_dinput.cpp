// pad_dinput.cpp - PadHook's DirectInput source (revision 4) on 32-bit Linux: the press rules on both reads, the chord
// with its taps given back as events, the sticks and the POV, the lost-release rule, then the dispatcher on a bus (the
// raw record, the device's maker, the heartbeat, a second reader) and who drives the dispatch. Each rule has named checks
// (DI-1 ...), and each fails on a build without its rule (run.sh --self-test).
//
// A tick is what the client does once per main loop tick: the state read, then the event read. The "game" below is a
// model of what the client makes of the two reads (docs: the DirectInput research note): a button is held while its
// state byte has 0x80, and it gets a press edge from each press event and a release edge from each release event.
#include "PadHook/PadHook.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

DWORD g_fake_tick = 1000;

// what the dispatcher and the ownership code call on this path: every address is one image, the test's own module
namespace {
HMODULE const g_module = reinterpret_cast<HMODULE>(0x400000);
}
SIZE_T VirtualQuery(LPCVOID p, MEMORY_BASIC_INFORMATION* m, SIZE_T) {
    std::memset(m, 0, sizeof(*m));
    m->BaseAddress = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(p) & ~static_cast<std::uintptr_t>(0xFFF));
    m->AllocationBase = g_module;
    m->RegionSize = 0x1000;
    m->State = MEM_COMMIT;
    m->Protect = PAGE_EXECUTE_READWRITE;
    m->Type = MEM_IMAGE;
    return sizeof(*m);
}
BOOL GetModuleHandleExA(DWORD, LPCSTR, HMODULE* out) { *out = g_module; return 1; }
BOOL QueryPerformanceCounter(LARGE_INTEGER* t) { t->QuadPart = 12345; return 1; }
void Sleep(DWORD) {}
void* VirtualAlloc(void*, SIZE_T, DWORD, DWORD) { return nullptr; }   // the install paths are pad_disites.cpp's
BOOL VirtualFree(void*, SIZE_T, DWORD) { return 1; }
BOOL VirtualProtect(void*, SIZE_T, DWORD, DWORD* old) { if (old) *old = PAGE_EXECUTE_READ; return 1; }
BOOL FlushInstructionCache(HANDLE, LPCVOID, SIZE_T) { return 1; }
HANDLE GetCurrentProcess() { return nullptr; }

namespace {

int g_pass = 0, g_fail = 0;
void check(char const* name, bool ok) {
    if (ok) ++g_pass;
    else { ++g_fail; std::printf("FAIL %s\n", name); }
}

using padhook_detail::di_apply_events;
using padhook_detail::di_apply_state;

constexpr DWORD ASKED = 16;
constexpr unsigned L3 = 10, R3 = 11, CROSS = 1, CIRCLE = 2, SQUARE = 0, L1 = 4;   // a PlayStation pad's numbering
constexpr unsigned long CHORD = (1ul << L3) | (1ul << R3);

struct Game {                            // what the client made of the last tick
    unsigned long held = 0;              // state bytes with 0x80
    unsigned long press = 0, release = 0;   // edges from the events
    int presses[32] = {}, releases[32] = {};  // edges, ever
    LONG axes[8] = {};
    DWORD pov = PADHOOK_POV_CENTRED;
    DWORD n = 0;                         // events the client got
    bool order_ok = true;                // a release never came before its press within one read
};

struct World {
    PadDiPress k {};
    bool cap = false;
    unsigned char down[32] = {};         // the real pad
    LONG axes[8] = {};
    DWORD pov = PADHOOK_POV_CENTRED;
    PadDiEvent pending[256];             // the device's buffer
    DWORD npending = 0;
    DWORD seq = 0;
    DWORD asked = ASKED;
    Game g {};

    World() { k.primed = 1; k.pov_out = PADHOOK_POV_CENTRED; }
    void chord(unsigned long c) { k.chord = c; }
    void press(unsigned b) { down[b] = 0x80; pending[npending++] = PadDiEvent {0x30 + b, 0x80, g_fake_tick, ++seq, 0}; }
    void release(unsigned b) { down[b] = 0; pending[npending++] = PadDiEvent {0x30 + b, 0, g_fake_tick, ++seq, 0}; }
    void lose(unsigned b) { down[b] = 0; }       // a release the buffer never got
    void other(DWORD ofs, DWORD data) { pending[npending++] = PadDiEvent {ofs, data, g_fake_tick, ++seq, 0}; }

    // the state read, then the event read, as the client makes them
    Game& tick(DWORD ms = 16) {
        g_fake_tick += ms;
        unsigned char js[PADHOOK_JS_BYTES] = {};
        std::memcpy(js, axes, sizeof(axes));
        std::memcpy(js + PADHOOK_JS_POV, &pov, 4);
        std::memcpy(js + PADHOOK_JS_BUTTONS, down, 32);
        di_apply_state(k, js, cap, g_fake_tick);
        PadDiEvent ev[64];
        DWORD const n = npending < asked ? npending : asked;
        std::memcpy(ev, pending, n * sizeof(PadDiEvent));
        std::memmove(pending, pending + n, (npending - n) * sizeof(PadDiEvent));
        npending -= n;
        DWORD count = n;
        di_apply_events(k, ev, &count, asked, cap, g_fake_tick);
        g.held = g.press = g.release = 0;
        for (unsigned i = 0; i < 32; ++i) if (js[PADHOOK_JS_BUTTONS + i] & 0x80) g.held |= 1ul << i;
        std::memcpy(g.axes, js, sizeof(g.axes));
        std::memcpy(&g.pov, js + PADHOOK_JS_POV, 4);
        g.n = count;
        unsigned long seen_press = 0;
        for (DWORD j = 0; j < count && j < 64; ++j) {
            if (ev[j].ofs < 0x30 || ev[j].ofs >= 0x50) continue;
            unsigned const b = ev[j].ofs - 0x30;
            if (ev[j].data & 0x80) { g.press |= 1ul << b; seen_press |= 1ul << b; ++g.presses[b]; }
            else {
                g.release |= 1ul << b;
                ++g.releases[b];
            }
        }
        (void)seen_press;
        return g;
    }
};

bool has(unsigned long m, unsigned b) { return (m >> b) & 1; }

void rules() {
    {   // DI-1: nobody holds the pad: the game gets everything
        World w;
        w.press(CROSS);
        Game& g = w.tick();
        check("DI-1: a press reaches the game: its state byte", has(g.held, CROSS));
        check("DI-1: and its press event", has(g.press, CROSS) && g.n == 1);
        w.release(CROSS);
        w.tick();
        check("DI-1: and its release event", has(w.g.release, CROSS) && !has(w.g.held, CROSS));
    }
    {   // DI-2: a press made while the pad is held is hidden: state, press and release
        World w;
        w.cap = true;
        w.press(CROSS);
        Game& g = w.tick();
        check("DI-2: held: a new press is hidden from the state", !has(g.held, CROSS));
        check("DI-2: and its press event dropped", !has(g.press, CROSS) && g.n == 0 && w.k.hidden == 1);
        w.tick();
        check("DI-2: still hidden while held", !has(w.g.held, CROSS));
        w.release(CROSS);
        w.tick();
        check("DI-2: its release event dropped", !has(w.g.release, CROSS) && w.g.n == 0 && !has(w.k.hid, CROSS));
    }
    {   // DI-3: a press the game saw stays the game's while the pad is held
        World w;
        w.press(CIRCLE);
        w.tick();
        w.cap = true;
        w.tick();
        check("DI-3: held from before: the game keeps it", has(w.g.held, CIRCLE));
        w.release(CIRCLE);
        w.tick();
        check("DI-3: and gets its release", has(w.g.release, CIRCLE) && !has(w.k.game, CIRCLE));
        w.press(CIRCLE);
        w.tick();
        check("DI-3: pressed again while held: hidden", !has(w.g.held, CIRCLE) && !has(w.g.press, CIRCLE));
    }
    {   // DI-4: a hidden press stays hidden after the pad is given back, until its release
        World w;
        w.cap = true;
        w.press(SQUARE);
        w.tick();
        w.cap = false;
        w.tick();
        check("DI-4: given back: the press made under the hold stays hidden", !has(w.g.held, SQUARE));
        w.tick();
        check("DI-4: still hidden a tick later", !has(w.g.held, SQUARE) && w.g.n == 0);
        w.release(SQUARE);
        w.tick();
        check("DI-4: its release dropped too", !has(w.g.release, SQUARE) && w.g.n == 0);
        w.press(SQUARE);
        w.tick();
        check("DI-4: the next press is the game's", has(w.g.held, SQUARE) && has(w.g.press, SQUARE));
    }
    {   // DI-5: the sticks and sliders read 0 while the pad is held
        World w;
        w.axes[0] = 100; w.axes[1] = -90; w.axes[2] = 55; w.axes[5] = -127; w.axes[6] = 33;
        w.tick();
        check("DI-5: raw sticks reach the game", w.g.axes[0] == 100 && w.g.axes[1] == -90 && w.g.axes[5] == -127);
        w.cap = true;
        w.tick();
        bool zero = true;
        for (int a = 0; a < 8; ++a) zero = zero && w.g.axes[a] == 0;
        check("DI-5: held: every axis 0", zero);
        w.cap = false;
        w.tick();
        check("DI-5: given back: the sticks again", w.g.axes[0] == 100 && w.g.axes[6] == 33);
    }
    {   // DI-6: the POV
        World w;
        w.pov = 9000;
        w.tick();
        check("DI-6: the POV reaches the game", w.g.pov == 9000);
        w.cap = true;
        w.tick();
        check("DI-6: held from before the hold: the game keeps it", w.g.pov == 9000);
        w.pov = 18000;
        w.tick();
        check("DI-6: still off centre: still the game's", w.g.pov == 18000);
        w.pov = PADHOOK_POV_CENTRED;
        w.tick();
        w.pov = 0;
        w.tick();
        check("DI-6: centred and pressed again while held: centred", w.g.pov == PADHOOK_POV_CENTRED);
        w.cap = false;
        w.tick();
        check("DI-6: given back while still pressed: centred until it centres", w.g.pov == PADHOOK_POV_CENTRED);
        w.pov = PADHOOK_POV_CENTRED;
        w.tick();
        w.pov = 27000;
        w.tick();
        check("DI-6: centred, then pressed: the game's again", w.g.pov == 27000);
        World v;
        v.cap = true;
        v.pov = 4500;
        v.tick();
        check("DI-6: taken while centred, pressed under the hold: centred", v.g.pov == PADHOOK_POV_CENTRED);
    }
    {   // DI-7: the chord in one tick: kept from the game
        World w;
        w.chord(CHORD);
        w.press(L3);
        w.press(R3);
        w.tick();
        check("DI-7: both chord buttons in one tick: hidden", !has(w.g.held, L3) && !has(w.g.held, R3) && w.g.n == 0);
        check("DI-7: counted as a chord", w.k.chords == 1 && has(w.k.hid, L3) && has(w.k.hid, R3));
        w.release(L3);
        w.release(R3);
        w.tick();
        check("DI-7: their releases dropped", w.g.n == 0 && w.g.presses[L3] == 0 && w.g.releases[R3] == 0);
    }
    {   // DI-8: the partner within the time
        World w;
        w.chord(CHORD);
        w.press(L3);
        w.tick();
        check("DI-8: the first chord button waits", !has(w.g.held, L3) && w.g.n == 0 && has(w.k.wait, L3));
        w.press(R3);
        w.tick(60);
        check("DI-8: its partner 60 ms later: the chord, hidden", !has(w.g.held, L3) && !has(w.g.held, R3) && w.k.chords == 1);
        w.tick(200);
        check("DI-8: still hidden past the wait", !has(w.g.held, L3) && w.g.presses[L3] == 0);
    }
    {   // DI-9: a chord button tapped alone: the game gets a press and a release
        World w;
        w.chord(CHORD);
        w.press(L3);
        w.tick();
        w.release(L3);
        w.tick(40);
        check("DI-9: tapped alone: the game gets its press", w.g.presses[L3] == 1 && has(w.g.press, L3));
        check("DI-9: and its release", w.g.releases[L3] == 1);
        w.tick();
        check("DI-9: once", w.g.presses[L3] == 1 && w.g.releases[L3] == 1 && w.k.injected == 1);
    }
    {   // DI-9: up in the state, its release still behind a full buffer
        World w;
        w.chord(CHORD);
        w.press(L3);
        w.tick();
        w.asked = 1;
        w.other(0x00, 3);                    // a stick event first: the release waits in the buffer
        w.release(L3);
        w.tick();
        check("DI-9: the release not read yet: the tap waits, nothing held", w.g.presses[L3] == 0 && !has(w.g.held, L3)
            && has(w.k.tap, L3));
        w.asked = ASKED;
        w.tick();
        check("DI-9: its release read: the game gets a press, then that release", w.g.presses[L3] == 1 && w.g.releases[L3] == 1
            && w.g.n == 2 && w.k.tap == 0 && w.k.swallow == 0);
        w.tick();
        check("DI-9: and nothing more", w.g.presses[L3] == 1 && w.g.releases[L3] == 1 && w.g.n == 0);
    }
    {   // DI-9: up in the state and the buffer empty: the release was lost, the game still gets the tap
        World w;
        w.chord(CHORD);
        w.press(L3);
        w.tick();
        w.lose(L3);
        w.tick();
        check("DI-9: a lost release: the game gets the tap", w.g.presses[L3] == 1 && w.g.releases[L3] == 1 && w.k.swallow == 0);
    }
    {   // DI-10: a tap shorter than a tick
        World w;
        w.chord(CHORD);
        w.press(R3);
        w.release(R3);
        w.tick();
        check("DI-10: a tap inside one tick: a press then a release", w.g.presses[R3] == 1 && w.g.releases[R3] == 1 && w.g.n == 2);
    }
    {   // DI-11: a chord button held alone too long: the game's then
        World w;
        w.chord(CHORD);
        w.press(L3);
        w.tick();
        w.tick(100);
        check("DI-11: held 100 ms: still waits", !has(w.g.held, L3) && w.g.presses[L3] == 0);
        w.tick(100);
        check("DI-11: past 150 ms: the game gets its state", has(w.g.held, L3));
        check("DI-11: and a press event", w.g.presses[L3] == 1 && has(w.k.game, L3));
        w.press(R3);
        w.tick();
        check("DI-11: the partner after that is no chord", has(w.g.held, R3) && w.k.chords == 0);
        w.release(L3);
        w.tick();
        check("DI-11: the late button's release reaches the game", w.g.releases[L3] == 1);
    }
    {   // DI-12: released after the wait ran out but before its press event went in
        World w;
        w.chord(CHORD);
        w.press(L3);
        w.tick();
        w.asked = 1;
        for (int i = 0; i < 3; ++i) w.other(0x00, 5);   // the buffer full of stick events: no room
        w.tick(200);
        check("DI-12: no room: the late press waits", w.g.presses[L3] == 0 && has(w.k.late, L3));
        w.lose(L3);
        w.asked = ASKED;
        w.tick();
        check("DI-12: released meanwhile: the game gets the tap, press then release", w.g.presses[L3] == 1 && w.g.releases[L3] == 1);
    }
    {   // DI-13: no wait when the partner is already the game's
        World w;
        w.press(R3);
        w.tick();
        w.chord(CHORD);
        w.press(L3);
        w.tick();
        check("DI-13: the partner already held by the game: no wait", has(w.g.held, L3) && w.g.presses[L3] == 1 && w.k.wait == 0);
    }
    {   // DI-14: never more events than asked
        World w;
        w.chord(CHORD);
        w.asked = 2;
        w.press(L3);
        w.tick();
        w.release(L3);
        w.press(CROSS);
        w.tick(30);                          // the release and Cross: 2 events in, a press and a release owed
        check("DI-14: never more than asked", w.g.n <= 2);
        check("DI-14: Cross not lost", w.g.presses[CROSS] == 1);
        w.tick();
        check("DI-14: the tap at the next read", w.g.presses[L3] == 1 && w.g.releases[L3] == 1 && w.g.n == 2);
    }
    {   // DI-15: a lost release
        World w;
        w.cap = true;
        w.press(CROSS);
        w.tick();
        w.lose(CROSS);
        w.tick();
        check("DI-15: up in the state and the buffer empty: the hidden press is over", !has(w.k.hid, CROSS) && w.k.ended == 1);
        w.cap = false;
        w.press(CROSS);
        w.tick();
        check("DI-15: the next press is the game's", has(w.g.held, CROSS) && has(w.g.press, CROSS));
        World v;
        v.cap = true;
        v.press(CROSS);
        v.tick();
        v.asked = 1;
        v.lose(CROSS);
        v.other(0x00, 1);
        v.other(0x04, 1);
        v.tick();
        check("DI-15: the buffer not empty: the press goes on", has(v.k.hid, CROSS));
    }
    {   // DI-18: other events pass
        World w;
        w.cap = true;
        w.other(0x00, 77);                   // lX
        w.other(0x20, 9000);                 // POV
        w.other(0x30 + 40, 0x80);            // button 40, which the client does not map
        w.tick();
        check("DI-18: axis, POV and high button events pass untouched", w.g.n == 3);
    }
}

// ---- the dispatcher, on a bus ----

struct FakeDev { void* const* vtbl; };
LONG g_info_hr = 0;
std::uint32_t g_info_guid1 = 0x09CC054C;         // PID 09CC, VID 054C
bool g_info_hid = true;
int g_info_calls = 0;
extern "C" __attribute__((stdcall)) LONG dev_info(FakeDev*, unsigned char* inst) {
    ++g_info_calls;
    DWORD size;
    std::memcpy(&size, inst, 4);
    if (size != 580) return static_cast<LONG>(0x80070057);
    std::memcpy(inst + 20, &g_info_guid1, 4);
    unsigned char const tail[12] = {0, 0, 0, 0, 0, 0, 'P', 'I', 'D', 'V', 'I', 'D'};
    std::memcpy(inst + 24, tail, 12);
    if (!g_info_hid) inst[30] = 'X';
    std::strcpy(reinterpret_cast<char*>(inst + 300), "Wireless Controller with a long name");
    return g_info_hr;
}
void* g_vtbl[32] = {};
FakeDev g_dev {g_vtbl};

PadBus g_bus {};

void bus_tick(unsigned long buttons, LONG hr = 0, DWORD pov = PADHOOK_POV_CENTRED, LONG lx = 0) {
    using namespace padhook_detail;
    g_fake_tick += 16;
    unsigned char js[PADHOOK_JS_BYTES] = {};
    std::memcpy(js, &lx, 4);
    std::memcpy(js + PADHOOK_JS_POV, &pov, 4);
    for (unsigned i = 0; i < 32; ++i) if (buttons & (1ul << i)) js[PADHOOK_JS_BUTTONS + i] = 0x80;
    di_dispatch(PADHOOK_DI_STATE, hr, js, nullptr, PADHOOK_JS_BYTES, &g_dev);
    PadDiEvent ev[4];
    DWORD n = 0;
    di_dispatch(PADHOOK_DI_EVENTS, 0, ev, &n, ASKED, &g_dev);
}

void dispatcher() {
    using namespace padhook_detail;
    g_vtbl[15] = reinterpret_cast<void*>(&dev_info);
    cached_bus() = &g_bus;
    g_bus.owner_slot = -1;
    // DI-19: the raw record, before the mask
    bus_tick(1ul << CROSS, 0, 9000, 120);
    PadDiState s {};
    check("DI-19: the record is readable", padhook_di_read(&g_bus, &s));
    check("DI-19: raw buttons, POV and sticks", s.seq != 0 && s.buttons == (1ul << CROSS) && s.pov == 9000 && s.axes[0] == 120);
    check("DI-19: the read's result and time", s.hr == 0 && s.tick == g_fake_tick && s.reads == 1);
    unsigned long const seq = s.seq;
    bus_tick(1ul << CROSS, 0, 9000, 120);
    padhook_di_read(&g_bus, &s);
    check("DI-19: nothing changed: the same seq", s.seq == seq && s.reads == 2);
    bus_tick(0, static_cast<LONG>(0x8007001E));
    padhook_di_read(&g_bus, &s);
    check("DI-19: a failed read: nothing held, the error kept", s.seq != seq && s.buttons == 0 && s.hr < 0
        && s.pov == PADHOOK_POV_CENTRED && g_bus.di.press.state_ok == 0);
    // DI-20: the device's maker and name, once per device
    check("DI-20: maker and product from guidProduct", s.vidpid == 0x09CC054C && g_info_calls == 1);
    check("DI-20: the name, cut short", std::strcmp(s.name, "Wireless Controller with a long") == 0);
    bus_tick(0);
    check("DI-20: asked once per device", g_info_calls == 1);
    FakeDev other {g_vtbl};
    g_info_hid = false;
    unsigned char js[PADHOOK_JS_BYTES] = {};
    di_dispatch(PADHOOK_DI_STATE, 0, js, nullptr, PADHOOK_JS_BYTES, &other);
    padhook_di_read(&g_bus, &s);
    check("DI-20: not a HID product id: the maker is not known", g_info_calls == 2 && s.vidpid == 0 && s.device == &other);
    g_info_hid = true;
    // DI-16: a driver that starts gives every held button to the game
    std::memset(&g_bus.di.press, 0, sizeof(g_bus.di.press));
    g_bus.clients[0].occupied = 2;
    g_bus.clients[0].capture = 1;
    g_bus.clients[0].alive_tick = static_cast<LONG>(g_fake_tick);
    unsigned char held[PADHOOK_JS_BYTES] = {};
    held[PADHOOK_JS_BUTTONS + CIRCLE] = 0x80;
    g_fake_tick += 16;
    g_bus.clients[0].alive_tick = static_cast<LONG>(g_fake_tick);
    di_dispatch(PADHOOK_DI_STATE, 0, held, nullptr, PADHOOK_JS_BYTES, &g_dev);
    check("DI-16: held when the driving started: the game's", held[PADHOOK_JS_BUTTONS + CIRCLE] == 0x80 && g_bus.di.press.primed == 1);
    // DI-17: the heartbeat
    g_fake_tick += 300;
    unsigned char js2[PADHOOK_JS_BYTES] = {};
    js2[PADHOOK_JS_BUTTONS + CROSS] = 0x80;
    di_dispatch(PADHOOK_DI_STATE, 0, js2, nullptr, PADHOOK_JS_BYTES, &g_dev);
    check("DI-17: a hold not renewed for 300 ms lapsed: the press is the game's", js2[PADHOOK_JS_BUTTONS + CROSS] == 0x80
        && g_bus.clients[0].capture == 0 && g_bus.lapses == 1);
    // DI-21: a second reader while the first writes: what is hidden stays hidden, nothing else changes
    g_bus.di.press.hid = 1ul << SQUARE;
    g_bus.di.press.was_captured = 1;
    g_bus.di.write_lock = 1;
    unsigned char js3[PADHOOK_JS_BYTES] = {};
    LONG lx = 99;
    std::memcpy(js3, &lx, 4);
    js3[PADHOOK_JS_BUTTONS + SQUARE] = 0x80;
    js3[PADHOOK_JS_BUTTONS + CROSS] = 0x80;
    di_dispatch(PADHOOK_DI_STATE, 0, js3, nullptr, PADHOOK_JS_BYTES, &g_dev);
    LONG lx_out;
    std::memcpy(&lx_out, js3, 4);
    check("DI-21: busy: the hidden button cleared, the rest kept", js3[PADHOOK_JS_BUTTONS + SQUARE] == 0
        && js3[PADHOOK_JS_BUTTONS + CROSS] == 0x80 && lx_out == 0);
    PadDiEvent ev[2] = {{0x30 + SQUARE, 0, 0, 0, 0}, {0x30 + CROSS, 0x80, 0, 0, 0}};
    DWORD n = 2;
    di_dispatch(PADHOOK_DI_EVENTS, 0, ev, &n, ASKED, &g_dev);
    check("DI-21: busy: the hidden button's event dropped", n == 1 && ev[0].ofs == 0x30 + CROSS);
    g_bus.di.write_lock = 0;
    std::memset(&g_bus.clients[0], 0, sizeof(g_bus.clients[0]));
}

// ---- who drives the dispatch ----

void owners() {
    using namespace padhook_detail;
    static PadBus bus {};
    cached_bus() = &bus;
    bus.owner_slot = -1;
    // a bus where only the DirectInput source is live (the XInput route refused, as on a client with none)
    static unsigned char block[16] = {};
    bus.di.dispatch_ptr = reinterpret_cast<padhook_di_fn volatile*>(block);
    bus.di.installed = 1;
    int const a = padhook_register(&bus, "chronicle");
    check("DI-22: a client joins while only DirectInput is live", a >= 0 && bus.clients[a].di_dispatch == &di_dispatch);
    check("DI-22: and drives it", *bus.di.dispatch_ptr == &di_dispatch && bus.di.owner == a + 1 && bus.di.owner_rev == PADHOOK_REV);
    check("DI-22: the driver started: the next state read primes", bus.di.press.primed == 0);
    check("DI-22: the XInput dispatch untouched", bus.owner_slot == -1);
    // a rev 3 client: no DirectInput dispatcher; it never drives the source, and its chord is not the hook's
    PadClient& old = bus.clients[5];
    old.occupied = 2;
    old.rev = 3;
    old.module = g_module;
    old.dispatch = &dispatch;
    old.di_chord = 0xFFFFFFFFul;         // what a rev 3 module never writes: a stale value is ignored
    check("DI-23: a rev 3 client's slot is no chord", di_chords(&bus) == 0);
    padhook_di_chord(&bus, a, CHORD);
    check("DI-23: the rev 4 client's chord", di_chords(&bus) == CHORD);
    // a second rev 4 client, then the driver leaves: the dispatch goes to it, never to the rev 3 one
    int const b = padhook_register(&bus, "hud");
    check("DI-22: a second client of the same revision does not take it", b >= 0 && bus.di.owner == a + 1);
    padhook_unregister(&bus, a, false);
    check("DI-22: the driver leaves: the other rev 4 client drives", bus.di.owner == b + 1 && *bus.di.dispatch_ptr == &di_dispatch);
    check("DI-23: the leaver's chord is gone", di_chords(&bus) == 0 && bus.clients[a].di_dispatch == nullptr);
    padhook_unregister(&bus, b, false);
    check("DI-22: the last leaves: nobody drives (the rev 3 client cannot)", bus.di.owner == 0 && *bus.di.dispatch_ptr == nullptr);
    check("DI-22: the dispatch changed twice (taken, then dropped)", bus.di.handovers == 2);
    // padhook_join: blocked only when both sources are
    static PadBus j {};
    cached_bus() = &j;
    j.owner_slot = -1;
    wrapper_scan().state = 2;            // route 1 refused for good in this module
    wrapper_scan().why = "was not found (the test)";
    j.blocked = 1;
    j.di.blocked = 1;
    check("DI-24: both refused: blocked", padhook_join(&j, "x") == PADHOOK_JOIN_BLOCKED);
    j.di.blocked = 0;
    j.di.installed = 1;
    j.di.dispatch_ptr = reinterpret_cast<padhook_di_fn volatile*>(block);
    check("DI-24: XInput refused, DirectInput live: joined", padhook_join(&j, "x") >= 0);
    std::memset(block, 0, sizeof(block));
}

}  // namespace

// FFXiMain.dll is never loaded here, so the route 1 scan and the DirectInput scan wait; route 0 says why
HMODULE GetModuleHandleA(LPCSTR) { return nullptr; }
void* GetProcAddress(HMODULE, LPCSTR) { return nullptr; }

int main() {
    rules();
    dispatcher();
    owners();
    std::printf("dinput: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
