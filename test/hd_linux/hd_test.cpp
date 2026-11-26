// hd_test.cpp - Assets::load against a made-up install, natively on Linux and with no game files: file_read and
// file_read_at below serve the install from memory.
//
// An HD texture pack stores the game's UI art at 2, 4 or 8 times its size. Enlarged texel by texel, such a texture
// must load to exactly the stock atlas, frame recipe and glyphs, in every texture format the loader reads (DXT1,
// DXT3, DXT5, and 4, 8, 16, 24 and 32 bit uncompressed), with the pack's sprite sheets scaled with it or left
// alone, and with the atlas decoded or only measured (a module that finds the atlas in the shared cache). The DXT
// textures are enlarged inside their blocks, so the decoded texels are the stock ones exactly. Also: a texture at
// 3 times is refused, an HD texture whose pixels run past the end of its DAT is refused rather than read, the box
// filter weights colour by alpha, and a block with no opaque texel keeps its colour.
//
//   sh test/hd_linux/run.sh
#include "assets.hpp"
#include "client.hpp"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

std::map<std::string, std::vector<unsigned char>> g_fs;

}  // namespace

namespace nui {

bool file_read(std::string const& path, std::vector<unsigned char>& out, std::uint64_t cap, std::size_t limit,
               std::uint64_t* size) {
    auto it = g_fs.find(path);
    if (it == g_fs.end() || it->second.size() > cap) return false;
    if (size) *size = it->second.size();
    std::size_t const n = limit && limit < it->second.size() ? limit : it->second.size();
    out.assign(it->second.begin(), it->second.begin() + std::ptrdiff_t(n));
    return true;
}

bool file_read_at(std::string const& path, std::uint64_t off, void* dst, std::size_t n) {
    auto it = g_fs.find(path);
    if (it == g_fs.end() || off + n > it->second.size()) return false;
    std::memcpy(dst, it->second.data() + off, n);
    return true;
}

}  // namespace nui

namespace {

int g_pass = 0, g_fail = 0;
void check(std::string const& name, bool ok, std::string const& detail = "") {
    if (ok) ++g_pass;
    else {
        ++g_fail;
        std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str());
    }
}

uint32_t g_seed = 0x9E3779B9u;
uint32_t rnd() {                        // xorshift32: the same made-up art on every run
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    return g_seed;
}

// ---- textures ------------------------------------------------------------------------------------------------

enum Fmt { DXT1, DXT3, DXT5, RAW4, RAW8, RAW16, RAW24, RAW32 };

bool is_dxt(Fmt f) { return f == DXT1 || f == DXT3 || f == DXT5; }
int block_bytes(Fmt f) { return f == DXT1 ? 8 : 16; }
int texel_bytes(Fmt f) { return f == RAW16 ? 2 : f == RAW24 ? 3 : f == RAW32 ? 4 : 1; }   // 4 and 8 bit: one index

struct Tex {
    std::string name;                   // 16 bytes, as the DAT spells it
    Fmt fmt;
    int w, h;
    std::vector<unsigned char> data;    // DXT: the blocks, top block row first; else each texel's bytes, top row first
    std::vector<unsigned char> pal;     // 4 and 8 bit: B G R A per entry
};

Tex random_tex(char const* name, Fmt fmt, int w, int h) {
    Tex t {name, fmt, w, h, {}, {}};
    std::size_t const n = is_dxt(fmt) ? std::size_t(w / 4) * std::size_t(h / 4) * std::size_t(block_bytes(fmt))
                                      : std::size_t(w) * std::size_t(h) * std::size_t(texel_bytes(fmt));
    t.data.resize(n);
    for (auto& b : t.data) b = static_cast<unsigned char>(rnd());
    if (fmt == RAW4) for (auto& b : t.data) b &= 0x0F;
    if (fmt == RAW4 || fmt == RAW8) {
        t.pal.resize(fmt == RAW4 ? 64 : 1024);
        for (auto& b : t.pal) b = static_cast<unsigned char>(rnd());
    }
    return t;
}

// The 2-bit colour index (and the alpha, by format) of texel j (0-15) of a DXT block, read and written.
unsigned get_bits(unsigned char const* b, int bit, int n) {
    unsigned v = 0;
    for (int i = 0; i < n; ++i) v |= ((b[(bit + i) / 8] >> ((bit + i) % 8)) & 1u) << i;
    return v;
}
void set_bits(unsigned char* b, int bit, int n, unsigned v) {
    for (int i = 0; i < n; ++i) {
        unsigned char const m = static_cast<unsigned char>(1u << ((bit + i) % 8));
        if ((v >> i) & 1u) b[(bit + i) / 8] |= m;
        else b[(bit + i) / 8] &= static_cast<unsigned char>(~m);
    }
}
void copy_texel(Fmt f, unsigned char const* src, int j, unsigned char* dst, int i) {
    int const colour = f == DXT1 ? 32 : 96;                         // the colour block's index bits, in bits
    set_bits(dst, colour + 2 * i, 2, get_bits(src, colour + 2 * j, 2));
    if (f == DXT3) set_bits(dst, 4 * i, 4, get_bits(src, 4 * j, 4));
    if (f == DXT5) set_bits(dst, 16 + 3 * i, 3, get_bits(src, 16 + 3 * j, 3));
}

// t at s times its size, each texel repeated s x s: what an HD pack holds if it was made by plain enlargement. A
// DXT texture is enlarged inside its blocks: each new block takes the colours (and DXT5's alpha pair) of the one
// block its texels come from, and each texel takes its source texel's indices, so it decodes to the same texels.
Tex enlarge(Tex const& t, int s) {
    Tex o = t;
    o.w = t.w * s;
    o.h = t.h * s;
    if (is_dxt(t.fmt)) {
        int const bb = block_bytes(t.fmt), bw = t.w / 4, obw = o.w / 4, obh = o.h / 4;
        o.data.assign(std::size_t(obw) * std::size_t(obh) * std::size_t(bb), 0);
        for (int by = 0; by < obh; ++by)
            for (int bx = 0; bx < obw; ++bx) {
                int const sbx = bx * 4 / s / 4, sby = by * 4 / s / 4;
                unsigned char const* src = &t.data[(std::size_t(sby) * std::size_t(bw) + std::size_t(sbx)) * std::size_t(bb)];
                unsigned char* dst = &o.data[(std::size_t(by) * std::size_t(obw) + std::size_t(bx)) * std::size_t(bb)];
                std::memcpy(dst, src, std::size_t(bb));
                for (int i = 0; i < 16; ++i) {
                    int const x = (bx * 4 + (i & 3)) / s, y = (by * 4 + (i >> 2)) / s;
                    copy_texel(t.fmt, src, (y & 3) * 4 + (x & 3), dst, i);
                }
            }
    } else {
        int const tb = texel_bytes(t.fmt);
        o.data.assign(std::size_t(o.w) * std::size_t(o.h) * std::size_t(tb), 0);
        for (int y = 0; y < o.h; ++y)
            for (int x = 0; x < o.w; ++x)
                std::memcpy(&o.data[(std::size_t(y) * std::size_t(o.w) + std::size_t(x)) * std::size_t(tb)],
                            &t.data[(std::size_t(y / s) * std::size_t(t.w) + std::size_t(x / s)) * std::size_t(tb)],
                            std::size_t(tb));
    }
    return o;
}

// ---- the DAT -------------------------------------------------------------------------------------------------

void put_chunk(std::vector<unsigned char>& dat, unsigned type, std::vector<unsigned char> const& body) {
    std::size_t const len = (16 + body.size() + 15) / 16 * 16;
    std::size_t const off = dat.size();
    dat.resize(off + len, 0);
    std::memcpy(&dat[off], "test", 4);
    uint32_t const info = type | uint32_t(len / 16) << 7;
    std::memcpy(&dat[off + 4], &info, 4);
    std::memcpy(&dat[off + 16], body.data(), body.size());
}

template <class T> void put(std::vector<unsigned char>& b, std::size_t at, T v) { std::memcpy(&b[at], &v, sizeof(v)); }

// A 0x20 chunk: a flag byte and the 16-byte name, then the texture header at chunk + 0x21 (width +4, height +8,
// depth +14, fourcc +40) and the palette or the pixels. Uncompressed rows go in bottom to top, as the client stores
// them. `keep` (> 0): only that many pixel bytes, as a DAT cut short would hold.
void put_texture(std::vector<unsigned char>& dat, Tex const& t, std::size_t keep = 0) {
    static uint32_t const kFourcc[] = {0x44585431, 0x44585433, 0x44585435};
    static uint16_t const kBits[] = {0, 0, 0, 4, 8, 16, 24, 32};
    std::vector<unsigned char> body(1 + 16 + 44, 0);
    std::memcpy(&body[1], t.name.data(), 16);
    put(body, 17 + 0, uint32_t(40));
    put(body, 17 + 4, int32_t(t.w));
    put(body, 17 + 8, int32_t(t.h));
    put(body, 17 + 12, uint16_t(1));
    put(body, 17 + 14, kBits[t.fmt]);
    put(body, 17 + 40, is_dxt(t.fmt) ? kFourcc[t.fmt] : uint32_t(0));
    std::vector<unsigned char> px;
    if (is_dxt(t.fmt)) {
        body.resize(body.size() + 8, 0);
        px = t.data;
    } else {
        body.insert(body.end(), t.pal.begin(), t.pal.end());
        int const tb = texel_bytes(t.fmt);
        px.assign(t.fmt == RAW4 ? (std::size_t(t.w) * std::size_t(t.h) + 1) / 2
                                : std::size_t(t.w) * std::size_t(t.h) * std::size_t(tb), 0);
        for (int r = 0; r < t.h; ++r) {                       // stored row r is the image's row h - 1 - r
            int const y = t.h - 1 - r;
            for (int x = 0; x < t.w; ++x) {
                std::size_t const p = std::size_t(r) * std::size_t(t.w) + std::size_t(x);
                unsigned char const* v = &t.data[(std::size_t(y) * std::size_t(t.w) + std::size_t(x)) * std::size_t(tb)];
                if (t.fmt == RAW4) px[p / 2] |= static_cast<unsigned char>((p & 1) ? v[0] : v[0] << 4);
                else std::memcpy(&px[p * std::size_t(tb)], v, std::size_t(tb));
            }
        }
    }
    if (keep && keep < px.size()) px.resize(keep);
    body.insert(body.end(), px.begin(), px.end());
    put_chunk(dat, 0x20, body);
}

struct Quad {
    int16_t v[8];
    uint16_t u, vv, w, h;
    std::string tex;
};

// A 0x31 chunk: the 16-byte name, then no texture names, the sprite count and each sprite's quads (61 bytes each).
void put_sheet(std::vector<unsigned char>& dat, char const* name, std::vector<std::vector<Quad>> const& sprites) {
    std::vector<unsigned char> body(16 + 1 + 2, 0);
    std::memcpy(&body[0], name, 16);
    put(body, 17, uint16_t(sprites.size()));
    for (auto const& sp : sprites) {
        body.push_back(static_cast<unsigned char>(sp.size()));
        for (Quad const& q : sp) {
            std::size_t const at = body.size();
            body.resize(at + 61, 0);
            std::memcpy(&body[at], q.v, 16);
            put(body, at + 16, q.w);
            put(body, at + 18, q.h);
            put(body, at + 20, q.u);
            put(body, at + 22, q.vv);
            for (int k = 0; k < 16; ++k) body[at + 25 + std::size_t(k)] = static_cast<unsigned char>(0x80 + k);
            body[at + 41] = 1;
            std::memcpy(&body[at + 45], q.tex.data(), 16);
        }
    }
    put_chunk(dat, 0x31, body);
}

// the sheet as a pack that scaled it with its textures stores it: every texel rectangle s times larger
std::vector<std::vector<Quad>> scaled(std::vector<std::vector<Quad>> sheet, int s) {
    for (auto& sp : sheet)
        for (Quad& q : sp) {
            q.u = uint16_t(q.u * s);
            q.vv = uint16_t(q.vv * s);
            q.w = uint16_t(q.w * s);
            q.h = uint16_t(q.h * s);
        }
    return sheet;
}

Quad quad(char const* tex, int u, int vv, int w, int h) {
    Quad q {{0, 0, int16_t(w), 0, 0, int16_t(h), int16_t(w), int16_t(h)}, uint16_t(u), uint16_t(vv), uint16_t(w),
            uint16_t(h), tex};
    return q;
}

// ---- the install ---------------------------------------------------------------------------------------------

unsigned const kWindowType = 6, kWindowFid = 13 + kWindowType, kMenuFid = 50;

struct Art {
    Tex newtex, hfr1, vfr1, corner, moji, gauge, font, m2f;
    std::vector<std::vector<Quad>> win00, fontshp;
};

// Every format once, each where it still fits a DAT chunk at 8x: a chunk's length has 19 bits of 16-byte units,
// so it holds at most 8 MB, and the 256 x 256 fonts at 2048 x 2048 fit only at 8 bits a texel or less.
Art stock_art() {
    Art a;
    a.newtex = random_tex("menu    newtex  ", DXT3, 128, 128);
    a.hfr1 = random_tex("menu    hfr1    ", DXT1, 32, 32);
    a.vfr1 = random_tex("menu    vfr1    ", RAW16, 32, 32);
    a.corner = random_tex("menu    corner  ", RAW32, 32, 32);
    a.moji = random_tex("font    moji    ", RAW8, 1024, 48);        // taller than the 32 rows read: the top ones
    a.gauge = random_tex("menu    gauge   ", RAW24, 64, 64);
    a.font = random_tex("font    font    ", DXT5, 256, 256);
    a.m2f = random_tex("menu    menu2fon", RAW4, 256, 256);
    a.win00 = {{quad("menu    newtex  ", 0, 0, 128, 128)},
               {quad("menu    hfr1    ", 0, 0, 32, 32)},
               {quad("menu    vfr1    ", 0, 0, 32, 32)},
               {quad("menu    corner  ", 0, 0, 32, 32)},
               {quad("menu    newtex  ", 16, 8, 32, 40), quad("menu    hfr1    ", 4, 0, 8, 32)},
               {quad("menu    vfr1    ", 0, 12, 32, 6)},
               {quad("menu    corner  ", 8, 8, 16, 16)},
               {quad("menu    newtex  ", 96, 100, 32, 28)},
               {quad("menu    hfr1    ", 20, 4, 12, 24)},
               {quad("MENU    CORNER  ", 0, 16, 16, 16)}};    // window type 1 spells its names in capitals
    for (int i = 0; i < 95; ++i) {
        int const w = 8 + i % 8;
        Quad q = quad("font    font    ", (i % 16) * 16, (i / 16) * 16, w, 12);
        q.v[1] = q.v[3] = -2;                                  // the glyph's top against the line
        a.fontshp.push_back({q});
    }
    return a;
}

// the same art, every texture `s` times larger; the sheets scaled with them, or not
Art hd_art(Art const& a, int s, bool scale_sheets) {
    Art h = a;
    for (Tex* t : {&h.newtex, &h.hfr1, &h.vfr1, &h.corner, &h.moji, &h.gauge, &h.font, &h.m2f}) *t = enlarge(*t, s);
    if (scale_sheets) {
        h.win00 = scaled(h.win00, s);
        h.fontshp = scaled(h.fontshp, s);
    }
    return h;
}

void install(std::string const& dir, Art const& a, std::size_t keep_newtex = 0) {
    std::vector<unsigned char> vt(kMenuFid + 1, 0), ft((kMenuFid + 1) * 2, 0);
    auto map = [&](unsigned fid, unsigned folder, unsigned file) {
        vt[fid] = 1;                                           // ROM, the first one
        put(ft, fid * 2, uint16_t(folder << 7 | file));
    };
    map(kWindowFid, 0, kWindowFid);
    map(kMenuFid, 1, 5);
    std::vector<unsigned char> w, m;
    put_texture(w, a.hfr1);
    put_texture(w, a.vfr1);
    put_texture(w, a.corner);
    put_sheet(w, "menu    win00   ", a.win00);
    put_texture(w, a.newtex, keep_newtex);                     // last: a cut-short one ends the file
    put_texture(m, a.moji);
    put_texture(m, a.gauge);
    put_texture(m, a.font);
    put_texture(m, a.m2f);
    put_sheet(m, "font    fontshp ", a.fontshp);
    g_fs[dir + "VTABLE.DAT"] = vt;
    g_fs[dir + "FTABLE.DAT"] = ft;
    g_fs[dir + "ROM\\0\\19.DAT"] = w;
    g_fs[dir + "ROM\\1\\5.DAT"] = m;
}

void uninstall(std::string const& dir) {
    for (auto it = g_fs.begin(); it != g_fs.end();) it = it->first.compare(0, dir.size(), dir) == 0 ? g_fs.erase(it) : std::next(it);
}

nui::Assets load(std::string const& dir, Art const& a, bool pixels = true, std::size_t keep_newtex = 0) {
    install(dir, a, keep_newtex);
    nui::Assets out;
    out.load(dir, int(kWindowType), kMenuFid, pixels);
    uninstall(dir);
    return out;
}

// ---- comparing -----------------------------------------------------------------------------------------------

std::string hex(uint32_t v) {
    char b[12];
    std::snprintf(b, sizeof(b), "%08x", v);
    return b;
}

bool same_quad(nui::SpriteQuad const& a, nui::SpriteQuad const& b) {
    return std::memcmp(a.v, b.v, sizeof(a.v)) == 0 && a.u == b.u && a.vv == b.vv && a.w == b.w && a.h == b.h &&
           std::memcmp(a.col, b.col, sizeof(a.col)) == 0 && std::memcmp(a.mode, b.mode, sizeof(a.mode)) == 0 &&
           std::memcmp(a.tex, b.tex, sizeof(a.tex)) == 0;
}

void same_metrics(std::string const& tag, nui::Assets const& want, nui::Assets const& got) {
    check(tag + ": the same pieces", want.have_window == got.have_window && want.have_moji == got.have_moji &&
          want.have_gauge == got.have_gauge && want.have_title == got.have_title && want.have_wtitle == got.have_wtitle,
          got.log);
    check(tag + ": the same log", want.log == got.log, got.log);
    bool frame = want.frame.size() == got.frame.size();
    for (std::size_t s = 0; frame && s < want.frame.size(); ++s) {
        frame = want.frame[s].size() == got.frame[s].size();
        for (std::size_t k = 0; frame && k < want.frame[s].size(); ++k) frame = same_quad(want.frame[s][k], got.frame[s][k]);
    }
    check(tag + ": the same frame recipe, in the game's texels", frame);
    check(tag + ": the same title glyphs", std::memcmp(want.title, got.title, sizeof(want.title)) == 0);
    check(tag + ": the same window title glyphs", std::memcmp(want.wtitle, got.wtitle, sizeof(want.wtitle)) == 0);
    check(tag + ": the same widths", std::memcmp(want.widths, got.widths, sizeof(want.widths)) == 0);
}

void same_atlas(std::string const& tag, nui::Assets const& want, nui::Assets const& got) {
    if (want.atlas.size() != got.atlas.size()) {
        check(tag + ": the atlas", false, "size " + std::to_string(got.atlas.size()));
        return;
    }
    std::size_t diff = 0, first = 0;
    for (std::size_t i = 0; i < want.atlas.size(); ++i)
        if (want.atlas[i] != got.atlas[i] && diff++ == 0) first = i;
    check(tag + ": the same atlas, texel for texel", diff == 0,
          std::to_string(diff) + " texels differ, the first at " + std::to_string(first % nui::kAtlasW) + "," +
              std::to_string(first / nui::kAtlasW) + ": " + hex(got.atlas[first]) + ", stock " + hex(want.atlas[first]));
}

// a texel of a 32-bit texture: B G R A, the alpha in the client's convention (0x80 is opaque)
void set_bgra(Tex& t, int x, int y, unsigned char b, unsigned char g, unsigned char r, unsigned char a) {
    unsigned char* p = &t.data[(std::size_t(y) * std::size_t(t.w) + std::size_t(x)) * 4];
    p[0] = b;
    p[1] = g;
    p[2] = r;
    p[3] = a;
}

uint32_t atlas_at(nui::Assets const& a, int x, int y) { return a.atlas[std::size_t(y) * nui::kAtlasW + std::size_t(x)]; }

}  // namespace

int main() {
    Art const stock = stock_art();
    nui::Assets const base = load("C:\\stock\\", stock);
    check("stock: every piece loads", base.have_window && base.have_moji && base.have_gauge && base.have_title &&
          base.have_wtitle && base.title_ok(), base.log);
    nui::Assets const base_m = load("C:\\stock-metrics\\", stock, false);

    for (int s : {2, 4, 8}) {
        for (bool sheets : {true, false}) {
            std::string const tag = std::to_string(s) + "x, sheets " + (sheets ? "scaled" : "left alone");
            Art const hd = hd_art(stock, s, sheets);
            nui::Assets const got = load("C:\\hd\\", hd);
            same_metrics(tag, base, got);
            same_atlas(tag, base, got);
            same_metrics(tag + ", measured only", base_m, load("C:\\hd-metrics\\", hd, false));
        }
    }

    // a texture at 3 times the game's size is not one an HD pack makes: refused, by name and size
    {
        Art a = stock;
        a.newtex = enlarge(a.newtex, 3);
        nui::Assets const got = load("C:\\x3\\", a);
        check("3x: refused", !got.have_window && got.log.find("newtex 384x384 not 128x128") != std::string::npos, got.log);
        check("3x: the rest still loads", got.have_moji && got.have_title, got.log);
    }

    // an HD texture whose pixels stop short: enough for the game's 128 rows, not for the 256 an HD texture needs.
    // Refused, not read past the end.
    {
        Art a = stock;
        a.newtex = enlarge(a.newtex, 2);
        std::size_t const need_game = std::size_t(256 / 4) * std::size_t(128 / 4) * 16;
        nui::Assets const got = load("C:\\cut\\", a, true, need_game + 1024);
        check("cut short: refused", !got.have_window && got.log.find("pixels past the end of the DAT") != std::string::npos,
              got.log);
    }

    // The box filter. At 2x, the corner's first block: two opaque red texels and two clear black ones give red at
    // half alpha, not a darker red. The second block: four clear texels keep their colour, as the stock decode
    // keeps a clear texel's colour (linear filtering samples it at an edge).
    {
        Art a = stock;
        a.corner = enlarge(a.corner, 2);
        set_bgra(a.corner, 0, 0, 0, 0, 255, 0x80);
        set_bgra(a.corner, 1, 0, 0, 0, 255, 0x80);
        set_bgra(a.corner, 0, 1, 0, 0, 0, 0);
        set_bgra(a.corner, 1, 1, 0, 0, 0, 0);
        for (int y = 0; y < 2; ++y)
            for (int x = 2; x < 4; ++x) set_bgra(a.corner, x, y, 0x60, 0x40, 0x20, 0);
        nui::Assets const got = load("C:\\box\\", a);
        uint32_t const edge = atlas_at(got, nui::kCornerX, nui::kCornerY);
        uint32_t const clear = atlas_at(got, nui::kCornerX + 1, nui::kCornerY);
        check("box: colour weighted by alpha", edge == 0x80FF0000u, hex(edge));
        check("box: a clear block keeps its colour", clear == 0x00204060u, hex(clear));
    }

    std::printf("hd: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
