// key_sites.cpp - KeyHook's sites on 32-bit Linux: the signatures over the client's own bytes (the 2026-11-02 build,
// from the research report), the refusals, and then those bytes executed. A stand-in input update carries both
// read sites byte for byte, at the same 8-byte alignment as the client, around a stand-in keyboard device. It runs
// before the patch, through the patch with nobody driving, and with PadHook's way of driving (the real dispatcher).
// Every run checks that the stack is balanced, that ebx, esi, edi and ebp come back, and that the flags and eax
// after each site's cmp are the client's own. Last, the patch goes in while a thread is inside the read.
//   key_sites <layout_keyhook.txt>
#include "KeyHook/KeyHook.h"

#include "../key_layout.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <sys/mman.h>

DWORD g_fake_tick = 1000;

// what KeyHook.h calls on this path
void* VirtualAlloc(void*, SIZE_T n, DWORD, DWORD) {
    void* p = mmap(nullptr, n < 4096 ? 4096 : n, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}
BOOL VirtualFree(void* p, SIZE_T, DWORD) { return munmap(p, 4096) == 0; }
BOOL VirtualProtect(void*, SIZE_T, DWORD, DWORD* old) { if (old) *old = PAGE_EXECUTE_READ; return 1; }
BOOL FlushInstructionCache(HANDLE, LPCVOID, SIZE_T) { return 1; }
HANDLE GetCurrentProcess() { return nullptr; }

namespace {

int g_pass = 0, g_fail = 0;
void check(char const* name, bool ok) {
    if (ok) ++g_pass;
    else { ++g_fail; std::printf("FAIL %s\n", name); }
}

using keyhook_detail::SiteScan;
using keyhook_detail::scan_sites;
using keyhook_detail::scan_verdict;

// ---- the client's bytes, 2026-11-02 (docs: keyboard-client-report.md §3) ----

std::uintptr_t const kFn = 0x10005A40, kSiteA = 0x10005A61, kSiteB = 0x10005AFB;
unsigned char const kFnBytes[] = {       // 0x10005A40, 64 bytes
    0x81, 0xEC, 0x90, 0x01, 0x00, 0x00, 0x53, 0x55, 0x56, 0x8B, 0xF1, 0x57, 0x89, 0x74, 0x24, 0x34,
    0xE8, 0x9B, 0xF2, 0x00, 0x00, 0xE8, 0x12, 0xD8, 0x30, 0x00, 0x0F, 0xBF, 0xC0, 0x89, 0x44, 0x24,
    0x40, 0x8B, 0x86, 0x88, 0x00, 0x00, 0x00, 0xDB, 0x44, 0x24, 0x40, 0x85, 0xC0, 0xD9, 0x5C, 0x24,
    0x40, 0x0F, 0x84, 0x61, 0x04, 0x00, 0x00, 0x8B, 0x08, 0x8D, 0xBE, 0x98, 0x00, 0x00, 0x00, 0x57,
};
unsigned char const kA[] = {             // 0x10005A61, 64 bytes: the state read
    0x8B, 0x86, 0x88, 0x00, 0x00, 0x00, 0xDB, 0x44, 0x24, 0x40, 0x85, 0xC0, 0xD9, 0x5C, 0x24, 0x40,
    0x0F, 0x84, 0x61, 0x04, 0x00, 0x00, 0x8B, 0x08, 0x8D, 0xBE, 0x98, 0x00, 0x00, 0x00, 0x57, 0x68,
    0x00, 0x01, 0x00, 0x00, 0x50, 0xFF, 0x51, 0x24, 0x3D, 0xAA, 0x00, 0x07, 0x80, 0x7F, 0x55, 0x74,
    0x0E, 0x3D, 0x0C, 0x00, 0x07, 0x80, 0x74, 0x07, 0x3D, 0x1E, 0x00, 0x07, 0x80, 0x75, 0x49, 0xA0,
};
unsigned char const kB[] = {             // 0x10005AFB, 64 bytes: the event read
    0x8B, 0x86, 0x88, 0x00, 0x00, 0x00, 0x8D, 0xAE, 0xDC, 0x08, 0x00, 0x00, 0x8D, 0x9E, 0x9C, 0x07,
    0x00, 0x00, 0x6A, 0x00, 0x55, 0xC7, 0x45, 0x00, 0x10, 0x00, 0x00, 0x00, 0x8B, 0x10, 0x53, 0x6A,
    0x14, 0x50, 0xFF, 0x52, 0x28, 0x3D, 0xAA, 0x00, 0x07, 0x80, 0x7F, 0x55, 0x74, 0x0E, 0x3D, 0x0C,
    0x00, 0x07, 0x80, 0x74, 0x07, 0x3D, 0x1E, 0x00, 0x07, 0x80, 0x75, 0x49, 0xA0, 0xF8, 0x76, 0x45,
};
constexpr std::size_t kSigA = 45, kSigB = 42;

// the function from 0x10005A40 to the end of site B's bytes, with the bytes the report does not give left as int3
constexpr std::size_t kImg = (kSiteB + 64) - kFn;
void client_image(unsigned char* img) {
    std::memset(img, 0xCC, kImg);
    std::memcpy(img, kFnBytes, sizeof(kFnBytes));
    std::memcpy(img + (kSiteA - kFn), kA, sizeof(kA));
    std::memcpy(img + (kSiteB - kFn), kB, sizeof(kB));
}

SiteScan scan(unsigned char const* p, std::size_t n, std::uintptr_t base) {
    SiteScan s {0, 0, 0, 0, 0, 0, 0};
    scan_sites(p, n, base, s);
    return s;
}

bool says(char const* why, char const* part) { return why && std::strstr(why, part); }

void scan_tests() {
    unsigned char img[kImg + 0x800];
    client_image(img);
    SiteScan s = scan(img, kImg, kFn);
    check("SITE-1: the client's bytes: one state read at 10005A61", s.state_count == 1 && s.state_at == kSiteA);
    check("SITE-1: one event read at 10005AFB", s.events_count == 1 && s.events_at == kSiteB);
    check("SITE-1: the device at +0x88 in both, 16 events asked", s.state_disp == 0x88 && s.events_disp == 0x88 && s.asked == 16);
    check("SITE-1: the cmps at 10005A89 and 10005B20", s.state_at + keyhook_detail::kStateCmpAt == 0x10005A89
        && s.events_at + keyhook_detail::kEventsCmpAt == 0x10005B20);
    check("SITE-1: can be patched", scan_verdict(s) == nullptr);
    // the signatures end at the cmp: one byte short of either is no match
    s = scan(img + (kSiteA - kFn), kSigA - 1, kSiteA);
    check("SITE-2: the state read needs its cmp", s.state_count == 0);
    s = scan(img + (kSiteB - kFn), kSigB, kSiteB);
    check("SITE-2: the event read up to its cmp is a match", s.events_count == 1);
    s = scan(img + (kSiteB - kFn), kSigB - 1, kSiteB);
    check("SITE-2: one byte short is no match", s.events_count == 0);
    // refusals
    unsigned char two[2 * kImg];
    client_image(two);
    client_image(two + kImg);
    s = scan(two, sizeof(two), kFn);
    check("SITE-3: two of each: not unique", s.state_count == 2 && says(scan_verdict(s), "not unique"));
    unsigned char c[kImg];
    client_image(c);
    c[(kSiteB - kFn) + 34] = 0xFF;                 // call [edx+0x28] -> another call: no event read
    c[(kSiteB - kFn) + 36] = 0x2C;
    s = scan(c, kImg, kFn);
    check("SITE-4: no event read: not found", s.events_count == 0 && says(scan_verdict(s), "no GetDeviceData"));
    client_image(c);
    c[(kSiteA - kFn) + 41] = 0xAB;                 // the cmp's constant changed
    s = scan(c, kImg, kFn);
    check("SITE-4: another cmp: not found", s.state_count == 0 && says(scan_verdict(s), "no GetDeviceState"));
    client_image(c);
    c[(kSiteB - kFn) + 2] = 0x8C;                  // the event read loads another device
    s = scan(c, kImg, kFn);
    check("SITE-5: two devices: refused", says(scan_verdict(s), "two devices"));
    // the event read before the state read, or too far after it
    unsigned char far_img[0x800];
    std::memset(far_img, 0xCC, sizeof(far_img));
    std::memcpy(far_img + 0x11, kB, sizeof(kB));       // 0x..11: B's cmp at +0x36, still 8-aligned? 0x11+37 = 0x36
    std::memcpy(far_img + 0x101, kA, sizeof(kA));
    s = scan(far_img, sizeof(far_img), 0x10000000);
    check("SITE-6: the event read first: not one function", says(scan_verdict(s), "one function"));
    std::memset(far_img, 0xCC, sizeof(far_img));
    std::memcpy(far_img + 0x01, kA, sizeof(kA));
    std::memcpy(far_img + 0x01 + KEYHOOK_SITE_SPAN + 2, kB, sizeof(kB));
    s = scan(far_img, sizeof(far_img), 0x10000000);
    check("SITE-6: too far apart: not one function", says(scan_verdict(s), "one function"));
    // alignment: a cmp at 8n+1 and 8n+0 can be written (the client's), 8n+4 cannot
    client_image(img);
    s = scan(img, kImg, kFn + 3);                      // A's cmp at 8n+4
    check("SITE-7: a cmp at 8n+4: refused", says(scan_verdict(s), "8-byte"));
    s = scan(img, kImg, kFn + 2);                      // A's cmp at 8n+3, B's at 8n+2
    check("SITE-7: a cmp at 8n+3: can be patched", scan_verdict(s) == nullptr);
    s = scan(img, kImg, kFn - 1);                      // A's cmp at 8n+0, B's at 8n+7
    check("SITE-7: a cmp at 8n+7: refused", says(scan_verdict(s), "8-byte"));
    // the count asked for comes from the bytes
    client_image(c);
    c[(kSiteB - kFn) + 24] = 0x20;
    s = scan(c, kImg, kFn);
    check("SITE-8: the count asked for is read (32)", s.asked == 32 && scan_verdict(s) == nullptr);
}

// ---- the client's bytes, executed ----

struct FakeDev { void* const* vtbl; };
FakeDev g_dev {nullptr};
unsigned char g_state[256];
KeyEvent g_events[32];
DWORD g_nev = 0;
LONG g_hr_state = 0, g_hr_data = 0;
int g_state_calls = 0, g_data_calls = 0;
bool g_args_ok = true;
void (*g_inside)() = nullptr;           // run inside the state read once (the patch going in mid-call)

extern "C" __attribute__((stdcall, noinline, force_align_arg_pointer)) LONG dev_state(FakeDev* self, DWORD cb, void* buf) {
    ++g_state_calls;
    g_args_ok = g_args_ok && self == &g_dev && cb == 0x100 && buf;
    std::memcpy(buf, g_state, 256);
    if (g_inside) { void (*f)() = g_inside; g_inside = nullptr; f(); }
    return g_hr_state;
}
extern "C" __attribute__((stdcall, noinline, force_align_arg_pointer)) LONG dev_data(FakeDev* self, DWORD cb, KeyEvent* ev,
                                                                                    DWORD* inout, DWORD flags) {
    ++g_data_calls;
    g_args_ok = g_args_ok && self == &g_dev && cb == 0x14 && flags == 0 && inout && *inout == 16;
    DWORD const n = g_nev < *inout ? g_nev : *inout;
    std::memcpy(ev, g_events, n * sizeof(KeyEvent));
    *inout = n;
    return g_hr_data;
}
void* g_vtbl[16] = {};

alignas(16) unsigned char g_input[0xDC4];
unsigned char* in_state() { return g_input + 0x98; }
KeyEvent* in_events() { return reinterpret_cast<KeyEvent*>(g_input + 0x79C); }
DWORD in_count() { DWORD n; std::memcpy(&n, g_input + 0x8DC, 4); return n; }

// what the stand-in records right after each site's cmp
extern "C" {
DWORD g_flags_a, g_eax_a, g_edi_a, g_flags_b, g_eax_b, g_ebx_b, g_ebp_b;
DWORD g_sp, g_frame;                     // run_checked's own esp and frame, kept out of the stack under test
}

// Call the stand-in (cdecl, one argument) the way the client calls its update. *delta = esp after less esp before,
// *regs = 1 when ebx, esi, edi and ebp came back.
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

// The stand-in update: a frame like the client's, site A byte for byte at 8n+1 (its cmp at 8n+1, as 10005A89),
// a recorder, site B byte for byte at 8n+3 (its cmp at 8n+0, as 10005B20), a recorder, and the exit, which is
// also where site A's jz goes when there is no device.
Code build() {
    Code c {};
    c.page = static_cast<unsigned char*>(VirtualAlloc(nullptr, 4096, 0, 0));
    std::memset(c.page, 0x90, 4096);
    unsigned char* p = c.page + 0x40;
    c.fn = p;
    unsigned char const pro[] = {
        0x55, 0x53, 0x56, 0x57,                          // push ebp / ebx / esi / edi
        0x83, 0xEC, 0x60,                                // sub esp, 0x60
        0x8B, 0x74, 0x24, 0x74,                          // mov esi, [esp+0x74]      the input object
        0xC7, 0x44, 0x24, 0x40, 0, 0, 0, 0,              // mov dword [esp+0x40], 0  what site A's fild reads
    };
    std::memcpy(p, pro, sizeof(pro));
    p += sizeof(pro);
    while ((reinterpret_cast<std::uintptr_t>(p) & 7) != 1) ++p;
    c.a = p;
    std::memcpy(p, kA, kSigA);
    p += kSigA;
    unsigned char* jz_rel = c.a + 18;
    p[0] = 0x9C; p[1] = 0x8F; p[2] = 0x05; put32(p + 3, addr(&g_flags_a));   // pushfd / pop [g_flags_a]
    p[7] = 0xA3; put32(p + 8, addr(&g_eax_a));                                // mov [g_eax_a], eax
    p[12] = 0x89; p[13] = 0x3D; put32(p + 14, addr(&g_edi_a));                // mov [g_edi_a], edi
    p += 18;
    while ((reinterpret_cast<std::uintptr_t>(p) & 7) != 3) ++p;
    c.b = p;
    std::memcpy(p, kB, kSigB);
    p += kSigB;
    p[0] = 0x9C; p[1] = 0x8F; p[2] = 0x05; put32(p + 3, addr(&g_flags_b));
    p[7] = 0xA3; put32(p + 8, addr(&g_eax_b));
    p[12] = 0x89; p[13] = 0x1D; put32(p + 14, addr(&g_ebx_b));                // mov [g_ebx_b], ebx
    p[18] = 0x89; p[19] = 0x2D; put32(p + 20, addr(&g_ebp_b));                // mov [g_ebp_b], ebp
    p += 24;
    unsigned char* skip = p;
    unsigned char const epi[] = {0x83, 0xC4, 0x60, 0x5F, 0x5E, 0x5B, 0x5D, 0xC3};
    std::memcpy(p, epi, sizeof(epi));
    put32(jz_rel, static_cast<std::uint32_t>(skip - (jz_rel + 4)));   // the only byte of the sites that differs
    return c;
}

struct Run { bool balanced; DWORD fa, ea, fb, eb; bool regs_ok; };
constexpr DWORD kFlags = 0x8D5;                  // CF PF AF ZF SF OF: what cmp sets

Run run(Code const& c) {
    int delta = -1, regs = 0;
    g_flags_a = g_eax_a = g_flags_b = g_eax_b = 0;
    g_edi_a = g_ebx_b = g_ebp_b = 0;
    g_fake_tick += 16;
    run_checked(c.fn, g_input, &delta, &regs);
    Run r {delta == 0 && regs == 1, g_flags_a & kFlags, g_eax_a, g_flags_b & kFlags, g_eax_b,
           g_edi_a == addr(in_state()) && g_ebx_b == addr(in_events()) && g_ebp_b == addr(g_input + 0x8DC)};
    return r;
}

void set_keys(std::initializer_list<unsigned> down, std::initializer_list<unsigned> pressed) {
    std::memset(g_state, 0, sizeof(g_state));
    for (unsigned k : down) g_state[k] = 0x80;
    DWORD n = 0;
    for (unsigned k : pressed) { g_events[n] = KeyEvent {k, 0x80, 0, n + 1, 0}; ++n; }
    g_nev = n;
}

LONG const kHrs[] = {0, 1, static_cast<LONG>(0x800700AA), static_cast<LONG>(0x8007001E), static_cast<LONG>(0x8007000C),
                     static_cast<LONG>(0x80070005), 0x7FFFFFFF};
constexpr int kNHr = sizeof(kHrs) / sizeof(kHrs[0]);

KeyBus g_bus {};
KeyBus g_bus2 {};
Code g_mid {};
SiteScan g_mid_scan {};

void dump(char const* path, void const* p, std::size_t n) {
    if (FILE* f = std::fopen(path, "wb")) { std::fwrite(p, 1, n, f); std::fclose(f); }
}

void exec_tests() {
    using namespace keyhook_detail;
    g_vtbl[9] = reinterpret_cast<void*>(&dev_state);
    g_vtbl[10] = reinterpret_cast<void*>(&dev_data);
    g_dev.vtbl = g_vtbl;
    std::memset(g_input, 0, sizeof(g_input));
    FakeDev* devp = &g_dev;
    std::memcpy(g_input + 0x88, &devp, 4);

    Code const c = build();
    // the client's own results, before any patch
    Run base[kNHr];
    for (int i = 0; i < kNHr; ++i) {
        g_hr_state = g_hr_data = kHrs[i];
        set_keys({0xC8}, {0xC8});
        base[i] = run(c);
    }
    bool ok = true;
    for (int i = 0; i < kNHr; ++i) ok = ok && base[i].balanced && base[i].regs_ok && base[i].ea == static_cast<DWORD>(kHrs[i]);
    check("EXEC-0: unpatched: the stand-in runs the client's bytes", ok && g_args_ok && g_state_calls == kNHr && g_data_calls == kNHr);

    SiteScan s = scan(c.page, 4096, reinterpret_cast<std::uintptr_t>(c.page));
    check("EXEC-1: found in memory", s.state_count == 1 && s.events_count == 1 && s.state_at == reinterpret_cast<std::uintptr_t>(c.a)
        && s.events_at == reinterpret_cast<std::uintptr_t>(c.b) && !scan_verdict(s));
    cached_bus() = &g_bus;
    char const* why = install(&g_bus, s);
    check("EXEC-1: installed", why == nullptr);
    if (why) { std::printf("  %s\n", why); return; }
    auto const block = reinterpret_cast<std::uintptr_t>(g_bus.block);
    unsigned char* const cmp_a = c.a + kStateCmpAt;
    unsigned char* const cmp_b = c.b + kEventsCmpAt;
    auto rel_to = [](unsigned char* at) { return reinterpret_cast<std::uintptr_t>(at) + 5 + static_cast<std::int32_t>(le32(at + 1)); };
    check("EXEC-1: each cmp is now a call to its thunk", cmp_a[0] == 0xE8 && rel_to(cmp_a) == block + kStateThunkAt
        && cmp_b[0] == 0xE8 && rel_to(cmp_b) == block + kEventsThunkAt);
    check("EXEC-1: every other byte as it was", std::memcmp(c.a, kA, 18) == 0 && std::memcmp(c.a + 22, kA + 22, kStateCmpAt - 22) == 0 && cmp_a[5] == 0x9C && cmp_a[6] == 0x8F
        && std::memcmp(c.b, kB, kEventsCmpAt) == 0 && cmp_b[5] == 0x9C && cmp_b[6] == 0x8F && cmp_b[7] == 0x05);
    check("EXEC-1: the bus", g_bus.site_state == reinterpret_cast<std::uintptr_t>(cmp_a)
        && g_bus.site_events == reinterpret_cast<std::uintptr_t>(cmp_b) && g_bus.device_disp == 0x88 && *g_bus.dispatch_ptr == nullptr);
    dump("site_a.bin", c.a, 64);
    dump("site_b.bin", c.b, 64);
    dump("thunk_state.bin", g_bus.block + kStateThunkAt, kThunkBytes);
    dump("thunk_events.bin", g_bus.block + kEventsThunkAt, kThunkBytes);
    if (std::FILE* v = std::fopen("vma.txt", "w")) {
        std::fprintf(v, "%#lx %#lx %#lx %#lx\n", static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(c.a)),
                     static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(c.b)), static_cast<unsigned long>(block + kStateThunkAt),
                     static_cast<unsigned long>(block + kEventsThunkAt));
        std::fclose(v);
    }

    // nobody drives: the thunks run the client's cmp and nothing else
    ok = true;
    bool data_ok = true;
    for (int i = 0; i < kNHr; ++i) {
        g_hr_state = g_hr_data = kHrs[i];
        set_keys({0xC8}, {0xC8});
        Run r = run(c);
        ok = ok && r.balanced && r.regs_ok && r.fa == base[i].fa && r.ea == base[i].ea && r.fb == base[i].fb && r.eb == base[i].eb;
        data_ok = data_ok && in_state()[0xC8] == 0x80 && in_count() == 1;
    }
    check("EXEC-2: nobody drives: the client's flags and eax after each cmp", ok);
    check("EXEC-2: nobody drives: nothing hidden, the dispatcher not called", data_ok && g_bus.calls == 0);

    // a dispatcher drives, and a client hides Up
    *g_bus.dispatch_ptr = &dispatch;
    g_bus.keys.primed = 1;
    KeyClient& cl = g_bus.clients[0];
    cl.occupied = 2;
    unsigned long mask[KEYHOOK_WORDS] = {};
    mask[0xC8 >> 5] = 1ul << (0xC8 & 31);
    for (int w = 0; w < KEYHOOK_WORDS; ++w) cl.hide[w] = mask[w];
    cl.hiding = 1;
    ok = true;
    for (int i = 0; i < kNHr; ++i) {
        g_hr_state = g_hr_data = kHrs[i];
        cl.alive_tick = static_cast<LONG>(g_fake_tick);
        set_keys({0xC8, 0x11}, {0xC8, 0x11});
        Run r = run(c);
        ok = ok && r.balanced && r.regs_ok && r.fa == base[i].fa && r.ea == base[i].ea && r.fb == base[i].fb && r.eb == base[i].eb;
    }
    check("EXEC-3: driving: the client's flags and eax after each cmp", ok);
    check("EXEC-3: driving: the dispatcher was given the client's device", g_bus.keys.device == &g_dev && g_bus.calls == 2 * kNHr);
    // one clean tick: Up hidden from both reads, W kept
    std::memset(&g_bus.keys, 0, sizeof(g_bus.keys));
    g_bus.keys.primed = 1;
    g_hr_state = g_hr_data = 0;
    cl.alive_tick = static_cast<LONG>(g_fake_tick);
    set_keys({0xC8, 0x11}, {0xC8, 0x11});
    Run r = run(c);
    check("EXEC-4: Up cleared from the client's state, W kept", r.balanced && in_state()[0xC8] == 0 && in_state()[0x11] == 0x80);
    check("EXEC-4: Up's press dropped from the client's events, W kept", in_count() == 1 && in_events()[0].ofs == 0x11);
    // no device: site A's own test skips both reads
    FakeDev* none = nullptr;
    std::memcpy(g_input + 0x88, &none, 4);
    int const sc = g_state_calls, dc = g_data_calls;
    LONG const calls = g_bus.calls;
    r = run(c);
    check("EXEC-5: no device: the client skips the reads, the thunks never run", r.balanced && g_state_calls == sc
        && g_data_calls == dc && g_bus.calls == calls);
    std::memcpy(g_input + 0x88, &devp, 4);
    // 200 ticks more, balanced throughout
    ok = true;
    for (int i = 0; i < 200; ++i) {
        cl.alive_tick = static_cast<LONG>(g_fake_tick);
        set_keys({static_cast<unsigned>(i & 0xFF)}, {static_cast<unsigned>((i * 7) & 0xFF)});
        Run t = run(c);
        ok = ok && t.balanced && t.regs_ok;
    }
    check("EXEC-6: 200 more ticks balanced", ok);
    // the patch is not undone: a second install on the same bytes finds no cmp there
    SiteScan again = scan(c.page, 4096, reinterpret_cast<std::uintptr_t>(c.page));
    check("EXEC-7: patched bytes: the signatures no longer match", again.state_count == 0 && again.events_count == 0);
}

// The patch goes in while the client is inside its state read: the read returns to the cmp's address, which now
// holds the whole new call.
void install_mid() {
    keyhook_detail::install(&g_bus2, g_mid_scan);
}
void mid_tests() {
    using namespace keyhook_detail;
    std::memset(g_input + 0x8DC, 0, 4);
    g_mid = build();
    g_hr_state = g_hr_data = 0;
    set_keys({0xC8}, {0xC8});
    Run const before = run(g_mid);
    g_mid_scan = scan(g_mid.page, 4096, reinterpret_cast<std::uintptr_t>(g_mid.page));
    cached_bus() = &g_bus2;
    g_inside = &install_mid;
    Run const r = run(g_mid);
    check("EXEC-8: patched from inside the read: the read returns to a whole call", r.balanced && r.regs_ok
        && g_bus2.block && (g_mid.a + kStateCmpAt)[0] == 0xE8);
    check("EXEC-8: and the client's flags and eax", r.fa == before.fa && r.ea == before.ea && r.fb == before.fb && r.eb == before.eb);
}

}  // namespace

int main(int argc, char** argv) {
    scan_tests();
    exec_tests();
    mid_tests();
    if (argc > 1) {
        int const bad = keyhook_layout_compare(argv[1]);
        check("LAYOUT: layout_keyhook.txt is the header's", bad == 0);
    }
    std::printf("key_sites: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
