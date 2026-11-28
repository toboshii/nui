// binds_test.cpp - KeyBinds.h in a Windows process, against the stand-in client (binds_game.cpp).
//   binds_test.exe main      before the client loads: not read, and asked again later. After: one copy with the
//                            stand-in's values, a change the client makes shows in the next copy, the mode and its
//                            object, and the stand-in's code bytes are unchanged (nothing is patched)
//   binds_test.exe nomatch   a client whose keyboard mode test has another byte: refused for good, and the reason
//                            names it
#include "KeyBinds/KeyBinds.h"

#include <cstdio>
#include <cstring>

static int g_pass = 0, g_fail = 0;
#define CHECK(name, cond)                                                       \
    do {                                                                        \
        if (cond) ++g_pass;                                                     \
        else { ++g_fail; std::printf("FAIL %s (line %d)\n", name, __LINE__); }  \
    } while (0)

using keybinds::BindRead;
typedef void(__cdecl* set_fn)(int, int, unsigned long);

static unsigned long text_sum(HMODULE m) {
    auto const base = reinterpret_cast<unsigned char const*>(m);
    auto const* nt = reinterpret_cast<IMAGE_NT_HEADERS32 const*>(base + reinterpret_cast<IMAGE_DOS_HEADER const*>(base)->e_lfanew);
    auto const* s = IMAGE_FIRST_SECTION(nt);
    unsigned long sum = 0;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s)
        if (s->Characteristics & IMAGE_SCN_MEM_EXECUTE)
            for (DWORD k = 0; k < s->Misc.VirtualSize; ++k) sum = sum * 31 + base[s->VirtualAddress + k];
    return sum;
}

static int run_main() {
    static BindRead r;
    char const* why = nullptr;
    CHECK("BIND-W1 no client: not read", !keybinds::read_binds(r, &why) && why && std::strstr(why, "not loaded"));
    CHECK("BIND-W1 no client: asked again later", keybinds::bind_find().state == 0);
    HMODULE game = LoadLibraryA("game\\FFXiMain.dll");
    if (!game) { std::printf("FAIL cannot load game\\FFXiMain.dll\n"); return 1; }
    auto set = reinterpret_cast<set_fn>(reinterpret_cast<void*>(GetProcAddress(game, "game_set")));
    unsigned long const before = text_sum(game);
    set(3, 0, 1);
    set(4, 0, 2);
    bool const ok = keybinds::read_binds(r, &why);
    CHECK("BIND-W2 the client: read", ok);
    if (!ok) { std::printf("    why: %s\n", why ? why : "?"); return 1; }
    CHECK("BIND-W3 the bindings", r.actions[0] == 0x1015 && r.actions[1] == 0x001C && r.actions[2] == 0x009C);
    CHECK("BIND-W3 not the pad table", r.actions[187 * 3 + 2] == 0);
    CHECK("BIND-W4 the categories", r.categories[0] == 0xFFFF);
    CHECK("BIND-W5 one move, up to the first with no bits", r.n_moves == 1 && r.moves[0] == 0x040011);
    CHECK("BIND-W6 the mode and the layout", r.mode == 1 && r.layout == 2);
    set(0, 3 * 20 + 1, 0x8025);
    set(2, 1, 0x060825);
    set(3, 0, 2);
    keybinds::read_binds(r, &why);
    CHECK("BIND-W7 a change shows in the next copy", r.actions[61] == 0x8025 && r.n_moves == 2 && r.moves[1] == 0x060825 && r.mode == 2);
    set(5, 0, 0);
    keybinds::read_binds(r, &why);
    CHECK("BIND-W8 no mode object yet: unknown", r.mode == -1 && r.layout == 2);
    set(3, 0, 7); set(5, 0, 1);
    keybinds::read_binds(r, &why);
    CHECK("BIND-W8 a mode out of range: unknown", r.mode == -1);
    CHECK("BIND-W9 nothing patched", text_sum(game) == before);
    CHECK("BIND-W10 nosig: the not-found path", !keybinds::read_binds(r, &why, true) && std::strstr(why, "were not found"));
    return 0;
}

static int run_nomatch() {
    static BindRead r;
    char const* why = nullptr;
    HMODULE game = LoadLibraryA("game_nomatch\\FFXiMain.dll");
    if (!game) { std::printf("FAIL cannot load game_nomatch\\FFXiMain.dll\n"); return 1; }
    CHECK("BIND-W11 another client: not read", !keybinds::read_binds(r, &why));
    CHECK("BIND-W11 the reason names the keyboard mode test", why && std::strstr(why, "keyboard mode test"));
    CHECK("BIND-W11 refused for good", keybinds::bind_find().state == 2);
    return 0;
}

int main(int argc, char** argv) {
    int const rc = argc > 1 && std::strcmp(argv[1], "nomatch") == 0 ? run_nomatch() : run_main();
    std::printf("binds_test %s: %d passed, %d failed\n", argc > 1 ? argv[1] : "main", g_pass, g_fail);
    return rc || g_fail ? 1 : 0;
}
