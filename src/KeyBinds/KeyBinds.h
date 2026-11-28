// KeyBinds.h - a read-only look at the client's keyboard bindings.
//
// The client keeps its keyboard bindings in two tables in FFXiMain.dll's .data, and rewrites them at run time: at
// login from the character's config values (the movement keys are config ids 74..105, the shortcut keys 106..137),
// and from its key-assignment object. Config id 66 is the keyboard mode: the movement keys act only in modes 1 and 2.
// Research: Chronicle's docs/key-bindings-research.md.
//
// This header finds the tables and two values by signature over the client's code, checks what it found, and copies
// them. It patches nothing, takes no lock, calls no client code and keeps no shared state, so it has no bus and no
// frozen layout. The game writes the tables on its own thread: a torn copy can hold one stale binding until the
// next read.
//
// What it finds, in the 2026-11-02 build (each signature holds the address as a 32-bit operand):
//   the action table    0x1036E6E8  188 actions x 3 bindings, one WORD each: the DIK code, then a rule byte
//   the category table  0x1036DF98  188 WORDs
//   the movement table  0x1036F128  entries of 16 bytes: DIK, modifier rule, context, 5 unused, 8 bytes of bits;
//                                   the client stops at the first entry whose 8 bytes are all zero
//   the keyboard mode   [[0x10663924]+0x44]   0, 1 or 2
//   the layout          [[0x10457BD8]+8]
//
// The pure part (the signatures, scan_binds, binds_verdict, in_sections) runs on Linux in test/binds_linux.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include "../KeyHook/KeyHook.h"
#endif

namespace keybinds {

constexpr unsigned kActions = 188;
constexpr unsigned kPerAction = 3;
constexpr std::size_t kActionBytes = kActions * kPerAction * 2;   // 0x468: the pad table follows at once
constexpr std::size_t kCategoryBytes = kActions * 2;
constexpr unsigned kMoveEntry = 16;
constexpr unsigned kMoveMax = 64;                                 // the client has 43, the last one empty
constexpr std::uint32_t kModeAt = 0x44;                           // the mode in the object the mode pointer holds
constexpr std::uint32_t kLayoutAt = 0x08;                         // the layout in the object the layout pointer holds

enum Sig { S_ACT_GET, S_ACT_QUERY, S_PAD, S_CAT, S_MOVE_GET, S_MOVE_READ, S_MODE, S_LAYOUT, S_COUNT };

struct SigDef {
    short const* bytes;     // -1 is a wildcard
    std::size_t n;
    std::size_t ops[4];     // the offsets of its 32-bit operands (0 ends the list: no operand sits at 0)
};

// The bytes of the 2026-11-02 build, with each operand and each jump as a wildcard.
constexpr short kActGet[] = {      // 0x10124B00: return the first binding's DIK of an action
    0x0F, 0xBF, 0x44, 0x24, 0x04,               // movsx eax, word [esp+4]
    0x8D, 0x04, 0x40,                           // lea eax, [eax+eax*2]
    0x8A, 0x04, 0x45, -1, -1, -1, -1,           // mov al, [eax*2+table]
    0xC3};
constexpr short kActQuery[] = {    // 0x10124FC6: the action query reads one binding
    0x66, 0x8B, 0x04, 0x55, -1, -1, -1, -1,     // mov ax, [edx*2+table]
    0x8D, 0x34, 0x55, -1, -1, -1, -1};          // lea esi, [edx*2+table]
constexpr short kPad[] = {         // 0x10124F89: the pad table, right after the action table
    0x8B, 0x04, 0xFD, -1, -1, -1, -1,           // mov eax, [edi*8+pad]
    0x85, 0xC0};                                // test eax, eax
constexpr short kCat[] = {         // 0x10124F26: an action's category
    0x66, 0x8B, 0x04, 0x7D, -1, -1, -1, -1,     // mov ax, [edi*2+categories]
    0x66, 0x85, 0xC0};                          // test ax, ax
constexpr short kMoveGet[] = {     // 0x10124AD0: a movement entry's DIK
    0x0F, 0xBE, 0x44, 0x24, 0x04,               // movsx eax, byte [esp+4]
    0xC1, 0xE0, 0x04,                           // shl eax, 4
    0x8A, 0x80, -1, -1, -1, -1,                 // mov al, [eax+table]
    0xC3};
constexpr short kMoveRead[] = {    // 0x10124C25: the movement reader's loop start
    0x8B, 0x0D, -1, -1, -1, -1,                 // mov ecx, [table+8]
    0x88, 0x44, 0x24, -1,                       // mov [esp+..], al
    0x0B, 0x0D, -1, -1, -1, -1,                 // or ecx, [table+12]
    0x0F, 0x84, -1, -1, -1, -1,                 // jz
    0x57,                                       // push edi
    0xBF, -1, -1, -1, -1,                       // mov edi, table+8
    0xBE, -1, -1, -1, -1};                      // mov esi, table+1
constexpr short kMode[] = {        // 0x101B94B0: "do letters stay out of the chat line?" (mode 1 or 2)
    0xA1, -1, -1, -1, -1,                       // mov eax, [mode pointer]
    0x85, 0xC0, 0x74, 0x11,                     // test eax, eax / jz
    0x8B, 0x40, 0x44,                           // mov eax, [eax+0x44]
    0x83, 0xE8, 0x00, 0x74, 0x09,               // sub eax, 0 / jz
    0x48, 0x74, 0x03, 0x48, 0x75, 0x03,         // dec / jz / dec / jnz
    0xB0, 0x01, 0xC3};                          // mov al, 1 / ret
constexpr short kLayout[] = {      // 0x1019E4C3 and 0x1019E80E: the defaults by layout
    0xA1, -1, -1, -1, -1,                       // mov eax, [layout pointer]
    0x8B, 0x40, 0x08,                           // mov eax, [eax+8]
    0x83, 0xE8, 0x02};                          // sub eax, 2

constexpr SigDef kSigs[S_COUNT] = {
    {kActGet, sizeof(kActGet) / sizeof(short), {11, 0, 0, 0}},
    {kActQuery, sizeof(kActQuery) / sizeof(short), {4, 11, 0, 0}},
    {kPad, sizeof(kPad) / sizeof(short), {3, 0, 0, 0}},
    {kCat, sizeof(kCat) / sizeof(short), {4, 0, 0, 0}},
    {kMoveGet, sizeof(kMoveGet) / sizeof(short), {10, 0, 0, 0}},
    {kMoveRead, sizeof(kMoveRead) / sizeof(short), {2, 12, 24, 29}},
    {kMode, sizeof(kMode) / sizeof(short), {1, 0, 0, 0}},
    {kLayout, sizeof(kLayout) / sizeof(short), {1, 0, 0, 0}},
};

inline std::uint32_t le32(unsigned char const* p) {
    std::uint32_t v = 0;
    std::memcpy(&v, p, 4);
    return v;
}

struct BindScan {
    int count[S_COUNT];                 // the matches of each signature
    std::uint32_t op[S_COUNT][4];       // the operands of its first match
    bool layout_differs;                // a later layout match had another operand
};

inline bool sig_at(unsigned char const* p, SigDef const& d) {
    for (std::size_t i = 0; i < d.n; ++i)
        if (d.bytes[i] >= 0 && p[i] != static_cast<unsigned char>(d.bytes[i])) return false;
    return true;
}

// Every signature over n bytes at p, which the process sees at `base`. Adds to s (several runs make one scan).
inline void scan_binds(unsigned char const* p, std::size_t n, std::uintptr_t base, BindScan& s) {
    (void)base;
    for (std::size_t off = 0; off < n; ++off) {
        for (int k = 0; k < S_COUNT; ++k) {
            SigDef const& d = kSigs[k];
            if (off + d.n > n || p[off] != static_cast<unsigned char>(d.bytes[0]) || !sig_at(p + off, d)) continue;
            if (s.count[k]++ == 0) {
                for (int j = 0; j < 4 && d.ops[j]; ++j) s.op[k][j] = le32(p + off + d.ops[j]);
            } else if (k == S_LAYOUT && le32(p + off + d.ops[0]) != s.op[k][0]) {
                s.layout_differs = true;
            }
        }
    }
}

// The addresses the scan gives, once binds_verdict passed.
struct BindAt {
    std::uint32_t actions, categories, moves, mode_ptr, layout_ptr;
};
inline BindAt binds_at(BindScan const& s) {
    return {s.op[S_ACT_GET][0], s.op[S_CAT][0], s.op[S_MOVE_GET][0], s.op[S_MODE][0], s.op[S_LAYOUT][0]};
}

// What the scan means: null when the tables were found and agree, otherwise why not.
inline char const* binds_verdict(BindScan const& s) {
    static char const* const names[S_COUNT] = {"the binding read", "the action query", "the pad table read",
                                               "the category read", "the movement read", "the movement loop",
                                               "the keyboard mode test", "the layout read"};
    static char why[96];
    for (int k = 0; k < S_COUNT; ++k) {
        if (s.count[k] == 0) {
            std::snprintf(why, sizeof(why), "were not found: no %s (a client update?)", names[k]);
            return why;
        }
#ifndef KEYBINDS_FAULT_FIRST_MATCH
        if (s.count[k] > 1 && k != S_LAYOUT) {
            std::snprintf(why, sizeof(why), "are not unique: %d of %s", s.count[k], names[k]);
            return why;
        }
#endif
    }
    if (s.layout_differs) return "are not unique: the layout reads disagree";
#ifndef KEYBINDS_FAULT_NO_AGREE
    std::uint32_t const act = s.op[S_ACT_GET][0], move = s.op[S_MOVE_GET][0];
    if (s.op[S_ACT_QUERY][0] != act || s.op[S_ACT_QUERY][1] != act) return "disagree: two action tables";
    if (s.op[S_PAD][0] != act + kActionBytes) return "disagree: the action table has another size";
    if (s.op[S_MOVE_READ][0] != move + 8 || s.op[S_MOVE_READ][1] != move + 12 || s.op[S_MOVE_READ][2] != move + 8
        || s.op[S_MOVE_READ][3] != move + 1)
        return "disagree: two movement tables";
#endif
    return nullptr;
}

struct Section {
    std::uintptr_t begin, end;
    bool writable, exec;
};

// Is [address, address + size) inside one writable section that holds no code?
inline bool in_sections(std::uintptr_t address, std::size_t size, Section const* sec, int n) {
    if (address == 0 || size == 0) return false;
#ifdef KEYBINDS_FAULT_NO_PLACE
    return n > 0 && sec != nullptr;
#endif
    for (int i = 0; i < n; ++i)
        if (sec[i].writable && !sec[i].exec && address >= sec[i].begin && address + size <= sec[i].end) return true;
    return false;
}

// Where each table and value must lie: null when they all fit, otherwise why not. Pure: tests call it.
inline char const* place_verdict(BindAt const& at, Section const* sec, int n) {
    if (!in_sections(at.actions, kActionBytes, sec, n)) return "are not in the client's data (the action table)";
    if (!in_sections(at.categories, kCategoryBytes, sec, n)) return "are not in the client's data (the categories)";
    if (!in_sections(at.moves, kMoveEntry, sec, n)) return "are not in the client's data (the movement table)";
    if (!in_sections(at.mode_ptr, 4, sec, n)) return "are not in the client's data (the keyboard mode)";
    if (!in_sections(at.layout_ptr, 4, sec, n)) return "are not in the client's data (the layout)";
    return nullptr;
}

// How many movement entries to copy from bytes p (`avail` bytes in the section): up to and without the first
// entry whose 8 bits bytes are all zero, as the client's reader stops there. Pure: tests call it.
inline unsigned move_count(unsigned char const* p, std::size_t avail) {
    unsigned n = 0;
    while (n < kMoveMax && (n + 1) * static_cast<std::size_t>(kMoveEntry) <= avail) {
        unsigned char const* e = p + n * kMoveEntry;
#ifndef KEYBINDS_FAULT_MOVE_ALL
        if (le32(e + 8) == 0 && le32(e + 12) == 0) break;
#else
        (void)e;
#endif
        ++n;
    }
    return n;
}

// One copy of everything.
struct BindRead {
    std::uint16_t actions[kActions * kPerAction];
    std::uint16_t categories[kActions];
    std::uint32_t moves[kMoveMax];      // DIK | modifier rule << 8 | context << 16
    unsigned n_moves;
    int mode;                           // 0..2, or -1 when unknown (no object yet, or another value)
    int layout;                         // 0..15, or -1 when unknown
};

// Copy from an image whose tables are at `at` (read through a pointer translation, so tests can use a buffer).
// read(addr, size) returns the bytes at that address or null; avail(addr) gives the bytes left in its section.
template <typename Read, typename Avail>
inline void copy_binds(BindAt const& at, Read&& read, Avail&& avail, BindRead& out) {
    std::memset(&out, 0, sizeof(out));
    if (auto const* p = static_cast<unsigned char const*>(read(at.actions, kActionBytes)))
        std::memcpy(out.actions, p, kActionBytes);
    if (auto const* p = static_cast<unsigned char const*>(read(at.categories, kCategoryBytes)))
        std::memcpy(out.categories, p, kCategoryBytes);
    std::size_t const room = avail(at.moves);
    std::size_t const want = room < kMoveMax * kMoveEntry ? room : kMoveMax * kMoveEntry;
    if (auto const* p = static_cast<unsigned char const*>(read(at.moves, want))) {
        out.n_moves = move_count(p, want);
        for (unsigned i = 0; i < out.n_moves; ++i) {
            unsigned char const* e = p + i * kMoveEntry;
            out.moves[i] = e[0] | (static_cast<std::uint32_t>(e[1]) << 8) | (static_cast<std::uint32_t>(e[2]) << 16);
        }
    }
    out.mode = out.layout = -1;
    if (auto const* p = static_cast<unsigned char const*>(read(at.mode_ptr, 4))) {
        std::uint32_t const obj = le32(p);
        if (auto const* q = static_cast<unsigned char const*>(obj ? read(obj + kModeAt, 4) : nullptr)) {
            std::uint32_t const v = le32(q);
            if (v <= 2) out.mode = static_cast<int>(v);
        }
    }
    if (auto const* p = static_cast<unsigned char const*>(read(at.layout_ptr, 4))) {
        std::uint32_t const obj = le32(p);
        if (auto const* q = static_cast<unsigned char const*>(obj ? read(obj + kLayoutAt, 4) : nullptr)) {
            std::uint32_t const v = le32(q);
            if (v <= 15) out.layout = static_cast<int>(v);
        }
    }
}

#ifdef _WIN32

// The scan, once per process (the client's code never changes). state: 0 not yet (the client is not loaded),
// 1 found, 2 refused for good.
struct BindFind {
    int state;
    BindAt at;
    Section sec[16];
    int n_sec;
    char const* why;
};
inline BindFind& bind_find() { static BindFind f {}; return f; }

inline BindFind& find_binds() {
    BindFind& f = bind_find();
    if (f.state != 0) return f;
    BindScan s {};
    HMODULE game = nullptr;
    int n_sec = 0;
    Section sec[16] {};
    bool const headers = keyhook_detail::for_each_game_section(&game, [&](std::uintptr_t begin, std::uintptr_t end, DWORD flags) {
        if (n_sec < 16) sec[n_sec++] = {begin, end, (flags & IMAGE_SCN_MEM_WRITE) != 0, (flags & IMAGE_SCN_MEM_EXECUTE) != 0};
        if (!(flags & IMAGE_SCN_MEM_EXECUTE)) return;
        keyhook_detail::for_each_readable_run(begin, end, [&](unsigned char const* p, std::size_t n, std::uintptr_t at) {
            scan_binds(p, n, at, s);
        });
    });
    if (!game) { f.why = "were not looked for (" KEYHOOK_GAME " is not loaded)"; return f; }
    f.state = 2;
    if (!headers) { f.why = "were not looked for (" KEYHOOK_GAME "'s headers are unreadable)"; return f; }
    if ((f.why = binds_verdict(s)) != nullptr) return f;
    f.at = binds_at(s);
    std::memcpy(f.sec, sec, sizeof(sec));
    f.n_sec = n_sec;
    if ((f.why = place_verdict(f.at, f.sec, f.n_sec)) != nullptr) return f;
    f.state = 1;
    return f;
}

// One live copy. False (with why) when the tables cannot be read in this process.
inline bool read_binds(BindRead& out, char const** why, bool simulate_nosig = false) {
    if (simulate_nosig) { *why = "were not found: no the binding read (a client update?)"; return false; }
    BindFind& f = find_binds();
    if (f.state != 1) { *why = f.why; return false; }
    auto read = [&](std::uintptr_t a, std::size_t n) -> void const* {
        return keyhook_detail::span_readable(a, n) ? reinterpret_cast<void const*>(a) : nullptr;
    };
    auto avail = [&](std::uintptr_t a) -> std::size_t {
        for (int i = 0; i < f.n_sec; ++i)
            if (a >= f.sec[i].begin && a < f.sec[i].end) return f.sec[i].end - a;
        return 0;
    };
    copy_binds(f.at, read, avail, out);
    *why = nullptr;
    return true;
}

#endif  // _WIN32

}  // namespace keybinds
