## [Unreleased]

### Fixed

- A font file that is cut short is refused instead of crashing the game. An interrupted copy, or a file still being written, used to be read past its end.
- A font the overlay cannot draw is refused once instead of on every frame. Each refusal rebuilt the whole atlas and re-uploaded it: 61 ms per frame, inside the game.
- The daemon keeps its own clock while a program on Discord's port floods it. Messages stopped expiring, the channel stopped being reconciled, and the daemon did not stop when it was asked to.
- The daemon waits before trying again when something answers on Discord's port and drops the connection. It used to reconnect at once, thousands of times in a few seconds.
- The settings window opens faster. It built the list of the machine's fonts at startup, for everybody, whether or not the Appearance page was ever opened: about two seconds here, with 271 families installed. The list is built when you go for a font now.
- The map of your display has the shape of the display the overlay is sized for. It used to take its shape from the screen the window happens to be on and its resolution from the first display by name.
- A horizontal panel is previewed at the width the overlay draws it. The preview was capped at the width a vertical panel gets, so it ended at the third person where the game drew four.
- The Debug section shows no warning when the daemon is simply stopped. It reported a fault and then named the OpenGL preload as the cause, two lines above its own row saying the preload is active.
- The opacity settings say "Little or no background" where they used to say "No background". The sentence appears below 15%, and at 8% there is still a background there.
- The package is built with the compiler flags the machine building it asks for. It used to pin its own optimisation level, overriding what `makepkg.conf` had chosen. Assertions stay compiled out either way: an assertion inside injected code would end the game rather than the overlay.
- The 32-bit libraries are built with the same hardening as the 64-bit ones. They had been built without `_FORTIFY_SOURCE` and without full RELRO since the first release, because the packaging replaced the compiler and linker flags it should have added to — and those are the libraries preloaded into every 32-bit process of the session.

## [0.1.3] - 5-08-2026

### Added

- The voice panel can be laid out horizontally, as a line of people along an edge of the screen instead of a column. It is a chip under Appearance, and every preview in the window follows it.
- The overlay can draw in any TrueType font installed on the machine. The settings window lists what it finds and resolves the family to its regular and bold files; the overlay keeps its own Inter merged underneath for anything the chosen font does not carry, and falls back to it entirely, saying why in the log, whenever the file cannot be read or is not a font it can draw.

### Fixed

- The overlay's colours are right in games that render in sRGB. Where the game's framebuffer carries the sRGB encoding itself — a very common choice on both Vulkan and OpenGL — the overlay's colours were being encoded a second time, which left the panel a light grey instead of dark slate. White text and the speaking ring were unaffected, which is why it went unnoticed.
- The previews draw the overlay's own placeholder for somebody with no picture. They used to draw the desktop's generic user icon on top of it, which put two different silhouettes in one circle.
- The silhouette on a picture that has not arrived no longer has a dark rim under its shoulders. Where the shape met the edge of the disc, both were drawn with a soft edge and the game showed through between them.
- The message box can be turned down to no background at all. Its opacity slider stopped at 20%, which made it the one surface in the overlay that could not be switched off.
- A message's text is drawn in the panel's idle grey, so it can be told from the sender's name above it. The two had been a part in 255 apart, which is one colour to the eye.
- The Debug section carries the bottom bar every other page has, with a Clear button that empties the overlay's own journals. The daemon's log is left alone: those lines belong to the system journal.
- The list of past sessions scrolls, and no longer repeats the same sentence under every crashed one.
- Several settings had descriptions two and three lines long. They say the same thing in one.

## [0.1.2] - 3-08-2026

### Fixed

- The text of a message appears in a game that is itself a Flatpak. 0.1.1 carried the sender's name and picture across the sandbox, but not the text, which sits in its own segment that only exists while the message is on screen.
- The daemon logs how many bytes of text Discord sent and which field they came from, never the text itself.
- The overlay logs, once per message, when the text of a message it should draw is missing.

## [0.1.1] - 3-08-2026

Games that are themselves Flatpaks.

### Added

- The overlay appears in a game installed as a Flatpak. It ships as a Vulkan layer extension carrying both the 64-bit and the 32-bit halves; one branch covers Steam, Heroic, Sober, Lutris, Bottles and PrismLauncher.
- The daemon publishes the voice state, your settings and the cached pictures into the directory Flatpak shares between an application and the host. No ABI change, so games already running keep their overlay.
- A sandbox is served only while the overlay inside it is drawing. Switching a game off on the Applications page also stops what is sent to it.
- The OpenGL interposer can be preloaded inside a Flatpak, with one `flatpak override`. The command is in the README; the `$LIB` token the host's session preload uses does not work there.

### Known limitations

- A game rendering below its display's mode draws an overlay proportionally larger than intended, since the overlay is sized by the display rather than by the window. Roblox under Sober does this with HiDPI off. Telling the game to render at native resolution is the remedy.

## [0.1.0] - 3-08-2026

First release.

### Added

- A Discord voice overlay drawn into the game's own frame, where exclusive fullscreen cannot cover it.
- A Vulkan implicit layer, loaded by the Vulkan loader into every Vulkan application. `VOCEM_DISABLE=1` switches it off for one process.
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
- `vocemd` is the only component that talks to Discord; the code inside a game opens no sockets and decodes no images. Its access token is stored with mode 0600, and its scopes include `messages.read`, for the message box.
- The daemon connects only to `127.0.0.1`, only on Discord's documented RPC port range of 6463 to 6472, and checks that the process listening there belongs to you before it sends the token. Its systemd user unit is confined.

### Known limitations

- A game the detection does not recognise draws nothing until you tick its box on the Applications page. The row says what the detection went on.
- A game inside a Flatpak sandbox is out of reach. Neither the session's preload nor the layer manifest crosses the sandbox. (Lifted in 0.1.1.)
- A launch script that assigns `LD_PRELOAD` instead of appending to it takes the OpenGL overlay out of that game silently. The Vulkan path is not affected.
- Anti-cheat has not been tested. This injects into a game's process and hooks its rendering. `VOCEM_DISABLE=1` for Vulkan and `env LD_PRELOAD=` for OpenGL keep it out of a title you would rather not risk.
- The session's preload expands to `lib` and `lib32` only, so a Debian-style multiarch layout would need a different arrangement.
- The daemon authenticates as Discord's own Streamkit application, which is why the authorisation prompt names Streamkit.
