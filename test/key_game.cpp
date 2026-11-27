// key_game.cpp - a stand-in for the client's FFXiMain.dll, for key_test.exe (built as game/FFXiMain.dll). Its input
// update carries the client's two keyboard read sites with the exact bytes of the 2026-11-02 build, at the client's
// 8-byte alignment (the state read's cmp at 8n+1, the event read's at 8n+0). After each cmp it records eax and the
// flags, as the client's own error checks would read them. Built with -DGAME_NOMATCH as game_nomatch/FFXiMain.dll,
// both cmps use another encoding of the same instruction (81 F8 imm32), so neither signature matches (a client
// update).
//
// The exports do what the client does: game_update(input) runs the update on an input object whose device is at
// +0x88, with the state at +0x98, the events at +0x79C and the count at +0x8DC.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern "C" {
__attribute__((used)) DWORD g_rec[4] = {0, 0, 0, 0};    // eax and flags after the state read's cmp, then the event read's
void __cdecl game_update_asm(void* input);
}

#ifndef GAME_NOMATCH
#define CMP_HR ".byte 0x3D,0xAA,0x00,0x07,0x80\n"
#else
#define CMP_HR ".byte 0x81,0xF8,0xAA,0x00,0x07,0x80\n"
#endif

asm(".text\n.balign 16\n"
    ".globl _game_update_asm\n"
    "_game_update_asm:\n"
    "    pushl %ebp\n    pushl %ebx\n    pushl %esi\n    pushl %edi\n"
    "    subl $0x60, %esp\n"
    "    movl 0x74(%esp), %esi\n"
    "    movl $0, 0x40(%esp)\n"
    "    .balign 8, 0x90\n    nop\n"
    // the state read, 0x10005A61 in the client
    "    .byte 0x8B,0x86,0x88,0x00,0x00,0x00\n"          // mov eax, [esi+0x88]
    "    .byte 0xDB,0x44,0x24,0x40,0x85,0xC0\n"          // fild [esp+0x40] / test eax, eax
    "    .byte 0xD9,0x5C,0x24,0x40\n"                    // fstp [esp+0x40]
    "    .byte 0x0F,0x84\n    .long 9f - . - 4\n"        // jz: no device
    "    .byte 0x8B,0x08,0x8D,0xBE,0x98,0x00,0x00,0x00,0x57\n"
    "    .byte 0x68,0x00,0x01,0x00,0x00,0x50\n"
    "    .byte 0xFF,0x51,0x24\n"                         // call [ecx+0x24]: GetDeviceState
    CMP_HR
    "    movl %eax, _g_rec\n    pushfl\n    popl _g_rec+4\n"
    "    .balign 8, 0x90\n    nop\n    nop\n    nop\n"
    // the event read, 0x10005AFB in the client
    "    .byte 0x8B,0x86,0x88,0x00,0x00,0x00\n"          // mov eax, [esi+0x88]
    "    .byte 0x8D,0xAE,0xDC,0x08,0x00,0x00\n"          // lea ebp, [esi+0x8DC]
    "    .byte 0x8D,0x9E,0x9C,0x07,0x00,0x00\n"          // lea ebx, [esi+0x79C]
    "    .byte 0x6A,0x00,0x55\n"
    "    .byte 0xC7,0x45,0x00,0x10,0x00,0x00,0x00\n"     // mov dword [ebp], 16
    "    .byte 0x8B,0x10,0x53,0x6A,0x14,0x50\n"
    "    .byte 0xFF,0x52,0x28\n"                         // call [edx+0x28]: GetDeviceData
    CMP_HR
    "    movl %eax, _g_rec+8\n    pushfl\n    popl _g_rec+12\n"
    "9:  addl $0x60, %esp\n"
    "    popl %edi\n    popl %esi\n    popl %ebx\n    popl %ebp\n"
    "    ret\n");

extern "C" __declspec(dllexport) void __cdecl game_update(void* input) { game_update_asm(input); }
extern "C" __declspec(dllexport) DWORD const* __cdecl game_rec() { return g_rec; }
extern "C" __declspec(dllexport) void* __cdecl game_code() { return reinterpret_cast<void*>(&game_update_asm); }

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
