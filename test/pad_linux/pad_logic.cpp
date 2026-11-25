#include "PadHook/PadHook.h"
#include <cstdio>
DWORD g_fake_tick = 1000;
static int pass = 0, fail = 0;
static void check(const char* n, bool ok) { if (ok) ++pass; else { ++fail; std::printf("FAIL %s\n", n); } }
static PadMask m; static unsigned pk = 0;
static PadXState feed(unsigned b, bool cap, int lt = 0, int lx = 0) {
    PadXState s{}; s.packet = ++pk; s.buttons = (WORD)b; s.lt = (BYTE)lt; s.lx = (SHORT)lx;
    padhook_detail::apply_mask(m, s, cap); return s; }
int main() {
    const unsigned A=0x1000,B=0x2000,X=0x4000;
    PadXState s = feed(A, false, 0, 1234);
    check("pass", s.buttons==A && s.lx==1234);
    feed(B, false);
    s = feed(B|A, true, 200, 20000);
    check("held before passes", (s.buttons&B)!=0); check("new hidden", (s.buttons&A)==0);
    check("stick 0", s.lx==0); check("trig 0", s.lt==0);
    s = feed(A, true); check("B released", s.buttons==0);
    s = feed(A|B, true); check("B again hidden", s.buttons==0);
    DWORD p1 = s.packet; s = feed(A|B|X, true); check("same packet", s.packet==p1 && s.buttons==0);
    s = feed(A|B|X, false); check("given back hidden", s.buttons==0);
    s = feed(A, false, 0, 500); check("sticks back", s.lx==500 && s.buttons==0);
    feed(0,false); s = feed(A,false); check("pressed again visible", s.buttons==A && s.packet!=p1);
    // trigger held from before capture passes until released
    feed(0,false); s = feed(0,false,200); check("lt visible", s.lt==200);
    s = feed(0,true,200); check("lt held before passes", s.lt==200);
    s = feed(0,true,0); s = feed(0,true,200); check("lt pressed in capture hidden", s.lt==0);
    s = feed(0,false,200); check("lt stays hidden after give back", s.lt==0);
    s = feed(0,false,0); s = feed(0,false,200); check("lt back", s.lt==200);
    // heartbeat
    static PadBus bus{}; bus.clients[0].occupied=2; bus.clients[0].capture=1; bus.clients[0].alive_tick=1000;
    g_fake_tick=1100; check("fresh", padhook_detail::captured(&bus, g_fake_tick));
    g_fake_tick=1300; check("lapsed", !padhook_detail::captured(&bus, g_fake_tick)); check("lapse counted", bus.lapses==1 && bus.clients[0].capture==0);
    // thunk bytes
    unsigned char t[16]; padhook_detail::thunk_bytes(0x10000000u, 0x20000010u, t);
    FILE* f = std::fopen("thunk.bin","wb"); std::fwrite(t,1,16,f); std::fclose(f);
    std::printf("logic: %d passed, %d failed\n", pass, fail); return fail?1:0; }
