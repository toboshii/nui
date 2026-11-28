// binds.hpp - the client's keyboard bindings, read only (KeyBinds/KeyBinds.h).
//
//   key_bindings([flags]) -> table | nil, why    one copy of the client's keyboard bindings. It patches nothing.
//        actions     188 x 3 numbers, the bindings of action id a at [a*3+1 .. a*3+3]: the DIK code in the low byte
//                    and the rule bits in the high byte (0x01 Shift, 0x02 Ctrl, 0x04 Alt, 0x08 Ctrl and Alt,
//                    0x10 Ctrl or Alt, 0x20 not Shift, 0x40 not while the chat line has focus, 0x80 neither Ctrl
//                    nor Alt). 0 is an empty binding.
//        categories  188 numbers, the category word of each action id (at [a+1])
//        moves       the movement entries the client reads: DIK | modifier rule << 8 | context << 16. The modifier
//                    rule is 0 none, 1 Ctrl, 2 Alt, 4 Shift, 8 not Shift, 7 any.
//        mode        the keyboard mode, 0..2 (the movement keys act in 1 and 2), or absent while unknown
//        layout      the keyboard layout (2 and 3 remap DIK codes), or absent while unknown
//        Without the tables (no client, or a signature not found, not unique or not where it must be): nil and why.
//        flags "nosig": the not-found path, always (the fail-closed test).
#pragma once
#include "luaapi.hpp"

namespace nui {

int __cdecl l_key_bindings(lua_State* L);

}  // namespace nui
