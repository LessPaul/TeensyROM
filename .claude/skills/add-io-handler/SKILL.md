---
name: add-io-handler
description: Checklist for adding a new cartridge/peripheral IO Handler to TeensyROM's MinimalBoot firmware (a new IOH_<Name>.c under Source/Teensy/MinimalBoot/Common/IO_Handlers/). Use this whenever the user wants to add, wire up, integrate, or review a new IO handler, freezer cartridge, or peripheral emulation for TeensyROM/MinimalBoot — including when reviewing a handler someone else contributed before it's registered. Covers the three files that must be updated in sync, and the two hardware-emulation gotchas (stale reused-globals at init, freeze-completion bypassing side effects) found while integrating the Final Cartridge III handler.
---

# Adding an IO Handler to TeensyROM's MinimalBoot

A new piece of hardware (cartridge, freezer, peripheral) plugs into MinimalBoot as an
`stcIOHandlers` struct of function pointers. Most of the work is mechanical wiring across a
few files that must stay in sync; the rest is a handful of hardware-emulation conventions
that are easy to get subtly wrong because the compiler won't catch them.

## 1. Write the handler file

Create `Source/Teensy/MinimalBoot/Common/IO_Handlers/IOH_<Name>.c`. Use an existing handler
of the same cartridge *class* as your template rather than starting from a blank page —
`IOH_RetroReplay.c` and `IOH_SuperSnapshotV5.c` are the reference implementations for
freezer-style carts with a control register and a physical freeze button.

Shape:
```c
void InitHndlr_<Name>();
void IO1Hndlr_<Name>(uint8_t Address, bool R_Wn);
// ...other handler prototypes you need

stcIOHandlers IOHndlr_<Name> =
{
  "<Name>",              // Name of handler, IOHNameLength max
  &InitHndlr_<Name>,     // Called once at handler startup
  &IO1Hndlr_<Name>,      // IO1 R/W handler
  &IO2Hndlr_<Name>,      // IO2 R/W handler
  NULL,                  // ROML Read handler (leave NULL if unused)
  NULL,                  // ROMH Read handler (leave NULL if unused)
  NULL,                  // Polled in main routine (leave NULL if unused)
  &CycleHndlr_<Name>,    // called at the end of EVERY c64 cycle (leave NULL if unused)
};
```
Any slot you don't need stays `NULL` — don't stub out empty functions for them.

## 2. Register it in three places, same relative order

There are explicit "Synch order/qty" comments in the code warning that these must match —
add your handler in the same position (relative to neighboring handlers) in all three:

- **`Source/Teensy/MinimalBoot/Common/Menu_Regs.h`** — add `IOH_<Name>` to the
  `enumIOHandlers` enum.
- **`Source/Teensy/MinimalBoot/Common/IOHandlers.h`** — add both the
  `#include "IO_Handlers/IOH_<Name>.c"` line and the `&IOHndlr_<Name>` entry in the
  `IOHandler[]` array, at the same position as the enum.
- **`Source/Teensy/MinimalBoot/Common/DriveDirLoad.h`** — if it's a CRT-detectable
  cartridge type, add a `(uint16_t)Cart_<Name>, IOH_<Name>,` row to `HWID_IOH_Assoc[]`
  (the `Cart_<Name>` constant is usually already `#define`d nearby from CRT-format docs).

**Include-order matters** if your handler references another handler's global (see the
reused-global pattern below) — your `#include` must come after the file that declares that
global, since these are all textually included into one translation unit.

If the cartridge needs boot/CRT-type detection help (e.g. distinguishing which freezer cart
produced a given CRT file), that logic lives in `Source/Teensy/FileParsers.ino`'s
`SetTypeFromCRT()`, typically behind the same feature-flag `#ifdef` as the handler
registration.

## 3. Watch for these two things (found integrating Final Cartridge III)

### Reused globals must be reset in *your* Init, not just passed as a parameter

To save RAM, a handler may alias an existing global from another handler that's never
active at the same time, e.g. `#define FC3_ControlReg RR_StatusReg`, with a comment
explaining the reuse. This is a legitimate, established pattern — but MinimalBoot's
dispatcher (`Source/Teensy/IOHandlers.ino` and `Source/Teensy/MinimalBoot/Min_DriveDirLoad.ino`)
only calls the new handler's `InitHndlr()` when switching handlers. It never clears shared
globals in between, so whatever the *previous* handler left in that memory is still there
when yours starts.

If your init logic does something like `ProcessControlReg(0); // HW power-on reset` and
that function reads the persisted alias for anything (a status/hidden bit, a mode flag),
passing `0` as a function *parameter* doesn't touch the alias itself — you must explicitly
assign it too (`FC3_ControlReg = 0;`) before or as part of that call. Otherwise the first
thing your handler does on load is act on stale garbage from whatever ran before it.
Trace every read of a reused global back to where it's written, and make sure your Init
path covers all of them, not just the ones a fresh parameter conveniently overwrites.

### Freeze-completion should reuse your control-register function, not hand-set pins

Freezer cartridges share a button state machine: `CycleHndlr_<Name>(bool R_Wn)` runs every
C64 cycle, using shared globals `CycleCountdown`, `CycCntFreeze`, `CycCntNumWr` — a button
press sets `CycleCountdown = CycCntFreeze`, the handler waits for a read (asserting NMI),
then counts down `CycCntNumWr` consecutive writes before completing the transition into the
frozen state.

At that completion point, route the transition through your own control-register-processing
function with a synthetic value representing the end state — e.g. RetroReplay does
`ProcessRRControlReg(RR_CR_nGAME | RR_CR_EXROM)` — rather than hand-setting individual pins
(`SetNMIDeassert; SetGameAssert;`). The control-register function is where *all* the side
effects of a mode change live (bank switching, ROM pointer updates, and — critically for
Ultimax-mode freezers — toggling `EmulateVicCycles`/disabling `IRQ_ENET`/`IRQ_PIT`, which
prevents graphics tearing in the freezer menu). Hand-setting a couple of pins reproduces the
visible pin state but silently skips everything else that function does, which is exactly
the kind of bug that only shows up as a glitch at the exact moment of freezing.

## 4. Other things worth double-checking, not just copying

- **Bit polarity**: hardware pins are typically active-low. The codebase's comment
  convention for the EXROM/GAME combinations is `//rtBin8kHi or None` /
  `//rtBin16k or 8kLo` — reuse this shorthand so the four combinations stay easy to read
  across handlers.
- **Bank masking**: `BankNum` is set from control-register bits and indexes
  `CrtChips[BankNum].ChipROM`. The bit-mask width must exactly match how many banks the
  ROM size implies for *your* cartridge's variants (e.g. a 4-chip 64K variant needs `& 3`,
  a 16-chip 256K variant needs `& 15`) — derive this from the actual ROM size math for your
  hardware, don't copy a mask from a handler with a different bank count.

## Before calling a new handler done

Re-read your `InitHndlr_<Name>()` against every global it reads (including reused/aliased
ones) and confirm none of them can hold a stale value from whatever handler ran previously.
Re-read your `CycleHndlr_<Name>()`'s freeze-completion branch and confirm it transitions
state through the same function a real register write would use, not a hand-picked subset
of pins.
