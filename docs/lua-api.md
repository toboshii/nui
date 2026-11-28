# The Lua module

`require('_youraddon')` returns a table with the functions below. They are the whole API; there is
no global state to configure and nothing else to call.

Everything here runs on Windower's Lua thread, which the game thread waits on. Nothing in this API
blocks on the render thread or on disk, with the exceptions called out below.

## Lifecycle

### `start([ffxi_dir [, flags]]) -> ok, msg [, retry_ms]`

Reads the game's art, then joins the hook.

- `ok == true`: you are in. Nothing is drawn until you send a layer and a frame runs.
- `ok == false` **with** `retry_ms`: no module has found the hook yet. This is normal at load time,
  before the client's renderer has been through `EndScene`. Call `start` again after `retry_ms`
  milliseconds. Most addons do this from `prerender` with a five second timer, say so once, and
  stay quiet after.
- `ok == false` **without** `retry_ms`: final. Print `msg` and do not retry.

`ffxi_dir` overrides where the install is looked for; leave it out in game. `flags` is a string
that may contain:

| Flag | Effect |
|---|---|
| `noshare` | keep this module's own textures instead of the shared cache's. The pixels are identical; it costs memory. An escape hatch, worth exposing as a setting. |
| `nosig` | search for a deliberately wrong signature. The fail-closed test: `start` must fail with a retry. |

### `stop() -> "stopped"`

Leave the hook and the cache, release every texture and layer. Call it from your addon's unload
handler, **before** you save anything, so the game is not drawing while your addon winds down.

### `version() -> string`

The version string from your `HostConfig`.

## Drawing

### `screen(w, h [, auto [, f]])`

The coordinate space your Lua draws in. Pass the client's resolution for 1:1 pixels.

Three spaces meet here, and only two of them are the same size:

| Space | What it is |
|---|---|
| render px | the target nui draws into, which `auto` scales to by `render width / w` |
| window px | the game window, `x_res` from `windower.get_windower_settings()` |
| UI px | the UI drawn in that window, `ui_x_res`, the window over the launcher's UI Scale |

`w` and `h` are window px, and one Lua unit is `f` of them.

**Windower's `mouse` event reports UI px, not window px.** They are equal only at UI Scale 1.
Converting a mouse event into Lua units is therefore

```lua
local uiscale = ws.x_res / ws.ui_x_res     -- 1 unless the player scaled the UI
local lua_x = event_x * uiscale / f
```

Getting this wrong costs nothing on the machine you develop on and makes every click land
short for anyone who scales their UI. It is the same space Windower's own `libs/texts.lua`
uses, which anchors a right-aligned text at `ui_x_res`.

### `layer(id, z, text)` and `layer(id)`

Set or remove one of 16 layers. `z` orders them back to front. `text` is the command string
documented in [layer-commands.md](layer-commands.md). Returns the command count, or `nil, err` with
the line number when a command is malformed.

### `layer_pos(id, ox, oy [, x0, y0, x1, y1])`

Move a layer by an offset, and optionally clip it, without re-sending its commands. This is how you
scroll: the commands are already parsed, so moving them is close to free.

## Textures

### `tex_load(path [, max_w, max_h]) -> handle, w, h`

Load a PNG or JPEG, downscaled to fit the bound you give. The file is read and hashed, then looked
for in the shared cache: if another addon already has those bytes, nothing is decoded at all.
Otherwise it decodes on a thread of this module's own and is drawn from the first frame after the
pixels arrive, so a load never stalls the game. A file that does not decode is simply never drawn;
`prof().decode_failed` counts those.

### `tex_free(handle)`

Drop a picture. The texture goes when the last holder lets go.

### `icon(item_id) -> boolean` and `icon_pixels(item_id) -> string`

Whether the client has an icon for that item, and its 32 by 32 BGRA pixels as a 4,096-byte string
if you want to do something else with them. Drawing one is the `I` command; you do not need either
of these for that.

## Layout

### `metrics() -> table`

| Field | What |
|---|---|
| `moji` | 96 advance widths, ASCII 0x20 to 0x7F, for the body font |
| `title` | per-glyph `{w, h, top}` for the nameplate serif |
| `wtitle` | advances for the window-title font |
| `from_game` | false when the client's own width table could not be read and fallbacks are in use |
| `title_font` | false when the nameplate serif is not available |

These are the client's own numbers, so a string you measure measures the same in game.

## The window style

### `wintype(n)`

Draw in window style `n`, 1 to 8.

### `wtread() -> type, source`

The style the player has chosen, read from the client's config table. `source` is `"config"` or
`"watch"`. Call it when you load and when the player might have changed it, and pass the result to
`wintype`.

### `wtfind(value [, restart])` and `wtwatch(rva, enc)`

A fallback scanner for a future client where the config table has moved: ask the player what style
they are using, call `wtfind` with it, and `wtread` uses what it learns. Most addons never need
these.

## The controller (1.3.0)

nui can read the pad the game reads, and keep presses from the game while the addon uses them. It
hooks the client's XInput read once per process, shared by every nui module (PadHook 1, see
[abi.md](abi.md)). Since 1.5.0 the hook patches the client's own XInput wrapper in `FFXiMain.dll`.
Where that wrapper is not found, it hot-patches `xinput1_3.dll!XInputGetState` instead.

Since 1.8.0 the hook also reads the client's DirectInput pad. The game reads one pad. It uses XInput
when its **Enable XInput** setting is on and the pad is an XInput device. Otherwise it reads a
DirectInput device: with the setting off, or for a pad such as a DualShock without Steam Input.
`pad()` returns the XInput pad, and `pad_dinput()` the DirectInput pad. The hold and the heartbeat
are the same for both.

### `pad_start([flags]) -> ok, msg [, retry_ms]`

Join the pad hook, and patch the client's XInput read if no module has. On the client's wrapper
(1.5.0), this succeeds before the client loads its XInput shim, and in DirectInput mode too.

When the wrapper is not found, the hook tries `xinput1_3.dll!XInputGetState`. As with `start()`,
`retry_ms` then means try again later (`xinput1_3.dll` is not loaded yet, or the wrapper can still
work). No `retry_ms` means never in this process: the wrapper is refused for good, and the function
is already hooked by another tool or is not hot-patchable. `msg` gives the reason for both routes. `flags` may contain `nosig`, which always takes both not-found paths (the
fail-closed test).

### `pad() -> seq, buttons, lt, rt, lx, ly, rx, ry, index, holding`

What the game last read, raw (before any mask), from the user index that last read a connected pad.
Returns numbers, not a table, so polling it every frame allocates nothing.

| Value | What |
|---|---|
| `seq` | changes with every change of the rest; 0 = nothing read yet; -1 = the read was torn (try next frame) |
| `buttons` | `XINPUT_GAMEPAD_*` bits; `0x10000` LT held, `0x20000` RT held |
| `lt`, `rt` | 0 to 255 |
| `lx`, `ly`, `rx`, `ry` | -32768 to 32767 |
| `index` | the XInput user index, -1 when not connected |
| `holding` | this module holds the pad |

Each call is also the hold's heartbeat: poll it from `prerender`. On the client's wrapper, the first
call that finds the client's shim loaded also pins `xinputdll.dll`, so the client can never unload it
while a read is on its way through the hook.

### `pad_capture(on)`

Hold the pad (`true`) or give it back. While held, new presses, the sticks and the triggers are hidden
from the game; see [abi.md](abi.md) for the exact rules on buttons held across the change. A hold
lapses by itself 250 ms after the last `pad()`.

### `pad_chord(buttons)` (1.4.0)

The buttons that, pressed together, take your pad: `0xC0` for L3 + R3. The hook keeps that chord from
the game, even when the game maps those buttons itself. You still see it raw in `pad()`, and call
`pad_capture(true)` yourself. A chord button pressed alone reaches the game up to 150 ms late, and
a tap of one still reaches it. `0` turns the chord off.

Since 1.8.0, `pad_chord(buttons, di_buttons)` also names the chord as DirectInput buttons: bit `i` is
button `i`, so `0xC00` is buttons 10 and 11 (L3 and R3 on a PlayStation pad). DirectInput pads number
their buttons differently by maker, so the addon chooses the bits. The hook keeps that chord from the
game in the same way. A tap of one chord button goes back to the game as a press event and a release
event.

### `pad_dinput() -> seq, buttons, pov, x, y, z, rx, ry, rz, slider0, slider1, connected, holding, vendor, product` (1.8.0)

The DirectInput pad the game last read, raw (before any mask). Returns numbers, not a table.

| Value | What |
|---|---|
| `seq` | changes with every change of the rest; 0 = nothing read yet; -1 = the read was torn (try next frame) |
| `buttons` | bit `i` held: button `i`, 0 to 31 (the buttons the client maps) |
| `pov` | POV 0 in hundredths of a degree, clockwise from up (0, 9000, 18000, 27000); -1 = centred |
| `x` .. `slider1` | -127 to 127, as the client ranges them. `y` is positive down |
| `connected` | the client read the pad in the last second, and the read succeeded |
| `holding` | this module holds the pad |
| `vendor`, `product` | the pad's USB ids (`0x054C` Sony, `0x045E` Microsoft, `0x057E` Nintendo); 0 when not known |

Which axis is which stick depends on the pad. A PlayStation pad has its right stick on `z` and `rz`.
An Xbox pad read through DirectInput has it on `rx` and `ry`, and both triggers on `z`. Each call is
also the hold's heartbeat, as `pad()` is.

### `pad_config() -> {key, padmode, padsin}` or `nil, why`

The game's own pad settings, read from the registry, as the client's config tool writes them:
`padmode` is six comma-separated flags (the sixth is Enable XInput), `padsin` is 27 button ids, one per
game action (5 Cancel, 6 Main menu, 7 Confirm, 8 Active window, ...).

### `pad_status() -> table`

`state` (`live`, `waiting`, `blocked`, `no bus`), `describe` (one line), `joined`, `holding`, `calls`,
`clients`, `masked`, `hidden`, `lapses`, `reads`, `source`, `site`, `rev`, `owner_rev`.

Since 1.5.0 it also has:

| Field | What |
|---|---|
| `route` | `wrapper` (the client's XInput wrapper), `export` (`xinput1_3.dll!XInputGetState`), or `none` while nothing is patched |
| `shim` | on `wrapper`: the client has loaded `xinputdll.dll` (it has not with Enable XInput off) |
| `pinned` | on `wrapper`: this module has pinned `xinputdll.dll` |

`site` is the patched address: the wrapper on `wrapper`, the function on `export`. `describe` names the
route, for example `live at FFXiMain's XInput wrapper 10008EA0; 1 client: chronicle r3 (drives)`.

Since 1.8.0 it also has the DirectInput source:

| Field | What |
|---|---|
| `di_state` | `live`, `waiting` (the client module is not loaded) or `blocked` |
| `di_describe` | one line, for example `live at FFXiMain's DirectInput pad reads 1000619A, 10006A0B; driven by chronicle r4`, or why it is blocked |
| `di_connected` | the client read a DirectInput pad in the last second |
| `di_name`, `di_vendor`, `di_product` | the pad's product name and USB ids (0 when not known) |
| `di_reads`, `di_calls` | state reads recorded, and reads through the hook |
| `di_chord` | the DirectInput chord the hook keeps from the game (every client's) |
| `di_masked`, `di_hidden`, `di_injected`, `di_chords` | buttons hidden now, presses hidden ever, taps given back as events ever, chords kept ever |

When `pad_start()` fails, `msg` names the reasons of both sources.

### `pad_stop()`

Give the pad back and leave the hook. `stop()` and unloading do this too.

## The keyboard (1.6.0)

nui can keep single keys from the game. Windower's `keyboard` event cannot do that: a `true` return
leaves the key with the game. `keyboard_blockinput` blocks every key, so the player cannot chat, use a
macro or run a Windower bind meanwhile. nui patches the client's two keyboard reads once per
process, shared by every nui module (KeyHook 1, see [abi.md](abi.md#keyhook-1)). The keys you name
are hidden from the game's actions: the camera, the menus, the action bindings, and Enter and Escape
in the chat line.

What the hook does not see: the chat line's own keys. Space, `/` and typed characters reach the chat
line as window messages, so a key you hide can still open the chat line or type into it. While the
player types into your addon, use `keyboard_blockinput` to keep the text from the game.

Windower reads the keyboard before nui does. Its `keyboard` event, its binds and its console see every
key, hidden or not.

### `key_start([flags]) -> ok, msg [, retry_ms]`

Join the key hook, and patch the client's keyboard reads if no module has. `retry_ms` means try again
later (`FFXiMain.dll` is not loaded yet, which never happens in game). No `retry_ms` means never in this
process: the reads were not found, are not unique, or do not have the expected shape. Keep keys from
the game some other way then. `msg` gives the reason. `flags` may contain `nosig`, which always takes
the not-found path (the fail-closed test).

### `key_hide(code, ...)`

The DirectInput key codes (0 to 255) to hide from the game, one argument each. To pass a list, call
`key_hide(unpack(t))`. With no codes, nothing is hidden. Each call replaces the list before it, and
each call is also its heartbeat. The list lapses by itself 250 ms after the last call, so call it
every frame while it holds keys, from `prerender`.

The hook decides each press when the game first reads it: hidden if its key is in the list then. A
hidden press stays hidden until its release, even after the key leaves the list or the list lapses. A
press the game saw stays the game's until its release, even if the key goes into the list meanwhile.
So the game never sees half a press. The hook decides a tap shorter than a frame the same way.

### `key_status() -> table`

`state` (`live`, `waiting`, `blocked`, `no bus`), `describe` (one line), `joined`, `hiding` (this
module's list holds keys), `keys` (how many), `held_hidden` (presses hidden now, until their release),
`reads`, `events`, `hidden`, `dropped`, `ended`, `lapses`, `calls`, `clients`, `rev`, `owner_rev`,
`site_state`, `site_events`, and `device`: the module that owns the keyboard device the game reads
(`Hook.dll` for Windower's wrapper). `describe` reads, for example, `live at FFXiMain's keyboard
reads 10005A89, 10005B20; 1 client: chronicle r1 (drives, hides 10)`.

### `key_stop()`

Give the keys back and leave the hook. `stop()` and unloading do this too.

### `key_bindings([flags]) -> table | nil, why` (1.7.0)

One copy of the client's keyboard bindings, so an addon can tell when a key it hides is also a game key.
It needs no `key_start` and patches nothing. The client rewrites these tables at run time: at login from
the character's config, and when the player changes a key. So read them again when you need them.

The table has these fields:

- `actions`: 188 x 3 numbers. The bindings of action id `a` are at `[a*3+1]` to `[a*3+3]`. The low byte
  is the DirectInput key code, and 0 is an empty binding. The high byte holds the rules: `0x01` Shift
  held, `0x02` Ctrl held, `0x04` Alt held, `0x08` Ctrl and Alt held, `0x10` Ctrl or Alt held, `0x20`
  Shift not held, `0x40` not while the chat line has focus, `0x80` neither Ctrl nor Alt held. A binding
  with no rule bits acts with any modifiers.
- `categories`: 188 numbers, the category word of each action id, at `[a+1]`.
- `moves`: the movement entries the client reads, as `key | rule << 8 | context << 16`. The rule is 0
  for none, 1 Ctrl, 2 Alt, 4 Shift, 8 not Shift and 7 any. A rule 0 key also acts with Ctrl or Alt
  held, unless it is a shortcut key.
- `mode`: the keyboard mode, the game's "Keyboard size" (Config, Misc. 2): 0 Full, 1 Compact 1, 2 Compact 2. The
  movement keys act only in the two compact sizes. Absent while unknown.
- `layout`: the keyboard layout. Layouts 2 and 3 move some keys. Absent while unknown.

Without the tables it returns `nil` and the reason: the client is not loaded, or a signature was not
found, is not unique, or points outside the client's data. `flags` may contain `nosig`, which always
takes the not-found path (the fail-closed test).

## Diagnostics

### `status() -> table`

The table worth putting behind a `//youraddon status` command.

| Field | What |
|---|---|
| `running`, `message` | whether it is drawing, and why not |
| `hook`, `hook_state`, `describe` | the shared hook: `live`, `waiting`, `blocked` or `no bus`, and one line a player can read out to you |
| `owner`, `owner_rev`, `rev`, `clients`, `slot`, `site` | who drives the hook, who else is on it, where it is patched |
| `drawing` | running, and not held back by a texture upload that failed and is being retried |
| `device_ok`, `calls`, `drawn`, `uploads` | the renderer |
| `layers`, `quads`, `batches` | what your last layer came to |
| `last_us`, `max_us`, `build_us` | time in the draw and in the build |
| `cache` | `shared`, `why`, `shared_textures`, `private_textures`, `hits`, `creates`, `live_kb`, `clients`, and more |
| `native_errors`, `pending_releases`, `final_render` | counters that should stay at zero or near it |

### `prof() -> table`

Counters since the last call, then reset. Render thread, Lua thread and decode thread are reported
separately: lock waits, build times, draws, uploads, file reads, decode failures. Reading it costs
nothing between calls, so it is safe to poll.

### `us() -> number`

Microseconds since the module was opened, from the performance counter. Useful because Windower's
own clock is not fine enough to measure a frame.
