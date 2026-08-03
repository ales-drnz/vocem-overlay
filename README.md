# Vocem Overlay

#### Discord voice overlay drawn inside the game, for Linux.

[![](https://img.shields.io/badge/license-BSD--3--Clause-blue.svg?style=for-the-badge)](LICENSE)
[![](https://img.shields.io/badge/Vulkan-implicit%20layer-red.svg?style=for-the-badge&logo=vulkan&logoColor=white)]()
[![](https://img.shields.io/badge/OpenGL-interposer-5586A4.svg?style=for-the-badge&logo=opengl&logoColor=white)]()
[![](https://img.shields.io/badge/Arch-PKGBUILD-1793D1.svg?style=for-the-badge&logo=archlinux&logoColor=white)](#installation)
[![](https://img.shields.io/github/stars/ales-drnz/vocem-overlay?style=for-the-badge&logo=github&logoColor=white)](https://github.com/ales-drnz/vocem-overlay)

<table>
<tr>
<td valign="middle" width="90"><img src="https://raw.githubusercontent.com/ales-drnz/vocem-overlay/master/imgs/vocem-overlay.png" width="70" alt="logo"></td>
<td valign="middle"><code>vocem-overlay</code> shows who is in your Discord voice channel, who is speaking, and the direct messages you receive, <b>inside the game's own picture</b> rather than in a window on top of it. A Vulkan layer and an OpenGL interposer put it there, and a small daemon holds the Discord connection. It is not a Discord client and is not affiliated with Discord Inc.</td>
</tr>
</table>

---

## Installation

### Arch Linux and derivatives

```bash
git clone --recurse-submodules https://github.com/ales-drnz/vocem-overlay.git
cd vocem-overlay/packaging
makepkg -si
```

Then **log out and back in**. The OpenGL half reaches games through a variable the
session sets at login, so anything already running, Steam included, does not have
it yet.

`PKGBUILD` builds from the tagged release. To build your clone as it stands, use
`makepkg -p PKGBUILD.local -si`.

### Anywhere else, without root

```bash
git clone --recurse-submodules https://github.com/ales-drnz/vocem-overlay.git
cd vocem-overlay
cmake -S . -B build -G Ninja -DVOCEM_ENABLE_BY_DEFAULT=ON
cmake --build build
cmake --install build --prefix ~/.local
cp packaging/systemd/vocemd.service ~/.config/systemd/user/
systemctl --user enable --now vocemd
```

For 32-bit games, see [32-bit games](#7-32-bit-games).

## Requirements

| | Minimum | Notes |
| :--- | :--- | :--- |
| **Desktop** | any Linux with X11 or Wayland | the overlay is inside the game, so the compositor is not involved |
| **Graphics** | Vulkan 1.0, or OpenGL 3.0 and OpenGL ES 2.0 | `vulkan-icd-loader`. Both paths can be active at once |
| **Discord** | the desktop client, running | the browser client exposes no local RPC socket |
| **Build** | CMake 3.20, C++17, Ninja | plus `vulkan-headers`, `qt6-base`, `qt6-declarative` and `curl` |
| **32-bit games** | a `-m32` toolchain and 32-bit Vulkan | `lib32-gcc-libs` and `lib32-vulkan-icd-loader` on Arch |

---

## Contents

* [Visuals](#visuals)
* [Features](#features)
* [Quick start](#quick-start)
* [Guide](#guide)
  <details>
  <summary><a href="#1-getting-it-into-a-game"><b>1. Getting it into a game</b></a></summary>

  * [1.1 Vulkan](#11-vulkan)
  * [1.2 OpenGL](#12-opengl)
  * [1.3 Which applications it appears in](#13-which-applications-it-appears-in)
  * [1.4 Games with anti-cheat](#14-games-with-anti-cheat)

  </details>

  <details>
  <summary><a href="#2-the-settings-window"><b>2. The settings window</b></a></summary>

  * [2.1 Position and size](#21-position-and-size)
  * [2.2 Appearance](#22-appearance)
  * [2.3 The configuration file](#23-the-configuration-file)

  </details>

  <details>
  <summary><a href="#3-the-daemon"><b>3. The daemon</b></a></summary>

  * [3.1 Authorisation](#31-authorisation)
  * [3.2 Pictures](#32-pictures)
  * [3.3 Reading it from a terminal](#33-reading-it-from-a-terminal)

  </details>

  <details>
  <summary><a href="#4-environment-variables"><b>4. Environment variables</b></a></summary>
  </details>

  <details>
  <summary><a href="#5-what-it-costs-a-game"><b>5. What it costs a game</b></a></summary>
  </details>

  <details>
  <summary><a href="#6-opengl-games"><b>6. OpenGL games</b></a></summary>
  </details>

  <details>
  <summary><a href="#7-32-bit-games"><b>7. 32-bit games</b></a></summary>
  </details>
* [Troubleshooting](#troubleshooting)
* [Uninstalling](#uninstalling)
* [Building from source](#building-from-source)
* [Name and licence](#name-and-licence)

---

## Visuals

#### Inside a game

<p align="center">
  <img src="https://raw.githubusercontent.com/ales-drnz/vocem-overlay/master/imgs/in-game.png" width="100%">
</p>

The overlay wrote that frame out of the running program's own framebuffer. The
program is a small OpenGL scene rather than a real game; what the overlay draws is
the same either way.

#### The panel, on a map of your display

<p align="center">
  <img src="https://raw.githubusercontent.com/ales-drnz/vocem-overlay/master/imgs/settings-panel.png" width="100%">
</p>

#### Appearance, previewed before you apply

<p align="center">
  <img src="https://raw.githubusercontent.com/ales-drnz/vocem-overlay/master/imgs/settings-appearance.png" width="100%">
</p>

#### Every application it was loaded into, and what it decided

<p align="center">
  <img src="https://raw.githubusercontent.com/ales-drnz/vocem-overlay/master/imgs/settings-applications.png" width="100%">
</p>

#### The tray icon shows your voice state

<p align="center">
  <img src="https://raw.githubusercontent.com/ales-drnz/vocem-overlay/master/imgs/settings-tray.png" width="100%">
</p>

---

## Features

<table>
<tr>
<td valign="middle" width="48"><img src="https://raw.githubusercontent.com/ales-drnz/svg-icons/main/png/layers.png" width="32"></td>
<td valign="middle" width="45%"><b>Part of the frame</b><br>drawn into the game's own framebuffer, so exclusive fullscreen cannot cover it and no compositor decides where it goes.</td>
<td valign="middle" width="48"><img src="https://raw.githubusercontent.com/ales-drnz/svg-icons/main/png/zap.png" width="32"></td>
<td valign="middle" width="45%"><b>Enabled once</b><br>no per-game launch options, no nested compositor, nothing to switch on per title.</td>
</tr>
<tr>
<td valign="middle"><img src="https://raw.githubusercontent.com/ales-drnz/svg-icons/main/png/activity.png" width="32"></td>
<td valign="middle"><b>Live voice state</b><br>who is in the channel, who is speaking, who is muted or deafened.</td>
<td valign="middle"><img src="https://raw.githubusercontent.com/ales-drnz/svg-icons/main/png/music.png" width="32"></td>
<td valign="middle"><b>Direct messages</b><br>the sender, their picture and the text, for as many seconds as you choose.</td>
</tr>
<tr>
<td valign="middle"><img src="https://raw.githubusercontent.com/ales-drnz/svg-icons/main/png/sliders-horizontal.png" width="32"></td>
<td valign="middle"><b>Live settings</b><br>position, size, opacity, colours and spacing reach a running game a couple of seconds after you apply.</td>
<td valign="middle"><img src="https://raw.githubusercontent.com/ales-drnz/svg-icons/main/png/scale.png" width="32"></td>
<td valign="middle"><b>Sized by your display</b><br>a fixed fraction of the display's height, so it stays put when a game changes resolution.</td>
</tr>
<tr>
<td valign="middle"><img src="https://raw.githubusercontent.com/ales-drnz/svg-icons/main/png/cpu.png" width="32"></td>
<td valign="middle"><b>HDR aware</b><br>on an HDR10 or scRGB swapchain the colours are encoded for it instead of being blown out.</td>
<td valign="middle"><img src="https://raw.githubusercontent.com/ales-drnz/svg-icons/main/png/audio-lines.png" width="32"></td>
<td valign="middle"><b>Names in full</b><br>emoji in colour, symbols, Greek, Cyrillic and CJK punctuation, from fonts carried in the binary.</td>
</tr>
<tr>
<td valign="middle"><img src="https://raw.githubusercontent.com/ales-drnz/svg-icons/main/png/shield-check.png" width="32"></td>
<td valign="middle"><b>Nothing to do inside the game</b><br>one daemon holds the Discord connection. The code in your games opens no sockets and decodes no images.</td>
<td valign="middle"><img src="https://raw.githubusercontent.com/ales-drnz/svg-icons/main/png/terminal.png" width="32"></td>
<td valign="middle"><b>It tells you why</b><br>every application it was loaded into gets a row saying what the detection went on.</td>
</tr>
</table>

---

## Quick start

1. Install, then log out and back in.
2. Open the settings window with `vocem-config`.
3. Press the button it offers you.
4. Accept the authorisation prompt in the Discord client.
5. Join a voice channel and start a game.

The window always says what state things are in, and offers one action:

* **Start**, when the daemon is not running.
* **Connect to Discord**, when permission was declined.
* **Reconnect to Discord**, under About, when the permission has to be replaced.

---

## Guide

### 1. Getting it into a game

#### 1.1 Vulkan

Nothing to do. The Vulkan loader finds the layer's manifest in
`/usr/share/vulkan/implicit_layer.d` and loads it into every Vulkan application.

To switch it off for one process:

```bash
VOCEM_DISABLE=1 the-game
```

#### 1.2 OpenGL

OpenGL has no layer mechanism, so the library is preloaded instead. The package
installs a 14 KB shim and an `environment.d` file that preloads it into everything
you start after your next login.

To start one program with it, without the session-wide preload:

```bash
vocem-run minecraft-launcher
```

In a Steam game's launch options, use `vocem-run %command%`. In a launcher with a
wrapper-command field, such as Prism Launcher, put `vocem-run` there.

Preloading a launcher is fine. The game it starts inherits the preload, and the
overlay does not draw on the launcher itself.

#### 1.3 Which applications it appears in

The overlay appears in games and leaves everything else alone. It decides from,
in order:

* a Steam app id in the environment
* a launcher's own identifiers: Lutris, Heroic, umu, gamescope, Minecraft
* the game's own arguments
* the desktop entry it was started from

The **Applications** page lists everything it was loaded into and what it found for
each. The switch beside a row overrides the verdict either way, and reaches a
running game within a couple of seconds.

A game it does not recognise draws nothing until you tick its box. A game inside a
Flatpak sandbox is out of reach: neither the preload nor the layer manifest crosses
the sandbox.

#### 1.4 Games with anti-cheat

This injects into a game's process and hooks its rendering, which is what
anti-cheat systems are built to notice. It has not been run against EAC, BattlEye
or anything similar, and it does not hide itself.

To keep it out of a title entirely:

```bash
VOCEM_DISABLE=1 the-game      # Vulkan
env LD_PRELOAD= the-game      # OpenGL
```

Switching the game off on the **Applications** page stops the drawing but still
loads the library. The commands above do not.

### 2. The settings window

```bash
vocem-config
```

Closing it leaves it behind the tray icon, which shows your voice state. **Quit**
from the tray stops the daemon. Opening the window starts it again.

#### 2.1 Position and size

The **Panel** page draws a map of your display. Drag the panel, or click one of six
anchor points; a drag released near an anchor snaps to it.

**Panel size** scales text, pictures and spacing together, because the panel is
sized from the display's height rather than from the game's window.

#### 2.2 Appearance

Four presets: Transparent, Dark, Light and Purple. Colour and opacity are then
adjustable for both boxes independently, and previewed as you edit.

The default is **Transparent**: names, pictures, ring and badge straight on the
game, with no box behind them.

#### 2.3 The configuration file

Settings are written to `~/.config/vocem/config.ini`, and a running game picks them
up within two seconds. You can edit the file by hand; unknown keys are ignored.

### 3. The daemon

`vocemd` is the only component that talks to Discord. It publishes voice state into
shared memory, which is what the overlay reads. Nothing inside a game opens a
socket or decodes an image.

#### 3.1 Authorisation

The first time it reaches Discord, accept the prompt in the Discord client. The
token is stored in `~/.local/state/vocem/token` with mode 0600 and reused from then
on.

Two things to know before you grant it:

* The scopes include `messages.read`, which is what a message box needs.
* The prompt names **Streamkit**, not this project. The daemon uses Discord's own
  Streamkit application id, which is how a local client works without a client
  secret.

#### 3.2 Pictures

Pictures are cached under `~/.cache/vocem/avatars`. Deleting that directory is
harmless; they are downloaded again on the next update. Somebody whose picture has
not arrived, or who has none, gets a placeholder.

#### 3.3 Reading it from a terminal

```bash
vocem --watch
```

This prints what the daemon is publishing, through the same code the overlay uses.
It is the quickest way to tell a daemon problem from a drawing problem. Without
`--watch` it prints once and exits.

### 4. Environment variables

| Variable | Effect |
| :--- | :--- |
| `VOCEM_DISABLE=1` | switches the overlay off for one process. |
| `VOCEM_DEBUG=1` | debug log on stderr, from every component. |
| `VOCEM_LOG_FILE=path` | the OpenGL overlay appends its log to a file as well. For launchers that swallow a game's stderr. |
| `VOCEM_HDR_NITS=n` | where the overlay's white lands on an HDR swapchain. Default 203, accepted 40 to 1000. |
| `VOCEM_FORCE_AUTHORISE=1` | ask Discord for authorisation again. |
| `VOCEM_EMOJI_BANK=path` | read a colour emoji bank other than the installed one. |

Four more exist for development: `VOCEM_CAPTURE_FRAME`, `VOCEM_GL_LIBRARY`,
`VOCEM_NO_DLSYM` and `VOCEM`. They are documented in the source.

### 5. What it costs a game

* A program that never draws a frame pays for one 14 KB mapping.
* A program that draws but is not a game loads the overlay library, about 260 kB
  resident, decides once, and draws nothing. On a desktop session that is most
  graphical applications.
* A game that gets the overlay also holds its font atlas, a few megabytes.
* Uploads and font rebuilds happen after the frame has been presented, never
  during it.

### 6. OpenGL games

A launch script that **assigns** `LD_PRELOAD` instead of appending to it takes the
overlay out of that game, with nothing logged anywhere. Steam launch options are
often passed around as `LD_PRELOAD="" %command%`. The Vulkan path is unaffected,
because the loader needs no environment variable.

The Minecraft launcher swallows the game's stderr. Use `VOCEM_LOG_FILE` to see what
the overlay is doing in there.

### 7. 32-bit games

Windows games under Proton are often 32-bit, and DXVK translates them to 32-bit
Vulkan. The package installs 32-bit libraries alongside the 64-bit ones, and one
value in the session's preload covers both widths. Nothing to configure.

The daemon and the command-line tool stay 64-bit. They never enter a game process.

---

## Troubleshooting

#### A program will not start any more

```bash
env LD_PRELOAD= the-program
```

For Steam, `env LD_PRELOAD= steam`. For a Vulkan application, `VOCEM_DISABLE=1
the-program`. If it runs that way and not otherwise, the overlay is the cause.
Please open an issue with the program's name and what `VOCEM_DEBUG=1` printed.

#### The window says "Waiting for Discord"

The **desktop** Discord client has to be running; the browser client exposes no
local RPC socket. If it is running, `VOCEM_DEBUG=1 vocemd` says which ports it
tried and what answered on each.

#### The overlay is not in one particular game

Check the **Applications** page. A row for that game says what the detection went
on, and its switch overrides the verdict. No row at all means the overlay was never
loaded into it: see the `LD_PRELOAD` case in [6](#6-opengl-games), or a Flatpak
sandbox.

#### The panel has no background

That is the **Transparent** preset, which is the default. Change it under
**Appearance**, or raise **Opacity**.

---

## Uninstalling

```bash
sudo pacman -Rns vocem-overlay
```

Then log out and back in. Until you do, every program you start prints
`ld.so: ... cannot be preloaded: ignored` to its stderr. The line ends in
`ignored`, so nothing breaks.

Your own files are left in place. To remove those too:

```bash
rm -rf ~/.config/vocem ~/.cache/vocem ~/.local/state/vocem
```

---

## Building from source

Dear ImGui is a submodule, so clone with `--recurse-submodules`, or run
`git submodule update --init` in an existing clone.

```bash
cmake -S . -B build -G Ninja
cmake --build build
cd build && ctest
```

For the 32-bit libraries:

```bash
cmake -S . -B build32 -G Ninja -DVOCEM_BUILD_HOST_TOOLS=OFF -DCMAKE_CXX_FLAGS=-m32 -DCMAKE_SHARED_LINKER_FLAGS=-m32
cmake --build build32
```

A test that needs a display, `bwrap`, a GPU or a 32-bit toolchain reports itself
skipped rather than passing without having run.

---

## Name and licence

*Vocem* is Latin for "voice". The project is not affiliated with Discord Inc.

The project is **BSD 3-Clause**, in [LICENSE](LICENSE). Three things in the tree are
not, and travel with their own texts, which the package installs beside it under
`/usr/share/licenses/vocem-overlay/`:

| | |
| :--- | :--- |
| the colour emoji bank in `common/emoji/`, and the embedded font subsets | **OFL 1.1**, as Modified Versions of Noto Color Emoji, Noto Emoji, Noto Sans JP and Inter. Extracting a font's glyphs into another format is modification by the OFL's own definition, so the bank carries the same licence as its source. Noto Sans JP reserves the name "Source", which the subset does not use. |
| Dear ImGui, in both injected libraries, and nlohmann/json, in the daemon | **MIT**, with their notices installed beside the rest. |
| Wuffs in `third_party/wuffs/`, the image decoder inside the daemon | dual **Apache-2.0 or MIT**, taken here under MIT. |

`stb` is in the tree but ships in nothing. It is the reference the daemon's image
decoder is compared against, in one test.


---

*Developed by Alessandro Di Ronza*
