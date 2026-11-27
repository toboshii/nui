// key_layout.hpp - KeyHook 1's frozen layout as text, from the header it is compiled with. Compared line for line
// with layout_keyhook.txt by test/key_linux and key_test.exe. Existing lines never change; a revision may only add
// lines for fields it appends into a reserved tail or after KeyBus::status.
#ifndef KEY_LAYOUT_HPP
#define KEY_LAYOUT_HPP

#include "../src/KeyHook/KeyHook.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

inline int keyhook_layout_text(char* out, int cap) {
    int n = 0;
    auto line = [&](char const* key, unsigned long value) {
        if (n < cap) n += std::snprintf(out + n, static_cast<std::size_t>(cap - n), "%s %lu\n", key, value);
    };
#define UL(x) static_cast<unsigned long>(x)
    line("KEYHOOK_ABI_VERSION", UL(KEYHOOK_ABI_VERSION));
    line("KEYHOOK_MAX_CLIENTS", UL(KEYHOOK_MAX_CLIENTS));
    line("KEYHOOK_KEYS", UL(KEYHOOK_KEYS));
    line("KEYHOOK_WORDS", UL(KEYHOOK_WORDS));
    line("KEYHOOK_MAP_BYTES", UL(KEYHOOK_MAP_BYTES));
    if (n < cap) n += std::snprintf(out + n, static_cast<std::size_t>(cap - n), "KEYHOOK_MAP_NAME_FMT %s\n", KEYHOOK_MAP_NAME_FMT);
    line("KEYHOOK_HEARTBEAT_MS", UL(KEYHOOK_HEARTBEAT_MS));
    line("KEYHOOK_READ_STATE", UL(KEYHOOK_READ_STATE));
    line("KEYHOOK_READ_EVENTS", UL(KEYHOOK_READ_EVENTS));
    line("sizeof(KeyEvent)", UL(sizeof(KeyEvent)));
#define E(f) line("KeyEvent." #f, UL(offsetof(KeyEvent, f)))
    E(ofs); E(data); E(time); E(seq); E(app);
#undef E
    line("sizeof(KeyClient)", UL(sizeof(KeyClient)));
#define C(f) line("KeyClient." #f, UL(offsetof(KeyClient, f)))
    C(occupied); C(hiding); C(alive_tick); C(dispatch); C(rev); C(module); C(nonce_at); C(nonce); C(name); C(hide); C(reserved);
#undef C
    line("sizeof(KeyState)", UL(sizeof(KeyState)));
#define S(f) line("KeyState." #f, UL(offsetof(KeyState, f)))
    S(saw); S(hid); S(down); S(state_ok); S(state_reads); S(event_reads); S(hidden); S(dropped); S(ended); S(device); S(tick);
    S(primed); S(reserved);
#undef S
#define B(f) line("KeyBus." #f, UL(offsetof(KeyBus, f)))
    B(ready); B(abi_version); B(size); B(install_lock); B(hook_installed); B(owner_slot); B(in_flight); B(installs);
    B(calls); B(block); B(dispatch_ptr); B(site_state); B(site_events); B(owner_rev); B(reg_lock); B(handovers); B(blocked);
    B(serial); B(write_lock); B(lapses); B(device_disp); B(reserved_hdr); B(keys); B(clients); B(status);
#undef B
    line("sizeof(KeyBus.reserved_hdr)", UL(sizeof(KeyBus::reserved_hdr)));
    line("sizeof(KeyBus.status)", UL(sizeof(KeyBus::status)));
    line("KEYHOOK_LIVE", UL(KEYHOOK_LIVE));
    line("KEYHOOK_WAIT", UL(KEYHOOK_WAIT));
    line("KEYHOOK_BLOCKED", UL(KEYHOOK_BLOCKED));
#undef UL
    return n;
}

// The layout text against a file of it: every line of the file must be in the text, and the text must have no line
// the file lacks. -> the number of lines that differ (0: the same), each printed.
inline int keyhook_layout_compare(char const* path) {
    char have[4096];
    keyhook_layout_text(have, sizeof(have));
    std::FILE* f = std::fopen(path, "r");
    if (!f) { std::printf("FAIL layout: cannot open %s\n", path); return 1; }
    char want[4096];
    std::size_t const got = std::fread(want, 1, sizeof(want) - 1, f);
    std::fclose(f);
    want[got] = 0;
    // the file's own lines, comments left out
    char body[4096];
    std::size_t len = 0;
    for (char const* p = want; *p;) {
        char const* e = p;
        while (*e && *e != '\n') ++e;
        std::size_t const l = static_cast<std::size_t>(e - p);
        if (l > 0 && p[0] != '#' && len + l + 1 < sizeof(body)) {
            std::size_t ll = l;
            if (ll > 0 && p[ll - 1] == '\r') --ll;
            std::memcpy(body + len, p, ll);
            len += ll;
            body[len++] = '\n';
        }
        p = *e ? e + 1 : e;
    }
    body[len] = 0;
    int bad = 0;
    if (std::strcmp(body, have) != 0) {
        std::printf("FAIL layout: the header and %s differ. The header gives:\n%s", path, have);
        bad = 1;
    }
    return bad;
}

#endif  // KEY_LAYOUT_HPP
