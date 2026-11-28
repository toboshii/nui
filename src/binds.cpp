// binds.cpp - see binds.hpp
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "binds.hpp"

#include "KeyBinds/KeyBinds.h"

#include <cstring>

namespace nui {
namespace {

LuaApi& L_ = lua();

void array(lua_State* L, char const* name, std::uint32_t const* v, unsigned n) {
    L_.createtable(L, static_cast<int>(n), 0);
    for (unsigned i = 0; i < n; ++i) {
        L_.pushnumber(L, v[i]);
        L_.rawseti(L, -2, static_cast<int>(i + 1));
    }
    L_.setfield(L, -2, name);
}

}  // namespace

int __cdecl l_key_bindings(lua_State* L) {
    bool simulate = false;
    if (L_.gettop(L) >= 1 && L_.type(L, 1) == kTString) {
        char const* f = L_.tolstring(L, 1, nullptr);
        simulate = f && std::strstr(f, "nosig") != nullptr;
    }
    static keybinds::BindRead r;            // 2 KB: off the stack
    char const* why = nullptr;
    if (!keybinds::read_binds(r, &why, simulate)) {
        L_.pushnil(L);
        L_.pushstring(L, why ? why : "were not read");
        return 2;
    }
    static std::uint32_t wide[keybinds::kActions * keybinds::kPerAction];
    L_.createtable(L, 0, 5);
    for (unsigned i = 0; i < keybinds::kActions * keybinds::kPerAction; ++i) wide[i] = r.actions[i];
    array(L, "actions", wide, keybinds::kActions * keybinds::kPerAction);
    for (unsigned i = 0; i < keybinds::kActions; ++i) wide[i] = r.categories[i];
    array(L, "categories", wide, keybinds::kActions);
    array(L, "moves", r.moves, r.n_moves);
    if (r.mode >= 0) { L_.pushnumber(L, r.mode); L_.setfield(L, -2, "mode"); }
    if (r.layout >= 0) { L_.pushnumber(L, r.layout); L_.setfield(L, -2, "layout"); }
    return 1;
}

}  // namespace nui
