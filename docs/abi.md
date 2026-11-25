# The shared buses

Addons that use nui each compile their own copy of it, which means several copies of the library
can be alive in one game process at once, at different versions, loading and unloading in any
order. Three things cannot be done per copy: patching the client's renderer, holding a texture, and
patching the controller read. Those go through named shared memory, and their layouts are frozen.

Frozen means the offsets never move. New fields are only ever appended into reserved space with a
revision number, and zero always means "the old behaviour", because the mapping starts zeroed. A
module built against revision 1 works beside a module built against revision 5, in either role.
`test/frozen/` holds the layouts as first released, and `test/frozen_check.py` fails any change to
them that is not an append. `test/layout_*.txt` is regenerated from the headers and compared
against it.

## UiHook 2

`Local\FFXiUiHook_v2_<pid>`, 4,096 bytes, mapped whole.

The client has a 13-byte wrapper around its renderer's `EndScene` call. There can only be one patch
of it, so there can only ever be one of these buses, which is why the ABI is 2 forever and
everything after is a revision within it.

- The first module to arrive finds the wrapper by byte signature and patches it to jump to a thunk
  in shared memory. Every module after that registers a draw callback with the bus already there.
- The thunk uses `pushad`/`popad`, so the client's `EndScene` runs once with its own `this` and its
  return value comes back untouched. With no dispatcher registered the thunk is a pass-through.
- The frame is driven by the live client with the **highest revision**. A newer module takes it
  over when it registers; when it leaves, the frame passes to the highest-revision survivor, or to
  nobody. Whichever module drives, it calls every live client's draw through the one frozen
  signature. That is the entire compatibility contract between versions.
- Joining needs only that somebody has patched. A module whose own signature search fails, because
  the client was updated and only a newer module knows the new shape, still joins and still draws.
- A module leaves in `DllMain(DLL_PROCESS_DETACH)`, which hands the frame on before its code is
  unmapped. It takes no lock and waits on nothing, because that runs under the loader lock.
- As defence in depth, a dispatcher skips and frees a client whose draw function is no longer inside
  its module's mapped image, and a register replaces an owner whose dispatcher has gone.
- If a pre-release ABI 1 patch is already on the wrapper, nui never patches over it. `start()`
  returns `blocked`, with no retry, and nothing is drawn.

The hook is modelled on BroGuyPal's SceneHook, which hooks a different function (inside
`draw_scene`, under the game's UI rather than over it) and uses a different mapping name. The two
never meet, and an addon can use both.

## NuiCache major 1

`Local\FFXiNuiCache_v1_<pid>`.

One `IDirect3DTexture8` per distinct piece of art per process, however many addons draw it.

Entries are keyed by what is in them, never by who asked:

| Kind | Key |
|---|---|
| `NC_ATLAS` | window style 1 to 8, plus a hash of the install folder |
| `NC_ITEM_ICON` | item id, plus a hash of the install folder |
| `NC_PICTURE` | the maximum width and height asked for, plus a 64-bit hash of the file's **contents** |

Each kind also carries a pixel-format version, bumped whenever a change to the library alters a
single output byte of that kind's decode. Two library versions that would decode the same source
differently therefore never share a texture, and `test/golden_fmt.json` fails a build that changes
the pixels without bumping the version.

A change that lets a load succeed where it used to fail also alters the output, even when a stock
install decodes exactly as before. The golden test sees only a stock install, so it cannot catch
that, and the version has to be bumped by hand. nui 1.4.1 missed this. It read the art of HD texture
packs, which 1.2.10 cannot read, under the same atlas version. An addon on each version then shared
one atlas, and the one that created it decided whether the other drew any window art. nui 1.4.2
bumps the atlas to version 3.

- Textures are created on the render thread, in the managed pool.
- Holder bits track which modules still need an entry. An entry nobody holds survives ten seconds,
  so `//lua reload youraddon` finds its own atlas still there rather than decoding it again, and
  reloading one addon never throws away art another addon is still drawing. Past that, unheld
  entries go oldest first once they are over the budget.
- The `noshare` flag makes a module keep its own textures. Identical pixels, more memory. It exists
  so that a bug in the cache can never be the thing standing between a player and their interface.
- A new cache major would simply not share with this one. There is no negotiation and no fallback
  path to get wrong.

On a scene with 25 distinct textures, a second addon sharing them adds nothing measurable, against
roughly 12 MiB when it holds its own.

## PadHook 1

`Local\FFXiPadHook_v1_<pid>`, 4,096 bytes, mapped whole. Added in nui 1.3.0
(`src/PadHook/PadHook.h`, frozen as `test/frozen/layout_padhook.1.3.0.txt`).

With the client's **Enable XInput** setting on, the game reads the pad with
`xinput1_3.dll!XInputGetState` every frame. There can only be one patch of that function, so, as with
UiHook, there is one bus, its ABI is 1 forever, and everything after is a revision within it.

- **The patch is the Windows hot-patch form.** A long jump goes into the five bytes of padding before
  the function, where no thread runs, and then the function's first two bytes (`8B FF`,
  `mov edi, edi`) become a short jump back to it, in one locked 16-bit write. Calling the function's
  third byte is the original, so nothing is displaced. Anything else at the entry (another tool's hook,
  code that is not hot-patchable) is never patched over: `pad_start()` fails without a retry and says
  what bytes it found.
- **nui never loads `xinput1_3.dll`.** While it is not loaded (the game reads a DirectInput pad, or has
  not read one yet), `pad_start()` asks to be retried. Once found, it is pinned, so it can never unload
  under the patch.
- **The thunk is a pass-through when nobody drives.** As UiHook: the newest revision drives, a module
  hands the dispatch on in `DllMain`, and a register replaces a driver whose module has gone.
- **What the dispatcher does.** It calls the original, records the state per user index behind a
  seqlock (for every module to read), and applies one shared mask:
  - while any live client **holds the pad**, presses made from then on are hidden from the game, and
    the sticks read centred and the triggers released;
  - a button the game had already seen go down stays the game's until it is released;
  - a press the game never saw stays hidden until it is released, even after the pad is given back,
    so the game never sees half a press;
  - `dwPacketNumber` is renumbered, so it changes exactly when what the game sees changes.
- **The chord (revision 2, nui 1.4.0).** A client can name the buttons that take its pad together
  (`pad_chord`, for example L3 + R3). While nobody holds the pad, the first of them to go down is held
  back from the game for up to 150 ms. If its partner arrives in that time, that is the chord, and the
  game never sees either button, even where it maps them itself (L3 is sit or heal, R3 the camera, by
  default). If the button is released alone in time, the game is shown the press for two reads, so a
  tap still works. If it is still held alone when the time is up, the game gets it then. Nothing
  else is ever delayed. A module of revision 1 leaves the field zero, and zero is no chord.
- **A hold needs a heartbeat.** A client that has not renewed its hold for 250 ms (`pad()` renews it)
  no longer holds the pad. An addon that errors or stops polling can never leave the player without a
  controller.
- **Designed for a second source.** `PadState::source` says where a state came from (`xinput`, and
  `dinput` reserved), and the mask works on one button word in XInput's layout with the triggers as
  two more bits. A DirectInput hook in a later revision adds a source, not a new contract.
