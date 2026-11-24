// pad_layout.hpp - PadHook 1's frozen layout as text, from the header it is compiled with. Compared
// line for line with layout_padhook.txt by pad_test.exe. Existing lines never change; a revision may
// only add lines for fields it appends into a reserved tail or after PadBus::status.
#ifndef PAD_LAYOUT_HPP
#define PAD_LAYOUT_HPP

#include "../src/PadHook/PadHook.h"

#include <cstddef>
#include <cstdio>

inline int padhook_layout_text(char* out, int cap) {
    int n = 0;
    auto line = [&](char const* key, unsigned long value) {
        if (n < cap) n += std::snprintf(out + n, static_cast<std::size_t>(cap - n), "%s %lu\n", key, value);
    };
#define UL(x) static_cast<unsigned long>(x)
    line("PADHOOK_ABI_VERSION", UL(PADHOOK_ABI_VERSION));
    line("PADHOOK_MAX_CLIENTS", UL(PADHOOK_MAX_CLIENTS));
    line("PADHOOK_INDEXES", UL(PADHOOK_INDEXES));
    line("PADHOOK_MAP_BYTES", UL(PADHOOK_MAP_BYTES));
    if (n < cap) n += std::snprintf(out + n, static_cast<std::size_t>(cap - n), "PADHOOK_MAP_NAME_FMT %s\n", PADHOOK_MAP_NAME_FMT);
    line("PADHOOK_HEARTBEAT_MS", UL(PADHOOK_HEARTBEAT_MS));
    line("PADHOOK_LT", UL(PADHOOK_LT));
    line("PADHOOK_RT", UL(PADHOOK_RT));
    line("sizeof(PadXState)", UL(sizeof(PadXState)));
    line("sizeof(PadState)", UL(sizeof(PadState)));
#define S(f) line("PadState." #f, UL(offsetof(PadState, f)))
    S(seq); S(buttons); S(lt); S(rt); S(connected); S(source); S(lx); S(ly); S(rx); S(ry); S(tick); S(reads); S(reserved);
#undef S
    line("sizeof(PadMask)", UL(sizeof(PadMask)));
#define M(f) line("PadMask." #f, UL(offsetof(PadMask, f)))
    M(masked); M(passthru); M(was_captured); M(out_packet); M(last_out); M(hidden); M(reserved);
#undef M
    line("sizeof(PadClient)", UL(sizeof(PadClient)));
#define C(f) line("PadClient." #f, UL(offsetof(PadClient, f)))
    C(occupied); C(capture); C(alive_tick); C(dispatch); C(rev); C(module); C(nonce_at); C(nonce); C(name); C(reserved);
#undef C
#define B(f) line("PadBus." #f, UL(offsetof(PadBus, f)))
    B(ready); B(abi_version); B(size); B(install_lock); B(hook_installed); B(owner_slot); B(in_flight); B(installs);
    B(calls); B(block); B(dispatch_ptr); B(site); B(original); B(owner_rev); B(reg_lock); B(handovers); B(blocked);
    B(serial); B(write_lock); B(wseq); B(last_index); B(lapses); B(reserved_hdr); B(pads); B(masks); B(clients); B(status);
#undef B
    line("sizeof(PadBus.reserved_hdr)", UL(sizeof(PadBus::reserved_hdr)));
    line("sizeof(PadBus.status)", UL(sizeof(PadBus::status)));
    line("PADHOOK_LIVE", UL(PADHOOK_LIVE));
    line("PADHOOK_WAIT", UL(PADHOOK_WAIT));
    line("PADHOOK_BLOCKED", UL(PADHOOK_BLOCKED));
    line("PADHOOK_SRC_XINPUT", UL(PADHOOK_SRC_XINPUT));
    line("PADHOOK_SRC_DINPUT", UL(PADHOOK_SRC_DINPUT));
#undef UL
    return n;
}

#endif  // PAD_LAYOUT_HPP
