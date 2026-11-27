// key_logic.cpp - KeyHook's press rules on 32-bit Linux: apply_state, apply_events and the heartbeat, called the way
// the dispatcher calls them (the state read first in each tick, then the event read). Each rule has named checks
// (KEY-1 ...), and each fails on a build without its rule (run.sh --self-test).
#include "KeyHook/KeyHook.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

DWORD g_fake_tick = 1000;

namespace {

int g_pass = 0, g_fail = 0;
void check(char const* name, bool ok) {
    if (ok) ++g_pass;
    else { ++g_fail; std::printf("FAIL %s\n", name); }
}

using keyhook_detail::apply_events;
using keyhook_detail::apply_state;
using keyhook_detail::hide_mask;

constexpr unsigned UP = 0xC8, DOWN = 0xD0, ENTER = 0x1C, K = 0x25, W = 0x11, LCTRL = 0x1D;
constexpr DWORD ASKED = 16;

// one tick of the client: which keys are down at the state read, and the events the event read returns
struct Tick {
    unsigned char state[256];
    KeyEvent ev[64];
    DWORD n;
};

struct World {
    KeyState k {};
    unsigned long mask[KEYHOOK_WORDS] {};
    unsigned char down[256] {};          // the real keyboard
    KeyEvent pending[256];               // the device's buffer
    DWORD npending = 0;
    DWORD seq = 0;
    DWORD backlog = ASKED;               // events the client asks for

    void hide(std::initializer_list<unsigned> keys) {
        for (int w = 0; w < KEYHOOK_WORDS; ++w) mask[w] = 0;
        for (unsigned c : keys) mask[c >> 5] |= 1ul << (c & 31);
    }
    void press(unsigned c) { down[c] = 0x80; pending[npending++] = KeyEvent {c, 0x80, g_fake_tick, ++seq, 0}; }
    void release(unsigned c) { down[c] = 0; pending[npending++] = KeyEvent {c, 0, g_fake_tick, ++seq, 0}; }
    void lose(unsigned c) { down[c] = 0; }           // a release the buffer never got (focus lost, overflow)

    // the state read, then the event read, as the client makes them; out: what the game got
    Tick tick(bool state_too = true) {
        Tick t {};
        std::memcpy(t.state, down, 256);
        if (state_too) apply_state(k, t.state, mask);
        DWORD const n = npending < backlog ? npending : backlog;
        std::memcpy(t.ev, pending, n * sizeof(KeyEvent));
        std::memmove(pending, pending + n, (npending - n) * sizeof(KeyEvent));
        npending -= n;
        t.n = n;
        apply_events(k, t.ev, &t.n, ASKED, mask);
        g_fake_tick += 16;
        return t;
    }
};

bool st(Tick const& t, unsigned c) { return (t.state[c] & 0x80) != 0; }
int presses(Tick const& t, unsigned c) {
    int n = 0;
    for (DWORD j = 0; j < t.n; ++j) if (t.ev[j].ofs == c && (t.ev[j].data & 0x80)) ++n;
    return n;
}
int events_of(Tick const& t, unsigned c) {
    int n = 0;
    for (DWORD j = 0; j < t.n; ++j) if (t.ev[j].ofs == c) ++n;
    return n;
}
// what the client makes of a tick: the key counts as down when its state byte has 0x80 or a press came
bool game_down(Tick const& t, unsigned c) { return st(t, c) || presses(t, c) > 0; }

void rules() {
    // KEY-1: nothing hidden, every key the game's
    {
        World w;
        w.press(UP);
        Tick t = w.tick();
        check("KEY-1: no mask: the state byte stays", st(t, UP));
        check("KEY-1: no mask: the press event stays", presses(t, UP) == 1 && t.n == 1);
        w.release(UP);
        t = w.tick();
        check("KEY-1: no mask: the release stays", events_of(t, UP) == 1 && !st(t, UP));
    }
    // KEY-2: a key in the mask at its press is hidden from both reads, and its release too
    {
        World w;
        w.hide({UP, DOWN});
        w.press(W);
        w.press(UP);
        w.press(ENTER);
        Tick t = w.tick();
        check("KEY-2: hidden: the state byte cleared", !st(t, UP));
        check("KEY-2: hidden: the press event dropped", presses(t, UP) == 0);
        check("KEY-2: the others kept, in order", t.n == 2 && t.ev[0].ofs == W && t.ev[1].ofs == ENTER && st(t, W) && st(t, ENTER));
        check("KEY-2: counted", w.k.hidden == 1 && w.k.dropped == 1);
        t = w.tick();
        check("KEY-2: still held: still hidden", !st(t, UP));
        w.release(UP);
        t = w.tick();
        check("KEY-2: the release dropped", events_of(t, UP) == 0 && t.n == 0);
        check("KEY-2: the press is over", (w.k.hid[UP >> 5] & (1ul << (UP & 31))) == 0);
    }
    // KEY-3: a press the game saw stays the game's until its release
    {
        World w;
        w.press(UP);
        Tick t = w.tick();
        check("KEY-3: before the mask: the game's", game_down(t, UP));
        w.hide({UP});
        t = w.tick();
        check("KEY-3: in the mask while held: still the game's", st(t, UP));
        t = w.tick();
        check("KEY-3: a tick later: still the game's", st(t, UP));
        w.release(UP);
        t = w.tick();
        check("KEY-3: its release reaches the game", events_of(t, UP) == 1);
        w.press(UP);
        t = w.tick();
        check("KEY-3: the next press is hidden", !game_down(t, UP));
    }
    // KEY-4: a hidden press stays hidden after the key leaves the mask, until its release
    {
        World w;
        w.hide({DOWN});
        w.press(DOWN);
        Tick t = w.tick();
        check("KEY-4: hidden at its press", !game_down(t, DOWN));
        w.hide({});
        t = w.tick();
        check("KEY-4: out of the mask, still held: still hidden", !st(t, DOWN));
        t = w.tick();
        check("KEY-4: a tick later: still hidden", !st(t, DOWN));
        w.release(DOWN);
        t = w.tick();
        check("KEY-4: its release is dropped too", events_of(t, DOWN) == 0);
        w.press(DOWN);
        t = w.tick();
        check("KEY-4: the next press is the game's", game_down(t, DOWN));
    }
    // KEY-5: a tap inside one tick (the state never shows it): the events alone decide
    {
        World w;
        w.hide({ENTER});
        w.press(ENTER);
        w.release(ENTER);
        w.press(W);
        w.release(W);
        Tick t = w.tick();
        check("KEY-5: a hidden tap: both events dropped", events_of(t, ENTER) == 0);
        check("KEY-5: another tap kept", events_of(t, W) == 2 && t.n == 2);
        check("KEY-5: the tap is over", w.k.hid[0] == 0 && w.k.saw[0] == 0);
    }
    // KEY-6: a press between the two reads (the state read shows it up, the event read has its press)
    {
        World w;
        w.hide({UP});
        Tick t {};
        std::memcpy(t.state, w.down, 256);
        apply_state(w.k, t.state, w.mask);       // the state read: UP is up
        w.press(UP);                              // pressed now
        t.ev[0] = w.pending[0];
        t.n = 1;
        w.npending = 0;
        apply_events(w.k, t.ev, &t.n, ASKED, w.mask);
        check("KEY-6: decided at the event read: hidden", t.n == 0);
        check("KEY-6: not ended by the empty buffer (its press came last)", (w.k.hid[UP >> 5] & (1ul << (UP & 31))) != 0);
        t = w.tick();
        check("KEY-6: the next state read clears it", !st(t, UP));
    }
    // KEY-7: a release that never came: the press ends once the state shows the key up and the buffer is empty
    {
        World w;
        w.hide({UP});
        w.press(UP);
        w.tick();
        w.lose(UP);
        w.hide({});
        Tick t = w.tick();
        check("KEY-7: up in the state, buffer empty: the press ended", (w.k.hid[UP >> 5] & (1ul << (UP & 31))) == 0 && w.k.ended == 1);
        w.press(UP);
        t = w.tick();
        check("KEY-7: the next press is decided anew (the game's)", game_down(t, UP));
    }
    // KEY-8: with a backlog (the client got as many events as it asked for), the press does not end
    {
        World w;
        w.hide({UP});
        w.press(UP);
        w.tick();
        w.lose(UP);
        for (int i = 0; i < 20; ++i) { w.press(0x30 + (i % 8)); w.release(0x30 + (i % 8)); }
        w.tick();
        check("KEY-8: a full read: the hidden press is kept", (w.k.hid[UP >> 5] & (1ul << (UP & 31))) != 0);
        w.hide({});
        while (w.npending) w.tick();
        w.tick();
        check("KEY-8: drained: then it ends", (w.k.hid[UP >> 5] & (1ul << (UP & 31))) == 0);
    }
    // KEY-9: a failed state read leaves the record as it was, and no press ends on it
    {
        World w;
        w.hide({UP});
        w.press(UP);
        w.tick();
        InterlockedExchange(&w.k.state_ok, 0);           // what the dispatcher does on a failed state read
        w.lose(UP);
        Tick t = w.tick(false);
        (void)t;
        check("KEY-9: no state read: the press does not end", (w.k.hid[UP >> 5] & (1ul << (UP & 31))) != 0);
        w.tick();
        check("KEY-9: the next good read ends it", (w.k.hid[UP >> 5] & (1ul << (UP & 31))) == 0);
    }
    // KEY-10: events that are not keys are kept, and a count above the one asked for is cut to it
    {
        World w;
        w.hide({UP});
        Tick t {};
        t.ev[0] = KeyEvent {0x123, 0x80, 0, 1, 0};
        t.ev[1] = KeyEvent {UP, 0x80, 0, 2, 0};
        t.n = 2;
        apply_events(w.k, t.ev, &t.n, ASKED, w.mask);
        check("KEY-10: an offset past 255 kept", t.n == 1 && t.ev[0].ofs == 0x123);
        Tick u {};
        for (int i = 0; i < 20; ++i) u.ev[i] = KeyEvent {static_cast<DWORD>(0x10 + i), 0x80, 0, 0, 0};
        u.n = 20;
        apply_events(w.k, u.ev, &u.n, ASKED, w.mask);
        check("KEY-10: no more than asked for", u.n == ASKED);
    }
    // KEY-11: modifiers and keys out of the mask are never touched
    {
        World w;
        w.hide({UP});
        w.press(LCTRL);
        w.press(K);
        Tick t = w.tick();
        check("KEY-11: Ctrl and K reach the game", st(t, LCTRL) && st(t, K) && t.n == 2);
    }
}

// the heartbeat and the union of clients, through hide_mask as the dispatcher calls it
void heartbeat() {
    static KeyBus bus {};
    KeyClient& a = bus.clients[0];
    KeyClient& b = bus.clients[1];
    a.occupied = b.occupied = 2;
    a.hide[UP >> 5] = 1ul << (UP & 31);
    b.hide[ENTER >> 5] = 1ul << (ENTER & 31);
    a.hiding = b.hiding = 1;
    a.alive_tick = b.alive_tick = static_cast<LONG>(g_fake_tick);
    unsigned long m[KEYHOOK_WORDS];
    bool any = hide_mask(&bus, g_fake_tick + 10, m);
    check("KEY-12: two clients: the union", any && (m[UP >> 5] & (1ul << (UP & 31))) && (m[ENTER >> 5] & (1ul << (ENTER & 31))));
    b.alive_tick = static_cast<LONG>(g_fake_tick + 200);
    any = hide_mask(&bus, g_fake_tick + KEYHOOK_HEARTBEAT_MS + 100, m);
    check("KEY-13: a stale client lapses", any && !(m[UP >> 5] & (1ul << (UP & 31))) && (m[ENTER >> 5] & (1ul << (ENTER & 31))));
    check("KEY-13: once, and counted", a.hiding == 0 && bus.lapses == 1);
    hide_mask(&bus, g_fake_tick + KEYHOOK_HEARTBEAT_MS + 120, m);
    check("KEY-13: not counted twice", bus.lapses == 1);
    b.hiding = 0;
    any = hide_mask(&bus, g_fake_tick + 300, m);
    check("KEY-13: nobody hides: nothing", !any && m[ENTER >> 5] == 0);
    // a lapse leaves a hidden press hidden: the rule is the record's, not the mask's
    KeyState k {};
    unsigned long hide_up[KEYHOOK_WORDS] = {};
    hide_up[UP >> 5] = 1ul << (UP & 31);
    unsigned char s[256] = {};
    s[UP] = 0x80;
    apply_state(k, s, hide_up);
    unsigned long none[KEYHOOK_WORDS] = {};
    s[UP] = 0x80;
    apply_state(k, s, none);
    check("KEY-13: after a lapse a hidden press stays hidden", s[UP] == 0);
}

// the dispatcher itself: the kinds, a failed read, the device it was given, and nobody's bus
void dispatcher() {
    using keyhook_detail::dispatch;
    static KeyBus bus {};
    keyhook_detail::cached_bus() = &bus;
    KeyClient& a = bus.clients[0];
    a.occupied = 2;
    a.hide[UP >> 5] = 1ul << (UP & 31);
    a.hiding = 1;
    a.alive_tick = static_cast<LONG>(g_fake_tick);
    unsigned char s[256] = {};
    s[UP] = 0x80;
    int dev = 0;
    bus.keys.primed = 1;                                  // driving already: Up is a new press
    dispatch(KEYHOOK_READ_STATE, 0, s, nullptr, 0, &dev);
    check("KEY-14: the dispatcher hides from the state", s[UP] == 0 && bus.calls == 1 && bus.keys.device == &dev);
    KeyEvent ev[2] = {{UP, 0x80, 0, 1, 0}, {W, 0x80, 0, 2, 0}};
    DWORD n = 2;
    dispatch(KEYHOOK_READ_EVENTS, 1 /* DI_BUFFEROVERFLOW */, ev, &n, ASKED, &dev);
    check("KEY-14: and from the events, on DI_BUFFEROVERFLOW too", n == 1 && ev[0].ofs == W);
    n = 2;
    ev[0] = KeyEvent {UP, 0x80, 0, 3, 0};
    dispatch(KEYHOOK_READ_EVENTS, static_cast<LONG>(0x8007000C), ev, &n, ASKED, &dev);
    check("KEY-14: a failed event read is left alone", n == 2);
    s[UP] = 0x80;
    dispatch(KEYHOOK_READ_STATE, static_cast<LONG>(0x8007001E), s, nullptr, 0, &dev);
    check("KEY-14: a failed state read is left alone", s[UP] == 0x80 && bus.keys.state_ok == 0);
    s[UP] = 0x80;
    dispatch(7, 0, s, nullptr, 0, &dev);
    check("KEY-14: an unknown kind is left alone", s[UP] == 0x80);
    // a driver starts while a key is held (it went down unseen): that press stays the game's
    std::memset(&bus.keys, 0, sizeof(bus.keys));
    a.alive_tick = static_cast<LONG>(g_fake_tick);
    unsigned char held[256] = {};
    held[UP] = 0x80;
    dispatch(KEYHOOK_READ_STATE, 0, held, nullptr, 0, &dev);
    check("KEY-15: held when the driving started: the game's", held[UP] == 0x80 && bus.keys.primed == 1);
    held[UP] = 0x80;
    dispatch(KEYHOOK_READ_STATE, 0, held, nullptr, 0, &dev);
    check("KEY-15: and stays the game's until released", held[UP] == 0x80);
    held[UP] = 0;
    dispatch(KEYHOOK_READ_STATE, 0, held, nullptr, 0, &dev);
    KeyEvent none[1];
    DWORD zero = 0;
    dispatch(KEYHOOK_READ_EVENTS, 0, none, &zero, ASKED, &dev);    // the tick's event read: the buffer is empty
    held[UP] = 0x80;
    dispatch(KEYHOOK_READ_STATE, 0, held, nullptr, 0, &dev);
    check("KEY-15: the next press is hidden", held[UP] == 0);
    // set_owner re-primes only when nobody drove before
    keyhook_fn slot_ptr = nullptr;
    bus.dispatch_ptr = &slot_ptr;
    a.dispatch = &dispatch;
    bus.keys.primed = 1;
    keyhook_detail::set_owner(&bus, 0);
    check("KEY-15: a driver after nobody: primed again", bus.keys.primed == 0 && slot_ptr == &dispatch);
    bus.keys.primed = 1;
    keyhook_detail::set_owner(&bus, 0);
    check("KEY-15: a handover between drivers: not", bus.keys.primed == 1);
    bus.dispatch_ptr = nullptr;
    // another thread inside: only what is hidden is hidden, nothing recorded
    bus.write_lock = 1;
    unsigned long const reads = bus.keys.state_reads;
    s[UP] = 0x80;
    s[W] = 0x80;
    dispatch(KEYHOOK_READ_STATE, 0, s, nullptr, 0, &dev);
    check("KEY-14: two readers at once: the hidden press stays hidden", s[UP] == 0 && s[W] == 0x80 && bus.keys.state_reads == reads);
    bus.write_lock = 0;
    keyhook_detail::cached_bus() = nullptr;
    s[UP] = 0x80;
    dispatch(KEYHOOK_READ_STATE, 0, s, nullptr, 0, &dev);
    check("KEY-14: no bus: nothing changes", s[UP] == 0x80);
}

}  // namespace

int main() {
    rules();
    heartbeat();
    dispatcher();
    std::printf("key_logic: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
