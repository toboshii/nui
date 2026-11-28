// binds_game.cpp - a stand-in for the client's FFXiMain.dll, for binds_test.exe (built as binds/game/FFXiMain.dll).
// Its code holds the client's eight signature sites with the 2026-11-02 bytes, and the linker fills in the
// operands with this DLL's own tables. Its .data holds the tables the way the client lays them out: the pad table
// right after 188 x 3 bindings, and the input mask right after the movement table. Built with -DGAME_NOMATCH as
// binds/game_nomatch/FFXiMain.dll, the keyboard mode test has one other byte (a client update), so no table is read.
//
// game_set(kind, index, value) changes a table as the client would at run time: 0 a binding WORD, 1 a category,
// 2 a movement entry (DIK | rule << 8 | context << 16, bits 1), 3 the mode, 4 the layout, 5 the mode object's
// pointer (0: none yet).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern "C" {
struct Actions { unsigned short act[188 * 3]; unsigned long pad[376]; };
struct Moves { unsigned char e[43 * 16]; unsigned long mask; };
struct Obj { unsigned char b[0x48]; };
__attribute__((used)) Actions g_tab = {{0x1015, 0x001C, 0x009C}, {1}};
__attribute__((used)) unsigned short g_cat[188] = {0xFFFF};
__attribute__((used)) Moves g_moves = {{0x11, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0x80}, 1};
__attribute__((used)) Obj g_mode_obj = {{1}};
__attribute__((used)) Obj g_layout_obj = {{1}};
__attribute__((used)) Obj* g_mode_slot = &g_mode_obj;
__attribute__((used)) Obj* g_layout_slot = &g_layout_obj;
}

#ifndef GAME_NOMATCH
#define MODE_SUB ".byte 0x83,0xE8,0x00\n"
#else
#define MODE_SUB ".byte 0x83,0xE8,0x01\n"
#endif

// Never called: only the bytes matter.
asm(".text\n.balign 16\n"
    "_binds_sites:\n"
    "    .byte 0x0F,0xBF,0x44,0x24,0x04,0x8D,0x04,0x40,0x8A,0x04,0x45\n    .long _g_tab\n    .byte 0xC3\n"
    "    .byte 0x66,0x8B,0x04,0x55\n    .long _g_tab\n    .byte 0x8D,0x34,0x55\n    .long _g_tab\n"
    "    .byte 0x8B,0x04,0xFD\n    .long _g_tab+0x468\n    .byte 0x85,0xC0\n"
    "    .byte 0x66,0x8B,0x04,0x7D\n    .long _g_cat\n    .byte 0x66,0x85,0xC0\n"
    "    .byte 0x0F,0xBE,0x44,0x24,0x04,0xC1,0xE0,0x04,0x8A,0x80\n    .long _g_moves\n    .byte 0xC3\n"
    "    .byte 0x8B,0x0D\n    .long _g_moves+8\n    .byte 0x88,0x44,0x24,0x0A,0x0B,0x0D\n    .long _g_moves+12\n"
    "    .byte 0x0F,0x84,0xA3,0x01,0x00,0x00,0x57,0xBF\n    .long _g_moves+8\n    .byte 0xBE\n    .long _g_moves+1\n"
    "    .byte 0xA1\n    .long _g_mode_slot\n    .byte 0x85,0xC0,0x74,0x11,0x8B,0x40,0x44\n" MODE_SUB
    "    .byte 0x74,0x09,0x48,0x74,0x03,0x48,0x75,0x03,0xB0,0x01,0xC3\n"
    "    .byte 0xA1\n    .long _g_layout_slot\n    .byte 0x8B,0x40,0x08,0x83,0xE8,0x02\n"
    "    .byte 0xCC,0xCC,0xA1\n    .long _g_layout_slot\n    .byte 0x8B,0x40,0x08,0x83,0xE8,0x02,0xC3\n");

extern "C" __declspec(dllexport) void __cdecl game_set(int kind, int index, unsigned long value) {
    switch (kind) {
    case 0: g_tab.act[index] = static_cast<unsigned short>(value); break;
    case 1: g_cat[index] = static_cast<unsigned short>(value); break;
    case 2: {
        unsigned char* e = g_moves.e + index * 16;
        e[0] = value & 0xFF; e[1] = (value >> 8) & 0xFF; e[2] = (value >> 16) & 0xFF;
        e[8] = 1;
        break;
    }
    case 3: *reinterpret_cast<unsigned long*>(g_mode_obj.b + 0x44) = value; break;
    case 4: *reinterpret_cast<unsigned long*>(g_layout_obj.b + 0x08) = value; break;
    case 5: g_mode_slot = value ? &g_mode_obj : nullptr; break;
    }
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
