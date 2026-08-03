## [0.1.1] - 3-08-2026

Games that are themselves Flatpaks.

### Added

- The overlay appears in a game installed as a Flatpak, which it could not reach before. It ships as a Vulkan layer extension carrying both the 64-bit and the 32-bit halves, installed once with a single file, and one branch covers Steam, Heroic, Sober, Lutris, Bottles and PrismLauncher.
- The daemon publishes the voice state, your settings and the cached pictures into the one directory Flatpak shares between an application and the host. Nothing to grant and nothing to configure. No ABI change, so games already running keep their overlay.
- A sandbox is served only while the overlay inside it is drawing. Switching a game off on the Applications page stops what is sent to it, not only what is painted.
- The OpenGL interposer can be preloaded inside a Flatpak, with one `flatpak override`. The command is in the README; the `$LIB` token the host's session preload uses does not work there.

### Known limitations

- A game rendering below its display's mode draws an overlay proportionally larger than intended, since the overlay is sized by the display rather than by the window. Roblox under Sober does this with HiDPI off. Telling the game to render at native resolution is the remedy.

## [0.1.0] - 3-08-2026

First release.

### Added

- A Discord voice overlay drawn inside the game's own picture rather than in a window on top of it. Exclusive fullscreen cannot cover it, and no compositor decides where it goes.
- A Vulkan implicit layer, loaded by the Vulkan loader into every Vulkan application. Nothing to configure. `VOCEM_DISABLE=1` switches it off for one process.
- An OpenGL interposer, since OpenGL has no layer mechanism. A 14 KB shim is preloaded session wide, and loads the overlay itself only after a real frame. `vocem-run <command>` does the same for a single program.
- 32-bit libraries alongside the 64-bit ones, for Windows games under Proton. One value in the session's preload covers both widths.
- Who is in the channel, who is speaking, and who is muted or deafened, with the speaking ring rising and falling. Names keep their emoji in colour, their symbols, Greek, Cyrillic and the CJK punctuation a channel name often contains.
- Direct messages as a message box, with the sender, their picture and the text. The text lives in a shared segment that exists only while the box is on screen.
- A settings window with ten sections: the panel's position on a map of your display, notifications, appearance, spacing, who to show, the applications the overlay was loaded into, window behaviour, the system tray, a debug section and About. Changes reach a running game within two seconds of Apply.
- An Applications page listing every process the overlay was loaded into, with what the detection went on for each. The switch beside a row overrides the verdict either way.
- A tray icon showing your voice state, and a window that starts the daemon through its systemd user unit.
- HDR swapchains: on HDR10 and extended sRGB the overlay's colours are encoded for the target instead of being blown out, with reference white at 203 nits and `VOCEM_HDR_NITS` to override.
- A crash journal. Each drawing process writes a session log, a process that dies leaves it behind, and the Debug section shows it.
- `vocem`, a command-line reader of what the daemon publishes.
- `vocemd` is the only component that talks to Discord. Nothing inside a game opens a socket, parses network data or decodes an image. Its access token is stored with mode 0600, and its scopes include `messages.read`, which is what a message box needs.
- The daemon connects only to `127.0.0.1`, only on Discord's documented RPC port range of 6463 to 6472, and checks that the process listening there belongs to you before it sends the token. Its systemd user unit is confined.

### Known limitations

- A game the detection does not recognise draws nothing until you tick its box on the Applications page. The row says what the detection went on.
- A game inside a Flatpak sandbox is out of reach. Neither the session's preload nor the layer manifest crosses the sandbox. (Lifted in 0.1.1.)
- A launch script that assigns `LD_PRELOAD` instead of appending to it takes the OpenGL overlay out of that game silently. The Vulkan path is not affected.
- Anti-cheat has not been tested. This injects into a game's process and hooks its rendering, which is what such systems are built to notice. `VOCEM_DISABLE=1` for Vulkan and `env LD_PRELOAD=` for OpenGL keep it out of a title you would rather not risk.
- The session's preload expands to `lib` and `lib32` only, so a Debian-style multiarch layout would need a different arrangement.
- The daemon authenticates as Discord's own Streamkit application, which is why the authorisation prompt names Streamkit.
