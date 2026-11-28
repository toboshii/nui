// PadHook.h - shared hook on the controller state the game reads, for nui modules that take the pad.
// PadHook 1: one ABI forever, revisions within it (the rules UiHook/UiHook.h keeps, applied here).
//
// With its "Enable XInput" setting on (padmode000 flag 6), the client reads an XInput pad through its own
// shim, xinputdll.dll in the game folder, which calls xinput1_3.dll. Without that setting, or for a pad that
// is not an XInput device, the client uses DirectInput8. Revision 4 hooks that too (the DirectInput source,
// below): the same clients, the same hold and the same chord, on its own part of the bus.
//
// There are two routes to the state. A module tries route 1 first, then route 0. bus->route says which one
// is live (0 also on a bus that a revision 1 or 2 module patched).
//
// Route 1 (revision 3): the client's XInput wrapper in FFXiMain.dll. The client keeps the shim's handle in
// one slot (H) and the shim's XInputGetState_ in the next (F). One wrapper reads both. It is found by its
// signature over FFXiMain's code sections, never by address (it is at 10008EA0 in the 2026-10 and 2026-11
// builds):
//
//     +0   A1 <H>                  mov eax, [H]
//          85 C0 74 1B             test eax, eax / jz fail      ; the shim is not loaded
//     +9   A1 <F>                  mov eax, [F]                 ; the patch goes here
//          85 C0 74 12             test eax, eax / jz fail
//          8B 4C 24 08 8B 54 24 04 51 52        push the two arguments
//          FF D0 83 C4 08 C2 08 00              call eax / add esp, 8 / ret 8: the shim is __cdecl
//     fail: B8 01 00 00 00 C2 08 00              mov eax, 1 / ret 8
//
// The signature (these 44 bytes, H and F left out) also matches the SetState wrapper. The GetState wrapper is
// the match where F = H + 4.
// PadHook needs exactly one such match, both slots in a writable section of FFXiMain.dll, and wrapper + 8 on
// an 8-byte boundary. Once the client has loaded the shim (H is not zero), H must hold xinputdll.dll's
// handle and F must hold its XInputGetState_. While H is zero, the signature and the slot layout decide alone.
//
// The patch changes `mov eax, [F]` to `mov eax, thunk` (B8 <thunk>). It has the same length and stays one
// instruction. One locked 8-byte compare-and-exchange of wrapper + 8 .. + 15 writes it, and bytes 8, 14 and
// 15 go back unchanged. The client writes only data, so its rewrites of the slots (at input init, on each
// device re-enumeration, and about every 1.5 s while no pad is attached) never undo the patch. Nothing is
// saved that can go stale: the thunk and the adapter read F on every call.
//
//     block+16  A1 <&dispatch>   mov eax, [dispatch]
//               85 C0 74 0B      test eax, eax / jz pass
//               FF 74 24 08      push [esp+8]                   ; copy the two arguments
//               FF 74 24 08      push [esp+8]
//               FF D0 C3         call eax / ret                 ; the __stdcall dispatcher pops the copies
//     pass:     A1 <F>           mov eax, [F]
//               85 C0 74 02      test eax, eax / jz none
//               FF E0            jmp eax                        ; the shim returns to the wrapper itself
//     none:     B8 01 00 00 00   mov eax, 1                     ; what the wrapper returns without F
//               C3               ret
//
//     block+64  the adapter, bus->original: __stdcall, as the dispatcher calls it
//               A1 <F> 85 C0 74 10               mov eax, [F] / test eax, eax / jz none
//               FF 74 24 08 FF 74 24 08          push [esp+8] twice
//               FF D0 83 C4 08 C2 08 00          call eax / add esp, 8 / ret 8
//     none:     B8 01 00 00 00 C2 08 00          mov eax, 1 / ret 8
//
// The wrapper tests F before it calls, and the patch takes that test away, so the thunk and the adapter
// test F again. A thunk in the slot itself would crash the game: the wrapper calls F as __cdecl and pops the
// arguments itself, so a __stdcall dispatcher reached by a jump pops them a second time (test/pad_linux runs
// that case and requires the crash). The client's unloader frees the shim and zeros only H, and the wrapper
// tests H before it reaches the patch. padhook_pin, which pad() calls, pins xinputdll.dll once the client has
// loaded it, so the unloader can never unmap it while a read is on its way through the dispatcher.
//
// Route 0 (revisions 1 and 2, and the fallback of revision 3): the Windows hot-patch form of
// xinput1_3.dll!XInputGetState, never undone:
//
//     site-5:  E9 <rel32 -> thunk>     the five bytes of padding before the function (90 or CC)
//     site:    EB F9                   was 8B FF (mov edi, edi): a jump back into the padding
//
// The long jump is written first, where no thread executes; then the two-byte instruction at the
// entry is swapped for the short jump in one locked 16-bit write, so a thread entering the function
// sees either `mov edi, edi` or the jump, never half of either. Calling site+2 is the original
// function (mov edi, edi is a no-op), so there is no displaced code and no trampoline to build.
//
//     block+16  A1 <&dispatch> mov eax, [dispatch]
//               85 C0          test eax, eax
//               74 02          jz  orig
//               FF E0          jmp eax                 ; the dispatcher returns to the game itself
//     orig:     E9 <site+2>    jmp to the original
//
// eax is free at a stdcall entry, so the game's arguments and return address are untouched. On this route
// xinput1_3.dll is pinned (it can then never unload under the patch). Many builds of xinput1_3.dll are not
// hot-patchable (they start 55 8B EC): route 0 refuses those, and says which bytes it found.
//
// On both routes the patch is never undone, and the thunk lives in one VirtualAlloc block (128 bytes) that
// no module owns and nothing frees. block+0 holds the driving module's dispatcher (__stdcall, with
// XInputGetState's signature), or null. With null, the hook passes every call through.
//
// What the dispatcher does (any module's: the live client with the highest revision drives, as UiHook):
//   1. calls the original;
//   2. records the state it returned into the bus (PadState per user index, behind a seqlock) for
//      every module to read;
//   3. hides buttons from the game while any live client holds the pad (capture, renewed by a
//      heartbeat: a client that stops renewing for PADHOOK_HEARTBEAT_MS no longer holds it, so a
//      stuck or crashed addon never leaves the player without a controller). The rules:
//        - a button held when the pad is taken still reaches the game until it is released;
//        - a button pressed while the pad is held is hidden from the game, and stays hidden until it
//          is released, even after the pad is given back (the game never sees half a press);
//        - while the pad is held, the sticks read centred and the triggers read released (unless held
//          from before the capture), so neither the camera nor a macro bar moves;
//   4. renumbers dwPacketNumber: the number the game sees changes exactly when the state it sees
//      changes (a button hidden or shown while the pad itself is still has a new number).
//   5. (revision 2) the chord. A client names the buttons that take its pad together (padhook_chord, e.g.
//      L3 + R3). While nobody holds the pad, the first of them to go down is held back from the game for
//      up to PADHOOK_CHORD_MS: its partner arriving makes it the chord, and both stay hidden until released
//      (the game never sees the chord, even when it maps those buttons itself); released alone before
//      then, it is shown to the game for PADHOOK_REPLAY_READS reads (a tap still reaches it); still held
//      alone when the time is up, the game gets it then. Only those buttons are ever delayed, and only
//      while nobody holds the pad.
//
// The DirectInput source (revision 4). The client reads exactly one pad. It uses XInput when Enable XInput
// is on and the pad is an XInput device. Otherwise it reads a DirectInput device (DIJOYSTATE2, buttons 0..31
// mapped to game actions by padsin000), from its input update, once per main loop tick, with two calls:
//
//     GetDeviceState(device, 0x110, input+0xA44)            the 272-byte DIJOYSTATE2
//     GetDeviceData(device, 0x14, input+0xB54, &n, 0)       up to n = 16 buffered events, no DIGDD_PEEK
//
// (the 2026-11-02 build: the device at input+0x90). The client takes a button's state from the state read and
// its press and release edges from the events (DIDEVICEOBJECTDATA: ofs 0x30 + button, data 0x80 pressed, 0
// released). It reads the sticks (lX .. lRz and the two sliders, ranged -127..127) and POV 0 from the state.
// So hiding a press means clearing its state byte and dropping its press and release events, and a press the
// game must still get (a chord button tapped alone) goes back in as events.
//
// The sites. Each patch changes one whole instruction of 5 bytes into `call thunk` (E8 <rel32>), in one locked
// 8-byte compare-and-exchange of the aligned block that holds it, so a thread anywhere in the client's code
// returns to or reaches a whole instruction either way. The state read is followed by `cmp eax, 0x800700AA`:
// that is the patch, as KeyHook's. The event read is followed by `test eax, eax`, too short for a call. Its
// success branch (a jz that nothing else reaches) runs two stores and then `mov byte [esp+0x13], 1`
// (C6 44 24 13 01), before the client looks at the events: that store is the patch. Its thunk makes the store
// itself. The client ignores the buffer when the read fails, and so does the hook. Each signature (52 and 100
// bytes, the second through that store) must match exactly once in FFXiMain.dll's code sections, both must
// load the device from the same offset of the input object, the event read must follow the state read within
// PADHOOK_DI_SPAN bytes, and each patched instruction must lie in one aligned 8-byte block. Otherwise nothing
// is patched, for good in this module, and the status says why.
//
//     di block+0    the driving module's DirectInput dispatcher (padhook_di_fn), or null
//     di block+16   the state read's thunk: push eax, ecx, edx; if a dispatcher drives, call it with
//                   (PADHOOK_DI_STATE, hr, ebx+buffer, null, 0x110, [ebx+device]); pop; cmp eax, 0x800700AA; ret
//     di block+72   the event read's thunk: pushfd, pushad; if a dispatcher drives, call it with
//                   (PADHOOK_DI_EVENTS, 0, edi (the events), ebp (&n), asked, [ebx+device]); popad, popfd;
//                   mov byte [esp+0x17], 1 (the client's store, past the return address); ret
//
// The signatures fix the registers: ebx is the input object, edi the events and ebp the count. The read has
// already run when either thunk runs, so there is nothing to chain and no original to call. With nobody
// driving, a thunk runs the client's own instruction and nothing else.
//
// The press rules (di_apply_state, di_apply_events), one record per button for both reads:
//   - a new press is decided once, at the first read that shows it: hidden while a client holds the pad, a
//     chord button waits for its partner, the game's otherwise;
//   - a press the game saw stays the game's until its release, and a hidden press stays hidden until its
//     release, even after the pad is given back (its release event is dropped too);
//   - while a client holds the pad, every stick and slider reads 0 (the client's centre) and POV 0 reads
//     centred, unless the game already had the POV held from before;
//   - the chord, as XInput's: a chord button held back for up to PADHOOK_CHORD_MS; its partner in time makes it
//     the chord, hidden until released; released alone in time, the game gets a press and a release event; held
//     alone too long, the game gets a press event then;
//   - a lost release ends a press once the state shows the button up and an event read returns fewer events
//     than asked;
//   - when a module starts to drive after nobody did, every button held at its first state read is the game's.
// The device's maker and product (GetDeviceInfo, once per device) go into the bus for the client to read.
//
// Ownership of the DirectInput dispatch follows the XInput rules on its own: the live rev 4 client with the
// highest revision drives (PadDiHook::owner), and a rev 4 module hands it on in DllMain. Modules below rev 4
// never touch PadBus::di; their captures hold the DirectInput pad too.
//
// ABI and revisions: mapping Local\FFXiPadHook_v1_<pid>, always PADHOOK_MAP_BYTES, mapped whole.
// Offsets are frozen (test/layout_padhook.txt); new fields only go into the reserved tails, and zero
// means the old behaviour. A module leaves in DllMain(DLL_PROCESS_DETACH) with
// padhook_unregister(bus, slot, false), which hands the dispatch on before its code unmaps.
//
// Revisions meet on one bus. Any revision's dispatcher works through either route's thunk, and calls
// bus->original the same way. A revision 1 or 2 module that joins a route 1 bus registers as before. A
// revision 2 module sets bus->blocked when route 0 refuses. A revision 3 module still tries route 1 then,
// and clears bus->blocked when route 1 succeeds. To revision 3, bus->blocked means only that route 0 is
// refused for good: it calls the bus blocked (PADHOOK_BLOCKED) once route 1 is refused for good as well,
// and until then a host retries.
//
// PADHOOK_FAULT_* and PADHOOK_TEST_* are for tests only. Never define them in a module that ships.

#ifndef PADHOOK_H
#define PADHOOK_H

#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#define PADHOOK_ABI_VERSION 1u             // never changes again
#ifndef PADHOOK_REV
#define PADHOOK_REV 4                      // this file's revision within ABI 1 (tests override it); 2: the chord; 3: route 1; 4: DirectInput
#endif
#define PADHOOK_MAX_CLIENTS 16
#define PADHOOK_INDEXES 4                  // XInput user indexes 0..3
#define PADHOOK_MAP_BYTES 4096u
#define PADHOOK_MAP_NAME_FMT "Local\\FFXiPadHook_v1_%lu"
#define PADHOOK_RETRY_MS 5000              // how often a host retries padhook_join after PADHOOK_JOIN_RETRY
#define PADHOOK_HEARTBEAT_MS 250           // a capture not renewed for this long has lapsed
#define PADHOOK_CHORD_MS 150               // rev 2: a chord button is held back this long waiting for its partner
#define PADHOOK_REPLAY_READS 2             // rev 2: a chord button tapped alone is shown to the game for this many reads
#define PADHOOK_DRAIN_MS 100
#ifndef PADHOOK_DLL
#define PADHOOK_DLL "xinput1_3.dll"        // route 0's DLL (tests point it at a fake)
#endif
#define PADHOOK_GAME "FFXiMain.dll"        // route 1: the client module that holds the wrapper and the slots
#define PADHOOK_SHIM "xinputdll.dll"       // route 1: the client's own XInput shim
#define PADHOOK_SHIM_GET "XInputGetState_" // ... and its GetState, __cdecl
#define PADHOOK_WRAPPER "FFXiMain's XInput wrapper"   // route 1's name in status lines

// PadBus::route (rev 3; 0 on a bus a rev 1 or 2 module patched)
#define PADHOOK_ROUTE_EXPORT 0             // route 0: xinput1_3.dll!XInputGetState, hot-patched
#define PADHOOK_ROUTE_WRAPPER 1            // route 1: FFXiMain's XInput wrapper

// padhook_state()
#define PADHOOK_LIVE 0                     // one route is patched by this bus
#define PADHOOK_WAIT 1                     // not patched yet: no wrapper, and xinput1_3.dll not loaded (yet)
#define PADHOOK_BLOCKED 2                  // no wrapper, and route 0 cannot be patched safely (not hot-patchable, another hook)
#define PADHOOK_NO_BUS 3

#define PADHOOK_JOIN_RETRY (-1)
#define PADHOOK_JOIN_BLOCKED (-2)
#define PADHOOK_JOIN_FULL (-3)
#define PADHOOK_JOIN_NO_BUS (-4)

// PadState::source
#define PADHOOK_SRC_NONE 0
#define PADHOOK_SRC_XINPUT 1
#define PADHOOK_SRC_DINPUT 2               // rev 4: PadBus::di (the DirectInput source keeps its own state)

// rev 4: the DirectInput source
#define PADHOOK_DI_SITES "FFXiMain's DirectInput pad reads"   // the sites' name in status lines
#define PADHOOK_DI_SPAN 0x1000             // the event read follows the state read within this many bytes
#define PADHOOK_DI_BUTTONS 32              // the client maps DirectInput buttons 0..31 (padsin000 values below 32)
#define PADHOOK_DI_MAX_EVENTS 64           // the most events one read may ask for (the client asks for 16)
#define PADHOOK_DI_FRESH_MS 1000           // a pad the client read this recently is the pad the game reads
#define PADHOOK_POV_CENTRED 0xFFFFFFFFul   // DirectInput's centred POV
#define PADHOOK_DI_STATE 0                 // padhook_di_fn's kind: data is the DIJOYSTATE2, asked its size
#define PADHOOK_DI_EVENTS 1                // data is DIDEVICEOBJECTDATA[*count], asked the count asked for
#define PADHOOK_JS_BYTES 0x110             // sizeof(DIJOYSTATE2)
#define PADHOOK_JS_POV 0x20                // its rgdwPOV[0]
#define PADHOOK_JS_BUTTONS 0x30            // its rgbButtons[0]
#define PADHOOK_JS_AXES 8                  // lX lY lZ lRx lRy lRz rglSlider[0] rglSlider[1], LONGs from offset 0

// Buttons are XInput's XINPUT_GAMEPAD_* bits; the triggers take two bits of their own above them
// (a trigger is "held" past PADHOOK_TRIGGER_DOWN), so one mask covers everything that can be pressed.
#define PADHOOK_LT 0x10000ul
#define PADHOOK_RT 0x20000ul
#define PADHOOK_TRIGGER_DOWN 30            // XINPUT_GAMEPAD_TRIGGER_THRESHOLD

static_assert(sizeof(void*) == 4, "PadHook targets 32-bit FFXI");

#if defined(__GNUC__)
#define PADHOOK_ALIGN_STACK __attribute__((force_align_arg_pointer))
#else
#define PADHOOK_ALIGN_STACK
#endif

// XInput's XINPUT_STATE, declared here so no SDK header is needed (16 bytes, the Windows layout)
struct PadXState {
    DWORD packet;
    WORD buttons;
    BYTE lt, rt;
    SHORT lx, ly, rx, ry;
};
static_assert(sizeof(PadXState) == 16, "XINPUT_STATE is 16 bytes");

typedef DWORD(__stdcall* padhook_get_fn)(DWORD index, PadXState* state);

// rev 4: the DirectInput dispatcher. The thunks call it after each read, on the thread that read (the client's main thread).
typedef void(__stdcall* padhook_di_fn)(DWORD kind, LONG hr, void* data, DWORD* count, DWORD asked, void* device);

// DirectInput's DIDEVICEOBJECTDATA (DirectInput 8, 32-bit), declared here so no SDK header is needed
struct PadDiEvent {
    DWORD ofs;                       // DIJOYSTATE2: 0x30 + button for a button
    DWORD data;                      // 0x80 pressed, 0 released
    DWORD time;
    DWORD seq;
    DWORD app;
};
static_assert(sizeof(PadDiEvent) == 20, "DIDEVICEOBJECTDATA is 20 bytes");

// 32 bytes: what the game read last, raw (before any mask), per user index.
struct PadState {
    unsigned long seq;               // changes with every change of the fields below; 0 = never read
    unsigned long buttons;           // XINPUT_GAMEPAD_* | PADHOOK_LT | PADHOOK_RT
    unsigned char lt, rt;
    unsigned char connected;         // the last read succeeded
    unsigned char source;            // PADHOOK_SRC_*
    short lx, ly, rx, ry;
    unsigned long tick;              // GetTickCount() of the last read
    unsigned long reads;             // reads of this index
    unsigned char reserved[4];
};

// 48 bytes: the mask per user index (the dispatcher's own working state).
struct PadMask {
    unsigned long masked;            // hidden from the game now
    unsigned long passthru;          // held when the pad was taken: reaches the game until released
    volatile LONG was_captured;
    unsigned long out_packet;        // the dwPacketNumber the game sees
    PadXState last_out;              // what the game was last given
    unsigned long hidden;            // presses hidden from the game, ever (status)
    unsigned char reserved[12];
};

// 64 bytes; offsets frozen.
struct PadClient {
    volatile LONG occupied;          // 0 free, 1 being filled in, 2 live
    volatile LONG capture;           // 1: this client holds the pad (while its heartbeat is fresh)
    volatile LONG alive_tick;        // GetTickCount() of the client's last renewal
    padhook_get_fn dispatch;         // this client's module's dispatcher
    unsigned long rev;
    HMODULE module;
    volatile LONG const* nonce_at;
    LONG nonce;
    char name[16];
    unsigned long chord;             // rev 2: the buttons that take this client's pad together (0: none)
    unsigned long di_chord;          // rev 4: the same chord as DirectInput buttons (bit i: button i; 0: none)
    padhook_di_fn di_dispatch;       // rev 4: this client's module's DirectInput dispatcher (null below rev 4)
    unsigned char reserved[4];
};

// 24 bytes (rev 2, in the bus header's reserved tail): the chord step's working state.
struct PadChord {
    unsigned long buttons;           // the union of the live clients' chords, refreshed every read
    unsigned long deferred;          // chord buttons held back from the game, waiting for a partner
    unsigned long since;             // GetTickCount() when the first of them went down
    unsigned long replay;            // a tap released alone: shown to the game for `replay_left` more reads
    unsigned long replay_left;
    unsigned long prev;              // the chord buttons held at the last read
};
static_assert(sizeof(PadChord) == 24, "PadChord is 24 bytes forever");

// rev 4, 128 bytes: what the game read last from its DirectInput pad, raw (before the mask)
struct PadDiState {
    unsigned long seq;               // changes with every change of the fields below but tick and reads; 0 = never read
    unsigned long buttons;           // bit i: button i held (0..31), from the state read
    unsigned long pov;               // POV 0, hundredths of a degree clockwise from up (PADHOOK_POV_CENTRED: centred)
    long axes[PADHOOK_JS_AXES];      // lX lY lZ lRx lRy lRz and the two sliders (the client ranges them -127..127)
    unsigned long tick;              // GetTickCount() of the last state read
    unsigned long reads;             // state reads
    long hr;                         // the last state read's result
    unsigned long vidpid;            // the device's maker and product, MAKELONG(vendor, product) (0: not known)
    void* device;                    // the device the client read last
    char name[32];                   // its product name (cut short)
    unsigned char reserved[32];
};

// rev 4, 128 bytes: the DirectInput dispatcher's own working state, one bit per button (0..31)
struct PadDiPress {
    unsigned long chord;             // the union of the live clients' DirectInput chords, refreshed every read
    unsigned long game;              // the game saw the current press
    unsigned long hid;               // the current press is hidden from the game until its release
    unsigned long wait;              // chord buttons held back from the game, waiting for a partner
    unsigned long wait_since;        // GetTickCount() when the first of them went down
    unsigned long tap;               // waited buttons released alone in time: owed to the game as a press and a release
    unsigned long late;              // waited buttons held alone too long: the game's, owed a press event
    unsigned long swallow;           // their own release event is still to come, and is dropped
    unsigned long down;              // held at the last state read (raw)
    unsigned long pov_out;           // the POV the game was last given
    volatile LONG state_ok;          // the last state read succeeded (down is current)
    volatile LONG primed;            // 0: the next state read gives every held button to the game (a driver started)
    volatile LONG was_captured;
    unsigned char pov_pass;          // the POV was held, and the game had it, when the pad was taken: the game's until centred
    unsigned char pov_hid;           // the POV went off centre while the pad was held: centred until it centres
    unsigned char reserved_b[2];
    unsigned long hidden;            // presses hidden from the game, ever (status)
    unsigned long dropped;           // events dropped, ever
    unsigned long injected;          // presses given back to the game as events (chord taps, late chord buttons), ever
    unsigned long ended;             // hidden presses ended without their release event, ever
    unsigned long chords;            // chords kept from the game, ever
    unsigned long event_reads;
    unsigned char reserved[48];
};

// rev 4, 640 bytes: the DirectInput source, its own patch, dispatch, state and status
struct PadDiHook {
    volatile LONG installed;         // both sites are patched
    volatile LONG install_lock;
    volatile LONG blocked;           // the sites cannot be patched safely in this process
    unsigned char* block;            // never freed
    padhook_di_fn volatile* dispatch_ptr;
    std::uintptr_t site_state;       // the cmp after GetDeviceState
    std::uintptr_t site_events;      // the client's store on the event read's success branch
    unsigned long device_disp;       // where the input object holds the pad device (from the sites)
    unsigned long buffer_disp;       // ... and the state buffer
    volatile LONG owner;             // the driving client's slot + 1 (0: nobody drives)
    volatile LONG owner_rev;
    volatile LONG calls;             // reads through the dispatcher
    volatile LONG in_flight;
    volatile LONG write_lock;        // the dispatcher's writes (one thread at a time; others only hide)
    volatile LONG wseq;              // seqlock over `pad`: odd while written
    volatile LONG handovers;
    volatile LONG installs;
    unsigned char reserved_hdr[60];
    PadDiState pad;                  // at 128
    PadDiPress press;                // at 256
    char status[192];
    unsigned char reserved[64];
};

// 4096 bytes mapped; offsets frozen.
struct PadBus {
    volatile LONG ready;
    unsigned long abi_version;       // 1
    unsigned long size;
    volatile LONG install_lock;
    volatile LONG hook_installed;
    volatile LONG owner_slot;
    volatile LONG in_flight;
    volatile LONG installs;
    volatile LONG calls;             // game reads through the dispatcher
    unsigned char* block;            // never freed
    padhook_get_fn volatile* dispatch_ptr;
    std::uintptr_t site;             // route 0: XInputGetState; route 1: the wrapper
    padhook_get_fn original;         // route 0: site + 2; route 1: the adapter (block + 64)
    volatile LONG owner_rev;
    volatile LONG reg_lock;
    volatile LONG handovers;
    volatile LONG blocked;
    volatile LONG serial;
    volatile LONG write_lock;        // the dispatcher's state writes (one thread at a time; others skip)
    volatile LONG wseq;              // seqlock over `pads`: odd while written
    volatile LONG last_index;        // the user index that last read connected
    volatile LONG lapses;            // captures that ended by a missed heartbeat
    PadChord chord;                  // rev 2 (zero: no chord, the rev 1 behaviour)
    volatile LONG route;             // rev 3: PADHOOK_ROUTE_* (zero: route 0, the rev 2 behaviour)
    unsigned char reserved_hdr[12];
    PadState pads[PADHOOK_INDEXES];  // 128
    PadMask masks[PADHOOK_INDEXES];  // 192
    PadClient clients[PADHOOK_MAX_CLIENTS];   // 1024
    char status[192];
    PadDiHook di;                    // rev 4 (zero: no DirectInput source, the rev 3 behaviour)
    // append here only
};

static_assert(sizeof(PadState) == 32, "PadState is 32 bytes forever");
static_assert(sizeof(PadDiState) == 128, "PadDiState is 128 bytes forever");
static_assert(sizeof(PadDiPress) == 128, "PadDiPress is 128 bytes forever");
static_assert(sizeof(PadDiHook) == 640, "PadDiHook is 640 bytes forever");
static_assert(offsetof(PadBus, di) == 1664, "PadBus::di follows status");
static_assert(sizeof(PadMask) == 48, "PadMask is 48 bytes forever");
static_assert(sizeof(PadClient) == 64, "PadClient is 64 bytes forever");
static_assert(offsetof(PadBus, pads) == 128, "PadBus header is 128 bytes");
static_assert(sizeof(PadBus) <= PADHOOK_MAP_BYTES, "PadBus must fit the fixed mapping");

namespace padhook_detail {

inline PadBus*& cached_bus() { static PadBus* value = nullptr; return value; }
inline volatile LONG& module_nonce() { static volatile LONG value = 0; return value; }
inline volatile DWORD& last_dispatch_tid() { static volatile DWORD value = 0; return value; }

constexpr std::size_t kBlockBytes = 128;                  // rev 3: 64 before, with no adapter
constexpr std::size_t kThunkAt = 16;                      // either route's thunk
constexpr std::size_t kThunkBytes = 5 + 2 + 2 + 2 + 5;    // 16: route 0's thunk
constexpr std::size_t kWrapThunkBytes = 37;               // route 1's thunk
constexpr std::size_t kAdapterAt = 64;                    // route 1's adapter (bus->original)
constexpr std::size_t kAdapterBytes = 33;
constexpr std::size_t kWrapSigBytes = 44;                 // through the wrapper's own fail path
constexpr std::size_t kWrapPatchAt = 8;                   // wrapper + 8 .. + 15, written at once
constexpr DWORD kWrapperFail = 1;                         // what the wrapper returns without H or F
inline constexpr char kWrapChanged[] = "changed while it was patched";   // route 1 refused for good

inline bool page_ok(DWORD protect, bool exec) {
    if (protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    switch (protect & 0xFF) {
    case PAGE_EXECUTE: return exec;
    case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY: return true;
    case PAGE_READONLY: case PAGE_READWRITE: case PAGE_WRITECOPY: return !exec;
    default: return false;
    }
}

inline bool span_readable(std::uintptr_t address, std::size_t size) {
    if (address == 0 || size == 0) return false;
    MEMORY_BASIC_INFORMATION region {};
    if (!VirtualQuery(reinterpret_cast<void const*>(address), &region, sizeof(region))) return false;
    if (region.State != MEM_COMMIT || !(page_ok(region.Protect, false) || page_ok(region.Protect, true))) return false;
    std::uintptr_t const low = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
    return address >= low && address + size <= low + region.RegionSize;
}

inline bool code_in_module(void const* code, HMODULE module) {
    if (!code) return false;
    MEMORY_BASIC_INFORMATION region {};
    if (!VirtualQuery(code, &region, sizeof(region))) return false;
    if (region.State != MEM_COMMIT || region.Type != MEM_IMAGE || !page_ok(region.Protect, true)) return false;
    return !module || region.AllocationBase == reinterpret_cast<void*>(module);
}

inline bool module_mapped(HMODULE module) {
    MEMORY_BASIC_INFORMATION region {};
    if (!module || !VirtualQuery(module, &region, sizeof(region))) return false;
    return region.State == MEM_COMMIT && region.Type == MEM_IMAGE && region.AllocationBase == reinterpret_cast<void*>(module);
}

inline bool module_alive(PadClient const& c) {
    if (!c.module || !module_mapped(c.module)) return false;
    MEMORY_BASIC_INFORMATION region {};
    if (!c.nonce_at || !VirtualQuery(const_cast<LONG const*>(c.nonce_at), &region, sizeof(region))) return false;
    if (region.State != MEM_COMMIT || region.Type != MEM_IMAGE || region.AllocationBase != reinterpret_cast<void*>(c.module))
        return false;
    return *c.nonce_at == c.nonce;
}

inline bool can_own(PadClient const& c) {
    return c.occupied == 2 && c.dispatch && module_alive(c) && code_in_module(reinterpret_cast<void const*>(c.dispatch), c.module);
}

inline bool reg_lock(PadBus* bus, bool wait) {
    LONG const me = static_cast<LONG>(GetCurrentThreadId());
    DWORD const start = GetTickCount();
    for (;;) {
        if (InterlockedCompareExchange(&bus->reg_lock, me, 0) == 0) return true;
        if (!wait) return false;
        if (GetTickCount() - start > 50) {
            LONG const held = InterlockedCompareExchange(&bus->reg_lock, 0, 0);
            if (held != 0 && InterlockedCompareExchange(&bus->reg_lock, me, held) == held) return true;
        }
        Sleep(0);
    }
}
inline void reg_unlock(PadBus* bus) { InterlockedExchange(&bus->reg_lock, 0); }

inline void clear_client(PadClient& c) {
    InterlockedExchange(&c.capture, 0);
    c.chord = 0;
    c.di_chord = 0;
    c.di_dispatch = nullptr;
    c.dispatch = nullptr;
    c.rev = 0;
    c.module = nullptr;
    c.nonce_at = nullptr;
    c.nonce = 0;
    std::memset(c.name, 0, sizeof(c.name));
    MemoryBarrier();
    InterlockedExchange(&c.occupied, 0);
}

inline void set_owner(PadBus* bus, int slot) {
    padhook_get_fn const next = slot >= 0 ? bus->clients[slot].dispatch : nullptr;
    if (*bus->dispatch_ptr != next) InterlockedIncrement(&bus->handovers);
    *bus->dispatch_ptr = next;
    InterlockedExchange(&bus->owner_rev, slot >= 0 ? static_cast<LONG>(bus->clients[slot].rev) : 0);
    InterlockedExchange(&bus->owner_slot, slot);
}

inline void reassign_owner(PadBus* bus, int leaving) {
    if (!bus || !bus->dispatch_ptr) return;
    int best = -1;
    for (int i = 0; i < PADHOOK_MAX_CLIENTS; ++i) {
        if (i == leaving || !can_own(bus->clients[i])) continue;
        if (best < 0 || bus->clients[i].rev > bus->clients[best].rev) best = i;
    }
    set_owner(bus, best);
}

// Does any live client hold the pad now? (a capture whose heartbeat is older than PADHOOK_HEARTBEAT_MS
// has lapsed; it is dropped here, once, and counted)
inline bool captured(PadBus* bus, DWORD now, unsigned long* chords = nullptr) {
    bool any = false;
    unsigned long all = 0;
    for (int i = 0; i < PADHOOK_MAX_CLIENTS; ++i) {
        PadClient& c = bus->clients[i];
        if (c.occupied == 2) all |= c.chord;
        if (c.occupied != 2 || !c.capture) continue;
#ifndef PADHOOK_FAULT_NO_HEARTBEAT
        if (now - static_cast<DWORD>(c.alive_tick) > PADHOOK_HEARTBEAT_MS) {
            if (InterlockedCompareExchange(&c.capture, 0, 1) == 1) InterlockedIncrement(&bus->lapses);
            continue;
        }
#endif
        any = true;
    }
    if (chords) *chords = all;
    return any;
}

inline unsigned long held_of(PadXState const& s) {
    unsigned long h = s.buttons;
    if (s.lt > PADHOOK_TRIGGER_DOWN) h |= PADHOOK_LT;
    if (s.rt > PADHOOK_TRIGGER_DOWN) h |= PADHOOK_RT;
    return h;
}

// The mask rules (see the top of the file), applied to the state the original returned. Pure on its
// inputs: tests call it directly.
inline void apply_mask(PadMask& m, PadXState& s, bool cap, unsigned long hide = 0, unsigned long force = 0) {
    unsigned long const held = held_of(s);
#ifndef PADHOOK_FAULT_TAKE_HELD
    // held from before: what the game has already seen go down (its last state) stays the game's until
    // released; a press that arrives in the same read as the capture is the addon's
    if (cap && !m.was_captured) m.passthru = held_of(m.last_out) & held & ~m.masked;
#else
    if (cap && !m.was_captured) m.passthru = 0;
#endif
    m.passthru &= held;
#ifndef PADHOOK_FAULT_LEAK_HELD
    unsigned long const kept = m.masked & held;                       // hidden presses stay hidden until released
#else
    unsigned long const kept = cap ? (m.masked & held) : 0;
#endif
    unsigned long const now_masked = cap ? ((held & ~m.passthru) | kept) : kept;
    m.hidden += static_cast<unsigned long>(__builtin_popcountl(now_masked & ~m.masked));
    m.masked = now_masked;
    m.was_captured = cap ? 1 : 0;
    PadXState out = s;
    out.buttons = static_cast<WORD>(out.buttons & ~(m.masked & 0xFFFFul));
    if (m.masked & PADHOOK_LT) out.lt = 0;
    if (m.masked & PADHOOK_RT) out.rt = 0;
    if (cap) {
        if (!(m.passthru & PADHOOK_LT)) out.lt = 0;
        if (!(m.passthru & PADHOOK_RT)) out.rt = 0;
        out.lx = out.ly = out.rx = out.ry = 0;
    }
    out.buttons = static_cast<WORD>((out.buttons & ~(hide & 0xFFFFul)) | (force & 0xFFFFul));   // the chord step
    out.packet = 0;
    PadXState last = m.last_out;
    last.packet = 0;
#ifndef PADHOOK_FAULT_NO_RENUMBER
    if (std::memcmp(&out, &last, sizeof(out)) != 0 || m.out_packet == 0) ++m.out_packet;
    out.packet = m.out_packet;
#else
    out.packet = s.packet;
#endif
    m.last_out = out;
    s = out;
}

// The chord step (rev 2), before the mask: decides from the raw buttons what the game must not see yet
// (*hide), what it must see although released (*force), and makes a completed chord a hidden press in m
// (so it stays hidden until released, by the mask's own rule). Pure on its inputs: tests call it directly.
inline void apply_chord(PadChord& c, PadMask& m, unsigned long held, bool cap, DWORD now,
                        unsigned long* hide, unsigned long* force) {
    *hide = 0;
    *force = 0;
    unsigned long const C = c.buttons;
    unsigned long const on = held & C;
#ifndef PADHOOK_FAULT_NO_CHORD
    if (C == 0 || cap) {                             // nobody asks, or the pad is held (everything new is hidden then)
        c.deferred = 0;
        c.replay = c.replay_left = 0;
        c.prev = on;
        return;
    }
    if (c.replay_left > 0) {
        *force = c.replay & ~held;
        if (--c.replay_left == 0) c.replay = 0;
    }
    unsigned long const fresh = on & ~c.prev;
    // the chord: every button of it either new in this read or still held back (never one the game already has)
    if ((on & C) == C && ((fresh | c.deferred) & C) == C) {
        m.masked |= C;                               // hidden until each is released
        c.deferred = 0;
    } else if (c.deferred != 0) {
        if ((on & c.deferred) == 0) {                // released alone in time: a tap the game still gets
            c.replay = c.deferred;
            c.replay_left = PADHOOK_REPLAY_READS;
            *force |= c.replay & ~held;
            if (--c.replay_left == 0) c.replay = 0;
            c.deferred = 0;
        } else if (now - c.since > PADHOOK_CHORD_MS) {
            c.deferred = 0;                          // held alone too long: the game's now
        }
    } else if (fresh != 0 && (m.masked & fresh) == 0 && (on & ~fresh) == 0) {
        c.deferred = fresh;                          // the first chord button down: wait for its partner
        c.since = now;
    }
    *hide = c.deferred;
#else
    (void)cap; (void)now; (void)m;
#endif
    c.prev = on;
}

inline void record(PadBus* bus, DWORD index, DWORD result, PadXState const* s) {
    PadState& p = bus->pads[index];
    unsigned char const conn = result == ERROR_SUCCESS ? 1 : 0;
    unsigned long const buttons = conn ? held_of(*s) : 0;
    bool const same = p.connected == conn && (!conn || (p.buttons == buttons && p.lt == s->lt && p.rt == s->rt
        && p.lx == s->lx && p.ly == s->ly && p.rx == s->rx && p.ry == s->ry));
    InterlockedIncrement(&bus->wseq);                 // odd: being written
    MemoryBarrier();
    p.tick = GetTickCount();
    ++p.reads;
    if (!same || p.seq == 0) {
        p.connected = conn;
        p.source = PADHOOK_SRC_XINPUT;
        p.buttons = buttons;
        p.lt = conn ? s->lt : 0;
        p.rt = conn ? s->rt : 0;
        p.lx = conn ? s->lx : 0;
        p.ly = conn ? s->ly : 0;
        p.rx = conn ? s->rx : 0;
        p.ry = conn ? s->ry : 0;
        ++p.seq;
        if (p.seq == 0) p.seq = 1;
    }
    MemoryBarrier();
    InterlockedIncrement(&bus->wseq);                 // even again
    if (conn) InterlockedExchange(&bus->last_index, static_cast<LONG>(index));
}

// The dispatcher: one copy per module, entered from the thunk on whichever game thread reads the pad.
inline DWORD PADHOOK_ALIGN_STACK __stdcall dispatch(DWORD index, PadXState* state) {
    PadBus* bus = cached_bus();
    padhook_get_fn const original = bus ? bus->original : nullptr;
    if (!original) return ERROR_DEVICE_NOT_CONNECTED;     // cannot happen: the bus is set before the patch
    InterlockedIncrement(&bus->in_flight);
    InterlockedIncrement(&bus->calls);
    last_dispatch_tid() = GetCurrentThreadId();
    DWORD const r = original(index, state);
    if (index < PADHOOK_INDEXES && state && InterlockedCompareExchange(&bus->write_lock, 1, 0) == 0) {
        record(bus, index, r, state);
        if (r == ERROR_SUCCESS) {
            DWORD const now = GetTickCount();
            unsigned long chords = 0, hide = 0, force = 0;
            bool const cap = captured(bus, now, &chords);
            PadMask& m = bus->masks[index];
            // the chord follows one pad: the user index that last read connected
            if (static_cast<LONG>(index) == bus->last_index) {
                bus->chord.buttons = chords;
                apply_chord(bus->chord, m, held_of(*state), cap, now, &hide, &force);
            }
            apply_mask(m, *state, cap, hide, force);
        }
        InterlockedExchange(&bus->write_lock, 0);
    } else if (r == ERROR_SUCCESS && index < PADHOOK_INDEXES && state) {
        // another thread is writing (two readers at once): hide what is hidden, change nothing else
        PadMask const& m = bus->masks[index];
        state->buttons = static_cast<WORD>(state->buttons & ~(m.masked & 0xFFFFul));
        if (m.was_captured) { state->lx = state->ly = state->rx = state->ry = 0; }
    }
    InterlockedDecrement(&bus->in_flight);
    return r;
}

inline HMODULE self_module() {
    HMODULE m = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&dispatch), &m);
    return m;
}

inline void thunk_bytes(std::uintptr_t block, std::uintptr_t site, unsigned char* t) {
    auto const slot = static_cast<std::uint32_t>(block);
    t[0] = 0xA1;
    std::memcpy(t + 1, &slot, 4);
    unsigned char const body[] = {0x85, 0xC0, 0x74, 0x02, 0xFF, 0xE0, 0xE9};
    std::memcpy(t + 5, body, sizeof(body));
    std::uintptr_t const jmp_end = block + kThunkAt + kThunkBytes;
    std::int32_t const rel = static_cast<std::int32_t>((site + 2) - jmp_end);
    std::memcpy(t + 12, &rel, 4);
}

inline void hexbytes(char* out, std::size_t n, unsigned char const* p, int count) {
    std::size_t len = 0;
    out[0] = 0;
    for (int i = 0; i < count && len + 4 < n; ++i) len += static_cast<std::size_t>(std::snprintf(out + len, n - len, "%s%02X", i ? " " : "", p[i]));
}

// ---- route 1: the client's XInput wrapper (rev 3) ----

// The wrapper's first 44 bytes, through its fail path (mov eax, 1 / ret 8), so the value the thunk and the
// adapter return without F (kWrapperFail) is the client's own. Bytes 1..4 (H) and 10..13 (F) are the slot
// addresses, which differ by build.
constexpr unsigned char kWrapSig[kWrapSigBytes] = {
    0xA1, 0x00, 0x00, 0x00, 0x00, 0x85, 0xC0, 0x74, 0x1B, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x85, 0xC0, 0x74, 0x12,
    0x8B, 0x4C, 0x24, 0x08, 0x8B, 0x54, 0x24, 0x04, 0x51, 0x52, 0xFF, 0xD0, 0x83, 0xC4, 0x08, 0xC2, 0x08, 0x00,
    0xB8, 0x01, 0x00, 0x00, 0x00, 0xC2, 0x08, 0x00,
};

inline std::uint32_t le32(unsigned char const* p) {
    std::uint32_t v = 0;
    std::memcpy(&v, p, 4);
    return v;
}

inline bool wrap_sig_at(unsigned char const* p) {
    for (std::size_t i = 0; i < kWrapSigBytes; ++i) {
        if ((i >= 1 && i <= 4) || (i >= 10 && i <= 13)) continue;
        if (p[i] != kWrapSig[i]) return false;
    }
    return true;
}

struct WrapperHit {
    std::uintptr_t site;             // the wrapper
    std::uintptr_t handle_slot;      // H: the shim's handle
    std::uintptr_t fn_slot;          // F: its XInputGetState_ (H + 4)
};

// The GetState wrappers in n bytes at p, which the process sees at `base`: the signature's matches where
// F = H + 4 (the SetState wrapper's match has F = H + 8). *first = the first one. Pure: tests call it.
inline int scan_wrappers(unsigned char const* p, std::size_t n, std::uintptr_t base, WrapperHit* first) {
    int count = 0;
    for (std::size_t off = 0; off + kWrapSigBytes <= n; ++off) {
        if (p[off] != 0xA1 || !wrap_sig_at(p + off)) continue;
        std::uint32_t const h = le32(p + off + 1), f = le32(p + off + 10);
#ifndef PADHOOK_FAULT_FIRST_MATCH
        if (f != h + 4) continue;
#endif
        if (count == 0 && first) *first = WrapperHit {base + off, h, f};
        ++count;
#ifdef PADHOOK_FAULT_FIRST_MATCH
        break;
#endif
    }
    return count;
}

// What the scan's result means: null when it can be patched, otherwise why not. Pure: tests call it.
inline char const* scan_verdict(int count, WrapperHit const& hit) {
    if (count == 0) return "was not found (a client update?)";
    if (count > 1) return "is not unique";
    if ((hit.site + kWrapPatchAt) % 8 != 0) return "is not on an 8-byte boundary at +8";
    return nullptr;
}

// Route 1's thunk (kWrapThunkBytes, at block + kThunkAt).
inline void wrap_thunk_bytes(std::uintptr_t block, std::uintptr_t fn_slot, unsigned char* t) {
    auto const d = static_cast<std::uint32_t>(block);
    auto const f = static_cast<std::uint32_t>(fn_slot);
#ifndef PADHOOK_FAULT_OLD_THUNK
    unsigned char const code[kWrapThunkBytes] = {
        0xA1, 0, 0, 0, 0,                // mov eax, [dispatch]
        0x85, 0xC0, 0x74, 0x0B,          // test eax, eax / jz pass
        0xFF, 0x74, 0x24, 0x08,          // push [esp+8]
        0xFF, 0x74, 0x24, 0x08,          // push [esp+8]
        0xFF, 0xD0, 0xC3,                // call eax / ret
        0xA1, 0, 0, 0, 0,                // pass: mov eax, [F]
        0x85, 0xC0, 0x74, 0x02,          // test eax, eax / jz none
        0xFF, 0xE0,                      // jmp eax
        0xB8, 0x01, 0x00, 0x00, 0x00,    // none: mov eax, 1
        0xC3,                            // ret
    };
#else
    // route 0's thunk shape on this route: the __stdcall dispatcher is reached by a jump (the crash)
    unsigned char const code[kWrapThunkBytes] = {
        0xA1, 0, 0, 0, 0, 0x85, 0xC0, 0x74, 0x0B, 0xFF, 0xE0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
        0xA1, 0, 0, 0, 0, 0x85, 0xC0, 0x74, 0x02, 0xFF, 0xE0, 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3,
    };
#endif
    std::memcpy(t, code, kWrapThunkBytes);
    std::memcpy(t + 1, &d, 4);
    std::memcpy(t + 21, &f, 4);
}

// Route 1's adapter (kAdapterBytes, at block + kAdapterAt): the client's __cdecl F behind a __stdcall entry.
inline void wrap_adapter_bytes(std::uintptr_t fn_slot, unsigned char* t) {
    auto const f = static_cast<std::uint32_t>(fn_slot);
    unsigned char const code[kAdapterBytes] = {
        0xA1, 0, 0, 0, 0,                // mov eax, [F]
        0x85, 0xC0, 0x74, 0x10,          // test eax, eax / jz none
        0xFF, 0x74, 0x24, 0x08,          // push [esp+8]
        0xFF, 0x74, 0x24, 0x08,          // push [esp+8]
        0xFF, 0xD0,                      // call eax
        0x83, 0xC4, 0x08,                // add esp, 8
        0xC2, 0x08, 0x00,                // ret 8
        0xB8, 0x01, 0x00, 0x00, 0x00,    // none: mov eax, 1
        0xC2, 0x08, 0x00,                // ret 8
    };
    std::memcpy(t, code, kAdapterBytes);
    std::memcpy(t + 1, &f, 4);
}

// The 8 bytes at wrapper + 8 before the patch, and after it.
inline void wrap_patch_bytes(WrapperHit const& hit, std::uintptr_t thunk, unsigned char* expect, unsigned char* want) {
    auto const f = static_cast<std::uint32_t>(hit.fn_slot);
    auto const t = static_cast<std::uint32_t>(thunk);
    unsigned char const before[8] = {0x1B, 0xA1, 0, 0, 0, 0, 0x85, 0xC0};     // jz's offset / mov eax, [F] / test
    std::memcpy(expect, before, 8);
    std::memcpy(expect + 2, &f, 4);
    std::memcpy(want, before, 8);
    want[1] = 0xB8;                                                          // mov eax, thunk
    std::memcpy(want + 2, &t, 4);
}

// Patch a wrapper that the checks have passed: the block, then one locked 8-byte compare-and-exchange (as
// UiHook's write_site). Null on success, with bus->block, dispatch_ptr, site, original and route set.
inline char const* install_wrapper(PadBus* bus, WrapperHit const& hit) {
    auto* block = static_cast<unsigned char*>(VirtualAlloc(nullptr, kBlockBytes, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!block) return "could not be patched (no memory for the hook block)";
    std::memset(block, 0xCC, kBlockBytes);
    std::memset(block, 0, 4);
    auto const b = reinterpret_cast<std::uintptr_t>(block);
    wrap_thunk_bytes(b, hit.fn_slot, block + kThunkAt);
    wrap_adapter_bytes(hit.fn_slot, block + kAdapterAt);
    FlushInstructionCache(GetCurrentProcess(), block, kBlockBytes);
    bus->block = block;
    bus->dispatch_ptr = reinterpret_cast<padhook_get_fn volatile*>(block);
    bus->site = hit.site;
    bus->original = reinterpret_cast<padhook_get_fn>(block + kAdapterAt);
    InterlockedExchange(&bus->route, PADHOOK_ROUTE_WRAPPER);
    MemoryBarrier();
    auto undo = [&](char const* why) {
        bus->block = nullptr; bus->dispatch_ptr = nullptr; bus->site = 0; bus->original = nullptr;
        InterlockedExchange(&bus->route, PADHOOK_ROUTE_EXPORT);
        VirtualFree(block, 0, MEM_RELEASE);
        return why;
    };
    std::uintptr_t const at = hit.site + kWrapPatchAt;
    DWORD previous = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), 8, PAGE_EXECUTE_READWRITE, &previous))
        return undo("could not be patched (VirtualProtect failed)");
    unsigned char expect[8], want[8];
    wrap_patch_bytes(hit, b + kThunkAt, expect, want);
    LONGLONG old_v = 0, new_v = 0;
    std::memcpy(&old_v, expect, 8);
    std::memcpy(&new_v, want, 8);
    bool const wrote = InterlockedCompareExchange64(reinterpret_cast<LONGLONG volatile*>(at), new_v, old_v) == old_v;
    VirtualProtect(reinterpret_cast<void*>(at), 8, previous, &previous);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), 8);
    if (!wrote) return undo(kWrapChanged);
    return nullptr;
}

// The wrapper's handle slot on a route 1 bus (from the patched bytes, which keep H), or 0. Checked once per
// module: the bytes must be the patched wrapper, pointing at this bus's thunk.
inline std::uintptr_t handle_slot(PadBus const* bus) {
    static std::uintptr_t cached = 0;
    if (cached) return cached;
    if (!bus || bus->route != PADHOOK_ROUTE_WRAPPER || !bus->hook_installed || !bus->block) return 0;
    std::uintptr_t const site = bus->site;
    if (!span_readable(site, kWrapSigBytes)) return 0;
    auto const* p = reinterpret_cast<unsigned char const*>(site);
    if (p[0] != 0xA1 || p[9] != 0xB8 || le32(p + 10) != static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(bus->block) + kThunkAt))
        return 0;
    std::uintptr_t const slot = le32(p + 1);
    if (!span_readable(slot, 8)) return 0;
    cached = slot;
    return cached;
}

inline std::uintptr_t read_slot(std::uintptr_t slot) { return *reinterpret_cast<std::uintptr_t const volatile*>(slot); }

inline volatile LONG& shim_pinned() { static volatile LONG value = 0; return value; }   // one per module

// fn(section) for every section of the loaded client module, or false when it is not loaded or its
// headers are unreadable.
template <typename Fn>
inline bool for_each_game_section(HMODULE* out, Fn&& fn) {
    HMODULE module = GetModuleHandleA(PADHOOK_GAME);
    if (out) *out = module;
    if (!module) return false;
    auto const base = reinterpret_cast<std::uintptr_t>(module);
    if (!span_readable(base, sizeof(IMAGE_DOS_HEADER))) return false;
    auto const* dos = reinterpret_cast<IMAGE_DOS_HEADER const*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    std::uintptr_t const nt_at = base + static_cast<std::uintptr_t>(dos->e_lfanew);
    if (!span_readable(nt_at, sizeof(IMAGE_NT_HEADERS32))) return false;
    auto const* nt = reinterpret_cast<IMAGE_NT_HEADERS32 const*>(nt_at);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    auto const* section = IMAGE_FIRST_SECTION(nt);
    if (!span_readable(reinterpret_cast<std::uintptr_t>(section), sizeof(IMAGE_SECTION_HEADER) * nt->FileHeader.NumberOfSections))
        return false;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
        fn(base + section->VirtualAddress, base + section->VirtualAddress + section->Misc.VirtualSize, section->Characteristics);
    return true;
}

// fn(bytes, n, address) for every run of committed readable pages in [begin, end) (a VirtualProtect inside
// the module splits regions, so a section is never tested whole; as UiHook's scan_each)
template <typename Fn>
inline void for_each_readable_run(std::uintptr_t begin, std::uintptr_t end, Fn&& fn) {
    std::uintptr_t address = begin;
    while (address < end) {
        MEMORY_BASIC_INFORMATION region {};
        if (!VirtualQuery(reinterpret_cast<void const*>(address), &region, sizeof(region))) break;
        std::uintptr_t const region_end = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
        if (region_end <= address) break;
        bool const readable = region.State == MEM_COMMIT && (page_ok(region.Protect, false) || page_ok(region.Protect, true));
        if (!readable) { address = region_end; continue; }
        std::uintptr_t run_end = region_end;
        while (run_end < end) {
            MEMORY_BASIC_INFORMATION next {};
            if (!VirtualQuery(reinterpret_cast<void const*>(run_end), &next, sizeof(next)) || next.State != MEM_COMMIT
                || !(page_ok(next.Protect, false) || page_ok(next.Protect, true))) break;
            std::uintptr_t const next_end = reinterpret_cast<std::uintptr_t>(next.BaseAddress) + next.RegionSize;
            if (next_end <= run_end) break;
            run_end = next_end;
        }
        std::uintptr_t const limit = run_end < end ? run_end : end;
        fn(reinterpret_cast<unsigned char const*>(address), static_cast<std::size_t>(limit - address), address);
        address = run_end;
    }
}

inline bool page_writable(DWORD protect) {
    if (protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    switch (protect & 0xFF) {
    case PAGE_READWRITE: case PAGE_WRITECOPY: case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY: return true;
    default: return false;
    }
}

// Are both slots (H and F, 8 bytes) inside one writable section of the client module, and writable now?
inline bool slots_writable(WrapperHit const& hit) {
    bool ok = false;
    for_each_game_section(nullptr, [&](std::uintptr_t begin, std::uintptr_t end, DWORD flags) {
        if (hit.handle_slot >= begin && hit.handle_slot + 8 <= end && (flags & IMAGE_SCN_MEM_WRITE)) ok = true;
    });
    if (!ok) return false;
    MEMORY_BASIC_INFORMATION region {};
    if (!VirtualQuery(reinterpret_cast<void const*>(hit.handle_slot), &region, sizeof(region))) return false;
    if (region.State != MEM_COMMIT || region.Type != MEM_IMAGE || !page_writable(region.Protect)) return false;
    std::uintptr_t const low = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
    return hit.handle_slot >= low && hit.handle_slot + 8 <= low + region.RegionSize;
}

// The scan over the client's code sections, done once per module (the client's code never changes).
struct WrapperScan {
    int state;                       // 0 not yet (the client module is not loaded), 1 found, 2 refused for good
    WrapperHit hit;
    char const* why;
    int mismatches;                  // joins that found the slots holding other values (refused for good at 3)
};
inline WrapperScan& wrapper_scan() { static WrapperScan value = {0, {0, 0, 0}, nullptr, 0}; return value; }

// Can route 1 still go live, as far as this module knows? False once it is refused for good.
inline bool route1_open() { return wrapper_scan().state != 2; }

inline WrapperScan& find_wrapper() {
    WrapperScan& s = wrapper_scan();
    if (s.state != 0) return s;
    int count = 0;
    WrapperHit first {0, 0, 0};
    HMODULE game = nullptr;
    bool const headers = for_each_game_section(&game, [&](std::uintptr_t begin, std::uintptr_t end, DWORD flags) {
        if (!(flags & IMAGE_SCN_MEM_EXECUTE)) return;
        for_each_readable_run(begin, end, [&](unsigned char const* p, std::size_t n, std::uintptr_t at) {
            WrapperHit h {0, 0, 0};
            int const c = scan_wrappers(p, n, at, &h);
            if (c > 0 && count == 0) first = h;
            count += c;
        });
    });
    if (!game) { s.why = "was not looked for (" PADHOOK_GAME " is not loaded)"; return s; }
    s.state = 2;
    if (!headers) { s.why = "was not looked for (" PADHOOK_GAME "'s headers are unreadable)"; return s; }
    s.why = scan_verdict(count, first);
    if (!s.why && !slots_writable(first)) s.why = "has its slots outside a writable section of " PADHOOK_GAME;
    if (!s.why) { s.hit = first; s.state = 1; }
    return s;
}

// Once the client has loaded its shim (H is not zero): H holds xinputdll.dll's handle, and F its GetState.
// A few tries, a ms apart: the client may be writing the slots at that moment.
inline char const* slot_values(WrapperHit const& hit) {
    char const* why = nullptr;
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (attempt) Sleep(1);
        std::uintptr_t const h = read_slot(hit.handle_slot);
        if (h == 0) return nullptr;                  // not loaded yet: the signature and the layout decide alone
        HMODULE shim = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, PADHOOK_SHIM, &shim)
            || reinterpret_cast<std::uintptr_t>(shim) != h) {
            why = "has a handle slot that does not hold " PADHOOK_SHIM;
            continue;
        }
        auto const want = reinterpret_cast<std::uintptr_t>(reinterpret_cast<void*>(GetProcAddress(shim, PADHOOK_SHIM_GET)));
        if (!want || read_slot(hit.fn_slot) != want) {
            why = "has a function slot that does not hold " PADHOOK_SHIM "!" PADHOOK_SHIM_GET;
            continue;
        }
        return nullptr;
    }
    return why;
}

// ---- rev 4: the DirectInput source ----

inline volatile DWORD& last_di_tid() { static volatile DWORD value = 0; return value; }

inline bool pov_centred(unsigned long pov) { return (pov & 0xFFFFul) == 0xFFFFul; }
inline unsigned long di_bit(unsigned i) { return 1ul << (i & 31u); }
inline unsigned long di_count(unsigned long bits) { return static_cast<unsigned long>(__builtin_popcountl(bits)); }

// The buttons a DIJOYSTATE2 holds down, 0..31 (the ones the client maps).
inline unsigned long di_raw_buttons(unsigned char const* js) {
    unsigned long raw = 0;
    for (unsigned i = 0; i < PADHOOK_DI_BUTTONS; ++i) if (js[PADHOOK_JS_BUTTONS + i] & 0x80) raw |= di_bit(i);
    return raw;
}

// The union of the live rev 4 clients' DirectInput chords.
inline unsigned long di_chords(PadBus const* bus) {
    unsigned long all = 0;
    for (int i = 0; i < PADHOOK_MAX_CLIENTS; ++i) {
        PadClient const& c = bus->clients[i];
        if (c.occupied == 2 && c.rev >= 4) all |= c.di_chord;
    }
    return all;
}

// A new press of the buttons in `fresh` (from a state read, or one press event; `held` is every button held now):
// hidden while a client holds the pad; a chord button waits for its partner while no chord button the game already
// has is held; the game's otherwise. A chord whose buttons all wait is then kept from the game, each button until its
// release. Pure: tests call it.
inline void di_decide(PadDiPress& k, unsigned long fresh, unsigned long held, bool cap, DWORD now) {
    if (!fresh) return;
    if (cap) {
        k.hid |= fresh;
        k.hidden += di_count(fresh);
        return;
    }
    unsigned long const C = k.chord;
    unsigned long wait = 0;
#ifndef PADHOOK_FAULT_DI_NO_CHORD
    if (C && (held & C & ~fresh & ~k.wait) == 0) wait = fresh & C;
#else
    (void)held;
#endif
    if (wait) {
        if (!k.wait) k.wait_since = now;
        k.wait |= wait;
    }
    k.game |= fresh & ~wait;
    if (C && (C & ~k.wait) == 0) {
        k.hid |= C;
        k.wait &= ~C;
        ++k.chords;
    }
}

// The state read: the DIJOYSTATE2 that GetDeviceState filled, changed in place. Pure on its inputs: tests call it.
inline void di_apply_state(PadDiPress& k, unsigned char* js, bool cap, DWORD now) {
    unsigned long const raw = di_raw_buttons(js);
    unsigned long pov = 0;
    std::memcpy(&pov, js + PADHOOK_JS_POV, 4);
    if (cap && !k.was_captured) k.pov_pass = !pov_centred(k.pov_out) && !pov_centred(pov) ? 1 : 0;
#ifdef PADHOOK_FAULT_DI_TAKE_HELD
    if (cap && !k.was_captured) { k.hid |= k.game & raw; k.game &= ~raw; }
#endif
#ifdef PADHOOK_FAULT_DI_LEAK_HELD
    if (!cap) { k.game |= k.hid & raw; k.hid &= ~raw; }
#endif
    if (cap && k.wait) {                            // the pad taken while a chord button waits: a press made under the hold
        k.hid |= k.wait;
        k.hidden += di_count(k.wait);
        k.wait = 0;
    }
    // a waiting chord button released alone in time: a tap the game still gets, as a press and a release event
    unsigned long const gone = k.wait & ~raw;
#ifndef PADHOOK_FAULT_DI_NO_TAP
    k.tap |= gone;
    k.swallow |= gone;
#endif
    k.wait &= ~gone;
    // a chord button that waited too long went up before its press event went in: owed as a tap now
    unsigned long const late_gone = k.late & ~raw;
    k.late &= ~late_gone;
    k.game &= ~late_gone;
    k.tap |= late_gone;
    k.swallow |= late_gone;
    // held alone too long, or no chord any more: the game's now, with a press event at the next event read
    if (k.wait && (k.chord == 0 || now - k.wait_since > PADHOOK_CHORD_MS)) {
        k.late |= k.wait;
        k.game |= k.wait;
        k.wait = 0;
    }
    unsigned long const fresh = raw & ~(k.game | k.hid | k.wait | k.tap | k.swallow);
    di_decide(k, fresh, raw, cap, now);
    unsigned long const out = raw & (k.hid | k.wait | k.tap | k.swallow);
    for (unsigned i = 0; i < PADHOOK_DI_BUTTONS; ++i) if (out & di_bit(i)) js[PADHOOK_JS_BUTTONS + i] = 0;
    k.down = raw;
    InterlockedExchange(&k.state_ok, 1);
    // the sticks and the POV: centred while a client holds the pad
#ifndef PADHOOK_FAULT_DI_NO_CENTRE
    if (cap) {
        auto* axes = reinterpret_cast<LONG*>(js);
        for (int a = 0; a < PADHOOK_JS_AXES; ++a) axes[a] = 0;
    }
#endif
    if (pov_centred(pov)) {
        k.pov_pass = 0;
        k.pov_hid = 0;
    } else if (cap ? !k.pov_pass : k.pov_hid != 0) {
        k.pov_hid = 1;
        pov = PADHOOK_POV_CENTRED;
        std::memcpy(js + PADHOOK_JS_POV, &pov, 4);
    }
    k.pov_out = pov;
    k.was_captured = cap ? 1 : 0;
}

// The event read: the events GetDeviceData returned, rewritten in place, and *count with them. The client has room for
// `asked` events, so the presses owed to the game (a chord button tapped alone, or held alone too long) go in only while
// there is room; the rest wait for a later read. Pure on its inputs: tests call it.
inline void di_apply_events(PadDiPress& k, PadDiEvent* ev, DWORD* count, DWORD asked, bool cap, DWORD now) {
    ++k.event_reads;
    if (!ev || !count) return;
    DWORD n = *count;
    if (asked && n > asked) n = asked;
    DWORD const room = asked ? asked : n;
    if (n > PADHOOK_DI_MAX_EVENTS || room > PADHOOK_DI_MAX_EVENTS) {
        // more than the client ever asks for: drop what is hidden, add nothing
        DWORD o = 0;
        for (DWORD j = 0; j < n; ++j) {
            PadDiEvent const e = ev[j];
            if (e.ofs >= PADHOOK_JS_BUTTONS && e.ofs < PADHOOK_JS_BUTTONS + PADHOOK_DI_BUTTONS
                && ((k.hid | k.wait | k.swallow) & di_bit(e.ofs - PADHOOK_JS_BUTTONS))) { ++k.dropped; continue; }
            ev[o++] = e;
        }
        *count = o;
        return;
    }
    PadDiEvent in[PADHOOK_DI_MAX_EVENTS];
    std::memcpy(in, ev, n * sizeof(PadDiEvent));
    DWORD o = 0;
    auto put = [&](PadDiEvent const& e) { ev[o++] = e; };
    auto made = [&](unsigned i, DWORD data, DWORD time) { return PadDiEvent {PADHOOK_JS_BUTTONS + i, data, time, 0, 0}; };
    unsigned long pressed = 0;              // a press in this read, with no release after it
    for (DWORD j = 0; j < n; ++j) {
        PadDiEvent const e = in[j];
        DWORD const left = n - j - 1;      // input events still to come, each of which needs its slot
        if (e.ofs < PADHOOK_JS_BUTTONS || e.ofs >= PADHOOK_JS_BUTTONS + PADHOOK_DI_BUTTONS) { put(e); continue; }
        unsigned const i = e.ofs - PADHOOK_JS_BUTTONS;
        unsigned long const b = di_bit(i);
#ifdef PADHOOK_FAULT_DI_NO_EVENTS
        (void)left; (void)cap; (void)now; (void)made;
        put(e);
        continue;
#endif
        if (e.data & 0x80) {
            pressed |= b;
            if ((k.tap | k.swallow) & b) {   // a press before a tap's release came: that release was lost
                k.tap &= ~b;
                k.swallow &= ~b;
            }
            if (k.late & b) { k.late &= ~b; put(e); }           // the press the game was owed: this one
            else if (k.game & b) put(e);
            else if ((k.hid | k.wait) & b) ++k.dropped;
            else {
                di_decide(k, b, k.down | pressed, cap, now);
                if (k.game & b) put(e);
                else ++k.dropped;
            }
        } else {
            pressed &= ~b;
            if ((k.late | k.wait | k.tap) & b) {
                // released before the game got its press: the game gets a press, then this release
                bool const was_late = (k.late & b) != 0;
                k.late &= ~b;
                k.wait &= ~b;
                k.tap &= ~b;
                k.swallow &= ~b;
                k.game &= ~b;
                if (o + 2 + left <= room) {
                    put(made(i, 0x80, e.time));
                    put(e);
                    ++k.injected;
                } else {
                    k.tap |= b;              // no room now: a press and a release at a later read (this release is used up)
                    ++k.dropped;
                    (void)was_late;
                }
            } else if (k.game & b) { k.game &= ~b; put(e); }
            else if (k.hid & b) { k.hid &= ~b; ++k.dropped; }
            else if (k.swallow & b) { k.swallow &= ~b; ++k.dropped; }
            else put(e);                   // a release the game never saw pressed: as the client gets it without nui
        }
    }
    // what is still owed, while there is room
    for (unsigned i = 0; i < PADHOOK_DI_BUTTONS; ++i) {
        unsigned long const b = di_bit(i);
        if ((k.tap & b) && o + 2 <= room) {
            put(made(i, 0x80, now));
            put(made(i, 0, now));
            k.tap &= ~b;
            ++k.injected;
        } else if ((k.late & b) && o + 1 <= room) {
            put(made(i, 0x80, now));
            k.late &= ~b;
            ++k.injected;
        }
    }
    *count = o;
#ifndef PADHOOK_FAULT_DI_NO_DRAIN
    if (asked && n < asked && k.state_ok) {   // the buffer is empty: a press shown up in the state, with no press since, is over
        unsigned long const ends = (k.game | k.hid | k.swallow) & ~k.down & ~pressed & ~k.late;
        k.ended += di_count(k.hid & ends);
        k.game &= ~ends;
        k.hid &= ~ends;
        k.swallow &= ~ends;
    }
#endif
}

// A second thread reads while the first writes: hide what is hidden, change nothing else.
inline void di_hide_only(PadDiPress const& k, DWORD kind, void* data, DWORD* count, DWORD asked) {
    unsigned long const out = k.hid | k.wait | k.tap | k.swallow;
    if (kind == PADHOOK_DI_STATE && asked >= PADHOOK_JS_BYTES) {
        auto* js = static_cast<unsigned char*>(data);
        for (unsigned i = 0; i < PADHOOK_DI_BUTTONS; ++i) if (out & di_bit(i)) js[PADHOOK_JS_BUTTONS + i] = 0;
        if (k.was_captured) {
            auto* axes = reinterpret_cast<LONG*>(js);
            for (int a = 0; a < PADHOOK_JS_AXES; ++a) axes[a] = 0;
        }
    } else if (kind == PADHOOK_DI_EVENTS && count) {
        auto* ev = static_cast<PadDiEvent*>(data);
        DWORD o = 0, n = asked && *count > asked ? asked : *count;
        for (DWORD j = 0; j < n; ++j) {
            PadDiEvent const e = ev[j];
            if (e.ofs >= PADHOOK_JS_BUTTONS && e.ofs < PADHOOK_JS_BUTTONS + PADHOOK_DI_BUTTONS
                && (out & di_bit(e.ofs - PADHOOK_JS_BUTTONS))) continue;
            ev[o++] = e;
        }
        *count = o;
    }
}

// What the client read, raw, for every module to read (behind the seqlock). js: null when the read failed.
inline void di_record(PadDiHook& h, LONG hr, unsigned char const* js, DWORD now) {
    PadDiState& p = h.pad;
    unsigned long const buttons = js ? di_raw_buttons(js) : 0;
    unsigned long pov = PADHOOK_POV_CENTRED;
    long axes[PADHOOK_JS_AXES] = {0, 0, 0, 0, 0, 0, 0, 0};
    if (js) {
        std::memcpy(&pov, js + PADHOOK_JS_POV, 4);
        std::memcpy(axes, js, sizeof(axes));
    }
    bool const same = p.seq != 0 && (p.hr >= 0) == (hr >= 0) && p.buttons == buttons && p.pov == pov
        && std::memcmp(p.axes, axes, sizeof(axes)) == 0;
    InterlockedIncrement(&h.wseq);
    MemoryBarrier();
    p.tick = now;
    ++p.reads;
    p.hr = hr;
    if (!same) {
        p.buttons = buttons;
        p.pov = pov;
        std::memcpy(p.axes, axes, sizeof(axes));
        ++p.seq;
        if (p.seq == 0) p.seq = 1;
    }
    MemoryBarrier();
    InterlockedIncrement(&h.wseq);
}

// The device's maker, product and name, from its own GetDeviceInfo (vtable +0x3C), once per device the client reads.
// guidProduct is {PPPPVVVV-0000-0000-0000-504944564944} ("PIDVID") for a HID device; otherwise the maker is not known.
constexpr std::size_t kDiInstanceBytes = 580;             // DIDEVICEINSTANCEA
constexpr std::size_t kDiProductAt = 20, kDiNameAt = 300; // guidProduct, tszProductName
inline void di_identify(PadDiHook& h, void* device) {
    unsigned long vidpid = 0;
    char name[32] = {};
    auto const dev = reinterpret_cast<std::uintptr_t>(device);
    if (span_readable(dev, 4)) {
        std::uintptr_t const vtbl = *reinterpret_cast<std::uintptr_t const*>(dev);
        if (span_readable(vtbl + 0x3C, 4)) {
            void* const fn = *reinterpret_cast<void* const*>(vtbl + 0x3C);
            if (code_in_module(fn, nullptr)) {
                typedef LONG(__stdcall* info_fn)(void* self, void* instance);
                unsigned char info[kDiInstanceBytes] = {};
                DWORD const size = kDiInstanceBytes;
                std::memcpy(info, &size, 4);
                if (reinterpret_cast<info_fn>(fn)(device, info) >= 0) {
                    static unsigned char const kPidVid[10] = {0, 0, 0, 0, 0, 0, 'P', 'I', 'D', 'V'};
                    if (std::memcmp(info + kDiProductAt + 4, kPidVid, sizeof(kPidVid)) == 0
                        && info[kDiProductAt + 14] == 'I' && info[kDiProductAt + 15] == 'D')
                        std::memcpy(&vidpid, info + kDiProductAt, 4);
                    for (std::size_t i = 0; i + 1 < sizeof(name) && info[kDiNameAt + i]; ++i)
                        name[i] = static_cast<char>(info[kDiNameAt + i]);
                }
            }
        }
    }
    InterlockedIncrement(&h.wseq);
    MemoryBarrier();
    h.pad.device = device;
    h.pad.vidpid = vidpid;
    std::memcpy(h.pad.name, name, sizeof(name));
    ++h.pad.seq;
    if (h.pad.seq == 0) h.pad.seq = 1;
    MemoryBarrier();
    InterlockedIncrement(&h.wseq);
}

// The DirectInput dispatcher: one copy per module, called from the thunks after each read, on the client's main thread.
inline void PADHOOK_ALIGN_STACK __stdcall di_dispatch(DWORD kind, LONG hr, void* data, DWORD* count, DWORD asked, void* device) {
    PadBus* bus = cached_bus();
    if (!bus) return;
    PadDiHook& h = bus->di;
    InterlockedIncrement(&h.in_flight);
    InterlockedIncrement(&h.calls);
    last_di_tid() = GetCurrentThreadId();
    PadDiPress& k = h.press;
    if (InterlockedCompareExchange(&h.write_lock, 1, 0) == 0) {
        DWORD const now = GetTickCount();
        bool const cap = captured(bus, now);
        k.chord = di_chords(bus);
        if (kind == PADHOOK_DI_STATE) {
            if (device && device != h.pad.device) di_identify(h, device);
            if (hr >= 0 && data && asked >= PADHOOK_JS_BYTES) {
                auto* js = static_cast<unsigned char*>(data);
#ifndef PADHOOK_FAULT_DI_NO_PRIME
                if (!k.primed) {                 // a button held when the driving started stays the game's
                    k.game |= di_raw_buttons(js) & ~(k.hid | k.wait | k.tap | k.swallow);
                    InterlockedExchange(&k.primed, 1);
                }
#endif
                di_record(h, hr, js, now);
                di_apply_state(k, js, cap, now);
            } else {
                InterlockedExchange(&k.state_ok, 0);   // the client clears the state itself; down is stale
                di_record(h, hr, nullptr, now);
            }
        } else if (kind == PADHOOK_DI_EVENTS) {
            if (hr >= 0) di_apply_events(k, static_cast<PadDiEvent*>(data), count, asked, cap, now);
        }                                              // another kind: a later revision's, nothing to do
        InterlockedExchange(&h.write_lock, 0);
    } else if (hr >= 0 && data) {
        di_hide_only(k, kind, data, count, asked);
    }
    InterlockedDecrement(&h.in_flight);
}

// -1 is a wildcard. The device's and the buffer's offsets, and the count asked for, are read from the match.
constexpr short kDiStateSig[52] = {
    0x8A, 0x8B, -1, -1, -1, -1,                  // mov cl, [ebx+..]          the client's "needs Poll" flag
    0x84, 0xC9, 0x74, 0x06,                      // test cl, cl / jz +6
    0x8B, 0x08, 0x50, 0xFF, 0x51, 0x64,          // mov ecx, [eax] / push eax / call [ecx+0x64]      Poll
    0x8B, 0xCB, 0xE8, -1, -1, -1, -1,            // mov ecx, ebx / call         the client's force feedback
    0x8B, 0x83, -1, -1, -1, -1,                  // mov eax, [ebx+disp]       the pad device
    0x8D, 0x8B, -1, -1, -1, -1, 0x51,            // lea ecx, [ebx+..] / push ecx              the state buffer
    0x68, 0x10, 0x01, 0x00, 0x00,                // push 0x110                sizeof(DIJOYSTATE2)
    0x8B, 0x10, 0x50, 0xFF, 0x52, 0x24,          // mov edx, [eax] / push eax / call [edx+0x24]   GetDeviceState
    0x3D, 0xAA, 0x00, 0x07, 0x80,                // cmp eax, 0x800700AA       the patch
};
constexpr short kDiEventsSig[100] = {
    0x8D, 0xAB, -1, -1, -1, -1,                  // lea ebp, [ebx+..]         the count
    0xC7, 0x45, 0x00, -1, -1, -1, -1,            // mov dword [ebp], asked
    0xA0, -1, -1, -1, -1, 0x84, 0xC0,            // mov al, [the gamepad setting] / test al, al
    0x0F, 0x84, -1, -1, -1, -1,                  // jz                        (the gamepad is off)
    0x8B, 0x83, -1, -1, -1, -1,                  // mov eax, [ebx+disp]       the pad device
    0x85, 0xC0, 0x0F, 0x84, -1, -1, -1, -1,      // test eax, eax / jz        (no pad)
    0x8B, 0x10,                                  // mov edx, [eax]
    0x8D, 0xBB, -1, -1, -1, -1,                  // lea edi, [ebx+..]         the events
    0x6A, 0x00, 0x55, 0x57, 0x6A, 0x14, 0x50,    // push 0 (no DIGDD_PEEK) / push ebp / push edi / push 0x14 / push eax
    0xFF, 0x52, 0x28,                            // call [edx+0x28]           GetDeviceData
    0x85, 0xC0, 0x74, 0x15,                      // test eax, eax / jz +0x15  (it read: to the stores below)
    0xB9, -1, -1, -1, -1, 0x33, 0xC0,            // mov ecx, .. / xor eax, eax      (it failed: the buffer cleared ...)
    0xC7, 0x45, 0x00, 0x00, 0x00, 0x00, 0x00,    // mov dword [ebp], 0
    0xF3, 0xAB, 0xE9, -1, -1, -1, -1,            // rep stosd / jmp           (... and the events skipped)
    0xA1, -1, -1, -1, -1,                        // mov eax, [..]
    0xC6, 0x05, -1, -1, -1, -1, 0x00,            // mov byte [..], 0
    0xC6, 0x44, 0x24, 0x13, 0x01,                // mov byte [esp+0x13], 1    the patch
};
constexpr std::size_t kDiStateCmpAt = 47, kDiStateDevAt = 25, kDiStateBufAt = 31;
constexpr std::size_t kDiEventsStoreAt = 95, kDiEventsDevAt = 28, kDiEventsAskedAt = 9;
constexpr unsigned char kDiCmp[5] = {0x3D, 0xAA, 0x00, 0x07, 0x80};         // cmp eax, 0x800700AA
constexpr unsigned char kDiStore[5] = {0xC6, 0x44, 0x24, 0x13, 0x01};       // mov byte [esp+0x13], 1
inline constexpr char kDiChanged[] = "changed while they were patched";

template <std::size_t N>
inline bool di_sig_at(unsigned char const* p, short const (&sig)[N]) {
    for (std::size_t i = 0; i < N; ++i)
        if (sig[i] >= 0 && p[i] != static_cast<unsigned char>(sig[i])) return false;
    return true;
}

struct DiScan {
    int state_count, events_count;
    std::uintptr_t state_at, events_at;      // the first match of each (its first byte)
    std::uint32_t state_disp, events_disp;   // the device's offset at each
    std::uint32_t buffer_disp;               // the state buffer's offset
    std::uint32_t asked;                     // the event read's count
};

// Both signatures over n bytes at p, which the process sees at `base`. Adds to s (several runs make one scan). Pure.
inline void di_scan_sites(unsigned char const* p, std::size_t n, std::uintptr_t base, DiScan& s) {
    constexpr std::size_t kA = sizeof(kDiStateSig) / sizeof(short), kB = sizeof(kDiEventsSig) / sizeof(short);
    for (std::size_t off = 0; off + kA <= n; ++off) {
        if (p[off] == 0x8A && p[off + 1] == 0x8B && di_sig_at(p + off, kDiStateSig)) {
#ifdef PADHOOK_FAULT_DI_FIRST_MATCH
            if (s.state_count > 0) continue;
#endif
            if (s.state_count++ == 0) {
                s.state_at = base + off;
                s.state_disp = le32(p + off + kDiStateDevAt);
                s.buffer_disp = le32(p + off + kDiStateBufAt);
            }
        }
        if (off + kB <= n && p[off] == 0x8D && p[off + 1] == 0xAB && di_sig_at(p + off, kDiEventsSig)) {
#ifdef PADHOOK_FAULT_DI_FIRST_MATCH
            if (s.events_count > 0) continue;
#endif
            if (s.events_count++ == 0) {
                s.events_at = base + off;
                s.events_disp = le32(p + off + kDiEventsDevAt);
                s.asked = le32(p + off + kDiEventsAskedAt);
            }
        }
    }
}

// Can 5 bytes at `at` be written with one aligned 8-byte compare-and-exchange?
inline bool di_fits_one_block(std::uintptr_t at) { return at % 8 <= 3; }

// What the scan means: null when it can be patched, otherwise why not. Pure: tests call it.
inline char const* di_scan_verdict(DiScan const& s) {
    if (s.state_count == 0) return "were not found: no GetDeviceState read of the pad (a client update?)";
    if (s.events_count == 0) return "were not found: no GetDeviceData read of the pad (a client update?)";
    if (s.state_count > 1) return "are not unique: several GetDeviceState reads of the pad";
    if (s.events_count > 1) return "are not unique: several GetDeviceData reads of the pad";
    if (s.state_disp != s.events_disp) return "read two devices (the offsets differ)";
    if (s.events_at <= s.state_at || s.events_at - s.state_at > PADHOOK_DI_SPAN) return "are not in one function";
    if (s.asked == 0 || s.asked > PADHOOK_DI_MAX_EVENTS) return "ask for an odd count of events";
    if (!di_fits_one_block(s.state_at + kDiStateCmpAt) || !di_fits_one_block(s.events_at + kDiEventsStoreAt))
        return "are not on 8-byte boundaries that one write can patch";
    return nullptr;
}

constexpr std::size_t kDiBlockBytes = 128;
constexpr std::size_t kDiStateThunkAt = 16, kDiStateThunkBytes = 50;
constexpr std::size_t kDiEventsThunkAt = 72, kDiEventsThunkBytes = 39;

// The state read's thunk (kDiStateThunkBytes). ebx is the input object: the signature fixes it.
inline void di_state_thunk_bytes(std::uintptr_t block, std::uint32_t device_disp, std::uint32_t buffer_disp, unsigned char* t) {
    unsigned char code[kDiStateThunkBytes] = {
        0x50, 0x51, 0x52,                        // push eax / push ecx / push edx       eax: the read's HRESULT, kept
        0x8B, 0x0D, 0, 0, 0, 0,                  // mov ecx, [dispatch]
        0x85, 0xC9, 0x74, 0x1C,                  // test ecx, ecx / jz done               nobody drives: the cmp alone
        0xFF, 0xB3, 0, 0, 0, 0,                  // push [ebx+device]                     the device
        0x68, 0x10, 0x01, 0x00, 0x00,            // push 0x110                            the state's size, as asked
        0x6A, 0x00,                              // push 0                                no count
        0x8D, 0x93, 0, 0, 0, 0,                  // lea edx, [ebx+buffer]
        0x52,                                    // push edx                              the state
        0xFF, 0x74, 0x24, 0x18,                  // push [esp+24]                         the HRESULT
        0x6A, PADHOOK_DI_STATE,                  // push kind
        0xFF, 0xD1,                              // call ecx                              __stdcall: pops its 6 arguments
        0x5A, 0x59, 0x58,                        // done: pop edx / pop ecx / pop eax
        0x3D, 0xAA, 0x00, 0x07, 0x80,            // cmp eax, 0x800700AA                   the client's own cmp, on its eax
        0xC3,                                    // ret                                   ret keeps the flags
    };
    auto const d = static_cast<std::uint32_t>(block);
    std::memcpy(code + 5, &d, 4);
    std::memcpy(code + 15, &device_disp, 4);
    std::memcpy(code + 28, &buffer_disp, 4);
#ifdef PADHOOK_FAULT_DI_NO_CMP
    code[44] = code[45] = code[46] = code[47] = code[48] = 0x90;   // the flags the jumps after it test are the thunk's own
#endif
    std::memcpy(t, code, kDiStateThunkBytes);
}

// The event read's thunk (kDiEventsThunkBytes), on the read's success branch. ebx is the input object, edi the events
// and ebp the count: the signature fixes them.
inline void di_events_thunk_bytes(std::uintptr_t block, std::uint32_t device_disp, std::uint32_t asked, unsigned char* t) {
    unsigned char code[kDiEventsThunkBytes] = {
        0x9C, 0x60,                              // pushfd / pushad
        0x8B, 0x0D, 0, 0, 0, 0,                  // mov ecx, [dispatch]
        0x85, 0xC9, 0x74, 0x13,                  // test ecx, ecx / jz done               nobody drives: the store alone
        0xFF, 0xB3, 0, 0, 0, 0,                  // push [ebx+device]                     the device
        0x68, 0, 0, 0, 0,                        // push asked                            the count the client asked for
        0x55,                                    // push ebp                              the count
        0x57,                                    // push edi                              the events
        0x6A, 0x00,                              // push 0                                the read succeeded
        0x6A, PADHOOK_DI_EVENTS,                 // push kind
        0xFF, 0xD1,                              // call ecx                              __stdcall: pops its 6 arguments
        0x61, 0x9D,                              // done: popad / popfd
        0xC6, 0x44, 0x24, 0x17, 0x01,            // mov byte [esp+0x17], 1                the client's own store
        0xC3,                                    // ret
    };
    auto const d = static_cast<std::uint32_t>(block);
    std::memcpy(code + 4, &d, 4);
    std::memcpy(code + 14, &device_disp, 4);
    std::memcpy(code + 19, &asked, 4);
#ifdef PADHOOK_FAULT_DI_NO_STORE
    code[33] = code[34] = code[35] = code[36] = code[37] = 0x90;   // the client's flag is never set
#endif
    std::memcpy(t, code, kDiEventsThunkBytes);
}

// The 8 aligned bytes that hold the 5 at `at`, before (as found) and after the patch (E8 <rel32 -> thunk>).
inline void di_patch_bytes(std::uintptr_t at, std::uintptr_t thunk, unsigned char const* found, unsigned char* want) {
    std::size_t const k = at % 8;
    std::memcpy(want, found, 8);
    want[k] = 0xE8;
    auto const rel = static_cast<std::int32_t>(thunk - (at + 5));
    std::memcpy(want + k + 1, &rel, 4);
}

// One site: the aligned block read, checked to hold `expect` at `at`, and written in one locked compare-and-exchange.
// write = false: the check alone. Null on success, or why not.
inline char const* di_write_site(std::uintptr_t at, unsigned char const* expect, std::uintptr_t thunk, unsigned char* old_out,
                                 bool write = true) {
    std::uintptr_t const block = at & ~static_cast<std::uintptr_t>(7);
    if (!di_fits_one_block(at)) return "are not on 8-byte boundaries that one write can patch";
    unsigned char found[8];
    std::memcpy(found, reinterpret_cast<void const*>(block), 8);
    if (std::memcmp(found + (at - block), expect, 5) != 0) return kDiChanged;
    if (old_out) std::memcpy(old_out, found, 8);
    if (!write) return nullptr;
    unsigned char want[8];
    di_patch_bytes(at, thunk, found, want);
    DWORD previous = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(block), 8, PAGE_EXECUTE_READWRITE, &previous))
        return "could not be patched (VirtualProtect failed)";
    LONGLONG old_v = 0, new_v = 0;
    std::memcpy(&old_v, found, 8);
    std::memcpy(&new_v, want, 8);
    bool const wrote = InterlockedCompareExchange64(reinterpret_cast<LONGLONG volatile*>(block), new_v, old_v) == old_v;
    VirtualProtect(reinterpret_cast<void*>(block), 8, previous, &previous);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(block), 8);
    return wrote ? nullptr : kDiChanged;
}

// Put a site back as it was (only when the other site failed in the same install).
inline void di_unwrite_site(std::uintptr_t at, unsigned char const* old_bytes) {
    std::uintptr_t const block = at & ~static_cast<std::uintptr_t>(7);
    unsigned char now[8];
    std::memcpy(now, reinterpret_cast<void const*>(block), 8);
    DWORD previous = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(block), 8, PAGE_EXECUTE_READWRITE, &previous)) return;
    LONGLONG old_v = 0, new_v = 0;
    std::memcpy(&old_v, now, 8);
    std::memcpy(&new_v, old_bytes, 8);
    InterlockedCompareExchange64(reinterpret_cast<LONGLONG volatile*>(block), new_v, old_v);
    VirtualProtect(reinterpret_cast<void*>(block), 8, previous, &previous);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(block), 8);
}

// Patch both sites that the checks passed: the block, then the state site, then the event site (the state site goes
// back if the event site fails). Null on success, with the hook's block, dispatch_ptr, sites and offsets set.
inline char const* di_install(PadDiHook& h, DiScan const& s) {
    auto* block = static_cast<unsigned char*>(VirtualAlloc(nullptr, kDiBlockBytes, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!block) return "could not be patched (no memory for the hook block)";
    std::memset(block, 0xCC, kDiBlockBytes);
    std::memset(block, 0, 4);
    auto const b = reinterpret_cast<std::uintptr_t>(block);
    di_state_thunk_bytes(b, s.state_disp, s.buffer_disp, block + kDiStateThunkAt);
    di_events_thunk_bytes(b, s.events_disp, s.asked, block + kDiEventsThunkAt);
    FlushInstructionCache(GetCurrentProcess(), block, kDiBlockBytes);
    std::uintptr_t const at_state = s.state_at + kDiStateCmpAt, at_events = s.events_at + kDiEventsStoreAt;
    h.block = block;
    h.dispatch_ptr = reinterpret_cast<padhook_di_fn volatile*>(block);
    h.site_state = at_state;
    h.site_events = at_events;
    h.device_disp = s.state_disp;
    h.buffer_disp = s.buffer_disp;
    MemoryBarrier();
    auto clear = [&]() {
        h.block = nullptr; h.dispatch_ptr = nullptr; h.site_state = h.site_events = 0; h.device_disp = h.buffer_disp = 0;
    };
    unsigned char old_state[8], old_events[8];
    // both checked before either is written
    char const* why = di_write_site(at_state, kDiCmp, b + kDiStateThunkAt, old_state, false);
    if (!why) why = di_write_site(at_events, kDiStore, b + kDiEventsThunkAt, old_events, false);
    if (!why) why = di_write_site(at_state, kDiCmp, b + kDiStateThunkAt, old_state);
    if (why) {
        clear();
        VirtualFree(block, 0, MEM_RELEASE);
        return why;
    }
    if ((why = di_write_site(at_events, kDiStore, b + kDiEventsThunkAt, old_events)) != nullptr) {
        di_unwrite_site(at_state, old_state);
        clear();                         // the block stays: a thread may be inside the state thunk now (nobody drives yet)
        return why;
    }
    return nullptr;
}

// The scan over the client's code sections, done once per module (the client's code never changes).
struct DiFind {
    int state;                           // 0 not yet (the client module is not loaded), 1 found, 2 refused for good
    DiScan scan;
    char const* why;
};
inline DiFind& di_find() { static DiFind value = {0, {0, 0, 0, 0, 0, 0, 0, 0}, nullptr}; return value; }

inline DiFind& find_di_sites() {
    DiFind& f = di_find();
    if (f.state != 0) return f;
    DiScan s {0, 0, 0, 0, 0, 0, 0, 0};
    HMODULE game = nullptr;
    bool const headers = for_each_game_section(&game, [&](std::uintptr_t begin, std::uintptr_t end, DWORD flags) {
        if (!(flags & IMAGE_SCN_MEM_EXECUTE)) return;
        for_each_readable_run(begin, end, [&](unsigned char const* p, std::size_t n, std::uintptr_t at) {
            di_scan_sites(p, n, at, s);
        });
    });
    if (!game) { f.why = "were not looked for (" PADHOOK_GAME " is not loaded)"; return f; }
    f.state = 2;
    if (!headers) { f.why = "were not looked for (" PADHOOK_GAME "'s headers are unreadable)"; return f; }
    f.scan = s;
    f.why = di_scan_verdict(s);
    if (!f.why) f.state = 1;
    return f;
}

// Can this client drive the DirectInput source? (rev 4 or later, alive, its dispatcher in its own module)
inline bool can_own_di(PadClient const& c) {
    return c.rev >= 4 && c.di_dispatch && can_own(c) && code_in_module(reinterpret_cast<void const*>(c.di_dispatch), c.module);
}

inline void set_di_owner(PadBus* bus, int slot) {
    PadDiHook& h = bus->di;
    if (!h.dispatch_ptr) return;
    padhook_di_fn const next = slot >= 0 ? bus->clients[slot].di_dispatch : nullptr;
    // nobody drove: buttons went down and up unseen, so the next state read gives every held button to the game
    if (*h.dispatch_ptr == nullptr && next) InterlockedExchange(&h.press.primed, 0);
    if (*h.dispatch_ptr != next) InterlockedIncrement(&h.handovers);
    *h.dispatch_ptr = next;
    InterlockedExchange(&h.owner_rev, slot >= 0 ? static_cast<LONG>(bus->clients[slot].rev) : 0);
    InterlockedExchange(&h.owner, slot + 1);
}

// The best client to drive the DirectInput source (the highest revision), other than `leaving`, or nobody.
inline void reassign_di_owner(PadBus* bus, int leaving) {
    if (!bus || !bus->di.dispatch_ptr) return;
    int best = -1;
    for (int i = 0; i < PADHOOK_MAX_CLIENTS; ++i) {
        if (i == leaving || !can_own_di(bus->clients[i])) continue;
        if (best < 0 || bus->clients[i].rev > bus->clients[best].rev) best = i;
    }
    set_di_owner(bus, best);
}

// Does the client in `slot` take the DirectInput dispatch? (nobody drives it, the driver is gone, or this revision is higher)
inline void maybe_take_di(PadBus* bus, int slot) {
    PadDiHook& h = bus->di;
    if (!InterlockedCompareExchange(&h.installed, 0, 0) || !h.dispatch_ptr || !can_own_di(bus->clients[slot])) return;
    LONG const owner = InterlockedCompareExchange(&h.owner, 0, 0) - 1;
    bool take = owner < 0 || owner >= PADHOOK_MAX_CLIENTS;
    if (!take) {
        PadClient const& o = bus->clients[owner];
        take = !can_own_di(o) || o.di_dispatch != *h.dispatch_ptr;
    }
    if (!take) take = static_cast<LONG>(PADHOOK_REV) > InterlockedCompareExchange(&h.owner_rev, 0, 0);
    if (take) set_di_owner(bus, slot);
}

}  // namespace padhook_detail

// Attach to the process-wide bus, creating it on first use (leaked on purpose: it belongs to the process).
inline PadBus* padhook_attach() {
    PadBus*& cached = padhook_detail::cached_bus();
    if (cached) return cached;
    char name[64];
    std::snprintf(name, sizeof(name), PADHOOK_MAP_NAME_FMT, static_cast<unsigned long>(GetCurrentProcessId()));
    HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, PADHOOK_MAP_BYTES, name);
    if (!mapping) return nullptr;
    bool const existed = GetLastError() == ERROR_ALREADY_EXISTS;
    auto* bus = static_cast<PadBus*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, PADHOOK_MAP_BYTES));
    if (!bus) { CloseHandle(mapping); return nullptr; }
    if (!existed) {
        bus->abi_version = PADHOOK_ABI_VERSION;
        bus->size = sizeof(PadBus);
        bus->owner_slot = -1;
        bus->last_index = -1;
        std::snprintf(bus->status, sizeof(bus->status), "bus created");
        MemoryBarrier();
        InterlockedExchange(&bus->ready, 1);
    } else {
        for (int spin = 0; spin < 2000 && InterlockedCompareExchange(&bus->ready, 0, 0) == 0; ++spin) Sleep(1);
        if (InterlockedCompareExchange(&bus->ready, 0, 0) == 0) { UnmapViewOfFile(bus); CloseHandle(mapping); return nullptr; }
    }
    if (bus->abi_version != PADHOOK_ABI_VERSION || bus->size < offsetof(PadBus, status) + sizeof(bus->status)
        || bus->size > PADHOOK_MAP_BYTES) {
        UnmapViewOfFile(bus);
        CloseHandle(mapping);
        return nullptr;
    }
    for (;;) {
        LONG const seen = static_cast<LONG>(bus->size);
        if (seen >= static_cast<LONG>(sizeof(PadBus))) break;
        if (InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&bus->size), sizeof(PadBus), seen) == seen) break;
    }
    cached = bus;
    return bus;
}

// bus->blocked means route 0 is refused for good (a rev 2 module sets it too). Rev 3 calls the bus blocked only
// once route 1 is refused for good as well. Until then it waits, and a host retries padhook_join.
inline int padhook_state(PadBus const* bus) {
    if (!bus) return PADHOOK_NO_BUS;
    if (bus->hook_installed) return PADHOOK_LIVE;
    if (bus->blocked && !padhook_detail::route1_open()) return PADHOOK_BLOCKED;
    return PADHOOK_WAIT;
}

namespace padhook_detail {

// Route 1, under install_lock: null when it is live, otherwise why not.
inline char const* try_wrapper(PadBus* bus) {
    WrapperScan& s = find_wrapper();
    if (s.state != 1) return s.why;
    if (char const* why = slot_values(s.hit)) {
        if (++s.mismatches >= 3) { s.state = 2; s.why = why; }   // seconds apart: not the client mid-write
        return why;
    }
    char const* why = install_wrapper(bus, s.hit);
    if (why == kWrapChanged) { s.state = 2; s.why = why; }   // another tool's patch: never again in this module
    return why;
}

// Route 0, under install_lock: 0 live, 1 retry later, 2 never in this process; *why says why not.
inline int try_export(PadBus* bus, char* why, std::size_t n) {
    auto fail = [&](int verdict, char const* fmt, char const* a = "") {
        std::snprintf(why, n, fmt, a);
        return verdict;
    };
    // the game loads xinput1_3.dll itself when it uses XInput; nui never loads it (no XInput reads in DirectInput mode)
    HMODULE xi = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN, PADHOOK_DLL, &xi) || !xi)
        return fail(1, "%s is not loaded (the game reads the pad with DirectInput, or not yet)", PADHOOK_DLL);
    auto const site = reinterpret_cast<std::uintptr_t>(reinterpret_cast<void*>(GetProcAddress(xi, "XInputGetState")));
    if (!site || !span_readable(site - 5, 7)) return fail(2, "XInputGetState not found in %s", PADHOOK_DLL);
    auto const* p = reinterpret_cast<unsigned char const*>(site);
    bool pad_ok = true;
    for (int i = -5; i < 0; ++i) pad_ok = pad_ok && (p[i] == 0x90 || p[i] == 0xCC);
    if (p[0] == 0xEB || p[0] == 0xE9 || p[-5] == 0xE9) {
        char hex[40];
        hexbytes(hex, sizeof(hex), p - 5, 7);
        return fail(2, "XInputGetState is already hooked by another tool (%s): not patched", hex);
    }
    if (!pad_ok || p[0] != 0x8B || p[1] != 0xFF) {
        char hex[40];
        hexbytes(hex, sizeof(hex), p - 5, 7);
        return fail(2, "XInputGetState is not hot-patchable (%s): not patched", hex);
    }
    auto* block = static_cast<unsigned char*>(VirtualAlloc(nullptr, kBlockBytes, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!block) return fail(1, "hook block alloc failed%s");
    std::memset(block, 0xCC, kBlockBytes);
    std::memset(block, 0, 4);
    unsigned char* t = block + kThunkAt;
    thunk_bytes(reinterpret_cast<std::uintptr_t>(block), site, t);
    FlushInstructionCache(GetCurrentProcess(), block, kBlockBytes);
    bus->block = block;
    bus->dispatch_ptr = reinterpret_cast<padhook_get_fn volatile*>(block);
    bus->site = site;
    bus->original = reinterpret_cast<padhook_get_fn>(site + 2);
    InterlockedExchange(&bus->route, PADHOOK_ROUTE_EXPORT);
    MemoryBarrier();

    DWORD previous = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(site - 5), 7, PAGE_EXECUTE_READWRITE, &previous)) {
        bus->block = nullptr; bus->dispatch_ptr = nullptr; bus->site = 0; bus->original = nullptr;
        VirtualFree(block, 0, MEM_RELEASE);
        return fail(1, "VirtualProtect failed%s");
    }
    auto* w = reinterpret_cast<unsigned char*>(site - 5);
    std::int32_t const to_thunk = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(t) - site);
    w[0] = 0xE9;                                       // the padding: no thread runs here
    std::memcpy(w + 1, &to_thunk, 4);
    FlushInstructionCache(GetCurrentProcess(), w, 5);
    // the entry: 8B FF -> EB F9 in one locked 16-bit compare-and-exchange
    unsigned short const want = 0xF9EB, expect = 0xFF8B;
    bool const wrote = __sync_val_compare_and_swap(reinterpret_cast<unsigned short volatile*>(site), expect, want) == expect;
    if (!wrote) std::memset(w, 0x90, 5);
    VirtualProtect(reinterpret_cast<void*>(site - 5), 7, previous, &previous);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site - 5), 7);
    if (!wrote) {
        bus->block = nullptr; bus->dispatch_ptr = nullptr; bus->site = 0; bus->original = nullptr;
        VirtualFree(block, 0, MEM_RELEASE);
        return fail(2, "XInputGetState changed while patching%s");
    }
    return 0;
}

// The part of the status that names route 0's reason (rev 3 writes "<route 1's reason>; <route 0's>").
inline char const* export_reason(char const* status) {
    static char const prefix[] = PADHOOK_WRAPPER " ";
    if (std::strncmp(status, prefix, sizeof(prefix) - 1) != 0) return status;
    char const* tail = std::strstr(status, "; ");
    return tail ? tail + 2 : "";
}

}  // namespace padhook_detail

// Route 1: pin xinputdll.dll once the client has loaded it, so the client's unloader can never unmap it while
// a read is on its way through the dispatcher. pad() calls this. Until the client loads the shim it costs one
// read of the handle slot. True once this module has pinned it. A rev 1 or 2 module never pins it: on a bus
// where only such modules poll, the shim stays unpinned, as it is without nui.
inline bool padhook_pin(PadBus const* bus) {
    using namespace padhook_detail;
    volatile LONG& pinned = shim_pinned();
    if (pinned) return true;
    std::uintptr_t const slot = handle_slot(bus);
    if (!slot) return false;
    // a handle that was not the shim (the client between FreeLibrary and zeroing H, or something else): asked
    // about again at most once a second
    static std::uintptr_t refused = 0;
    static DWORD refused_at = 0;
    std::uintptr_t const h = read_slot(slot);
    if (h == 0 || (h == refused && GetTickCount() - refused_at < 1000)) return false;
    HMODULE shim = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, PADHOOK_SHIM, &shim)
        || reinterpret_cast<std::uintptr_t>(shim) != h) {
        refused = h;
        refused_at = GetTickCount();
        return false;
    }
    HMODULE held = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN, PADHOOK_SHIM, &held) || held != shim) return false;
    InterlockedExchange(&pinned, 1);
    return true;
}

// Is the client's shim loaded now (route 1: the handle slot is not zero)? False on route 0.
inline bool padhook_shim_loaded(PadBus const* bus) {
    std::uintptr_t const slot = padhook_detail::handle_slot(bus);
    return slot && padhook_detail::read_slot(slot) != 0;
}

// Patch one route if nobody has yet: route 1, the client's XInput wrapper, then route 0, the export. True when
// a patch is live, whoever wrote it. False with bus->status naming both routes' reasons: retry later
// (PADHOOK_WAIT), or never in this process (bus->blocked). A rev 2 module may have set bus->blocked: route 1 is
// still tried then, and clears it. simulate_missing: both not-found paths, always.
inline bool padhook_ensure_hook(PadBus* bus, bool simulate_missing = false, char const* who = nullptr) {
    using namespace padhook_detail;
    if (!bus) return false;
    if (InterlockedCompareExchange(&bus->hook_installed, 0, 0) != 0) return true;
    if (simulate_missing) {
        if (InterlockedCompareExchange(&bus->blocked, 0, 0) == 0)   // a blocked bus keeps route 0's real reason
            std::snprintf(bus->status, sizeof(bus->status), "%s was not found (simulated); %s is not loaded (simulated)",
                          PADHOOK_WRAPPER, PADHOOK_DLL);
        return false;
    }
    if (InterlockedCompareExchange(&bus->install_lock, 1, 0) != 0) {
        for (int spin = 0; spin < 2000 && InterlockedCompareExchange(&bus->hook_installed, 0, 0) == 0; ++spin) Sleep(1);
        return InterlockedCompareExchange(&bus->hook_installed, 0, 0) != 0;
    }
    auto done = [&](bool live) {
        if (live) {
            InterlockedIncrement(&bus->installs);
            InterlockedExchange(&bus->hook_installed, 1);
        }
        InterlockedExchange(&bus->install_lock, 0);
        return live;
    };
    if (InterlockedCompareExchange(&bus->hook_installed, 0, 0) != 0) {   // patched between the first check and the lock
        InterlockedExchange(&bus->install_lock, 0);
        return true;
    }
    char const* const who_s = who && *who ? who : "?";
#ifdef PADHOOK_FAULT_EXPORT_FIRST
    {
        char why0[176];
        if (!InterlockedCompareExchange(&bus->blocked, 0, 0) && try_export(bus, why0, sizeof(why0)) == 0) {
            std::snprintf(bus->status, sizeof(bus->status), "hook installed at %s!XInputGetState", PADHOOK_DLL);
            return done(true);
        }
    }
#endif
    char const* const why_wrapper = try_wrapper(bus);
    if (!why_wrapper) {
        std::snprintf(bus->status, sizeof(bus->status), "hook installed at %s %08lX by %s (rev %d)", PADHOOK_WRAPPER,
                      static_cast<unsigned long>(bus->site), who_s, PADHOOK_REV);
        InterlockedExchange(&bus->blocked, 0);
        done(true);
        padhook_pin(bus);
        return true;
    }
    char why_export[176];
    int verdict = 2;
    if (InterlockedCompareExchange(&bus->blocked, 0, 0) != 0) {
        std::snprintf(why_export, sizeof(why_export), "%s", export_reason(bus->status));   // refused before: never again
    } else {
        verdict = try_export(bus, why_export, sizeof(why_export));
    }
    if (verdict == 0) {
        std::snprintf(bus->status, sizeof(bus->status), "hook installed at %s!XInputGetState %08lX by %s (rev %d); %s %s",
                      PADHOOK_DLL, static_cast<unsigned long>(bus->site), who_s, PADHOOK_REV, PADHOOK_WRAPPER, why_wrapper);
        return done(true);
    }
    std::snprintf(bus->status, sizeof(bus->status), "%s %s; %s", PADHOOK_WRAPPER, why_wrapper, why_export);
    if (verdict == 2) InterlockedExchange(&bus->blocked, 1);
    return done(false);
}


// rev 4: the DirectInput source's state: PADHOOK_LIVE (both sites patched), PADHOOK_WAIT (FFXiMain.dll is not loaded),
// PADHOOK_BLOCKED (the sites cannot be patched safely in this process), or PADHOOK_NO_BUS.
inline int padhook_di_state(PadBus const* bus) {
    if (!bus) return PADHOOK_NO_BUS;
    if (bus->di.installed) return PADHOOK_LIVE;
    if (bus->di.blocked) return PADHOOK_BLOCKED;
    return PADHOOK_WAIT;
}

// rev 4: patch the DirectInput sites if nobody has yet, and give the dispatch to the best live client. True when they
// are live, whoever wrote them. False with bus->di.status saying why: retry later (PADHOOK_WAIT), or never in this
// process (bus->di.blocked). simulate_missing: the not-found path, always.
inline bool padhook_ensure_di(PadBus* bus, bool simulate_missing = false, char const* who = nullptr) {
    using namespace padhook_detail;
    if (!bus) return false;
    PadDiHook& h = bus->di;
    if (InterlockedCompareExchange(&h.installed, 0, 0) != 0) return true;
    if (simulate_missing) {
        if (InterlockedCompareExchange(&h.blocked, 0, 0) == 0)
            std::snprintf(h.status, sizeof(h.status), "%s were not found (simulated)", PADHOOK_DI_SITES);
        return false;
    }
    if (InterlockedCompareExchange(&h.install_lock, 1, 0) != 0) {
        for (int spin = 0; spin < 2000 && InterlockedCompareExchange(&h.installed, 0, 0) == 0; ++spin) Sleep(1);
        return InterlockedCompareExchange(&h.installed, 0, 0) != 0;
    }
    if (InterlockedCompareExchange(&h.installed, 0, 0) != 0) {    // patched between the first check and the lock
        InterlockedExchange(&h.install_lock, 0);
        return true;
    }
    if (InterlockedCompareExchange(&h.blocked, 0, 0) != 0) {      // refused before: the status says why
        InterlockedExchange(&h.install_lock, 0);
        return false;
    }
    char const* const who_s = who && *who ? who : "?";
    DiFind& f = find_di_sites();
    char const* why = f.why;
    if (f.state == 1) {
        why = di_install(h, f.scan);
        if (why) { f.state = 2; f.why = why; }
    }
    if (!why) {
        std::snprintf(h.status, sizeof(h.status), "hook installed at %s %08lX, %08lX by %s (rev %d)", PADHOOK_DI_SITES,
                      static_cast<unsigned long>(h.site_state), static_cast<unsigned long>(h.site_events), who_s, PADHOOK_REV);
        InterlockedIncrement(&h.installs);
        InterlockedExchange(&h.installed, 1);
        InterlockedExchange(&h.install_lock, 0);
        reg_lock(bus, true);
        reassign_di_owner(bus, -1);                   // a rev 4 client already on the bus drives it at once
        reg_unlock(bus);
        return true;
    }
    std::snprintf(h.status, sizeof(h.status), "%s %s", PADHOOK_DI_SITES, why);
    if (f.state == 2) InterlockedExchange(&h.blocked, 1);
    InterlockedExchange(&h.install_lock, 0);
    return false;
}

// A slot index, or -1. The dispatch goes to this module when nobody drives it, when its revision is
// higher than the driver's, or when the driver's dispatcher is no longer mapped. Rev 4: the same for the DirectInput
// dispatch, and a client may join while only the DirectInput source is live.
inline int padhook_register(PadBus* bus, char const* name = nullptr) {
    using namespace padhook_detail;
    if (!bus) return -1;
    bool const xin = bus->dispatch_ptr && InterlockedCompareExchange(&bus->hook_installed, 0, 0);
    bool const din = bus->di.dispatch_ptr && InterlockedCompareExchange(&bus->di.installed, 0, 0);
    if (!xin && !din) return -1;
    HMODULE const self = self_module();
    volatile LONG& nonce = module_nonce();
    while (nonce == 0) {
        LARGE_INTEGER t {};
        QueryPerformanceCounter(&t);
        InterlockedCompareExchange(&nonce, static_cast<LONG>((InterlockedIncrement(&bus->serial) << 20) ^ t.LowPart), 0);
    }
    reg_lock(bus, true);
    int slot = -1;
    for (int i = 0; i < PADHOOK_MAX_CLIENTS; ++i) {
        if (InterlockedCompareExchange(&bus->clients[i].occupied, 1, 0) != 0) continue;
        PadClient& c = bus->clients[i];
        c.capture = 0;
        c.chord = 0;
        c.di_chord = 0;
        c.di_dispatch = &di_dispatch;
        c.alive_tick = static_cast<LONG>(GetTickCount());
        c.dispatch = &dispatch;
        c.rev = PADHOOK_REV;
        c.module = self;
        c.nonce_at = &nonce;
        c.nonce = nonce;
        std::memset(c.name, 0, sizeof(c.name));
        if (name) std::strncpy(c.name, name, sizeof(c.name) - 1);
        MemoryBarrier();
        InterlockedExchange(&c.occupied, 2);
        slot = i;
        break;
    }
    if (slot >= 0 && xin) {
        LONG const owner = InterlockedCompareExchange(&bus->owner_slot, 0, 0);
        bool take = owner < 0 || owner >= PADHOOK_MAX_CLIENTS;
        if (!take) {
            PadClient const& o = bus->clients[owner];
            take = !can_own(o) || o.dispatch != *bus->dispatch_ptr;
        }
        if (!take) take = static_cast<LONG>(PADHOOK_REV) > InterlockedCompareExchange(&bus->owner_rev, 0, 0);
        if (take) set_owner(bus, slot);
    }
    if (slot >= 0) maybe_take_di(bus, slot);
    reg_unlock(bus);
    return slot;
}

// Leave the bus; also from DllMain(DLL_PROCESS_DETACH) with may_wait = false (loader lock).
inline void padhook_unregister(PadBus* bus, int slot, bool may_wait) {
    using namespace padhook_detail;
    if (!bus || slot < 0 || slot >= PADHOOK_MAX_CLIENTS) return;
    PadClient& c = bus->clients[slot];
    if (c.dispatch != &dispatch) return;
    reg_lock(bus, true);
    InterlockedExchange(&c.capture, 0);
    bool const drove = InterlockedCompareExchange(&bus->owner_slot, 0, 0) == slot;
    if (drove) reassign_owner(bus, slot);
    bool const drove_di = InterlockedCompareExchange(&bus->di.owner, 0, 0) == slot + 1;   // rev 4
    if (drove_di) reassign_di_owner(bus, slot);
    LONG const handed_at = InterlockedCompareExchange(&bus->calls, 0, 0);
    LONG const di_handed_at = InterlockedCompareExchange(&bus->di.calls, 0, 0);
    clear_client(c);
    reg_unlock(bus);
    if (may_wait) {
        for (int spin = 0; spin < 200 && (InterlockedCompareExchange(&bus->in_flight, 0, 0) != 0
                                          || InterlockedCompareExchange(&bus->di.in_flight, 0, 0) != 0); ++spin) Sleep(1);
        bool const wait_x = drove && last_dispatch_tid() != GetCurrentThreadId();
        bool const wait_d = drove_di && last_di_tid() != GetCurrentThreadId();
        if (wait_x || wait_d) {
            DWORD const t0 = GetTickCount();
            while (((wait_x && InterlockedCompareExchange(&bus->calls, 0, 0) - handed_at < 2)
                    || (wait_d && InterlockedCompareExchange(&bus->di.calls, 0, 0) - di_handed_at < 2))
                   && GetTickCount() - t0 < PADHOOK_DRAIN_MS) Sleep(1);
        }
    }
}

// Patch if nobody has, then register. A slot >= 0, or a PADHOOK_JOIN_* code. Rev 4: the DirectInput sites too, and
// the join succeeds while either source is live; it is blocked only when both are.
inline int padhook_join(PadBus* bus, char const* name = nullptr, bool simulate_missing = false) {
    if (!bus) return PADHOOK_JOIN_NO_BUS;
    bool const xin = padhook_ensure_hook(bus, simulate_missing, name);
    bool const din = padhook_ensure_di(bus, simulate_missing, name);
    if (!xin && !din)
        return padhook_state(bus) == PADHOOK_BLOCKED && padhook_di_state(bus) == PADHOOK_BLOCKED ? PADHOOK_JOIN_BLOCKED
                                                                                                 : PADHOOK_JOIN_RETRY;
    int const slot = padhook_register(bus, name);
    return slot >= 0 ? slot : PADHOOK_JOIN_FULL;
}

// Hold or give back the pad. Holding lasts while the client renews it (padhook_alive) at least every
// PADHOOK_HEARTBEAT_MS.
inline void padhook_capture(PadBus* bus, int slot, bool on) {
    if (!bus || slot < 0 || slot >= PADHOOK_MAX_CLIENTS) return;
    PadClient& c = bus->clients[slot];
    if (c.dispatch != &padhook_detail::dispatch) return;
    InterlockedExchange(&c.alive_tick, static_cast<LONG>(GetTickCount()));
    InterlockedExchange(&c.capture, on ? 1 : 0);
}

inline void padhook_alive(PadBus* bus, int slot) {
    if (!bus || slot < 0 || slot >= PADHOOK_MAX_CLIENTS) return;
    PadClient& c = bus->clients[slot];
    if (c.dispatch != &padhook_detail::dispatch) return;
    InterlockedExchange(&c.alive_tick, static_cast<LONG>(GetTickCount()));
}

// rev 2: the buttons that, pressed together, take this client's pad (0: none). The hook keeps the chord from
// the game (see the top of the file); the client still sees it raw, and takes the pad itself.
inline void padhook_chord(PadBus* bus, int slot, unsigned long buttons) {
    if (!bus || slot < 0 || slot >= PADHOOK_MAX_CLIENTS) return;
    PadClient& c = bus->clients[slot];
    if (c.dispatch != &padhook_detail::dispatch) return;
    c.chord = buttons;
    MemoryBarrier();
}

// Is this client's capture still on (it lapses without a heartbeat)?
inline bool padhook_holding(PadBus const* bus, int slot) {
    if (!bus || slot < 0 || slot >= PADHOOK_MAX_CLIENTS) return false;
    return bus->clients[slot].occupied == 2 && bus->clients[slot].capture != 0;
}

// A consistent copy of what the game last read at `index` (false while a write keeps it torn after a few tries).
inline bool padhook_read(PadBus const* bus, int index, PadState* out) {
    if (!bus || index < 0 || index >= PADHOOK_INDEXES || !out) return false;
    for (int tries = 0; tries < 8; ++tries) {
        LONG const a = bus->wseq;
        if (a & 1) { YieldProcessor(); continue; }
        MemoryBarrier();
        std::memcpy(out, const_cast<PadState const*>(&bus->pads[index]), sizeof(PadState));
        MemoryBarrier();
        if (bus->wseq == a) return true;
    }
    return false;
}

inline int padhook_active_clients(PadBus const* bus) {
    if (!bus) return 0;
    int n = 0;
    for (int i = 0; i < PADHOOK_MAX_CLIENTS; ++i) if (bus->clients[i].occupied == 2) ++n;
    return n;
}

// One line for a status display, e.g.
//   "live at FFXiMain's XInput wrapper 10008EA0; 1 client: chronicle r3 (drives)"
//   "live at FFXiMain's XInput wrapper 10008EA0 (the client has not loaded xinputdll.dll yet); 1 client: ..."
//   "live at xinput1_3.dll!XInputGetState 6C1A2B30; 2 clients: chronicle r3 (drives, holds), hud r2"
//   "blocked: FFXiMain's XInput wrapper was not found (a client update?); XInputGetState is not hot-patchable (...)"
inline void padhook_describe(PadBus const* bus, char* out, std::size_t n) {
    if (!out || n == 0) return;
    out[0] = 0;
    int const st = padhook_state(bus);
    if (st == PADHOOK_NO_BUS) { std::snprintf(out, n, "no bus"); return; }
    if (st != PADHOOK_LIVE) {
        std::snprintf(out, n, "%s: %s", st == PADHOOK_BLOCKED ? "blocked" : "waiting", bus->status);
        return;
    }
    int const clients = padhook_active_clients(bus);
    std::size_t len = 0;
    if (bus->route == PADHOOK_ROUTE_WRAPPER) {
        len = static_cast<std::size_t>(std::snprintf(out, n, "live at %s %08lX%s; %d client%s", PADHOOK_WRAPPER,
            static_cast<unsigned long>(bus->site), padhook_shim_loaded(bus) ? "" : " (the client has not loaded " PADHOOK_SHIM " yet)",
            clients, clients == 1 ? "" : "s"));
    } else {
        len = static_cast<std::size_t>(std::snprintf(out, n, "live at %s!XInputGetState %08lX; %d client%s", PADHOOK_DLL,
            static_cast<unsigned long>(bus->site), clients, clients == 1 ? "" : "s"));
    }
    char const* sep = ": ";
    for (int i = 0; i < PADHOOK_MAX_CLIENTS && len < n; ++i) {
        PadClient const& c = bus->clients[i];
        if (c.occupied != 2) continue;
        char nm[17];
        std::memcpy(nm, c.name, 16);
        nm[16] = 0;
        bool const drives = bus->owner_slot == i, holds = c.capture != 0;
        len += static_cast<std::size_t>(std::snprintf(out + len, n - len, "%s%s r%lu%s", sep, nm[0] ? nm : "?",
            static_cast<unsigned long>(c.rev), drives && holds ? " (drives, holds)" : drives ? " (drives)" : holds ? " (holds)" : ""));
        sep = ", ";
    }
}

// rev 4: the DirectInput buttons that take this client's pad together (bit i: button i; 0: none). The hook keeps that
// chord from the game as it keeps the XInput one (padhook_chord); the client still sees it raw, and takes the pad itself.
inline void padhook_di_chord(PadBus* bus, int slot, unsigned long buttons) {
    if (!bus || slot < 0 || slot >= PADHOOK_MAX_CLIENTS) return;
    PadClient& c = bus->clients[slot];
    if (c.dispatch != &padhook_detail::dispatch) return;
    c.di_chord = buttons;
    MemoryBarrier();
}

// rev 4: a consistent copy of what the game last read from its DirectInput pad (false while a write keeps it torn after a
// few tries).
inline bool padhook_di_read(PadBus const* bus, PadDiState* out) {
    if (!bus || !out) return false;
    PadDiHook const& h = bus->di;
    for (int tries = 0; tries < 8; ++tries) {
        LONG const a = h.wseq;
        if (a & 1) { YieldProcessor(); continue; }
        MemoryBarrier();
        std::memcpy(out, const_cast<PadDiState const*>(&h.pad), sizeof(PadDiState));
        MemoryBarrier();
        if (h.wseq == a) return true;
    }
    return false;
}

// rev 4: one line for a status display, e.g.
//   "live at FFXiMain's DirectInput pad reads 1000619A, 100069E3; driven by chronicle r4"
//   "blocked: FFXiMain's DirectInput pad reads were not found: no GetDeviceState read of the pad (a client update?)"
inline void padhook_di_describe(PadBus const* bus, char* out, std::size_t n) {
    if (!out || n == 0) return;
    out[0] = 0;
    int const st = padhook_di_state(bus);
    if (st == PADHOOK_NO_BUS) { std::snprintf(out, n, "no bus"); return; }
    PadDiHook const& h = bus->di;
    if (st != PADHOOK_LIVE) {
        std::snprintf(out, n, "%s: %s", st == PADHOOK_BLOCKED ? "blocked" : "waiting",
                      h.status[0] ? h.status : PADHOOK_DI_SITES " were not looked for yet");
        return;
    }
    LONG const owner = h.owner - 1;
    char nm[17] = "nobody";
    unsigned long rev = 0;
    if (owner >= 0 && owner < PADHOOK_MAX_CLIENTS && bus->clients[owner].occupied == 2) {
        std::memcpy(nm, bus->clients[owner].name, 16);
        nm[16] = 0;
        if (!nm[0]) std::snprintf(nm, sizeof(nm), "?");
        rev = bus->clients[owner].rev;
    }
    if (rev) std::snprintf(out, n, "live at %s %08lX, %08lX; driven by %s r%lu", PADHOOK_DI_SITES,
                           static_cast<unsigned long>(h.site_state), static_cast<unsigned long>(h.site_events), nm, rev);
    else std::snprintf(out, n, "live at %s %08lX, %08lX; nobody drives it", PADHOOK_DI_SITES,
                       static_cast<unsigned long>(h.site_state), static_cast<unsigned long>(h.site_events));
}

#endif  // PADHOOK_H
