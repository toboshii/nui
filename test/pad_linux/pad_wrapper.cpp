// pad_wrapper.cpp - route 1 (PadHook revision 3) on 32-bit Linux: the signature over the wrapper bytes of
// two client builds, then the November wrapper executed with PadHook's real patch, thunk, adapter and
// dispatcher in front of a __cdecl stand-in for the client's shim. Every call checks that the stack is
// balanced and that the callee-saved registers survive. Last, the revision 2 thunk in the client's slot,
// in a child process: it must crash once a dispatcher drives (the reason route 1 patches the wrapper).
//
// The GetState wrapper bytes are the client's own (2026-10-13 and 2026-11-02 builds). The SetState and
// Enable wrappers next to them are made to the shape the second test report describes. To run the November
// wrapper here, its two slot addresses are moved to slots in this process. The two builds differ only there.
#include "PadHook/PadHook.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

DWORD g_fake_tick = 1000;

// what PadHook.h calls on this path (pad_wrapper only)
void* VirtualAlloc(void*, SIZE_T n, DWORD, DWORD) {
    void* p = mmap(nullptr, n, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
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

using padhook_detail::WrapperHit;
using padhook_detail::scan_wrappers;
using padhook_detail::scan_verdict;

// the GetState wrapper at 10008EA0, 2026-11-02
unsigned char const kNov[48] = {
    0xA1, 0xFC, 0x76, 0x45, 0x10, 0x85, 0xC0, 0x74, 0x1B, 0xA1, 0x00, 0x77, 0x45, 0x10, 0x85, 0xC0,
    0x74, 0x12, 0x8B, 0x4C, 0x24, 0x08, 0x8B, 0x54, 0x24, 0x04, 0x51, 0x52, 0xFF, 0xD0, 0x83, 0xC4,
    0x08, 0xC2, 0x08, 0x00, 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC2, 0x08, 0x00, 0x90, 0x90, 0x90, 0x90,
};
std::uint32_t const kNovH = 0x104576FC, kNovF = 0x10457700, kOctH = 0x1045654C, kOctF = 0x10456550;
std::uintptr_t const kSite = 0x10008EA0;

void put32(unsigned char* p, std::uint32_t v) { std::memcpy(p, &v, 4); }

// the client's three wrappers as they sit in its code: GetState +0x00, SetState +0x30, Enable +0x60
void layout(unsigned char* img, std::uint32_t h, std::uint32_t f) {
    std::memset(img, 0xCC, 0x100);
    std::memcpy(img, kNov, 48);
    put32(img + 1, h);
    put32(img + 10, f);
    std::memcpy(img + 0x30, kNov, 48);               // SetState: the same shape, F = H + 8
    put32(img + 0x31, h);
    put32(img + 0x3A, h + 8);
    unsigned char const enable[] = {                 // Enable: one argument, another shape
        0xA1, 0, 0, 0, 0, 0x85, 0xC0, 0x74, 0x11, 0xA1, 0, 0, 0, 0, 0x85, 0xC0, 0x74, 0x08,
        0xFF, 0x74, 0x24, 0x04, 0xFF, 0xD0, 0x59, 0x90, 0xC2, 0x04, 0x00, 0xC2, 0x04, 0x00};
    std::memcpy(img + 0x60, enable, sizeof(enable));
    put32(img + 0x61, h);
    put32(img + 0x6A, h + 12);
}

void scan_tests() {
    unsigned char img[0x100];
    for (int build = 0; build < 2; ++build) {
        std::uint32_t const h = build ? kOctH : kNovH, f = build ? kOctF : kNovF;
        layout(img, h, f);
        WrapperHit hit {0, 0, 0};
        int const n = scan_wrappers(img, sizeof(img), kSite, &hit);
        char const* name = build ? "scan: October" : "scan: November";
        check(name, n == 1 && hit.site == kSite && hit.handle_slot == h && hit.fn_slot == f && !scan_verdict(n, hit));
    }
    layout(img, kNovH, kNovF);
    WrapperHit hit {0, 0, 0};
    // SetState's match alone (F = H + 8) is not the GetState wrapper
    int n = scan_wrappers(img + 0x30, 0xD0, kSite + 0x30, &hit);
    check("scan: SetState alone is no match", n == 0 && scan_verdict(n, hit) != nullptr);
    // a match that ends at the last byte
    n = scan_wrappers(img, padhook_detail::kWrapSigBytes, kSite, &hit);
    check("scan: a match up to the last byte", n == 1);
    n = scan_wrappers(img, padhook_detail::kWrapSigBytes - 1, kSite, &hit);
    check("scan: one byte short is no match", n == 0);
    // the fail path is part of the signature: a build that returns something else there is no match
    unsigned char other_fail[0x100];
    std::memcpy(other_fail, img, sizeof(other_fail));
    other_fail[37] = 0x8F;                           // mov eax, 0x48F (ERROR_DEVICE_NOT_CONNECTED)
    other_fail[38] = 0x04;
    n = scan_wrappers(other_fail, sizeof(other_fail), kSite, &hit);
    check("scan: another fail value is no match", n == 0);
    // two GetState wrappers: refused
    unsigned char two[0x140];
    std::memcpy(two, img, 0x100);
    std::memcpy(two + 0x100, img, 0x40);
    n = scan_wrappers(two, sizeof(two), kSite, &hit);
    check("scan: two GetState matches are refused", n == 2 && scan_verdict(n, hit) && std::strstr(scan_verdict(n, hit), "unique"));
    // wrapper + 8 off an 8-byte boundary: refused
    n = scan_wrappers(img, sizeof(img), kSite + 4, &hit);
    check("scan: +8 not 8-aligned is refused", n == 1 && scan_verdict(n, hit) && std::strstr(scan_verdict(n, hit), "8-byte"));
    // one fixed byte changed (ret 8 becomes ret): no match
    img[33] = 0xC3;
    n = scan_wrappers(img, sizeof(img), kSite, &hit);
    check("scan: a changed fixed byte is no match", n == 0 && std::strstr(scan_verdict(n, hit), "not found"));
}

// ---- the November wrapper, executed ----

PadXState g_pad {};
int g_shim_calls = 0, g_shim2_calls = 0;

// the client's shim: __cdecl, the caller pops the arguments
extern "C" __attribute__((cdecl, noinline, force_align_arg_pointer)) DWORD shim_get(DWORD index, PadXState* s) {
    ++g_shim_calls;
    if (index != 0) return ERROR_DEVICE_NOT_CONNECTED;
    *s = g_pad;
    return ERROR_SUCCESS;
}
// another function the client could put in F on a reload: Start held as well
extern "C" __attribute__((cdecl, noinline, force_align_arg_pointer)) DWORD shim_get2(DWORD index, PadXState* s) {
    ++g_shim2_calls;
    if (index != 0) return ERROR_DEVICE_NOT_CONNECTED;
    *s = g_pad;
    s->buttons = static_cast<WORD>(s->buttons | 0x0010);
    return ERROR_SUCCESS;
}
// the old design's "original": the shim behind a __stdcall entry, so only the thunk's shape is under test
extern "C" __attribute__((stdcall, noinline, force_align_arg_pointer)) DWORD shim_stdcall(DWORD index, PadXState* s) {
    return shim_get(index, s);
}

// Call a __stdcall function the way the game calls the wrapper. *delta = esp after the call less esp before
// (0 when balanced); *regs = 1 when ebx and edi came back as they went in (esi holds esp and ebp the frame,
// so a callee that broke either crashes here).
extern "C" DWORD call_checked(void const* fn, DWORD index, PadXState* s, int* delta, int* regs);
asm(R"(
    .text
    .globl call_checked
    .type call_checked, @function
call_checked:
    pushl %ebp
    movl %esp, %ebp
    pushl %ebx
    pushl %esi
    pushl %edi
    movl $0x0B0B0B0B, %ebx
    movl $0x0D0D0D0D, %edi
    movl %esp, %esi
    pushl 16(%ebp)
    pushl 12(%ebp)
    call *8(%ebp)
    movl %esp, %ecx
    subl %esi, %ecx
    movl 20(%ebp), %edx
    movl %ecx, (%edx)
    xorl %ecx, %ecx
    cmpl $0x0B0B0B0B, %ebx
    jne 1f
    cmpl $0x0D0D0D0D, %edi
    jne 1f
    movl $1, %ecx
1:  movl 24(%ebp), %edx
    movl %ecx, (%edx)
    movl %esi, %esp
    popl %edi
    popl %esi
    popl %ebx
    popl %ebp
    ret
)");

std::uintptr_t g_slots[4];                           // H, F, S, E: the client's four slots
std::uintptr_t const kHandle = 0x5A5A0000;           // the shim's handle (never dereferenced by the wrapper)
unsigned char* g_code = nullptr;                     // the client's code page
unsigned char* g_wrapper = nullptr;

struct Call { DWORD r; PadXState s; bool balanced; };
Call call(DWORD index = 0, void const* fn = nullptr) {
    Call c {};
    int delta = -1, regs = 0;
    g_fake_tick += 16;
    c.r = call_checked(fn ? fn : g_wrapper, index, &c.s, &delta, &regs);
    c.balanced = delta == 0 && regs == 1;
    return c;
}
Call press(unsigned buttons, int lt = 0, int lx = 0) {
    g_pad.packet += 1;
    g_pad.buttons = static_cast<WORD>(buttons);
    g_pad.lt = static_cast<BYTE>(lt);
    g_pad.lx = static_cast<SHORT>(lx);
    return call();
}

PadBus g_bus {};
bool g_all_balanced = true;
Call track(Call c) { g_all_balanced = g_all_balanced && c.balanced; return c; }

void dump(char const* path, void const* p, std::size_t n) {
    if (FILE* f = std::fopen(path, "wb")) { std::fwrite(p, 1, n, f); std::fclose(f); }
}

void run_tests() {
    using namespace padhook_detail;
    g_code = static_cast<unsigned char*>(VirtualAlloc(nullptr, 4096, 0, 0));
    g_wrapper = g_code + 0x40;                       // 16-aligned, as in the client
    std::memcpy(g_wrapper, kNov, 48);
    put32(g_wrapper + 1, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&g_slots[0])));
    put32(g_wrapper + 10, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&g_slots[1])));
    g_slots[0] = kHandle;
    g_slots[1] = reinterpret_cast<std::uintptr_t>(&shim_get);

    // unpatched, the wrapper calls the shim
    Call c = press(0x1000, 0, 1234);
    check("unpatched: the shim's state", c.r == 0 && c.s.buttons == 0x1000 && c.s.lx == 1234 && g_shim_calls == 1);
    check("unpatched: balanced", c.balanced);

    // the scan finds it in memory, and the patch goes in
    WrapperHit hit {0, 0, 0};
    int const n = scan_wrappers(g_code, 4096, reinterpret_cast<std::uintptr_t>(g_code), &hit);
    check("scan in memory", n == 1 && hit.site == reinterpret_cast<std::uintptr_t>(g_wrapper)
        && hit.handle_slot == reinterpret_cast<std::uintptr_t>(&g_slots[0]) && hit.fn_slot == reinterpret_cast<std::uintptr_t>(&g_slots[1])
        && !scan_verdict(n, hit));
    cached_bus() = &g_bus;
    char const* why = install_wrapper(&g_bus, hit);
    check("installed", why == nullptr);
    if (why) { std::printf("  %s\n", why); return; }
    auto const block = reinterpret_cast<std::uintptr_t>(g_bus.block);
    check("route 1 on the bus", g_bus.route == PADHOOK_ROUTE_WRAPPER && g_bus.site == hit.site);
    check("original is the adapter", reinterpret_cast<std::uintptr_t>(g_bus.original) == block + kAdapterAt);
    check("nobody drives yet", *g_bus.dispatch_ptr == nullptr);
    check("patched: mov eax, thunk at +9", g_wrapper[9] == 0xB8 && le32(g_wrapper + 10) == block + kThunkAt);
    check("patched: the rest unchanged", g_wrapper[0] == kNov[0] && std::memcmp(g_wrapper + 5, kNov + 5, 4) == 0
        && std::memcmp(g_wrapper + 14, kNov + 14, 34) == 0 && le32(g_wrapper + 1) == reinterpret_cast<std::uintptr_t>(&g_slots[0]));
    dump("wrapper.bin", g_wrapper, 48);
    dump("wrap_thunk.bin", g_bus.block + kThunkAt, kWrapThunkBytes);
    dump("adapter.bin", g_bus.block + kAdapterAt, kAdapterBytes);
    std::FILE* v = std::fopen("vma.txt", "w");
    if (v) { std::fprintf(v, "%#lx %#lx %#lx\n", static_cast<unsigned long>(hit.site), static_cast<unsigned long>(block + kThunkAt),
                          static_cast<unsigned long>(block + kAdapterAt)); std::fclose(v); }

    // 1. nobody driving: the thunk passes the call to the shim
    int shim0 = g_shim_calls;
    c = track(press(0x1000, 0, 1234));
    check("pass-through: the shim's state", c.r == 0 && c.s.buttons == 0x1000 && c.s.lx == 1234);
    check("pass-through: one shim call", g_shim_calls == shim0 + 1);
    check("pass-through: not through the dispatcher", g_bus.calls == 0);
    check("pass-through: balanced", c.balanced);
    c = track(call(1));
    check("pass-through: index 1 not connected", c.r == ERROR_DEVICE_NOT_CONNECTED && c.balanced);

    // 2. a dispatcher drives
    *g_bus.dispatch_ptr = &dispatch;
    shim0 = g_shim_calls;
    c = track(press(0x2000, 0, 555));
    check("driven: the shim's state", c.r == 0 && c.s.buttons == 0x2000 && c.s.lx == 555);
    check("driven: through the dispatcher once", g_bus.calls == 1 && g_shim_calls == shim0 + 1);
    check("driven: balanced", c.balanced);
    check("driven: recorded", g_bus.pads[0].buttons == 0x2000 && g_bus.pads[0].connected == 1 && g_bus.last_index == 0);
    c = track(call(1));
    check("driven: index 1 not connected", c.r == ERROR_DEVICE_NOT_CONNECTED && g_bus.pads[1].connected == 0 && c.balanced);
    for (int i = 0; i < 50; ++i) track(press(static_cast<unsigned>(i & 1) * 0x1000));
    check("driven: 50 more calls balanced", g_all_balanced && g_bus.calls == 52);

    // 3. the mask: a client holds the pad
    PadClient& cl = g_bus.clients[0];
    cl.occupied = 2;
    track(press(0x2000));                                    // B held before the capture
    cl.capture = 1;
    cl.alive_tick = static_cast<LONG>(g_fake_tick);
    c = track(press(0x2000 | 0x1000, 200, 20000));
    check("held: B held before is the game's", (c.s.buttons & 0x2000) != 0);
    check("held: a new A is hidden", (c.s.buttons & 0x1000) == 0);
    check("held: stick and trigger zeroed", c.s.lx == 0 && c.s.lt == 0);
    check("held: raw state recorded", g_bus.pads[0].buttons == (0x3000ul | PADHOOK_LT) && g_bus.pads[0].lx == 20000);
    cl.alive_tick = static_cast<LONG>(g_fake_tick);
    cl.capture = 0;
    c = track(press(0x1000));
    check("given back: A stays hidden until released", c.s.buttons == 0);
    track(press(0));
    c = track(press(0x1000));
    check("given back: a fresh A is the game's", c.s.buttons == 0x1000);
    track(press(0));
    // the heartbeat
    cl.capture = 1;
    cl.alive_tick = static_cast<LONG>(g_fake_tick);
    g_fake_tick += PADHOOK_HEARTBEAT_MS + 50;
    c = track(press(0x4000));
    check("lapsed: X reaches the game", c.s.buttons == 0x4000 && cl.capture == 0 && g_bus.lapses == 1);
    track(press(0));

    // 4. the chord (rev 2) through the same path
    unsigned const L3 = 0x40, R3 = 0x80;
    cl.chord = L3 | R3;
    track(press(0));
    c = track(press(L3));
    check("chord: L3 held back", (c.s.buttons & L3) == 0);
    c = track(press(L3 | R3));
    check("chord: never seen", (c.s.buttons & (L3 | R3)) == 0);
    track(press(0));
    track(press(L3));
    c = track(press(0));
    check("chord: a tap alone is replayed", (c.s.buttons & L3) != 0);
    track(press(0));
    track(press(0));
    cl.chord = 0;

    // 5. the client rewrites F between calls (a reload): the next call uses the new function
    g_slots[1] = reinterpret_cast<std::uintptr_t>(&shim_get2);
    c = track(press(0x1000));
    check("F rewritten, driven: the new function", g_shim2_calls == 1 && c.s.buttons == (0x1000 | 0x0010) && c.balanced);
    *g_bus.dispatch_ptr = nullptr;
    c = track(press(0x1000));
    check("F rewritten, pass-through: the new function", g_shim2_calls == 2 && c.s.buttons == (0x1000 | 0x0010) && c.balanced);
    g_slots[1] = reinterpret_cast<std::uintptr_t>(&shim_get);
    *g_bus.dispatch_ptr = &dispatch;
    c = track(press(0x1000));
    check("F back", g_shim2_calls == 2 && c.s.buttons == 0x1000);

    // 6. the unloader zeroed H: the wrapper returns before the patch
    LONG const calls0 = g_bus.calls;
    shim0 = g_shim_calls;
    g_slots[0] = 0;
    c = track(press(0x1000));
    check("H zero: the wrapper's own failure", c.r == padhook_detail::kWrapperFail && c.balanced);
    check("H zero: no shim call, no dispatch", g_shim_calls == shim0 && g_bus.calls == calls0);
    g_slots[0] = kHandle;

    // 7. H written but F not yet (the client between its two writes): what the wrapper returned before
    g_slots[1] = 0;
    c = track(press(0x1000));
    check("F zero, driven: the wrapper's failure", c.r == padhook_detail::kWrapperFail && g_shim_calls == shim0 && c.balanced);
    check("F zero, driven: recorded as not connected", g_bus.pads[0].connected == 0);
    *g_bus.dispatch_ptr = nullptr;
    c = track(press(0x1000));
    check("F zero, pass-through: the wrapper's failure", c.r == padhook_detail::kWrapperFail && g_shim_calls == shim0 && c.balanced);
    g_slots[1] = reinterpret_cast<std::uintptr_t>(&shim_get);
    *g_bus.dispatch_ptr = &dispatch;
    c = track(press(0x1000));
    check("F back: connected again", c.r == 0 && g_bus.pads[0].connected == 1);
    check("every call balanced", g_all_balanced);

    // 8. bytes that changed under the patch: refused, and the bus left as it was
    unsigned char* other = g_code + 0x100;
    std::memcpy(other, kNov, 48);
    put32(other + 1, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&g_slots[2])));
    put32(other + 10, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&g_slots[3])));
    WrapperHit oh {0, 0, 0};
    scan_wrappers(other, 48, reinterpret_cast<std::uintptr_t>(other), &oh);
    other[9] = 0xE9;                                         // another tool's jump went in after the scan
    static PadBus fresh {};
    why = install_wrapper(&fresh, oh);
    check("changed bytes: refused", why == kWrapChanged);
    check("changed bytes: the bus left clear", fresh.block == nullptr && fresh.route == 0 && fresh.original == nullptr && fresh.site == 0);
    check("changed bytes: untouched", other[9] == 0xE9);
}

// ---- the revision 2 thunk in the client's slot (the first design of step 3) ----

// returns the child's exit code, or -signal
int in_child(bool driven) {
    std::fflush(stdout);
    pid_t const pid = fork();
    if (pid == 0) {
        using namespace padhook_detail;
        unsigned char* code = g_code + 0x200;
        std::memcpy(code, kNov, 48);                         // unpatched: the old design patched only the slot
        static std::uintptr_t slots[2];
        put32(code + 1, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&slots[0])));
        put32(code + 10, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&slots[1])));
        auto* block = static_cast<unsigned char*>(VirtualAlloc(nullptr, 64, 0, 0));
        std::memset(block, 0xCC, 64);
        std::memset(block, 0, 4);
        // revision 2's thunk, its pass-through aimed at the shim
        thunk_bytes(reinterpret_cast<std::uintptr_t>(block), reinterpret_cast<std::uintptr_t>(&shim_get) - 2, block + kThunkAt);
        static PadBus bus {};
        bus.block = block;
        bus.dispatch_ptr = reinterpret_cast<padhook_get_fn volatile*>(block);
        bus.original = &shim_stdcall;
        cached_bus() = &bus;
        slots[0] = kHandle;
        slots[1] = reinterpret_cast<std::uintptr_t>(block + kThunkAt);
        if (driven) *bus.dispatch_ptr = &dispatch;
        g_pad.buttons = 0x1000;
        Call const c = call(0, code);
        _exit(c.r == 0 && c.s.buttons == 0x1000 && c.balanced ? 0 : 1);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) return -WTERMSIG(status);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 99;
}

void old_thunk_tests() {
    int const idle = in_child(false);
    check("old thunk in the slot, nobody driving: passes through", idle == 0);
    int const driven = in_child(true);
    check("old thunk in the slot, a dispatcher driving: crashes", driven < 0);
    std::printf("old thunk: nobody driving -> exit %d; a dispatcher driving -> %s %d\n", idle, driven < 0 ? "signal" : "exit",
                driven < 0 ? -driven : driven);
}

}  // namespace

int main() {
    scan_tests();
    run_tests();
    old_thunk_tests();
    std::printf("wrapper: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
