This is the text of FujiNet Go Intv's built-in Help (Help ▸ FujiNet Go Intv Help,
or Ctrl+Shift+H; ⌘? on a Mac). The app shows each part only to the frontend it
is about; here you see them all. Every topic below is one page in the app.

# Getting started

FujiNet Go Intv is a Mattel Intellivision with a FujiNet cartridge in its slot.
The console is jzIntv: the CP1610 processor, the STIC video chip, the PSG
sound chip, and the Entertainment Computer System and Intellivoice when you ask
for them. The FujiNet runs inside the app, so CONFIG comes up at once and can
load games from the network.

1. **Import the BIOS.** The console will not start without its Executive and
   Graphics ROMs, and none comes with the app. **Import BIOS…** takes your own
   copies. See [BIOS and ROMs](#bios-and-roms).
2. **Pick a game in CONFIG.** Browse with the disc and the keypad: choose a host
   and an image on it. See [FujiNet](#fujinet).
3. **Or put a cartridge image on the SD folder.** **Import Cartridge to SD…**
   copies `.rom`, `.bin`, `.int`, `.itv`, `.cfg` or `.zip` files there, so
   CONFIG can boot them. See [Cartridges](#cartridges).
4. **Play.** The keys, the gamepads and the Controllers window work both hand
   controllers. See [Controllers and keys](#controllers-and-keys).
5. **Change the machine.** [Preferences](#preferences) fits the ECS and the
   Intellivoice, picks NTSC or PAL, and sets up FujiNet.

## This Help

The list on the left has a topic for each part of the app; the search box
above it finds every section that has all the words you type. A **Help**
button in Preferences, the Debugger, the Controllers and ECS Keyboard windows and the
FujiNet console log opens the topic for what it shows, and
<kbd>Ctrl+Shift+H</kbd> does the same for the window you are in.

<!-- only: gnome kde windows -->

In Preferences, the Debugger and the console log, <kbd>F1</kbd> does too.

<!-- end -->

The same text is on GitHub, as
[data/help/help.md](https://github.com/FujiNetWIFI/fujinet-go-intv-desktop/blob/main/data/help/help.md).

<!-- only: macos -->

On a Mac, the Help menu's search field also finds Help topics.

<!-- end -->

<!-- context: main -->
# The window

The console's picture fills the window. The menus have:

- **Import BIOS…** and **Import BIOS from a Folder…**: see
  [BIOS and ROMs](#bios-and-roms).
- **Import Cartridge to SD…** and **Open SD Folder**: see
  [Cartridges](#cartridges).
- **Reset Game** and **Reset to CONFIG**.
- **FujiNet Configuration…** and **FujiNet Console Log…**.
- **Controllers** (<kbd>F9</kbd>), **ECS Keyboard** (<kbd>F10</kbd>) and
  **Debugger** (<kbd>F12</kbd>).
- **Preferences**, **Help** and **About**.

<!-- only: gnome -->

On GNOME, everything is in the main menu (the button at the right of the
header bar).

<!-- end -->

<!-- only: kde -->

The menu bar has Machine, View, FujiNet, Settings and Help. Settings has
Settings….

<!-- end -->

<!-- only: windows -->

The menu bar has Machine, FujiNet, View, Settings and Help. Settings has
**Settings…** and **System ROMs…**, and View has Fullscreen (<kbd>F11</kbd>).

<!-- end -->

<!-- only: macos -->

On a Mac, Settings and Quit are in the app menu, and the BIOS and cartridge
items are in the Machine menu.

<!-- end -->

<!-- context: nobios -->
## No BIOS

Until the Executive and Graphics ROMs are imported, the window shows **No
Intellivision BIOS**, with an **Import BIOS…** button. Mattel's ROMs are
copyrighted, so the app does not include them. See
[BIOS and ROMs](#bios-and-roms).

## Reset

- **Reset Game** (<kbd>Backspace</kbd>) restarts the cartridge in the slot, the
  game or CONFIG, as the console's own reset does.
- **Reset to CONFIG** (<kbd>Ctrl+R</kbd>) boots FujiNet's CONFIG again.

# Controllers and keys

The Intellivision has two hand controllers: a 16-position disc, three action
buttons (top, lower left and lower right) and a keypad of 1 to 9, Clear, 0 and
Enter.

## Default keys

Left controller (the hand controller on the left):

| Left controller | Keys |
|---|---|
| Disc | the arrow keys, or <kbd>K</kbd> <kbd>I</kbd> <kbd>J</kbd> <kbd>M</kbd> with <kbd>O</kbd> <kbd>U</kbd> <kbd>N</kbd> <kbd>,</kbd> for the diagonals |
| Keypad | <kbd>Keypad 7</kbd> to <kbd>Keypad 3</kbd> for 1 to 9, <kbd>Keypad 0</kbd> for Clear, <kbd>Keypad .</kbd> for 0 and <kbd>Keypad Enter</kbd> |
| Top button | <kbd>Right Shift</kbd> |
| Lower left | <kbd>Right Alt</kbd> |
| Lower right | <kbd>Right Ctrl</kbd> |

Right controller:

| Right controller | Keys |
|---|---|
| Disc | <kbd>D</kbd> <kbd>E</kbd> <kbd>S</kbd> <kbd>X</kbd> with <kbd>R</kbd> <kbd>W</kbd> <kbd>Z</kbd> <kbd>C</kbd> for the diagonals |
| Keypad | the number row: <kbd>1</kbd> to <kbd>9</kbd>, <kbd>-</kbd> for Clear, <kbd>0</kbd> and <kbd>=</kbd> for Enter |
| Top button | <kbd>Left Shift</kbd> |
| Lower left | <kbd>Left Alt</kbd> |
| Lower right | <kbd>Left Ctrl</kbd> |

The action buttons are crossed as jzIntv crosses them: the right-hand modifier
keys press the left controller's buttons, and the left-hand keys the right
one's.

| Console | Key |
|---|---|
| Reset Game | <kbd>Backspace</kbd> |
| Reset to CONFIG | <kbd>Escape</kbd>, and <kbd>Ctrl+R</kbd> on the menu |

A key does one thing: giving a key to a new job takes it from the old one.

The app keeps these keys for itself:

| Shortcut | Does |
|---|---|
| <kbd>Ctrl+R</kbd> | Reset to CONFIG |
| <kbd>Ctrl+Shift+H</kbd> | Help |
| <kbd>F9</kbd> | Controllers |
| <kbd>F10</kbd> | ECS Keyboard |
| <kbd>F12</kbd> | Debugger |

<!-- only: gnome windows -->

<kbd>F11</kbd> toggles full screen.

<!-- end -->

<!-- only: linux -->

Key combinations with the Super (Windows) key are the desktop's.

<!-- end -->

## The ECS keyboard

With the ECS fitted, Preferences ▸ Input ▸ **ECS Keyboard** routes the
keyboard to the ECS keyboard instead of the hand controllers: the keys then type
on the ECS. FujiNet's CONFIG has no ECS keyboard support yet, so while it is on,
CONFIG's menus can only be driven by a gamepad or the Controllers window.

## Gamepads

Gamepads are picked up as they are plugged in, and can come and go at any
time. A pad plays a hand controller, with its stick or D-pad on the disc and its
buttons on the action buttons. Other buttons can be given jobs in the Controllers
window. A pad's choice of hand controller is remembered, and comes back when it
is plugged in again. Gamepad trigger buttons can be given jobs too.

## Changing the controls

The controls are changed in the Controllers window (<kbd>F9</kbd>): click
**Map**, click a control (a key, an action button, a disc direction, or Reset
Game, Reset to CONFIG or Stop in debugger), then press the keyboard key or the
gamepad button to give it. The control's old key is taken from it. **Clear**
takes the key and button off the control you pick, and **Defaults** puts every
control back. **Show bindings** prints each control's key on the picture. The map
is kept in the settings, and only what differs from the defaults is written.
<kbd>F9</kbd>, <kbd>F10</kbd>, <kbd>F11</kbd> and <kbd>F12</kbd> belong to the app
and cannot be given to a control.

There are 127 controls in all: for each of the four hand controllers (the two
on the console and the ECS's two) 12 keypad keys, 3 action buttons and the 16
disc positions, which is 124, and the three system controls: Reset Game, Reset
to CONFIG and Stop in debugger. The controllers on the ECS only play when the
ECS is fitted.

<!-- context: controllers -->
# Controllers window

The Controllers window (<kbd>F9</kbd>) draws the two hand controllers of an
Intellivision II, with the console's reset button under them, and presses them
with the mouse. It floats on its own at a fixed size. With the ECS fitted, the
ECS's two hand controllers appear beside them (an ECS change needs a restart).
Each controller shows its 3 × 4 keypad (the keys are outlined in red, as on the
plastic), the 16-position disc, and the four side buttons: the two upper ones
are the same Top button, so either presses it. The controls that are held light
up in green, whoever holds them: the keyboard, a gamepad or the mouse.

| With the mouse | Does |
|---|---|
| Press a key or a side button | Holds it for as long as the button is down |
| Press and drag on the disc | Holds the direction under the pointer, one of 16 |
| Press the console's RESET | Resets the game |

Under the picture are buttons for **Reset to CONFIG** and **Stop in debugger**,
which the console itself does not have. Each gamepad plugged in has a row to pick
which hand controller it plays, and **Map**, **Clear**, **Defaults** and **Show bindings**
change and show the controls (see
[Changing the controls](#changing-the-controls)). The action buttons are crossed
as jzIntv crosses them (see [Controllers and keys](#controllers-and-keys)).

The console reads its controllers continuously, so a control is *held*, not
pulsed, as the plastic one is. Keys typed in the Controllers window go to the
console too. Switching to another window lets go of everything the mouse holds.

<!-- context: ecskbd -->
# ECS Keyboard window

The ECS Keyboard window (<kbd>F10</kbd>) is an on-screen ECS keyboard drawn like
the real one: 48 keys, with the shifted symbol above each, two Shift keys and a
white Return. It works whether or not Preferences ▸ **ECS Keyboard** is on, so
opening it never changes that setting. **Shift** and **Ctrl** latch, because a
mouse cannot hold two buttons to chord a shifted letter: a latched key stays lit
until you press it again, and the next key goes out with it held. Keys typed on
the keyboard light their keys too. It needs the ECS fitted.

# Machines

There is one console, the Mattel Intellivision, with a CP1610 processor, and
Preferences ▸ Machine fits what goes with it:

- **ECS**: the Entertainment Computer System, a second controller pair, a
  keyboard and extra sound. *Auto* lets jzIntv decide from the cartridge, *Off*
  and *On* decide for you. It needs `ecs.bin` imported (see
  [BIOS and ROMs](#bios-and-roms)); without it the choice is greyed out.
- **Intellivoice**: the speech synthesis peripheral, *Auto*, *Off* or *On*.
- **Video Standard**: *NTSC* (the default) or *PAL*.

# BIOS and ROMs

Mattel's Executive and Graphics ROMs are copyrighted, so none comes with the app,
and the console will not start without them.

## Importing them

**Import BIOS…** takes your own copies: `exec.bin` and `grom.bin` (and `ecs.bin`
for the ECS), a zip such as MAME's `intv` set, or a folder that holds them
(**Import BIOS from a Folder…**). It is in the Machine menu, on the No
Intellivision BIOS page and on the ROMs page of Preferences. They are copied
into the app's `roms` folder (see [Files and folders](#files-and-folders)).

# Cartridges

A cartridge is an image file. The cartridge in the slot is always the FujiNet
one: a game is loaded into it from CONFIG.

## Import Cartridge to SD

**Import Cartridge to SD…** copies `.rom`, `.bin`, `.int`, `.itv`, `.cfg` or
`.zip` files into FujiNet's SD folder, so CONFIG can list and boot them. Several
files can be picked at once, and the message says where they are. **Open SD
Folder** shows the folder in your file manager.

# FujiNet

The FujiNet is FujiNet's own firmware, running inside the app, and the
cartridge in the slot is its connection to the console. The cartridge dials
FujiNet over a local socket (BoIP, FujiBus over TCP), so the app starts FujiNet
first and waits for it.

## CONFIG

CONFIG is FujiNet's program for choosing what to run, driven by the disc and the
keypad:

- **Hosts** are where games come from: the SD folder and servers on the
  network.
- Pick a host and an image on it. FujiNet loads it into the cartridge and the
  console starts it.
- **Reset to CONFIG** brings CONFIG back.

## The FujiNet menu

- **Configuration…** opens FujiNet's own web page, at `http://127.0.0.1:64003/`.
  Only this computer can reach it.
- **Console Log…** shows what FujiNet prints. See
  [The console log](#the-console-log).

<!-- context: fujilog -->
## The console log

The FujiNet Console Log window shows FujiNet's own messages as they come,
following the end while you are at the bottom.

## An external FujiNet

Preferences ▸ FujiNet ▸ **Use external FujiNet** skips the built-in FujiNet and
dials another FujiNet instance at the **Address:port** you give, for example
`127.0.0.1:65503`. Start that instance listening there (its `[BOIP]` port).
Configuration and the console log belong to the built-in FujiNet, so they say the
FujiNet is external; use the other instance's own. The Debugger's FujiNet tab
still shows the traffic.

## Behind the scenes

- FujiNet's BoIP listener is on port 65503 and its web UI on 64003, on this
  computer only. They are the app's own, so another FujiNet on the same computer
  does not collide with it.
- FujiNet's settings and its SD folder are in the app's `fujinet` folder.

# Preferences

Preferences set up the machine. The Machine and FujiNet settings apply by
restarting the session when Preferences closes, and FujiNet stays connected. The
Input setting applies at once. The settings are one file, shared by the GNOME and
KDE apps on the computer.

<!-- only: macos -->

On a Mac this window is called **Settings** (⌘,).

<!-- end -->

<!-- only: windows -->

On Windows, **Settings…** and **System ROMs…** in the Settings menu are the two
halves of this window.

<!-- end -->

<!-- context: prefs/Machine -->
## Machine settings

- **ECS**: *Auto*, *Off* or *On* (setting `ecs`).
- **Intellivoice**: *Auto*, *Off* or *On* (setting `ivoice`).
- **Video Standard**: *NTSC* or *PAL* (setting `video_standard`).

## FujiNet settings

- **Use external FujiNet**: skip the built-in FujiNet and dial another instance
  (setting `fujinet_external`).
- **Address:port**: where to dial, for example `127.0.0.1:65503` (see
  [An external FujiNet](#an-external-fujinet); setting
  `fujinet_external_addr`).

<!-- context: prefs/ROMs -->
## ROMs settings

The ROMs page lists the ROMs the console needs and whether each is imported:

- **Executive ROM (exec.bin)** and **Graphics ROM (grom.bin)**: the System ROMs.
- **ECS ROM (ecs.bin)**: the Entertainment Computer System.
- Each shows *Imported* or *Not imported*; **Import…** and **Import a Folder…**
  add your own copies (see [Importing them](#importing-them)).

## The settings file

The settings file is `settings.ini` (see [Files and folders](#files-and-folders)).
Each setting above names its key. The others are `cart` (the opened cartridge),
`sym_dir` (where the debugger last loaded symbols from), `controllers_show_bindings`
and `bindings` (the controls that differ from the defaults).

<!-- context: prefs/Input -->
## Input settings

- **ECS Keyboard**: route the keyboard to the ECS keyboard instead of the hand
  controllers (setting `keyboard_mode`). See
  [The ECS keyboard](#the-ecs-keyboard).
- **Enable gamepads**: poll USB and Bluetooth gamepads (setting
  `enable_gamepad`). A change applies when the app restarts.
- **Gamepads** lists each one plugged in, and which hand controller it plays.
  A new gamepad plays the left controller, the next the right one; choose *Left*,
  *Right*, or with the ECS fitted *ECS Left* or *ECS Right*, or *Automatic*. The
  choice is remembered for that gamepad (setting `pad_assign`) and comes back
  when it is plugged in again.
- **Reset Controls** puts every key and button back to its default.
- **Open Controllers** shows the Controllers window.

# Debugger

The Debugger (<kbd>F12</kbd>) stops the console and shows what it is doing.
It has thirteen tabs; closing it lets the console go on as it was.

| Button | Does |
|---|---|
| Pause (<kbd>F5</kbd>) | Stops before the next instruction, or runs on |
| Step (<kbd>F7</kbd>) | One instruction |
| Step Over (<kbd>F8</kbd>) | Over a subroutine call |
| Step Out (<kbd>Shift+F8</kbd>) | On until the routine returns |
| Clear Breakpoints | Removes every breakpoint |
| Break at | A symbol name or hex address, then Enter |
| Load Symbols | Reads an `as1600 -s` symbol file |
| Clear Symbols | Drops loaded symbols, keeping the built-ins |
| Pop Out | Moves the tab that shows into a window of its own; **Dock** in that window puts it back |
| Help | Opens the topic of the tab that shows |

<!-- context: debugger/console -->
## Console

A prompt for everything the buttons do, and more. Type a command and press
Enter; <kbd>Tab</kbd> completes and the arrows recall earlier lines. `help`
lists them all.

| Command | Does |
|---|---|
| `step` `over` `out` `run` `stop` | Run control |
| `line` `overline` `lineout` | The same by BASIC line |
| `runto` | Runs to an address |
| `break` `bpr` `bpw` `bpa` | Execute, Read, Write and Access breakpoints, with an optional `if` condition |
| `delete` `enable` `clear` | Manage them |
| `disable` `breaks` `hits` | Likewise |
| `print` `regs` `set` | Evaluate an expression, show or set the registers |
| `mem` `poke` `disasm` | Look at memory, change it |
| `find` `trace` | Search; show the trace |
| `watch` `unwatch` | Variables to keep an eye on |
| `symbols` `label` `labels` | Symbols |
| `source` `locals` | IntyBASIC line and variables |
| `map` `io` `cart` | Memory map, controllers and ports, cartridge |
| `sound` `video` | The PSGs; the STIC |
| `save` | Writes `ram`, `gram`, `regs` or `trace` to a file |

Expressions take numbers (`$5000`, `0x5000`, `20480`), registers (`r0` to `r7`,
`pc`, `sp`), flags (`s z o c i d`), symbols, `[addr]` for the word at an
address, `frame` and `cycles`. A condition on an access can also use `value`,
`addr` and `old`, for instance `bpw $0310 if value > old`.

<!-- context: debugger/disassembly -->
## Disassembly

The CP1610 disassembled with symbols. The instruction the machine is on is a
green bar, and a breakpoint is a red dot in the margin; click a line to toggle
its breakpoint. The registers are in the strip under every tab: click one
and type a new value, then Enter, while the machine is stopped.

<!-- context: debugger/memory -->
## Memory

A hex view of memory: type an address or a symbol after **Address:** and press
Enter. The rows are labelled with the hardware that answers there (STIC, PSG,
EXEC ROM and so on). A variable, symbol or range you go to is highlighted, as is
a search hit.

**Search** finds hex words (`02A0 0268`) or text, one character per word, from
the highlight onward; **Find Next** and **Find Previous** step through the
matches.

**Poke** writes a value at an address while the machine is stopped: four hex
digits, or two in the 8-bit areas (scratch RAM, GRAM, GROM, ECS RAM). Values
are words, so a 16-bit poke to system RAM keeps all sixteen bits.

<!-- context: debugger/basic -->
## BASIC

Your IntyBASIC source, a line at a time. **Import File** loads a `.bas` file and
every file it `INCLUDE`s; **Import Folder** loads every `.bas` under a folder.
Which address is which line comes from the assembler's map, which the import
looks for beside the source (`game.smap` or `game.lst`). Build with:

```
intybasic game.bas game.asm
as1600 -o game -j game.smap -l game.lst -s game.sym game.asm
```

If there is no map, **Load Map** reads one; a `.sym` beside it is loaded too.
The file list picks the source shown. The line the machine is on is a green bar;
click a line that has code to toggle a breakpoint on it (a red dot).

On this tab Step, Step Over and Step Out (<kbd>F7</kbd>, <kbd>F8</kbd>,
<kbd>Shift+F8</kbd>) go by BASIC line. Step goes into a `GOSUB`, Step Over runs
it, and Step Out runs until the procedure returns. The IntyBASIC runtime is no
line of yours, so stepping runs straight through it. The map has to come from
the ROM that is running.

<!-- context: debugger/breakpoints -->
## Breakpoints

Every breakpoint and watchpoint. **Add** one with a type, an address, and a
condition, an expression such as `value > old` or `r1 == 5`:

- **Execute** stops before an instruction runs.
- **Read**, **Write** and **Access** (both) stop *after* the instruction that
  touched the words, and the status line says what it did, for instance
  `wrote $010C = $1D (was $1C)`; the values are words, two hex digits only in the 8-bit areas. They see the CPU's data accesses only: an
  instruction fetch is not one, so use Execute to catch code.

The address can be a number (`$010C`), a range (`$0100-$010F`), a symbol, or an
IntyBASIC variable: `x`, `#score`, or an array element `a(3)`. The box beside
each entry enables or disables it (a disabled one is a hollow dot in the
margin), and the list shows how many times each has fired.

<!-- context: debugger/watches -->
## Watches

Variables to keep an eye on, with their live values while the machine is
stopped. Type an address, a symbol, an IntyBASIC variable (`x`, `#score`,
`a(3)`) or a range after **Watch** and press Enter. **Show in Memory** jumps to
it in the Memory tab, and **Break on Change** adds a Write breakpoint that
fires when its value changes.

<!-- context: debugger/cart -->
## Cart & I/O

The cartridge that is running (its file, its title if it carries one, and
whether the FujiNet mailbox, the ECS and the JLP are in play) and the memory
map: every range the machine answers in, whether it is readable and writable,
and what answers there (STIC, PSG, the EXEC ROM, your cartridge, the FujiNet
mailbox). Stop the machine to read the map. Below it are the two hand
controllers (disc, keypad and side buttons), the PSG I/O ports, the ECS
keyboard rows, the Intellivoice and the JLP.

<!-- context: debugger/trace -->
## Trace

Every instruction the CPU runs, live, as the machine runs. **Record** turns it
on: from then on each instruction is kept, with its address, the registers
after it and the flags (the machine runs a little slower while it records).
**Branches only** shows just the branches, jumps and calls, and **In range**
the instructions in a range (`lo-hi` or a symbol). **Clear** empties it.

<!-- context: debugger/history -->
## History

The same record as the Trace tab, shown when the machine stops: what it did
just before it hit a breakpoint or a watch. It is the instructions leading up
to the stop, newest at the bottom.

<!-- context: debugger/video -->
## Video

The STIC, with the picture on the left and its sections on the right. The STIC
draws the whole frame at once, so the console keeps what it was given for every
frame.

| View | Shows |
|---|---|
| Display | The last finished frame, as the screen showed it |
| Decoded | One source at a time: the BACKTAB cards, the MOBs (sprites), the GRAM and GROM card sheet (page with ◀ and ▶) or the palette |
| Beam | Stopped only: where the beam was in this frame |
| Values | The BACKTAB words as hex |

**Zoom** goes from 1 to 16 times, or **Fit**. **Overlays** switch on the card
grid, MOB boxes, the border and blanking, the beam and a heat tint for the
words written lately.

Point at a pixel to see it in **Pixel**: the card or MOB, its address and word.
Click to pin it, click again to let go. **Break on writes** stops the machine
when anything writes that BACKTAB word, GRAM card byte or MOB register.

**Registers** shows the STIC registers with how long ago each changed,
**Colours** the sixteen colours and the colour stack, **Lines** the twelve card
rows, and **Timing** the frame and scanline length for NTSC or PAL.

<!-- context: debugger/sound -->
## Sound

The AY-3-8914 sound chips, read while the machine is stopped: the base PSG and,
with the ECS, the second one. Each shows its three tone periods and volumes,
the noise period, the mixer (which channels have tone and noise on), the
envelope and the I/O ports.

<!-- context: debugger/symbols -->
## Symbols

Every known symbol, the built-ins and the ones you loaded. **Filter** narrows
the list, and activating a row goes to that address.

<!-- context: debugger/fujinet -->
## FujiNet packets

Every FujiBus packet on the cartridge's link, SLIP-decoded, both ways, recorded
from the moment the machine starts. **Clear** empties the list. Beside it is
FujiNet's mailbox.

# Files and folders

The app keeps its settings in one file, and everything else in its data
folder:

<!-- only: linux -->

On Linux, the settings are `~/.config/fujinet-go-intv/settings.ini` and
the data is in `~/.local/share/fujinet-go-intv` (or under
`$XDG_CONFIG_HOME` and `$XDG_DATA_HOME`). The GNOME and KDE apps share them.
As a Flatpak, they are under `~/.var/app/`, in the app's own folder.

<!-- end -->

<!-- only: windows -->

On Windows, the settings are `%APPDATA%\fujinet-go-intv\settings.ini` and
the data is in `%LOCALAPPDATA%\fujinet-go-intv`.

<!-- end -->

<!-- only: macos -->

On a Mac, the settings are `~/.config/fujinet-go-intv/settings.ini` and
the data is in `~/.local/share/fujinet-go-intv`.

<!-- end -->

In the data folder:

| Folder | Has |
|---|---|
| `roms` | the imported ROMs |
| `fujinet` | FujiNet's settings, its SD folder (`SD`) and its data |

# Troubleshooting

- **No Intellivision BIOS**: the Executive and Graphics ROMs are not imported. See
  [BIOS and ROMs](#bios-and-roms).
- **The ECS is greyed out**: `ecs.bin` is not imported.
- **link down**: FujiNet is off, not started yet, or the external address is
  wrong. Check Preferences ▸ FujiNet.
- **CONFIG will not take the keyboard**: ECS Keyboard is on, and CONFIG does not
  read the ECS keyboard. Turn it off, or use a gamepad or the Controllers window.
- **Keys don't reach the console**: a dialog or another window is in front, or
  the key is one the app keeps (see
  [Controllers and keys](#controllers-and-keys)).
- **A game never sees the controller**: a control must be held, not tapped in an
  instant. The Controllers window holds a control as long as the button is down.
- **A port is taken**: another program is using 65503 or 64003. Close the other
  FujiNet, or point the app at it with **Use external FujiNet**.

# About and licences

FujiNet Go Intv is free software under the GNU General Public License, version 3
or later. Copyright © 2026 Thomas Cherryhomes.

- **jzIntv** (the Intellivision emulator, GPL-2.0-or-later) is the console.
- **The FujiNet firmware** (GPL-3.0-or-later) runs inside the app.
- No Mattel ROM is part of the published app; you import your own.
  "Intellivision" is a trademark of its owner, used to name the machine
  emulated; this project is not affiliated with it.

[fujinet.online](https://fujinet.online/) ·
[fujinet-go-intv-desktop](https://github.com/FujiNetWIFI/fujinet-go-intv-desktop)
