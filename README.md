# nui

[![build](https://github.com/jintawk/nui/actions/workflows/build.yml/badge.svg)](https://github.com/jintawk/nui/actions/workflows/build.yml)
[![tag](https://img.shields.io/github/v/tag/jintawk/nui)](https://github.com/jintawk/nui/tags)
[![licence](https://img.shields.io/github/license/jintawk/nui)](LICENSE)

A native user-interface library for Final Fantasy XI addons.

nui draws an addon's windows with Direct3D 8, on the game's own device, in the frame the game is
about to present. The window frames, the fonts, the gauges and the item icons are read out of the
client's own data files and drawn the way the client draws them, so an addon's window is built from
the same art as the game's own.

![A HUD built with nui, over the game](media/hud.png)

One addon, five windows: 1,046 quads in 18 batches, drawn above the game's own interface.

- [Summary](#summary)
- [Window styles](#window-styles)
- [Hello, window](#hello-window)
- [What it can draw](#what-it-can-draw)
- [Live data](#live-data)
- [Addons built with nui](#addons-built-with-nui): resizing, re-wrapping, scrolling, links,
  hiding with the game, snapping
- [How it works](#how-it-works)
- [Several addons at once](#several-addons-at-once)
- [What it costs](#what-it-costs)
- [Putting it in your addon](#putting-it-in-your-addon)
- [Drawing offline](#drawing-offline)
- [When the client changes](#when-the-client-changes)
- [Building and testing](#building-and-testing)
- [Licence](#licence)

## Summary

| | |
|---|---|
| **The game's own look** | Window frames in all eight of the client's window styles, its chat font, its nameplate serif, its window-title font, its HP/MP/TP gauge art, and any item icon by item id. All decoded from the installed DAT files at run time. |
| **The player's window style** | nui reads the window style the player chose in the game's own config menu and draws in it. Changing it in game changes every nui addon with it, in the same frame. |
| **Above the game's UI** | Drawing happens just before the frame's `EndScene`, after the client has finished its own 2D pass, so addon windows sit on top of the game's menus rather than under them. |
| **One hook, however many addons** | Every addon using nui shares a single patch of a single function, through a per-process bus with a frozen layout. Addons built against different versions of nui run side by side, load and unload in any order, and need no agreement between their authors. |
| **One texture, however many addons** | Two addons drawing the same window art, the same item icon or the same picture hold one texture between them, not two. On a scene with 25 textures that is about 12 MiB per extra addon. |
| **Nothing to install** | The library is compiled into the addon's own DLL, so the addon ships as one folder with one DLL in it. There is no runtime, no shared component and no install step. |
| **The controller, too** | One shared hook reads the pad the game reads and, while an addon holds it, keeps new presses from the game. A press is never cut in half, and a hold that is not renewed lapses by itself. |
| **Fails closed** | If nui does not find exactly the code it expects in the client, it patches nothing, draws nothing, and reports why. It never sends a packet, and reads nothing but the install's art files and, for the controller, the game's own pad settings. |
| **Two patches in the client's code** | nui patches the client's code in two places. Each is found by its byte signature and is never undone. The drawing hook patches the start of the client's `EndScene` wrapper. The controller hook (1.5.0) changes one instruction of the client's XInput wrapper. Where that wrapper is not found, the controller hook patches `xinput1_3.dll!XInputGetState` instead, if that DLL is hot-patchable. nui writes no other code of the client and none of its data. The one exception is the pad state that the client's own read returns: the controller hook masks it while an addon holds the pad. |

Built for Windower 4 and its stock Lua 5.1. The client is 32-bit, so the DLL is 32-bit.

## Window styles

Final Fantasy XI lets the player pick one of eight window styles in its own config menu. nui reads
that setting out of the client's config table (id 18) and draws in it, and follows it when it
changes. An addon needs no style setting of its own.

![The same two windows as the player cycles the game's window styles](media/theme.gif)

Each style is a 128 by 128 template: a body tile, four corners, the rules between them, per-corner
vertex colours and a per-piece blend mode, read from the client's `menu    win00` chunk and
assembled at the requested size the way the client assembles its own log window. A window is
therefore translucent over the world in the styles where the game's own windows are, and opaque in
the ones where they are not.

![The same window in all eight of the game's window styles](media/wintypes.png)

In Lua:

```lua
local style = nui.wtread()     -- the style the player has chosen
nui.wintype(style)             -- draw in it
```

## Hello, window

This is the whole native side of an addon. Everything else comes from the library compiled in
beside it.

```cpp
#include <windows.h>
#include "host.hpp"

namespace { nui::HostConfig const kConfig {"nuidemo", "1.0.0"}; }

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH) nui::host_detach();
    return TRUE;
}

extern "C" __declspec(dllexport) int __cdecl luaopen__nuidemo(lua_State* L) {
    return nui::host_open(L, kConfig);
}
```

The DLL loads like any other Lua module. Start it, then send it a layer of drawing commands. A
layer is text: one command per line, fields separated by tabs.

```lua
package.cpath = windower.addon_path:gsub('\\', '/') .. 'libs/?.dll;' .. package.cpath
local nui = require('_nuidemo')

local ws = windower.get_windower_settings()
nui.screen(ws.x_res or ws.ui_x_res, ws.y_res or ws.ui_y_res)
local ok, msg, retry_ms = nui.start()
if not ok then
    if retry_ms then return end          -- no addon has found the hook yet; try again later
    error(msg)                           -- something is wrong; say so and stay quiet
end

nui.layer(1, 0, table.concat({
    'W\t40\t40\t560\t150\tnui',                    -- a window in the game's frame art
    "T\t60\t78\tFFFFFFFF\tHello from Direct3D 8.", -- text in the game's chat font
}, '\n'))
```

![A window and a line of text](media/hello.png)

[`example/layer.lua`](example/layer.lua) wraps that as `b:window(40, 40, 560, 150, 'nui')` and
`b:text(60, 78, 'Hello', 'FFFFFFFF')`, so nothing counts tab-separated fields by hand. It is about
a hundred lines, meant to be copied into an addon and changed.

## What it can draw

### Three fonts, all the client's own

```lua
b:title(60, 96, 34, 'The nameplate serif', 'FFD98AFF')  -- scales to any size
b:text(60, 240, 'Body text is the chat font', 'FFFFFFFF')
b:wtitle(x, y, 'a window title', 'C0C0C0FF')
```

![The three fonts](media/fonts.png)

Character advances come from the client, so a string measures the same in an addon as it does in
the game. `metrics()` returns that table, for layout and hit testing.

### Gauges and item icons

```lua
b:gauge(104, 106, 300, 9, 0.82, '7CD97CFF')   -- the client's own gauge art, fill tinted
b:icon(60, 236, 32, 4096)                     -- Fire Crystal, straight out of the item DATs
b:icon(60, 314, 32, 4096, 'FFFFFFFF', true)   -- the same icon, greyed
```

![Gauges and icons](media/widgets.png)

### Your own pictures

```lua
local handle = nui.tex_load('data/map.png', 1024, 1024)
b:pic(handle, 60, 104, 128, 128, 0, 0, 128, 128)                     -- 1:1
b:pic(handle, 214, 104, 128, 128, 0, 0, 128, 128, nil, 'grey')       -- desaturated
b:pic(handle, 368, 104, 128, 128, 0, 0, 128, 128, 'FFD98AFF')        -- tinted
b:pic(handle, 676, 104, 128, 128, 32, 32, 40, 40, nil, 'linear')     -- a crop, smoothed
```

![Pictures, tinted, greyed and cropped](media/picture.png)

PNG and JPEG are decoded off the Lua thread, so a load does not stall the game, downscaled to the
given bound, and kept in the shared cache keyed by their contents. Point and linear filtering are
per quad, so pixel art stays sharp next to a smoothly scaled photograph.

### Clipping

```lua
b:clip(56, 70, 544, 320)
--  ... a list longer than the window ...
b:pop()
```

![A list clipped to its window](media/clip.png)

Clip rectangles nest. A whole layer can be moved and re-clipped with `layer_pos()` without sending
its commands again, which is what makes scrolling cheap.

## Live data

![A HUD updating frame by frame](media/live.gif)

An addon rebuilds a layer's text when its numbers change. nui parses that text once, merges it
into as few batches as the overlaps allow, and replays the result until a new one arrives. The
layer above is four gauges, seven icons and a handful of strings; nothing in it is re-parsed on a
frame where none of them changed.

## Addons built with nui

### Chronicle

A quest and mission tracker: every mission and quest in the game, with converted walkthroughs,
pictures, and progress read from the packets the server already sends you.
[github.com/jintawk/ffxi-chronicle](https://github.com/jintawk/ffxi-chronicle)

![Chronicle's home screen](media/addon-chronicle-home.png)

Area cards with their own art, a completion gauge each, and the missions in progress underneath
with the next step of each. One layer.

![A walkthrough page, scrolled, with a sticky tab bar and a link tooltip](media/addon-chronicle-page.png)

One page, scrolled fourteen hundred pixels: a tab bar stuck to the top of the view, a hovered link
with its tooltip, bullets nested four deep, coloured link spans for zones, NPCs and coordinates,
item icons inline in a sentence, and three maps fetched and cached. Laid out in Lua against the
font metrics, drawn as one clipped layer.

![A quest page showing its requirements with item icons](media/addon-chronicle-requirements.png)

Requirements with their item icons, cut out of the client's item DATs by item id.

### Resize, and text that re-wraps as you drag

![Dragging a window's grip while its text re-wraps](media/addon-chronicle-resize.gif)

Dragging the grip in the corner resizes the window; the paragraphs re-wrap to the new width and the
reading position is kept. The wrapping is done in Lua against the per-character advance widths
`metrics()` returns. Those are the client's own widths, so the measurement that decides where a
line breaks matches the drawing that follows it.

### Scrolling

![A long page scrolling under its clip rectangle](media/addon-chronicle-scroll.gif)

A clip rectangle and a layer offset. The commands were parsed once when the page was opened, and
scrolling replays them at a different origin without touching the parser, so a page of a thousand
words costs the same to scroll as a page of ten.

### Clickable links

![Links in a walkthrough, with a hover tooltip](media/addon-chronicle-links.png)

nui has no notion of a link, a button or a cursor. It draws text and reports its width. The addon
treats the span from x to x plus that width as a link, colours it, underlines it with a one-pixel
rectangle, tracks the mouse over it, and calls `windower.open_url` when it is clicked.

### A combat HUD

Another addon on nui: party, target, enemies, recasts, weapon skills, buffs and debuffs. It is the
one that has pushed the library hardest, which is why it is worth showing here.

![A party window](media/addon-hud-party.png)

Six rows with HP, MP and TP gauges, status icons cut from the game's own DATs, distance in yalms,
a KO'd member, a Poison tag, a Doom banner and a danger outline on the row that needs one. Each
status icon is its own texture; a second addon showing the same ones holds none of its own.

![A target window](media/addon-hud-target.png)

A target card with a cast bar counting down, a tracked-debuff list with a button per row, a
sub-target card, and twenty-four nearby mobs with HP pips, distance, bearing and what each one is
casting. One layer, clipped.

![A settings window](media/addon-hud-settings.png)

A category rail, toggles, sliders with their own hit areas and a three-way picker. Rectangles,
text and the gauge widget, positioned in Lua.

### Going quiet when the game does

![The same scene with the HUD drawn and with it hidden](media/addon-hud-hide.png)

The client closes its own windows when the player crosses a zone line, stops drawing them during a
cutscene, and hides everything when the hide-UI key is pressed. An addon that keeps drawing through
that is left on screen on its own.

The HUD above goes with them. It reads the client's app phase, its zone-in-ready flag, the hide-UI
toggle, and whether the chat log's row is being drawn, taking the first of those that answers, with
the packets as a fallback. When the game's chat log is not on screen, neither is the HUD; it comes
back on the same frame.

That reading is not nui's doing. `host_open` takes an array of extra Lua functions, so an addon can
add native functions of its own to the module table that `require` returns:

```cpp
nui::HostFn const kExtra[] = {{"gamestate", l_gamestate}, {"gamestate_rva", l_gamestate_rva}};

extern "C" __declspec(dllexport) int __cdecl luaopen__myaddon(lua_State* L) {
    return nui::host_open(L, kConfig, kExtra, 2);
}
```

nui supplies the hook, the renderer, the cache and the Lua binding. Anything else the DLL reads is
the addon's own, and it appears in the same module table as `layer()` and `metrics()`.

### Window snapping

nui draws; it does not handle input and has no concept of a window manager. `layer_pos()` moves an
already-parsed layer cheaply, which is what makes this affordable per frame:

![Dragging a window until it snaps to its neighbour](media/addon-hud-snap.gif)

That addon's window manager snaps a dragged window at twelve pixels to its neighbours, to the
game's chat log and to the screen edges, keeping a twelve pixel gap or aligning edges, and draws
the guide lines visible during the drag. Holding Shift drops the snapping mid-drag. None of that is
in nui; it is about a hundred lines on top of it.

## How it works

**The hook.** FFXiMain has a small wrapper around the renderer's `EndScene` call: thirteen bytes of
code and three of padding. nui finds it by byte signature, not by address, and replaces its first
instruction with a jump to a shared thunk, in one locked eight-byte write. The patch lives in a
named shared memory block (`UiHook 2`), so the first addon to arrive installs it and every addon
after that registers a callback with the one already there. Addons unload in any order. The patch
itself is written once and never undone, because undoing it safely while another thread might be
inside it is not a thing you can do: with nobody registered the thunk is a pass-through, a jump out
and a jump back, which is what the client's own padding was there for.

**The renderer.** Each callback gets the device the game is about to present with. nui saves every
render state it touches, draws its batches, and puts the states back as it found them. Textures are
created in the managed pool, so a device reset the addon never sees cannot lose them.

**The cache.** Window art, item icons and pictures are keyed by what is in them, in a second shared
block (`NuiCache major 1`). The first addon to want a texture creates it; the rest hold the same
one. Holder bits track who still needs it, and an entry nobody holds any more survives ten seconds,
so `//lua reload youraddon` finds its art still there instead of decoding two megabytes again.

**The layers.** Lua sends up to 16 layers, drawn back to front. A layer is parsed on the addon's
own worker thread, built into vertices there, and swapped in under a short lock. The render thread
draws once per displayed frame and never reads a file.

Both shared block layouts are frozen. Their offsets will not change: new fields are only ever
appended into reserved space, with a revision number, and a module built against an older revision
keeps working beside a newer one. `test/frozen/` holds the layouts as released, and a test fails
any change to them that is not an append.

## Several addons at once

Because the library is vendored, a player running three nui addons has three copies of nui in the
process, quite possibly at three different versions, loading in whatever order Windower happens to
load them. The two shared blocks are what makes that work, and their rules are the whole of the
contract between addons:

- **The first one in installs the patch.** The rest find it already there and register a callback.
  There is no leader to elect and no order to get right.
- **The newest one drives.** The frame is dispatched by the live client with the highest revision
  number. When it unloads, the frame passes to the highest-revision survivor. Whichever module is
  driving calls every other module's draw through one signature that never changes, so an addon
  built against an older nui is driven correctly by a newer one, and vice versa.
- **An addon that cannot find the wrapper still draws.** If a client update moves the code and only
  a newer nui knows the new shape, the older addon joins the bus the newer one patched.
- **Textures are keyed by content, not by owner.** Two addons that load the same picture, or draw
  the same item icon, get the same texture without knowing the other exists.
- **Unloading is safe in any order.** A module leaves in `DllMain`, handing the frame on before its
  code is unmapped. A dispatcher skips a callback whose module has gone, and a register replaces an
  owner that has gone.
- **A conflict fails closed.** The one case nui refuses is a pre-release ABI 1 patch already sitting
  on the wrapper: it is never patched over. `start()` returns `blocked`, with no retry, and nothing
  is drawn.

The test matrix covers this directly: two and three modules, every load order crossed with every
unload order, with and without `stop()`, including a module at a different revision, one that never
unregisters, one that cannot find the wrapper, and one built against the pre-release ABI.
[docs/abi.md](docs/abi.md) is the full contract.

## What it costs

Windower runs an addon's Lua on a worker thread that the **game thread waits on** for packets,
mouse and keys, so a slow Lua pass shows up as a stutter. That thread, not the draw, is what nui is
arranged around.

`example/perf.lua` measures it. The demo scenes in this README, at 3840 by 2160:

| Scene | Quads | Batches | Building the command text in Lua | `layer()`: parse and build vertices | Worst |
|---|---|---|---|---|---|
| `hello` | 178 | 3 | 5 us | 28 us | 64 us |
| `live` | 246 | 11 | 24 us | 45 us | 71 us |
| `hud` | 1,046 | 18 | 68 us | 182 us | 296 us |

The HUD screenshot at the top of this page, rebuilt from scratch in Lua and re-parsed into
vertices, costs a quarter of a millisecond on the thread the game waits on, and only on the frames
where something changed: a layer whose text is not re-sent is replayed from its vertex cache, and
`layer_pos()` does not rebuild it at all. Lock waits over 50 us during those runs: zero, on either
thread.

How:

- **One draw per displayed frame.** The client's `EndScene` wrapper runs several times a frame;
  nui draws on the call that has the back buffer bound, and falls back to drawing on every call
  only if none has for half a second.
- **The build is off the render thread.** Parsing, vertex building and cutting item icons happen
  on the addon's worker thread, outside the lock, and the finished work is swapped in.
- **Batches are merged.** After a layer is built, its quads are reordered into as few draw calls as
  their overlaps allow. The HUD above is 1,046 quads in 18 batches. A test that draws sixteen icons
  as sixteen batches and then as one measures no difference between them, so on a modern machine
  this matters less than it did; it costs nothing to do.
- **Nothing touches the disk on a hot path.** Pictures are decoded on a thread of their own; the
  render thread reads a file only if the worker somehow failed to supply pixels for a full second.
- **`prof()` reports the rest.** Per thread: lock waits, build times, draw times, uploads, file
  reads, decode failures. Reading it costs nothing between calls, so an addon can put it behind a
  command.

Memory, for the HUD scene: six textures, 2.1 MiB in total, all of them shared, and nearly all of
that is the one atlas of game art that every nui addon in the process draws from. A second addon
drawing the same window art and the same item icons adds none of its own; on a 25-texture scene
that is about 12 MiB saved per extra addon. The compiled DLL, with the whole library statically
linked and no runtime to install, is about 1.4 MB.

The client is a 32-bit process and dies somewhere near 2 GB of address space, which is a real
constraint on FFXI addons. The test suite watches the texture side of it directly: it counts live
textures and their holders across modules loading and unloading in every order, and fails on a leak,
a double hold or a use after free.

The table leaves out draw time on purpose. The offline harness draws on a hidden surface, so what
it measures there is mostly fill rate on whatever adapter it ran on, which does not transfer to
another machine. The Lua-thread numbers are CPU work and do transfer; the draw is a handful of
large alpha-blended quads, and the cost of that depends on the GPU.

## Putting it in your addon

nui is **vendored**, not installed: the source is copied into the addon and compiled into the
addon's own DLL. That is what makes an addon built on it a single self-contained folder.

1. Copy `src/` into the addon as `plugin/nui/`.
2. Write the glue file, the `.def` file and the build script. Start from
   [`example/DemoModule.cpp`](example/DemoModule.cpp), [`example/exports.def`](example/exports.def)
   and [`example/build.sh`](example/build.sh); the only addon-specific parts are the module name
   and the output filename.
3. Build with MinGW-w64 i686 and a static runtime:

   ```sh
   i686-w64-mingw32-g++ -std=c++17 -O2 -m32 -shared -Wall -Wextra -Inui \
       MyModule.cpp nui/*.cpp exports.def \
       -o ../libs/_myaddon.dll -s -static -static-libgcc -static-libstdc++
   ```

4. Add the addon's path to `consumers.txt` and run `python sync.py sync` to keep the vendored copy
   byte-identical to this repository's. `python sync.py check` fails a build when it has drifted,
   and `--dll` checks that a built DLL carries the right source hash, which catches a DLL built
   from a locally edited copy.

The Lua module table is documented in [docs/lua-api.md](docs/lua-api.md), the drawing commands in
[docs/layer-commands.md](docs/layer-commands.md), and the two shared buses in
[docs/abi.md](docs/abi.md). The short version of the module table:

| | |
|---|---|
| `start([dir [, flags]])` | join the hook; returns `ok, msg [, retry_ms]` |
| `stop()` | leave, release everything |
| `screen(w, h [, auto [, f]])` | the coordinate space your Lua draws in |
| `layer(id, z, text)` / `layer(id)` | set or remove one of 16 layers |
| `layer_pos(id, ox, oy [, clip])` | move and clip a layer without re-sending it |
| `tex_load(path [, w, h])` / `tex_free(h)` | pictures |
| `icon(item_id)` / `icon_pixels(item_id)` | item icons |
| `metrics()` | font advance tables, for layout and hit testing |
| `wintype(n)` / `wtread()` | the window style, 1 to 8 |
| `status()` / `prof()` / `us()` | state, counters, a microsecond clock |
| `pad_start()` / `pad()` / `pad_capture(on)` | the controller: read it, hold it (1.3.0) |
| `key_start()` / `key_hide(code, ...)` | keep single keys from the game (1.6.0) |

A DLL can add native functions of its own to that table: `host_open` takes an array of
`{name, lua_CFunction}` and appends them. See
[Going quiet when the game does](#going-quiet-when-the-game-does) for why you might want to.

Hit testing is not nui's job. It returns the font metrics and draws what it is given; what a click
means is the addon's business. The one input nui does take is the **controller** (1.3.0), because no
Windower API offers it: `pad()` reads the pad the game reads, and `pad_capture(true)` keeps new
presses from the game while the addon uses them. It is one shared hook, as fail closed as the drawing
hook. Since 1.5.0 it patches the client's own XInput wrapper in `FFXiMain.dll`, which works with every
build of `xinput1_3.dll`. Where that wrapper is not found, it falls back to a hot patch of
`xinput1_3.dll!XInputGetState`. See [docs/lua-api.md](docs/lua-api.md#the-controller-130) and
[docs/abi.md](docs/abi.md#padhook-1).

The **keyboard** (1.6.0) is the other input nui takes. Windower's `keyboard` event sees every key but
cannot keep one from the game, and `keyboard_blockinput` keeps all of them. `key_hide(...)` hides only
the keys an addon names, from the game's actions alone, so the player can still chat, use macros and
run Windower binds. It patches the client's two keyboard reads in `FFXiMain.dll`, after Windower has
seen them. The chat line reads typed text from window messages, which this hook does not see. See
[docs/lua-api.md](docs/lua-api.md#the-keyboard-160) and [docs/abi.md](docs/abi.md#keyhook-1).

## Drawing offline

The renderer does not need the game running. `example/render.lua` makes a real Direct3D 8 device on
a hidden window, loads a copy of FFXiMain so its `EndScene` wrapper is there to hook, and pushes
frames through it. What comes back is the same pixels the game would show, so a window's layout
can be iterated on without logging in, and layout checks can go in a test suite.

Every screenshot and GIF in this README was made that way, by
[`example/scenes.lua`](example/scenes.lua):

```sh
sh test/build_test.sh                      # builds cr_testhost.dll, among other things
sh example/build.sh                        # builds _nuidemo.dll
export NUI_TEST_HOST=... NUI_FFXI_COPY=... NUI_FFXI_DIR=...
lua example/render.lua out
python example/raw_to_png.py out
python example/make_gif.py out cycle 1000
lua example/perf.lua 600 hud                # and the numbers under "What it costs"
```

See [example/README.md](example/README.md) for what each of those variables needs to point at.

## When the client changes

Square Enix patch the client regularly. nui finds everything it needs by byte signature rather than
by address, so most patches change nothing. When one moves the code it hooks, nui patches nothing
and draws nothing, and `status()` reports it in a line an addon can print. Nothing is written to
the game's memory unless the expected bytes are at the expected place.

The art works the same way. If a DAT chunk is not what the library expects, that piece is not
drawn and the rest continues.

## Building and testing

The library is about 4,400 lines of C++17 plus `stb_image.h`, and builds with MinGW-w64 i686. There
are no dependencies beyond the Windows SDK headers MinGW ships.

The tests need two things this repository cannot ship:

- **An unpacked copy of `FFXiMain.dll`.** The shipped DLL has its code section encrypted, and the
  stub unpacks it only when the DLL is loaded. `NUI_FFXI_COPY` points at an unpacked copy. It must
  not point at the DLL in the install, because the tests load it.
- **A Final Fantasy XI installation**, read only, for the fonts, window art and item icons.

```sh
NUI_FFXI_COPY=/path/to/unpacked/FFXiMain.dll sh test/build_test.sh
NUI_FFXI_COPY=/path/to/unpacked/FFXiMain.dll sh test/build_test.sh --self-test
```

`--self-test` rebuilds the library once per deliberately introduced fault, eighty-five of them,
and requires that the test aimed at each fault is the test that fails. It checks that the suite
would notice a broken build rather than passing against one.

What the suites cover: the hook against the real unpacked wrapper; every load and unload order of
two and three modules, including a module that never unregisters and one built against the
pre-release ABI; the shared cache with real and fake textures across modules; the whole renderer
end to end with pixel comparisons; the command parser against both real captured layers and
fuzzed input; and the frozen shared-memory layouts.

One suite needs neither. `sh test/hd_linux/run.sh` builds the DAT reader natively with g++ on
Linux, against a made-up install held in memory. It checks that an HD texture pack, at 2, 4 and 8
times the game's size and in every texture format the reader takes, loads to exactly the stock
atlas, frame recipe and glyphs. CI runs it on every pull request.

The controller hook has two suites of its own, and neither needs game files.
`sh test/pad_linux/run.sh --self-test` needs only g++ with 32-bit support (`g++-multilib`). It runs
the mask, the chord and the client's real XInput wrapper bytes through the patch, with the stack
checked on every call. CI runs it on every pull request. `sh test/build_pad_test.sh --self-test`
builds Windows exes with stand-ins for the client, its shim and `xinput1_3.dll`. CI builds them,
but they must run on Windows.

The keyboard hook has two as well. `sh test/key_linux/run.sh --self-test` runs the press rules. It
also runs the client's real keyboard read bytes through the patch, and it checks the stack, the
registers and the client's own flags on every call. `sh test/build_key_test.sh --self-test` builds Windows exes
with a stand-in client and a second module. CI runs the first and builds the second.

## Licence

BSD 3-Clause. See [LICENSE](LICENSE).

`src/stb_image.h` is Sean Barrett's stb_image, public domain / MIT, carried unchanged.

The hook is modelled on BroGuyPal's SceneHook, with its own shared-memory name and its own ABI so
the two never collide; an addon can use both.

Final Fantasy XI is a trademark of Square Enix. This project is not affiliated with or endorsed by
Square Enix. It reads the art out of an existing installation and draws it on the same screen; it
ships none of it.

The Chronicle screenshots show walkthrough text and maps from BG-wiki, which that addon converts
and fetches. They are included to show the layout; they belong to their authors.
