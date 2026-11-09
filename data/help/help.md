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
4. **Play.** The keys, the gamepads and the Keypad window work both hand
   controllers. See [Controllers and keys](#controllers-and-keys).
5. **Change the machine.** [Preferences](#preferences) fits the ECS and the
   Intellivoice, picks NTSC or PAL, and sets up FujiNet.

## This Help

The list on the left has a topic for each part of the app; the search box
above it finds every section that has all the words you type. A **Help**
button in Preferences, the Debugger, the Keypad and ECS Keyboard windows and the
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
- **Keypad** (<kbd>F9</kbd>), **ECS Keyboard** (<kbd>F10</kbd>) and
  **Debugger** (<kbd>F12</kbd>).
- **Preferences**, **Help** and **About**.

<!-- only: gnome -->

On GNOME, everything is in the main menu (the button at the right of the
header bar).

<!-- end -->

<!-- only: kde -->

The menu bar has Machine, View, FujiNet, Settings and Help. Settings has
Settings…, and <kbd>F11</kbd> toggles full screen.

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
| Reset to CONFIG | <kbd>Ctrl+R</kbd> |

A key does one thing: giving a key to a new job takes it from the old one.

The app keeps these keys for itself:

| Shortcut | Does |
|---|---|
| <kbd>Ctrl+R</kbd> | Reset to CONFIG |
| <kbd>Ctrl+Shift+H</kbd> | Help |
| <kbd>F9</kbd> | Keypad |
| <kbd>F10</kbd> | ECS Keyboard |
| <kbd>F12</kbd> | Debugger |

<!-- only: gnome kde windows -->

<kbd>F11</kbd> toggles full screen.

<!-- end -->

<!-- only: linux -->

Key combinations with the Super (Windows) key are the desktop's.

<!-- end -->

## The ECS keyboard

With the ECS fitted, Preferences ▸ Input ▸ **ECS Keyboard** routes the
keyboard to the ECS keyboard instead of the hand controllers: the keys then type
on the ECS. FujiNet's CONFIG has no ECS keyboard support yet, so while it is on,
CONFIG's menus can only be driven by a gamepad or the Keypad window.

## Gamepads

Gamepads are picked up as they are plugged in, and can come and go at any
time. A pad plays a hand controller, with its stick or D-pad on the disc and its
buttons on the action buttons. Other buttons can be given jobs in the Keypad
window.

## Changing the controls

The controls are changed in the Keypad window (<kbd>F9</kbd>): click **Map**,
click a control (a key, an action button, a disc direction, or Reset Game or
Reset to CONFIG), then press the keyboard key or the gamepad button to give it.
The control's old key is taken from it. The map is kept in the settings, and only
what differs from the defaults is written.

<!-- context: keypad -->
# Keypad window

The Keypad window (<kbd>F9</kbd>) shows both hand controllers side by side:
the 3 × 4 keypad, the three action buttons and the 16-position disc of each, and
a System row with Reset Game and Reset to CONFIG. It presses them with the
mouse; on the disc, dragging while the button is down rolls the direction round
as a thumb would, and the disc marks all 16 sectors and lights the one under the
pointer.

| With the mouse | Does |
|---|---|
| Press a control | Holds it for as long as the button is down |
| Drag round the disc | Follows the pointer |

The console reads its controllers continuously, so a control is *held*, not
pulsed, as the plastic one is. Keys typed in the Keypad window go to the console
too. **Map** changes the controls (see
[Changing the controls](#changing-the-controls)).

<!-- context: ecskbd -->
# ECS Keyboard window

The ECS Keyboard window (<kbd>F10</kbd>) is an on-screen ECS keyboard: 48
buttons covering every key of the ECS's own keyboard. It works whether or not
Preferences ▸ **ECS Keyboard** is on, so opening it never changes that setting.
**Shift** and **Ctrl** latch, because a mouse cannot hold two buttons to chord a
shifted letter; the next key goes out with them held. It needs the ECS fitted.

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

## Input settings

- **ECS Keyboard**: route the keyboard to the ECS keyboard instead of the hand
  controllers (setting `keyboard_mode`). See
  [The ECS keyboard](#the-ecs-keyboard).

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
`sym_dir` (where the debugger last loaded symbols from) and `bindings` (the
controls that differ from the defaults).

# Debugger

The Debugger (<kbd>F12</kbd>) stops the console and shows what it is doing.
It has five tabs; closing it lets the console go on as it was.

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
| Help | Opens the topic of the tab that shows |

<!-- context: debugger/cpu -->
## CPU

The CP1610 disassembled with symbols; double-click a line to toggle its
breakpoint. Beside it are the registers, and below it a memory view: type an
address or a symbol after **Address:** and press Enter.

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
The file list picks the source shown. The line the machine is on is marked `>`;
double-click a line that has code to toggle a breakpoint on it (`*`).

On this tab Step, Step Over and Step Out (<kbd>F7</kbd>, <kbd>F8</kbd>,
<kbd>Shift+F8</kbd>) go by BASIC line. Step goes into a `GOSUB`, Step Over runs
it, and Step Out runs until the procedure returns. The IntyBASIC runtime is no
line of yours, so stepping runs straight through it. The map has to come from
the ROM that is running.

<!-- context: debugger/stic -->
## STIC

The STIC video chip, decoded from its live state: the BACKTAB, the MOBs
(sprites), the GRAM and GROM cards (page through them with ◀ and ▶), the
palette, and its registers and mode.

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
  read the ECS keyboard. Turn it off, or use a gamepad or the Keypad window.
- **Keys don't reach the console**: a dialog or another window is in front, or
  the key is one the app keeps (see
  [Controllers and keys](#controllers-and-keys)).
- **A game never sees the controller**: a control must be held, not tapped in an
  instant. The Keypad window holds a control as long as the button is down.
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
