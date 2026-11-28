// pad.hpp - the controller half of the module table (1.3.0): the pad the game reads, through the
// shared PadHook (PadHook/PadHook.h), and holding it while the addon uses it.
//
//   pad_start([flags]) -> ok, msg [, retry_ms]   join the hook (patching XInputGetState if nobody has).
//        retry_ms while xinput1_3.dll is not loaded (the game reads the pad with DirectInput, or has not
//        read it yet); no retry_ms when it can never work in this process (not hot-patchable, another
//        hook). flags "nosig": the not-loaded path, always (the fail-closed test).
//   pad() -> seq, buttons, lt, rt, lx, ly, rx, ry, index, holding
//        what the game last read, raw (before the mask), from the user index that last read connected.
//        seq changes with every change; 0 = no pad read yet. buttons: XINPUT_GAMEPAD_* bits, and
//        0x10000 / 0x20000 for LT / RT held. Each call is also the capture's heartbeat. nil when not started.
//   pad_capture(on)                              hold the pad (the game stops seeing new presses, the
//        sticks and the triggers) or give it back. A capture lapses by itself 250 ms after the last pad().
//   pad_chord(buttons)                           (1.4.0) the buttons that take the pad together (e.g. L3 + R3:
//        0xC0); the hook keeps that chord from the game, even where the game maps those buttons itself.
//        A chord button pressed alone reaches the game up to 150 ms late; tapped alone, it still does.
//   pad_chord(buttons, di_buttons)               (1.8.0) the same chord as DirectInput buttons (bit i: button i), for
//        a game that reads a DirectInput pad: the hook keeps it from the game the same way.
//   pad_dinput() -> seq, buttons, pov, x, y, z, rx, ry, rz, slider0, slider1, connected, holding, vendor, product
//        (1.8.0) the DirectInput pad the game reads, raw: buttons 0..31 as bits, pov in hundredths of a degree (-1:
//        centred), the axes -127..127 as the client ranges them, connected when the client read it in the last second,
//        and the device's USB vendor and product ids (0: not known). Each call is also the heartbeat. nil when not started.
//   pad_config() -> {key, padmode, padsin} | nil, why   the game's own pad settings (registry, read only)
//   pad_status() -> table                        state, describe, calls, clients, holding, masked, hidden, lapses, ...
//        (1.8.0) di_state, di_describe, di_name, di_vendor, di_product, di_connected, di_reads, di_hidden, ...
//   pad_stop()                                   leave the hook (also done by stop() and in DllMain)
#pragma once
#include "luaapi.hpp"

namespace nui {

void pad_set_name(char const* name);      // the client name on the bus (host_open)
void pad_detach();                        // DllMain(DLL_PROCESS_DETACH): leave without waiting
void pad_release();                       // stop(): give the pad back and leave

int __cdecl l_pad_start(lua_State* L);
int __cdecl l_pad(lua_State* L);
int __cdecl l_pad_dinput(lua_State* L);
int __cdecl l_pad_capture(lua_State* L);
int __cdecl l_pad_chord(lua_State* L);
int __cdecl l_pad_config(lua_State* L);
int __cdecl l_pad_status(lua_State* L);
int __cdecl l_pad_stop(lua_State* L);

}  // namespace nui
