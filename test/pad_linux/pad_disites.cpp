// pad_disites.cpp - PadHook's DirectInput sites (revision 4) on 32-bit Linux: the signatures over the client's own bytes
// (the 2026-11-02 build, from the saved image), every refusal, a scan of the whole saved image when it is at hand, and
// then those bytes executed. A stand-in input update carries both read sites byte for byte, at the client's 8-byte
// alignment, around a stand-in pad device. It runs before the patch, through the patch with nobody driving, and with the
// real dispatcher driving. Every run checks that the stack is balanced, that ebx, esi, edi and ebp come back, that the
// flags and eax after the state read's cmp are the client's own, and that the client's store on the event read's success
// branch still happens. Last, the patch goes in while a thread is inside each read.
//   pad_disites <layout_padhook.txt> [saved image]     (the image also from NUI_FFXI_IMAGE)
#include "PadHook/PadHook.h"

#include "../pad_layout.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/mman.h>
#include <vector>

DWORD g_fake_tick = 1000;

// what PadHook.h calls on this path
namespace {
HMODULE const g_module = reinterpret_cast<HMODULE>(0x400000);
}
void* VirtualAlloc(void*, SIZE_T n, DWORD, DWORD) {
    void* p = mmap(nullptr, n < 4096 ? 4096 : n, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}
BOOL VirtualFree(void* p, SIZE_T, DWORD) { return munmap(p, 4096) == 0; }
BOOL VirtualProtect(void*, SIZE_T, DWORD, DWORD* old) { if (old) *old = PAGE_EXECUTE_READ; return 1; }
BOOL FlushInstructionCache(HANDLE, LPCVOID, SIZE_T) { return 1; }
HANDLE GetCurrentProcess() { return nullptr; }
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

namespace {

int g_pass = 0, g_fail = 0;
void check(char const* name, bool ok) {
    if (ok) ++g_pass;
    else { ++g_fail; std::printf("FAIL %s\n", name); }
}

using padhook_detail::DiScan;
using padhook_detail::di_scan_sites;
using padhook_detail::di_scan_verdict;
using padhook_detail::kDiEventsStoreAt;
using padhook_detail::kDiStateCmpAt;

// ---- the client's bytes, 2026-11-02 (the saved image: 1000616B and 100069AC) ----

std::uintptr_t const kSiteA = 0x1000616B, kSiteB = 0x100069AC;
unsigned char const kA[64] = {           // the state read
    0x8A, 0x8B, 0x98, 0x0C, 0x00, 0x00, 0x84, 0xC9, 0x74, 0x06, 0x8B, 0x08, 0x50, 0xFF, 0x51, 0x64,
    0x8B, 0xCB, 0xE8, 0x5E, 0x19, 0x00, 0x00, 0x8B, 0x83, 0x90, 0x00, 0x00, 0x00, 0x8D, 0x8B, 0x44,
    0x0A, 0x00, 0x00, 0x51, 0x68, 0x10, 0x01, 0x00, 0x00, 0x8B, 0x10, 0x50, 0xFF, 0x52, 0x24, 0x3D,
    0xAA, 0x00, 0x07, 0x80, 0x0F, 0x8F, 0x9F, 0x00, 0x00, 0x00, 0x74, 0x40, 0x3D, 0x0C, 0x00, 0x07,
};
unsigned char const kB[104] = {          // the event read, through its success branch's store
    0x8D, 0xAB, 0x94, 0x0C, 0x00, 0x00, 0xC7, 0x45, 0x00, 0x10, 0x00, 0x00, 0x00, 0xA0, 0xE4, 0x6D,
    0x45, 0x10, 0x84, 0xC0, 0x0F, 0x84, 0xAC, 0x02, 0x00, 0x00, 0x8B, 0x83, 0x90, 0x00, 0x00, 0x00,
    0x85, 0xC0, 0x0F, 0x84, 0x9E, 0x02, 0x00, 0x00, 0x8B, 0x10, 0x8D, 0xBB, 0x54, 0x0B, 0x00, 0x00,
    0x6A, 0x00, 0x55, 0x57, 0x6A, 0x14, 0x50, 0xFF, 0x52, 0x28, 0x85, 0xC0, 0x74, 0x15, 0xB9, 0x50,
    0x00, 0x00, 0x00, 0x33, 0xC0, 0xC7, 0x45, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF3, 0xAB, 0xE9, 0x6B,
    0x0A, 0x00, 0x00, 0xA1, 0xFC, 0x2E, 0x4E, 0x10, 0xC6, 0x05, 0xFA, 0x76, 0x45, 0x10, 0x00, 0xC6,
    0x44, 0x24, 0x13, 0x01, 0x8B, 0x88, 0x24, 0x02,
};
constexpr std::size_t kSigA = 52, kSigB = 100;

// the stretch from site A to the end of site B, with the client's bytes between them left as int3
constexpr std::size_t kImg = (kSiteB + sizeof(kB)) - kSiteA;
void client_image(unsigned char* img) {
    std::memset(img, 0xCC, kImg);
    std::memcpy(img, kA, sizeof(kA));
    std::memcpy(img + (kSiteB - kSiteA), kB, sizeof(kB));
}

DiScan scan(unsigned char const* p, std::size_t n, std::uintptr_t base) {
    DiScan s {0, 0, 0, 0, 0, 0, 0, 0};
    di_scan_sites(p, n, base, s);
    return s;
}

bool says(char const* why, char const* part) { return why && std::strstr(why, part); }

void scan_tests() {
    static unsigned char img[kImg + 0x1100];
    client_image(img);
    DiScan s = scan(img, kImg, kSiteA);
    check("SITE-1: the client's bytes: one state read at 1000616B", s.state_count == 1 && s.state_at == kSiteA);
    check("SITE-1: one event read at 100069AC", s.events_count == 1 && s.events_at == kSiteB);
    check("SITE-1: the device at +0x90 in both, the state at +0xA44, 16 events asked", s.state_disp == 0x90 && s.events_disp == 0x90
        && s.buffer_disp == 0xA44 && s.asked == 16);
    check("SITE-1: the cmp at 1000619A, the store at 10006A0B", s.state_at + kDiStateCmpAt == 0x1000619A
        && s.events_at + kDiEventsStoreAt == 0x10006A0B);
    check("SITE-1: can be patched", di_scan_verdict(s) == nullptr);
    // the signatures end at the patched instruction: one byte short of either is no match
    s = scan(img, kSigA - 1, kSiteA);
    check("SITE-2: the state read needs its cmp", s.state_count == 0);
    s = scan(img + (kSiteB - kSiteA), kSigB, kSiteB);
    check("SITE-2: the event read up to its store is a match", s.events_count == 1);
    s = scan(img + (kSiteB - kSiteA), kSigB - 1, kSiteB);
    check("SITE-2: one byte short is no match", s.events_count == 0);
    // refusals
    static unsigned char two[2 * kImg];
    client_image(two);
    client_image(two + kImg);
    s = scan(two, sizeof(two), kSiteA);
    check("SITE-3: two of each: not unique", s.state_count == 2 && says(di_scan_verdict(s), "not unique"));
    static unsigned char c[kImg];
    client_image(c);
    c[(kSiteB - kSiteA) + 57] = 0x2C;               // call [edx+0x28] -> call [edx+0x2C]: no event read
    s = scan(c, kImg, kSiteA);
    check("SITE-4: no event read: not found", s.events_count == 0 && says(di_scan_verdict(s), "no GetDeviceData"));
    client_image(c);
    c[48] = 0xAB;                                   // the cmp's constant changed
    s = scan(c, kImg, kSiteA);
    check("SITE-4: another cmp: not found", s.state_count == 0 && says(di_scan_verdict(s), "no GetDeviceState"));
    client_image(c);
    c[15] = 0x60;                                   // call [ecx+0x64] (Poll) -> another method: not the pad's read
    s = scan(c, kImg, kSiteA);
    check("SITE-4: no Poll before the read: not the pad's read", s.state_count == 0);
    client_image(c);
    c[(kSiteB - kSiteA) + 61] = 0x16;               // the success branch goes elsewhere: not the store's
    s = scan(c, kImg, kSiteA);
    check("SITE-4: the success branch does not reach the store: not found", s.events_count == 0);
    client_image(c);
    c[(kSiteB - kSiteA) + 98] = 0x00;               // another store: not the client's flag
    s = scan(c, kImg, kSiteA);
    check("SITE-4: another store: not found", s.events_count == 0);
    client_image(c);
    c[(kSiteB - kSiteA) + 28] = 0x8C;               // the event read loads another device
    s = scan(c, kImg, kSiteA);
    check("SITE-5: two devices: refused", says(di_scan_verdict(s), "two devices"));
    // the event read before the state read, or too far after it
    static unsigned char far_img[0x1200];
    std::memset(far_img, 0xCC, sizeof(far_img));
    std::memcpy(far_img + 0x04, kB, sizeof(kB));
    std::memcpy(far_img + 0x103, kA, sizeof(kA));
    s = scan(far_img, sizeof(far_img), 0x10000000);
    check("SITE-6: the event read first: not one function", says(di_scan_verdict(s), "one function"));
    std::memset(far_img, 0xCC, sizeof(far_img));
    std::memcpy(far_img + 0x03, kA, sizeof(kA));
    std::memcpy(far_img + 0x04 + PADHOOK_DI_SPAN + 8, kB, sizeof(kB));
    s = scan(far_img, sizeof(far_img), 0x10000000);
    check("SITE-6: too far apart: not one function", says(di_scan_verdict(s), "one function"));
    // alignment: the client's cmp at 8n+2 and store at 8n+3 can be written; 8n+4 cannot
    client_image(img);
    s = scan(img, kImg, kSiteA + 1);                // cmp at 8n+3, store at 8n+4
    check("SITE-7: a store at 8n+4: refused", says(di_scan_verdict(s), "8-byte"));
    s = scan(img, kImg, kSiteA - 2);                // cmp at 8n+0, store at 8n+1
    check("SITE-7: cmp at 8n+0, store at 8n+1: can be patched", di_scan_verdict(s) == nullptr);
    s = scan(img, kImg, kSiteA + 2);                // cmp at 8n+4
    check("SITE-7: a cmp at 8n+4: refused", says(di_scan_verdict(s), "8-byte"));
    // the count asked for comes from the bytes
    client_image(c);
    c[(kSiteB - kSiteA) + 9] = 0x20;
    s = scan(c, kImg, kSiteA);
    check("SITE-8: the count asked for is read (32)", s.asked == 32 && di_scan_verdict(s) == nullptr);
    c[(kSiteB - kSiteA) + 9] = 0x41;
    s = scan(c, kImg, kSiteA);
    check("SITE-8: 65 asked: refused", says(di_scan_verdict(s), "odd count"));
    c[(kSiteB - kSiteA) + 9] = 0x00;
    s = scan(c, kImg, kSiteA);
    check("SITE-8: none asked: refused", says(di_scan_verdict(s), "odd count"));
}

// the whole saved image: one match of each, in .text, at the addresses above (skipped when the image is not at hand)
void image_test(char const* path) {
    if (!path || !*path) { std::printf("  (no saved image: IMAGE skipped)\n"); return; }
    std::FILE* f = std::fopen(path, "rb");
    if (!f) { std::printf("  (cannot open %s: IMAGE skipped)\n", path); return; }
    std::vector<unsigned char> d(13 * 1024 * 1024);
    std::size_t const n = std::fread(d.data(), 1, d.size(), f);
    std::fclose(f);
    DiScan s = scan(d.data(), n, 0x10000000);
    check("IMAGE: the saved image: one match of each", s.state_count == 1 && s.events_count == 1);
    check("IMAGE: at 1000616B and 100069AC", s.state_at == kSiteA && s.events_at == kSiteB);
    check("IMAGE: can be patched", di_scan_verdict(s) == nullptr);
}

// ---- the client's bytes, executed ----

struct FakeDev { void* const* vtbl; };
FakeDev g_dev {nullptr};
unsigned char g_js[PADHOOK_JS_BYTES];
PadDiEvent g_events[32];
DWORD g_nev = 0;
LONG g_hr_state = 0, g_hr_data = 0;
int g_state_calls = 0, g_data_calls = 0, g_polls = 0;
bool g_args_ok = true;
void (*g_inside_state)() = nullptr;
void (*g_inside_data)() = nullptr;

extern "C" __attribute__((stdcall, noinline, force_align_arg_pointer)) LONG dev_state(FakeDev* self, DWORD cb, void* buf) {
    ++g_state_calls;
    g_args_ok = g_args_ok && self == &g_dev && cb == PADHOOK_JS_BYTES && buf;
    std::memcpy(buf, g_js, PADHOOK_JS_BYTES);
    if (g_inside_state) { void (*f)() = g_inside_state; g_inside_state = nullptr; f(); }
    return g_hr_state;
}
extern "C" __attribute__((stdcall, noinline, force_align_arg_pointer)) LONG dev_data(FakeDev* self, DWORD cb, PadDiEvent* ev,
                                                                                    DWORD* inout, DWORD flags) {
    ++g_data_calls;
    g_args_ok = g_args_ok && self == &g_dev && cb == 0x14 && flags == 0 && inout && *inout == 16;
    DWORD const n = g_nev < *inout ? g_nev : *inout;
    std::memcpy(ev, g_events, n * sizeof(PadDiEvent));
    *inout = n;
    if (g_inside_data) { void (*f)() = g_inside_data; g_inside_data = nullptr; f(); }
    return g_hr_data;
}
extern "C" __attribute__((stdcall, noinline)) LONG dev_poll(FakeDev*) { ++g_polls; return 0; }
extern "C" __attribute__((stdcall, noinline)) LONG dev_info(FakeDev*, unsigned char* inst) {
    std::uint32_t const vidpid = 0x05C4054C;
    std::memcpy(inst + 20, &vidpid, 4);
    unsigned char const tail[12] = {0, 0, 0, 0, 0, 0, 'P', 'I', 'D', 'V', 'I', 'D'};
    std::memcpy(inst + 24, tail, 12);
    std::strcpy(reinterpret_cast<char*>(inst + 300), "Wireless Controller");
    return 0;
}
void* g_vtbl[32] = {};

alignas(16) unsigned char g_input[0xDC4];
unsigned char* in_state() { return g_input + 0xA44; }
PadDiEvent* in_events() { return reinterpret_cast<PadDiEvent*>(g_input + 0xB54); }
DWORD in_count() { DWORD n; std::memcpy(&n, g_input + 0xC94, 4); return n; }

// what the stand-in records after each site
extern "C" {
DWORD g_flags_a, g_eax_a, g_flags_b, g_eax_b, g_edi_b, g_ebp_b, g_flag_b, g_failed;
unsigned char g_gamepad_on = 1, g_sink1[4], g_sink2;
DWORD g_sp, g_frame;
}

// Call the stand-in (cdecl, one argument) as the client calls its update. *delta = esp after less esp before, *regs = 1
// when ebx, esi, edi and ebp came back.
extern "C" void run_checked(void const* fn, void* input, int* delta, int* regs);
asm(R"(
    .text
    .globl run_checked
    .type run_checked, @function
run_checked:
    pushl %ebp
    movl %esp, %ebp
    pushl %ebx
    pushl %esi
    pushl %edi
    movl %ebp, g_frame
    movl 12(%ebp), %eax
    movl 8(%ebp), %ecx
    movl $0x0B0B0B0B, %ebx
    movl $0x05050505, %esi
    movl $0x0D0D0D0D, %edi
    movl $0x0E0E0E0E, %ebp
    movl %esp, g_sp
    pushl %eax
    call *%ecx
    addl $4, %esp
    movl %esp, %ecx
    subl g_sp, %ecx
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
1:  movl g_sp, %esp
    movl g_frame, %ebp
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

struct Code {
    unsigned char* page;
    unsigned char* fn;
    unsigned char* a;                    // site A's first byte
    unsigned char* b;
};

void put32(unsigned char* p, std::uint32_t v) { std::memcpy(p, &v, 4); }
std::uint32_t addr(void const* p) { return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(p)); }
void rel32(unsigned char* at, unsigned char const* to) { put32(at, static_cast<std::uint32_t>(to - (at + 4))); }

// The stand-in update: a frame like the client's; the device in eax (as the client has it there) and a skip of site A
// without one; site A byte for byte at 8n+3 (its cmp at 8n+2, as 1000619A); a recorder; the flag cleared; site B byte for
// byte at 8n+4 (its store at 8n+3, as 10006A0B); a recorder of the flags, eax, edi, ebp and the client's flag; the
// failure target and the exit. Only the bytes that name addresses in the client differ from its own: they point here.
Code build() {
    Code c {};
    c.page = static_cast<unsigned char*>(VirtualAlloc(nullptr, 4096, 0, 0));
    std::memset(c.page, 0x90, 4096);
    unsigned char* stub = c.page + 0x10;                 // the client's force feedback call: nothing to do
    stub[0] = 0xC3;
    unsigned char* p = c.page + 0x40;
    c.fn = p;
    unsigned char const pro[] = {
        0x55, 0x53, 0x56, 0x57,                          // push ebp / ebx / esi / edi
        0x83, 0xEC, 0x60,                                // sub esp, 0x60
        0x8B, 0x5C, 0x24, 0x74,                          // mov ebx, [esp+0x74]      the input object
        0x8B, 0x83, 0x90, 0x00, 0x00, 0x00,              // mov eax, [ebx+0x90]      the device, as the client has it
        0x85, 0xC0, 0x0F, 0x84, 0, 0, 0, 0,              // test eax, eax / jz no_a  (the client tests it before site A)
    };
    std::memcpy(p, pro, sizeof(pro));
    unsigned char* jz_no_a = p + sizeof(pro) - 4;
    p += sizeof(pro);
    while ((reinterpret_cast<std::uintptr_t>(p) & 7) != 3) *p++ = 0x90;
    c.a = p;
    std::memcpy(p, kA, kSigA);
    rel32(c.a + 19, stub);
    p += kSigA;
    p[0] = 0x9C; p[1] = 0x8F; p[2] = 0x05; put32(p + 3, addr(&g_flags_a));   // pushfd / pop [g_flags_a]
    p[7] = 0xA3; put32(p + 8, addr(&g_eax_a));                                // mov [g_eax_a], eax
    p += 12;
    rel32(jz_no_a, p);
    p[0] = 0xC6; p[1] = 0x44; p[2] = 0x24; p[3] = 0x13; p[4] = 0x00;          // mov byte [esp+0x13], 0
    p += 5;
    while ((reinterpret_cast<std::uintptr_t>(p) & 7) != 4) *p++ = 0x90;
    c.b = p;
    std::memcpy(p, kB, kSigB);
    put32(c.b + 14, addr(&g_gamepad_on));
    put32(c.b + 84, addr(g_sink1));
    put32(c.b + 90, addr(&g_sink2));
    p += kSigB;
    p[0] = 0x9C; p[1] = 0x8F; p[2] = 0x05; put32(p + 3, addr(&g_flags_b));
    p[7] = 0xA3; put32(p + 8, addr(&g_eax_b));
    p[12] = 0x89; p[13] = 0x3D; put32(p + 14, addr(&g_edi_b));                // mov [g_edi_b], edi
    p[18] = 0x89; p[19] = 0x2D; put32(p + 20, addr(&g_ebp_b));                // mov [g_ebp_b], ebp
    p[24] = 0x0F; p[25] = 0xB6; p[26] = 0x44; p[27] = 0x24; p[28] = 0x13;     // movzx eax, byte [esp+0x13]
    p[29] = 0xA3; put32(p + 30, addr(&g_flag_b));                             // mov [g_flag_b], eax
    p[34] = 0xEB;                                                             // jmp exit (rel8 below)
    unsigned char* jmp_exit = p + 35;
    p += 36;
    unsigned char* failed = p;                           // site B's jmp on the failure branch (1000746A in the client)
    p[0] = 0xC7; p[1] = 0x05; put32(p + 2, addr(&g_failed)); put32(p + 6, 1); // mov dword [g_failed], 1
    p += 10;
    unsigned char* exit = p;
    unsigned char const epi[] = {0x83, 0xC4, 0x60, 0x5F, 0x5E, 0x5B, 0x5D, 0xC3};
    std::memcpy(p, epi, sizeof(epi));
    *jmp_exit = static_cast<unsigned char>(exit - (jmp_exit + 1));
    rel32(c.b + 22, exit);                               // the gamepad is off
    rel32(c.b + 36, exit);                               // no pad
    rel32(c.b + 79, failed);
    return c;
}

struct Run { bool balanced, regs_ok; DWORD fa, ea, flag, failed; };
constexpr DWORD kFlags = 0x8D5;                  // CF PF AF ZF SF OF: what cmp sets

Run run(Code const& c) {
    int delta = -1, regs = 0;
    g_flags_a = g_eax_a = g_flags_b = g_eax_b = g_flag_b = g_failed = 0;
    g_edi_b = g_ebp_b = 0;
    g_fake_tick += 16;
    run_checked(c.fn, g_input, &delta, &regs);
    Run r {delta == 0 && regs == 1, g_edi_b == 0 || (g_edi_b == addr(in_events()) && g_ebp_b == addr(g_input + 0xC94)),
           g_flags_a & kFlags, g_eax_a, g_flag_b, g_failed};
    return r;
}

void set_pad(std::initializer_list<unsigned> down, std::initializer_list<unsigned> pressed, LONG lx = 0) {
    std::memset(g_js, 0, sizeof(g_js));
    std::memcpy(g_js, &lx, 4);
    DWORD const centred = PADHOOK_POV_CENTRED;
    std::memcpy(g_js + PADHOOK_JS_POV, &centred, 4);
    for (unsigned b : down) g_js[PADHOOK_JS_BUTTONS + b] = 0x80;
    DWORD n = 0;
    for (unsigned b : pressed) { g_events[n] = PadDiEvent {0x30 + b, 0x80, 0, n + 1, 0}; ++n; }
    g_nev = n;
}

LONG const kHrs[] = {0, 1, static_cast<LONG>(0x800700AA), static_cast<LONG>(0x8007001E), static_cast<LONG>(0x8007000C),
                     static_cast<LONG>(0x80070005), 0x7FFFFFFF};
constexpr int kNHr = sizeof(kHrs) / sizeof(kHrs[0]);

PadBus g_bus {};
PadBus g_bus2 {};
Code g_mid {};
DiScan g_mid_scan {};

void dump(char const* path, void const* p, std::size_t n) {
    if (FILE* f = std::fopen(path, "wb")) { std::fwrite(p, 1, n, f); std::fclose(f); }
}

void exec_tests() {
    using namespace padhook_detail;
    g_vtbl[9] = reinterpret_cast<void*>(&dev_state);
    g_vtbl[10] = reinterpret_cast<void*>(&dev_data);
    g_vtbl[15] = reinterpret_cast<void*>(&dev_info);
    g_vtbl[25] = reinterpret_cast<void*>(&dev_poll);
    g_dev.vtbl = g_vtbl;
    std::memset(g_input, 0, sizeof(g_input));
    FakeDev* devp = &g_dev;
    std::memcpy(g_input + 0x90, &devp, 4);
    g_input[0xC98] = 1;                          // the device needs Poll

    Code const c = build();
    // the client's own results, before any patch
    Run base[kNHr][kNHr];
    bool ok = true;
    for (int i = 0; i < kNHr; ++i)
        for (int j = 0; j < kNHr; ++j) {
            g_hr_state = kHrs[i];
            g_hr_data = kHrs[j];
            set_pad({1}, {1});
            base[i][j] = run(c);
            Run const& r = base[i][j];
            ok = ok && r.balanced && r.regs_ok && r.ea == static_cast<DWORD>(kHrs[i]);
            // the client's success branch sets its flag; the failure branch clears the buffer and leaves
            ok = ok && (kHrs[j] == 0 ? (r.flag == 1 && !r.failed && in_count() == 1) : (r.failed == 1 && in_count() == 0));
        }
    check("EXEC-0: unpatched: the stand-in runs the client's bytes", ok && g_args_ok && g_state_calls == kNHr * kNHr
        && g_data_calls == kNHr * kNHr && g_polls == kNHr * kNHr);

    DiScan s = scan(c.page, 4096, reinterpret_cast<std::uintptr_t>(c.page));
    check("EXEC-1: found in memory", s.state_count == 1 && s.events_count == 1 && s.state_at == reinterpret_cast<std::uintptr_t>(c.a)
        && s.events_at == reinterpret_cast<std::uintptr_t>(c.b) && !di_scan_verdict(s));
    cached_bus() = &g_bus;
    char const* why = di_install(g_bus.di, s);
    check("EXEC-1: installed", why == nullptr);
    if (why) { std::printf("  %s\n", why); return; }
    auto const block = reinterpret_cast<std::uintptr_t>(g_bus.di.block);
    unsigned char* const cmp_a = c.a + kDiStateCmpAt;
    unsigned char* const st_b = c.b + kDiEventsStoreAt;
    auto rel_to = [](unsigned char* at) { return reinterpret_cast<std::uintptr_t>(at) + 5 + static_cast<std::int32_t>(le32(at + 1)); };
    check("EXEC-1: the cmp and the store are now calls to their thunks", cmp_a[0] == 0xE8 && rel_to(cmp_a) == block + kDiStateThunkAt
        && st_b[0] == 0xE8 && rel_to(st_b) == block + kDiEventsThunkAt);
    check("EXEC-1: every other byte as it was", std::memcmp(c.a, kA, 19) == 0 && std::memcmp(c.a + 23, kA + 23, kDiStateCmpAt - 23) == 0
        && cmp_a[5] == 0x9C && std::memcmp(c.b, kB, 14) == 0 && std::memcmp(c.b + 18, kB + 18, 4) == 0
        && std::memcmp(c.b + 26, kB + 26, 10) == 0 && std::memcmp(c.b + 40, kB + 40, 39) == 0 && st_b[5] == 0x9C);
    check("EXEC-1: the hook", g_bus.di.site_state == reinterpret_cast<std::uintptr_t>(cmp_a)
        && g_bus.di.site_events == reinterpret_cast<std::uintptr_t>(st_b) && g_bus.di.device_disp == 0x90
        && g_bus.di.buffer_disp == 0xA44 && *g_bus.di.dispatch_ptr == nullptr);
    dump("di_site_a.bin", c.a, 64);
    dump("di_site_b.bin", c.b, 112);
    dump("di_thunk_state.bin", g_bus.di.block + kDiStateThunkAt, kDiStateThunkBytes);
    dump("di_thunk_events.bin", g_bus.di.block + kDiEventsThunkAt, kDiEventsThunkBytes);
    if (std::FILE* v = std::fopen("di_vma.txt", "w")) {
        std::fprintf(v, "%#lx %#lx %#lx %#lx\n", static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(c.a)),
                     static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(c.b)), static_cast<unsigned long>(block + kDiStateThunkAt),
                     static_cast<unsigned long>(block + kDiEventsThunkAt));
        std::fclose(v);
    }

    auto same_as_base = [&](bool* data_ok) {
        bool all = true;
        for (int i = 0; i < kNHr; ++i)
            for (int j = 0; j < kNHr; ++j) {
                g_hr_state = kHrs[i];
                g_hr_data = kHrs[j];
                set_pad({1}, {1});
                Run const r = run(c);
                Run const& b = base[i][j];
                all = all && r.balanced && r.regs_ok && r.fa == b.fa && r.ea == b.ea && r.flag == b.flag && r.failed == b.failed;
                if (data_ok && kHrs[i] == 0 && kHrs[j] == 0) *data_ok = *data_ok && in_state()[0x30 + 1] == 0x80 && in_count() == 1;
            }
        return all;
    };
    // nobody drives: the thunks run the client's own instructions and nothing else
    bool data_ok = true;
    ok = same_as_base(&data_ok);
    check("EXEC-2: nobody drives: the client's flags and eax after the cmp, and its store", ok);
    check("EXEC-2: nobody drives: nothing hidden, the dispatcher not called", data_ok && g_bus.di.calls == 0);

    // the dispatcher drives
    *g_bus.di.dispatch_ptr = &di_dispatch;
    g_bus.di.press.primed = 1;
    g_bus.di.press.pov_out = PADHOOK_POV_CENTRED;
    PadClient& cl = g_bus.clients[0];
    cl.occupied = 2;
    cl.alive_tick = static_cast<LONG>(g_fake_tick);
    LONG const calls0 = g_bus.di.calls;
    ok = same_as_base(nullptr);
    check("EXEC-3: driving: the client's flags and eax after the cmp, and its store", ok);
    check("EXEC-3: driving: the state read always, the event read on its success branch",
          g_bus.di.calls - calls0 == kNHr * kNHr + kNHr);
    check("EXEC-3: the device read, and its maker", g_bus.di.pad.device == &g_dev && g_bus.di.pad.vidpid == 0x05C4054C
        && std::strcmp(g_bus.di.pad.name, "Wireless Controller") == 0);
    // one clean tick with the pad held: Cross pressed now is hidden from both reads, the stick centred
    std::memset(&g_bus.di.press, 0, sizeof(g_bus.di.press));
    g_bus.di.press.primed = 1;
    g_bus.di.press.pov_out = PADHOOK_POV_CENTRED;
    g_hr_state = g_hr_data = 0;
    cl.capture = 1;
    cl.alive_tick = static_cast<LONG>(g_fake_tick + 16);
    set_pad({1, 4}, {1}, 99);
    g_bus.di.press.game = 1ul << 4;              // L1 held from before: the game's
    Run r = run(c);
    LONG lx;
    std::memcpy(&lx, in_state(), 4);
    check("EXEC-4: held: Cross cleared from the client's state, L1 kept, the stick centred", r.balanced
        && in_state()[0x30 + 1] == 0 && in_state()[0x30 + 4] == 0x80 && lx == 0);
    check("EXEC-4: held: Cross's press dropped from the client's events", in_count() == 0 && r.flag == 1);
    PadDiState raw {};
    check("EXEC-4: the record is raw", padhook_di_read(&g_bus, &raw) && raw.buttons == ((1ul << 1) | (1ul << 4)) && raw.axes[0] == 99);
    cl.capture = 0;
    // no device: the stand-in skips site A as the client does, and site B's own test skips its read
    FakeDev* none = nullptr;
    std::memcpy(g_input + 0x90, &none, 4);
    int const sc = g_state_calls, dc = g_data_calls;
    LONG const calls = g_bus.di.calls;
    r = run(c);
    check("EXEC-5: no device: the client skips the reads, the thunks never run", r.balanced && g_state_calls == sc
        && g_data_calls == dc && g_bus.di.calls == calls && r.flag == 0);
    std::memcpy(g_input + 0x90, &devp, 4);
    // the gamepad off in the game's settings: the event read is skipped
    g_gamepad_on = 0;
    r = run(c);
    check("EXEC-5: the gamepad off: no event read, no store", r.balanced && r.flag == 0 && g_data_calls == dc);
    g_gamepad_on = 1;
    // 200 ticks more, balanced throughout
    ok = true;
    for (int i = 0; i < 200; ++i) {
        cl.alive_tick = static_cast<LONG>(g_fake_tick);
        cl.capture = (i / 20) & 1;
        set_pad({static_cast<unsigned>(i & 31)}, {static_cast<unsigned>((i * 7) & 31)}, i);
        Run t = run(c);
        ok = ok && t.balanced && t.regs_ok && t.flag == 1;
    }
    check("EXEC-6: 200 more ticks balanced", ok);
    // the patch is not undone: a second scan of the same bytes finds no site
    DiScan again = scan(c.page, 4096, reinterpret_cast<std::uintptr_t>(c.page));
    check("EXEC-7: patched bytes: the signatures no longer match", again.state_count == 0 && again.events_count == 0);
}

// The patch goes in while the client is inside each read: each read returns to a whole instruction.
void install_mid() { padhook_detail::di_install(g_bus2.di, g_mid_scan); }
void mid_tests() {
    using namespace padhook_detail;
    std::memset(g_input + 0xC94, 0, 4);
    for (int which = 0; which < 2; ++which) {
        g_mid = build();
        g_hr_state = g_hr_data = 0;
        set_pad({1}, {1});
        Run const before = run(g_mid);
        g_mid_scan = scan(g_mid.page, 4096, reinterpret_cast<std::uintptr_t>(g_mid.page));
        std::memset(&g_bus2, 0, sizeof(g_bus2));
        cached_bus() = &g_bus2;
        if (which == 0) g_inside_state = &install_mid;
        else g_inside_data = &install_mid;
        Run const r = run(g_mid);
        char const* n1 = which == 0 ? "EXEC-8: patched from inside the state read: balanced, a whole call after it"
                                    : "EXEC-8: patched from inside the event read: balanced, the store a whole call";
        check(n1, r.balanced && r.regs_ok && g_bus2.di.block && (g_mid.a + kDiStateCmpAt)[0] == 0xE8
            && (g_mid.b + kDiEventsStoreAt)[0] == 0xE8);
        check(which == 0 ? "EXEC-8: (state) and the client's flags, eax and store" : "EXEC-8: (events) and the client's flags, eax and store",
              r.fa == before.fa && r.ea == before.ea && r.flag == before.flag && r.flag == 1);
    }
}

}  // namespace

int main(int argc, char** argv) {
    scan_tests();
    char const* image = argc > 2 ? argv[2] : std::getenv("NUI_FFXI_IMAGE");
    image_test(image);
    exec_tests();
    mid_tests();
    if (argc > 1) {
        char text[16384];
        padhook_layout_text(text, sizeof(text));
        std::string want;
        if (std::FILE* f = std::fopen(argv[1], "rb")) {
            char buf[4096];
            std::size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) want.append(buf, n);
            std::fclose(f);
        }
        std::string got;
        for (std::size_t at = 0; at < want.size();) {
            std::size_t const end = want.find('\n', at);
            std::string line = want.substr(at, end == std::string::npos ? std::string::npos : end - at);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty() && line[0] != '#') got += line + "\n";
            at = end == std::string::npos ? want.size() : end + 1;
        }
        check("LAYOUT: layout_padhook.txt is the header's", got == text);
    }
    std::printf("disites: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
