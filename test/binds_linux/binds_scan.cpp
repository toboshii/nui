// binds_scan.cpp - KeyBinds.h's pure part on Linux: the signatures over the client's own bytes, every refusal, and
// the copy. With an image path (the saved image of the 2026-11-02 client, base 0x10000000), it also scans the whole
// image's .text and checks the tables it copies against the values in Chronicle's docs/key-bindings-research.md.
//   binds_scan [image]
#include "KeyBinds/KeyBinds.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace keybinds;

static int g_pass = 0, g_fail = 0;
#define CHECK(name, cond)                                                       \
    do {                                                                        \
        if (cond) ++g_pass;                                                     \
        else { ++g_fail; std::printf("FAIL %s (%s:%d)\n", name, __FILE__, __LINE__); } \
    } while (0)

// The bytes at each signature site in the 2026-11-02 build, with the client's own operands.
struct Site { std::uint32_t va; unsigned n; unsigned char b[40]; };
static Site const kSites[] = {
    {0x10124B00, 16, {0x0F, 0xBF, 0x44, 0x24, 0x04, 0x8D, 0x04, 0x40, 0x8A, 0x04, 0x45, 0xE8, 0xE6, 0x36, 0x10, 0xC3}},
    {0x10124FC6, 15, {0x66, 0x8B, 0x04, 0x55, 0xE8, 0xE6, 0x36, 0x10, 0x8D, 0x34, 0x55, 0xE8, 0xE6, 0x36, 0x10}},
    {0x10124F89, 9, {0x8B, 0x04, 0xFD, 0x50, 0xEB, 0x36, 0x10, 0x85, 0xC0}},
    {0x10124F26, 11, {0x66, 0x8B, 0x04, 0x7D, 0x98, 0xDF, 0x36, 0x10, 0x66, 0x85, 0xC0}},
    {0x10124AD0, 15, {0x0F, 0xBE, 0x44, 0x24, 0x04, 0xC1, 0xE0, 0x04, 0x8A, 0x80, 0x28, 0xF1, 0x36, 0x10, 0xC3}},
    {0x10124C25, 33, {0x8B, 0x0D, 0x30, 0xF1, 0x36, 0x10, 0x88, 0x44, 0x24, 0x0A, 0x0B, 0x0D, 0x34, 0xF1, 0x36, 0x10, 0x0F,
                      0x84, 0xA3, 0x01, 0x00, 0x00, 0x57, 0xBF, 0x30, 0xF1, 0x36, 0x10, 0xBE, 0x29, 0xF1, 0x36, 0x10}},
    {0x101B94B0, 26, {0xA1, 0x24, 0x39, 0x66, 0x10, 0x85, 0xC0, 0x74, 0x11, 0x8B, 0x40, 0x44, 0x83, 0xE8, 0x00, 0x74, 0x09,
                      0x48, 0x74, 0x03, 0x48, 0x75, 0x03, 0xB0, 0x01, 0xC3}},
    {0x1019E4C3, 11, {0xA1, 0xD8, 0x7B, 0x45, 0x10, 0x8B, 0x40, 0x08, 0x83, 0xE8, 0x02}},
    {0x1019E80E, 11, {0xA1, 0xD8, 0x7B, 0x45, 0x10, 0x8B, 0x40, 0x08, 0x83, 0xE8, 0x02}},
};
constexpr int kNSites = sizeof(kSites) / sizeof(kSites[0]);

// A code buffer: each site at its own offset from the code base, the rest 0xCC. skip: a site left out (-1: none).
// twice: a site written a second time further on (-1: none).
static std::vector<unsigned char> code(int skip = -1, int twice = -1) {
    std::vector<unsigned char> c(0x1C0000, 0xCC);
    for (int i = 0; i < kNSites; ++i) {
        if (i == skip) continue;
        std::memcpy(&c[kSites[i].va - 0x10100000], kSites[i].b, kSites[i].n);
    }
    if (twice >= 0) std::memcpy(&c[0x1B0000], kSites[twice].b, kSites[twice].n);
    return c;
}

static BindScan scan(std::vector<unsigned char> const& c) {
    BindScan s {};
    scan_binds(c.data(), c.size(), 0x10100000, s);
    return s;
}

// The 2026-11-02 sections that matter: .text (code) and .data.
static Section const kSec[] = {{0x10001000, 0x10329C2E, false, true}, {0x1032A000, 0x1034F6D5, false, false},
                               {0x10350000, 0x109FA8E9, true, false}};

static bool has(char const* why, char const* part) { return why && std::strstr(why, part) != nullptr; }

static void test_sites() {
    BindScan s = scan(code());
    CHECK("SIG-1 every signature matches the client's bytes once (the layout twice)",
          s.count[S_ACT_GET] == 1 && s.count[S_ACT_QUERY] == 1 && s.count[S_PAD] == 1 && s.count[S_CAT] == 1
              && s.count[S_MOVE_GET] == 1 && s.count[S_MOVE_READ] == 1 && s.count[S_MODE] == 1 && s.count[S_LAYOUT] == 2);
    CHECK("SIG-2 the verdict passes", binds_verdict(s) == nullptr);
    BindAt const at = binds_at(s);
    CHECK("SIG-3 the addresses", at.actions == 0x1036E6E8 && at.categories == 0x1036DF98 && at.moves == 0x1036F128
                                     && at.mode_ptr == 0x10663924 && at.layout_ptr == 0x10457BD8);
    CHECK("SIG-4 they are in .data", place_verdict(at, kSec, 3) == nullptr);

    char const* const names[S_COUNT] = {"binding read", "action query", "pad table", "category", "movement read",
                                        "movement loop", "keyboard mode", "layout read"};
    for (int k = 0; k < S_COUNT; ++k) {
        int const site = k == S_LAYOUT ? -1 : k;
        if (site < 0) continue;
        char const* why = binds_verdict(scan(code(site)));
        char label[96];
        std::snprintf(label, sizeof(label), "REF-1 no %s: refused", names[k]);
        CHECK(label, has(why, "were not found") && has(why, names[k]));
        why = binds_verdict(scan(code(-1, site)));
        std::snprintf(label, sizeof(label), "REF-2 two of %s: refused", names[k]);
        CHECK(label, has(why, "not unique"));
    }
    {   // both layout reads gone
        std::vector<unsigned char> c = code(7);
        std::memset(&c[kSites[8].va - 0x10100000], 0xCC, kSites[8].n);
        CHECK("REF-1 no layout read: refused", has(binds_verdict(scan(c)), "layout read"));
    }
    {   // a third layout read with the same pointer is fine, one with another pointer is not
        std::vector<unsigned char> c = code(-1, 7);
        CHECK("REF-3 a third layout read that agrees: passes", binds_verdict(scan(c)) == nullptr);
        c[0x1B0001] = 0xDC;
        CHECK("REF-3 a layout read with another pointer: refused", has(binds_verdict(scan(c)), "layout reads disagree"));
    }
    {   // the action query reads another table
        std::vector<unsigned char> c = code();
        c[kSites[1].va - 0x10100000 + 4] = 0xEA;
        CHECK("REF-4 the action query disagrees: refused", has(binds_verdict(scan(c)), "two action tables"));
        c = code();
        c[kSites[1].va - 0x10100000 + 11] = 0xEA;
        CHECK("REF-4 the action query's second operand disagrees: refused", has(binds_verdict(scan(c)), "two action tables"));
    }
    {   // the pad table is not right after 188 x 3 bindings
        std::vector<unsigned char> c = code();
        c[kSites[2].va - 0x10100000 + 3] = 0x52;
        CHECK("REF-5 another action count: refused", has(binds_verdict(scan(c)), "another size"));
    }
    for (int op = 0; op < 4; ++op) {   // the movement loop reads another table
        std::size_t const at_op[4] = {2, 12, 24, 29};
        std::vector<unsigned char> c = code();
        c[kSites[5].va - 0x10100000 + at_op[op]] ^= 0x40;
        CHECK("REF-6 the movement loop disagrees: refused", has(binds_verdict(scan(c)), "two movement tables"));
    }
    {   // the places
        BindAt bad = at;
        Section ro[3] = {kSec[0], kSec[1], {0x10350000, 0x109FA8E9, false, false}};
        CHECK("PLACE-1 a read-only .data: refused", place_verdict(at, ro, 3) != nullptr);
        Section rwx[3] = {kSec[0], kSec[1], {0x10350000, 0x109FA8E9, true, true}};
        CHECK("PLACE-2 a section with code: refused", place_verdict(at, rwx, 3) != nullptr);
        bad.actions = 0x10200000;
        CHECK("PLACE-3 the action table in .text: refused", has(place_verdict(bad, kSec, 3), "action table"));
        bad = at; bad.actions = 0x109FA8E9 - 0x400;
        CHECK("PLACE-4 an action table past the section's end: refused", has(place_verdict(bad, kSec, 3), "action table"));
        bad = at; bad.categories = 0x1032B000;
        CHECK("PLACE-5 the categories in .rdata: refused", has(place_verdict(bad, kSec, 3), "categories"));
        bad = at; bad.moves = 0;
        CHECK("PLACE-6 no movement table: refused", has(place_verdict(bad, kSec, 3), "movement"));
        bad = at; bad.mode_ptr = 0x20000000;
        CHECK("PLACE-7 the mode outside the client: refused", has(place_verdict(bad, kSec, 3), "keyboard mode"));
        bad = at; bad.layout_ptr = 0x10001000;
        CHECK("PLACE-8 the layout in .text: refused", has(place_verdict(bad, kSec, 3), "layout"));
    }
}

// A stand-in .data with tables and the two objects, read through a translation as read_binds does.
struct Data {
    std::uint32_t base = 0x10350000;
    std::vector<unsigned char> d = std::vector<unsigned char>(0x400000, 0);
    std::vector<unsigned char> heap = std::vector<unsigned char>(0x100, 0);   // the two objects, at 0x20000000
    void* at(std::uint32_t a, std::size_t n) {
        if (a >= base && a + n <= base + d.size()) return &d[a - base];
        if (a >= 0x20000000 && a + n <= 0x20000000 + heap.size()) return &heap[a - 0x20000000];
        return nullptr;
    }
    void put32(std::uint32_t a, std::uint32_t v) { std::memcpy(at(a, 4), &v, 4); }
    void put16(std::uint32_t a, std::uint16_t v) { std::memcpy(at(a, 2), &v, 2); }
    std::size_t avail(std::uint32_t a) { return a >= base && a < base + d.size() ? base + d.size() - a : 0; }
};

static BindRead copy(Data& m, BindAt const& at) {
    BindRead r;
    copy_binds(at, [&](std::uintptr_t a, std::size_t n) -> void const* { return m.at(static_cast<std::uint32_t>(a), n); },
               [&](std::uintptr_t a) { return m.avail(static_cast<std::uint32_t>(a)); }, r);
    return r;
}

static void move_entry(Data& m, std::uint32_t table, unsigned i, unsigned dik, unsigned mod, unsigned ctx, std::uint32_t bits) {
    unsigned char* e = static_cast<unsigned char*>(m.at(table + i * 16, 16));
    e[0] = static_cast<unsigned char>(dik); e[1] = static_cast<unsigned char>(mod); e[2] = static_cast<unsigned char>(ctx);
    std::memcpy(e + 8, &bits, 4);
}

static void test_copy() {
    BindAt const at = binds_at(scan(code()));
    Data m;
    m.put16(at.actions + 2 * (14 * 3 + 0), 0x1015);    // id 14: Ctrl or Alt + Y, Enter, NumEnter
    m.put16(at.actions + 2 * (14 * 3 + 1), 0x001C);
    m.put16(at.actions + 2 * (14 * 3 + 2), 0x009C);
    m.put16(at.actions + 2 * (187 * 3 + 2), 0x8025);   // the last binding of the last id
    m.put16(at.actions + kActionBytes, 0xBEEF);          // the pad table: not a binding
    m.put16(at.categories + 2 * 14, 0xFFFF);
    m.put16(at.categories + 2 * 187, 0x0040);
    move_entry(m, at.moves, 0, 0x11, 0, 4, 0x8000);
    move_entry(m, at.moves, 1, 0x25, 8, 6, 0x0010);
    move_entry(m, at.moves, 2, 0x00, 0, 4, 0x0001);      // an empty key: still an entry the client reads
    m.put32(at.mode_ptr, 0x20000000);
    m.put32(0x20000000 + kModeAt, 2);
    m.put32(at.layout_ptr, 0x20000080);
    m.put32(0x20000080 + kLayoutAt, 1);

    BindRead r = copy(m, at);
    CHECK("COPY-1 the bindings", r.actions[42] == 0x1015 && r.actions[43] == 0x001C && r.actions[44] == 0x009C);
    CHECK("COPY-2 the last binding, and not the pad table", r.actions[187 * 3 + 2] == 0x8025);
    CHECK("COPY-3 the categories", r.categories[14] == 0xFFFF && r.categories[187] == 0x0040);
    CHECK("COPY-4 the moves end at the first entry with no bits", r.n_moves == 3);
    CHECK("COPY-5 a move packs DIK, rule and context", r.moves[0] == 0x040011 && r.moves[1] == 0x060825 && r.moves[2] == 0x040000);
    CHECK("COPY-6 the mode and the layout", r.mode == 2 && r.layout == 1);

    m.put32(0x20000000 + kModeAt, 3);
    m.put32(0x20000080 + kLayoutAt, 16);
    r = copy(m, at);
    CHECK("COPY-7 a mode or a layout out of range is unknown", r.mode == -1 && r.layout == -1);
    m.put32(at.mode_ptr, 0);
    m.put32(at.layout_ptr, 0x30000000);
    r = copy(m, at);
    CHECK("COPY-8 a null or unreadable object is unknown", r.mode == -1 && r.layout == -1);

    for (unsigned i = 0; i < kMoveMax + 4; ++i) move_entry(m, at.moves, i, 0x10 + i, 0, 4, 1);
    r = copy(m, at);
    CHECK("COPY-9 at most kMoveMax moves", r.n_moves == kMoveMax);
    BindAt edge = at;
    edge.moves = m.base + static_cast<std::uint32_t>(m.d.size()) - 5 * 16 - 8;
    for (unsigned i = 0; i < 5; ++i) move_entry(m, edge.moves, i, 0x20, 0, 4, 1);
    r = copy(m, edge);
    CHECK("COPY-10 the moves stop at the section's end", r.n_moves == 5);
}

// The whole saved image: its .text scanned as nui scans the live client, then the copy from its .data.
static void test_image(char const* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::printf("FAIL IMG-0 cannot open %s\n", path); ++g_fail; return; }
    std::vector<unsigned char> img;
    unsigned char buf[1 << 16];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) img.insert(img.end(), buf, buf + n);
    std::fclose(f);
    std::uint32_t const base = 0x10000000;
    auto u32 = [&](std::size_t o) { return le32(&img[o]); };
    auto u16 = [&](std::size_t o) { return static_cast<unsigned>(img[o] | (img[o + 1] << 8)); };
    std::size_t const nt = u32(0x3C), opt = nt + 24, nsec = u16(nt + 6), optsz = u16(nt + 20);
    std::vector<Section> sec;
    BindScan s {};
    for (std::size_t i = 0; i < nsec; ++i) {
        std::size_t const h = opt + optsz + 40 * i;
        std::uint32_t const va = base + u32(h + 12), size = u32(h + 8), flags = u32(h + 36);
        sec.push_back({va, va + size, (flags & 0x80000000u) != 0, (flags & 0x20000000u) != 0});
        if ((flags & 0x20000000u) && va - base + size <= img.size()) scan_binds(&img[va - base], size, va, s);
    }
    char const* why = binds_verdict(s);
    CHECK("IMG-1 the whole image passes the verdict", why == nullptr);
    if (why) { std::printf("    why: %s\n", why); return; }
    CHECK("IMG-2 one match each in the whole image (the layout twice)",
          s.count[S_ACT_GET] == 1 && s.count[S_MODE] == 1 && s.count[S_MOVE_READ] == 1 && s.count[S_LAYOUT] == 2);
    BindAt const at = binds_at(s);
    CHECK("IMG-3 the addresses from the whole image", at.actions == 0x1036E6E8 && at.moves == 0x1036F128);
    CHECK("IMG-4 the places in the image's own sections", place_verdict(at, sec.data(), static_cast<int>(sec.size())) == nullptr);
    BindRead r;
    copy_binds(at,
               [&](std::uintptr_t a, std::size_t k) -> void const* {
                   return a >= base && a - base + k <= img.size() ? &img[a - base] : nullptr;
               },
               [&](std::uintptr_t a) -> std::size_t {
                   for (Section const& x : sec) if (a >= x.begin && a < x.end) return x.end - a;
                   return 0;
               },
               r);
    // research/bindings.txt in ffxi-private: the static image's values
    CHECK("IMG-5 id 14 is Y (Ctrl or Alt), Enter, NumEnter", r.actions[42] == 0x1015 && r.actions[43] == 0x001C && r.actions[44] == 0x009C);
    CHECK("IMG-6 id 15 is N (Ctrl or Alt), Escape", r.actions[45] == 0x1031 && r.actions[46] == 0x8001);
    CHECK("IMG-7 ids 108 to 117 are Ctrl + digits", r.actions[108 * 3] == 0x020B && r.actions[117 * 3] == 0x020A);
    CHECK("IMG-8 id 166 is D, with no rule", r.actions[166 * 3] == 0x0020);
    CHECK("IMG-9 the categories", r.categories[16] == 0x0010 && r.categories[0] == 0xFFFF);
    CHECK("IMG-10 42 movement entries before the empty one", r.n_moves == 42);
    CHECK("IMG-11 W moves, K without Shift in context 6", r.moves[0] == 0x040011 && r.moves[16] == 0x060825);
    CHECK("IMG-12 the saved image has no mode or layout object yet", r.mode == -1 && r.layout == -1);
}

int main(int argc, char** argv) {
    test_sites();
    test_copy();
    if (argc > 1) test_image(argv[1]);
    else std::printf("binds_scan: no image given, the whole-image checks did not run\n");
    std::printf("binds_scan: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
