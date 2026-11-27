# The shared buses

Addons that use nui each compile their own copy of it, which means several copies of the library
can be alive in one game process at once, at different versions, loading and unloading in any
order. Four things cannot be done per copy: patching the client's renderer, holding a texture,
patching the controller read, and patching the keyboard reads. Those go through named shared memory,
and their layouts are frozen.

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

A change that alters only the art of an HD texture pack also needs a bump, for the same reason. nui
1.5.0 bumps the atlas to version 4. Its box filter now keeps the colour of a block with no opaque
texel, where 1.4.2 made it black. No stock pixel changes, so the golden hashes stay the same.

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

With its **Enable XInput** setting on, the client reads the pad every frame. It does not call
Microsoft's DLL directly. It loads its own shim, `xinputdll.dll`, from the game folder, and the shim
calls `xinput1_3.dll`. There can only be one patch of that read, so, as with UiHook, there is one bus,
its ABI is 1 forever, and everything after is a revision within it.

There are two routes to the read. A module of revision 3 (nui 1.5.0) tries route 1 first. If route 1
fails, it tries route 0. `PadBus::route` says which route is live, and `pad_status().route` reports it.
On a bus that a revision 1 or 2 module patched, the field is zero, which is route 0.

- **Route 1: the client's XInput wrapper (revision 3, nui 1.5.0).** The client keeps the shim's handle
  in one slot and the shim's `XInputGetState_` in the next. One small wrapper in `FFXiMain.dll` reads
  both and calls the function.
  - nui finds the wrapper by byte signature over the client's code sections, never by address. The
    signature also matches the SetState wrapper. The GetState wrapper is the match whose function slot
    is 4 bytes after its handle slot. nui needs exactly one such match, both slots in a writable
    section of `FFXiMain.dll`, and the wrapper's patch bytes on an 8-byte boundary. Once the client
    has loaded the shim, the handle slot must hold `xinputdll.dll` and the function slot must hold its
    `XInputGetState_`.
  - The patch changes one instruction of the wrapper. `mov eax, [function slot]` becomes
    `mov eax, thunk`. It has the same length, and one locked 8-byte compare-and-exchange writes it.
  - The client writes only its slots, never its code. So the client's own rewrites of the slots (at
    input init, on each device re-enumeration, and about every 1.5 seconds while no pad is attached)
    never undo the patch. The thunk reads the function slot on every call, so no saved pointer can go
    stale.
  - The wrapper calls the shim as `__cdecl` and pops the arguments itself. The thunk therefore copies
    the two arguments before it calls the dispatcher, which is `__stdcall` and pops the copies. The
    dispatcher's "original" is a small `__stdcall` adapter in the same block, which calls the shim. A
    thunk put into the slot itself would crash the game, because the arguments would be popped twice.
    `test/pad_linux` runs that case on the client's real wrapper bytes and requires the crash.
  - The patch goes in before the client loads the shim, and it does nothing in DirectInput mode,
    because the wrapper returns before it reaches the patched instruction.
  - The client's unloader frees the shim and zeros only the handle slot. nui pins `xinputdll.dll` the
    first time `pad()` finds the handle slot set, so the unloader can never unmap it while a read is on
    its way through the dispatcher.
- **Route 0: the Windows hot-patch form of `xinput1_3.dll!XInputGetState`** (revisions 1 and 2, and
  the fallback of revision 3). A long jump goes into the five bytes of padding before the function,
  where no thread runs, and then the function's first two bytes (`8B FF`, `mov edi, edi`) become a
  short jump back to it, in one locked 16-bit write. Calling the function's third byte is the
  original, so nothing is displaced. Anything else at the entry (another tool's hook, code that is not
  hot-patchable) is never patched over. Many builds of `xinput1_3.dll` start with `55 8B EC` and are
  not hot-patchable, so route 0 refuses on those machines.
- **When neither route works.** `pad_start()` fails, and the status names both reasons. While
  `xinput1_3.dll` is not loaded and route 1 has failed, `pad_start()` asks to be retried. When route 0
  refuses for good, it fails without a retry and says which bytes it found. nui never loads
  `xinput1_3.dll` itself. On route 0 it pins the DLL once found, so it can never unload under the
  patch.
- **Neither patch is ever undone.** Both thunks live in one 128-byte block that no module owns and
  nothing frees.
- **Revisions meet on one bus.** Any revision's dispatcher works through either route's thunk and
  calls the bus's original the same way. A revision 2 module that joins a route 1 bus registers as
  before. A revision 2 module sets `blocked` when route 0 refuses. A revision 3 module still tries
  route 1 then, and clears `blocked` when route 1 succeeds.
- **To revision 3, `blocked` means only that route 0 is refused for good.** `pad_start()` stops asking
  for a retry only when route 1 is refused for good as well: no unique wrapper, its slots outside a
  writable section, its bytes changed by another tool, or slots that held other values on three
  tries. A route 1 failure that can pass, such as the client writing its slots at that moment, is
  retried.
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

## KeyHook 1

`Local\FFXiKeyHook_v1_<pid>`, 4,096 bytes, mapped whole. Added in nui 1.6.0
(`src/KeyHook/KeyHook.h`, frozen as `test/frozen/layout_keyhook.1.6.0.txt`).

The client reads the DirectInput keyboard in one function, once per main loop tick on its main
thread. It calls `GetDeviceState` for the 256-byte key state, then `GetDeviceData` for up to 16
buffered events. A key counts as down when its state byte says so, or when a press event for it came
in that tick. The client ignores release events.

There can only be one patch of those reads, so, as with UiHook and PadHook, there is one bus, its ABI
is 1 forever, and everything after is a revision within it.

- **The sites.** The instruction after each read is `cmp eax, 0x800700AA`. nui finds each read by a
  signature that runs from the load of the device to that `cmp` (45 and 42 bytes). It needs:
  - exactly one match of each in the client's code
  - both reads to load the device from the same place
  - the event read within 1 KB after the state read
  - each `cmp` inside one aligned 8-byte block

  Otherwise it patches nothing, for good in this process, and `key_start()` says why.
- **The patch** changes each `cmp` into a call of the same length to a thunk. One locked 8-byte
  compare-and-exchange writes it. A thread inside the read returns to a whole instruction either way.
  The thunk runs after the read. It passes the read's result, its buffer, the count and the device to
  the dispatcher. Then it runs the client's own `cmp` on the client's own `eax`, and returns with the
  flags that the `cmp` set. There is nothing to chain, because the read has already run.
- **Windower's wrapper.** With Windower loaded, the device the client reads is Windower's keyboard
  wrapper, which applies its own block, console key and binds first. The hook runs after it, so
  Windower's events and binds see every key.
- **Neither patch is ever undone.** Both thunks live in one 128-byte block that no module owns and
  nothing frees. With nobody driving, a thunk runs the client's `cmp` and nothing else.
- **The thunks call the dispatcher with one frozen signature**, `keyhook_fn(kind, hr, data, count,
  asked, device)`. The newest revision drives, a module hands the dispatch on in `DllMain`, and a
  register replaces a driver whose module has gone, as with UiHook. A dispatcher ignores a `kind` it
  does not know.
- **What the dispatcher does.** It hides the union of the live clients' key masks (256 bits each). It
  keeps one record per key for both reads: the game saw this press, or this press is hidden.
  - The dispatcher decides a new press once, at the first read that shows it.
  - A press the game saw stays the game's until its release.
  - A hidden press stays hidden until its release, even after its key leaves every mask.
  - The state read clears a hidden key's byte. The event read drops a hidden key's events, moves the
    rest down and writes the new count.
  - A press ends at its release event. If that event is lost, the press ends once the state shows the
    key up and the event read returned fewer events than the client asked for.
- **A mask needs a heartbeat.** A client that did not renew its mask in the last 250 ms hides
  nothing new. An addon that errors or stops can never leave the player without keys. A press that
  is already hidden still waits for its release.
- **What it does not see.** The chat line reads Space, `/` and typed characters from window messages,
  not from DirectInput.

