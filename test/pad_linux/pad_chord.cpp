#include "PadHook/PadHook.h"
#include <cstdio>
DWORD g_fake_tick = 1000;
static int pass = 0, fail = 0;
static void check(const char* n, bool ok) { if (ok) ++pass; else { ++fail; std::printf("FAIL %s\n", n); } }
static PadChord c; static PadMask m; static unsigned pk = 0;
const unsigned L3 = 0x40, R3 = 0x80, A = 0x1000;
// one read of the game: the raw buttons, whether someone holds the pad -> what the game sees
static unsigned rd(unsigned b, bool cap = false, DWORD dt = 16) {
    g_fake_tick += dt;
    PadXState s{}; s.packet = ++pk; s.buttons = (WORD)b;
    unsigned long hide, force;
    padhook_detail::apply_chord(c, m, padhook_detail::held_of(s), cap, g_fake_tick, &hide, &force);
    padhook_detail::apply_mask(m, s, cap, hide, force);
    return s.buttons; }
int main() {
    c.buttons = L3 | R3;
    check("idle", rd(0) == 0);
    check("other buttons pass at once", rd(A) == A); rd(0);
    // the chord, L3 first: the game never sees either
    check("L3 down: held back", rd(L3) == 0);
    check("still waiting", rd(L3) == 0);
    check("R3 joins: the chord, hidden", rd(L3 | R3) == 0);
    check("held: hidden", rd(L3 | R3) == 0);
    check("L3 up, R3 held: hidden", rd(R3) == 0);
    check("released", rd(0) == 0);
    // both in the same read
    check("both at once: hidden", rd(L3 | R3) == 0); rd(0);
    // a tap of L3 alone reaches the game
    check("tap: held back", rd(L3) == 0);
    unsigned t1 = rd(0), t2 = rd(0), t3 = rd(0);
    check("tap: replayed on release", t1 == L3 && t2 == L3);
    check("tap: then released", t3 == 0);
    // held alone past the window: the game gets it then, and keeps it
    rd(L3);
    unsigned late = 0; int reads = 0;
    for (int i = 0; i < 20 && late == 0; ++i) { late = rd(L3); ++reads; }
    check("held alone: reaches the game after the window", late == L3 && reads * 16 > PADHOOK_CHORD_MS - 20 && reads * 16 < PADHOOK_CHORD_MS + 40);
    check("held alone: stays", rd(L3) == L3);
    check("then R3: the game's too (no chord any more)", rd(L3 | R3) == (L3 | R3)); rd(0);
    // nobody asked for a chord: rev 1 behaviour
    c.buttons = 0;
    check("no chord: L3 at once", rd(L3) == L3); rd(0);
    c.buttons = L3 | R3;
    // while the pad is held, nothing is deferred (the mask hides new presses); giving it back with both held: hidden till released
    rd(0, true);
    check("captured: chord hidden", rd(L3 | R3, true) == 0);
    check("given back held: hidden", rd(L3 | R3, false) == 0);
    rd(0); check("after release: a fresh L3 is deferred again", rd(L3) == 0); rd(0); rd(0); rd(0);
    std::printf("chord: %d passed, %d failed\n", pass, fail); return fail ? 1 : 0; }
