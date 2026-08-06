## [0.1.4] - 6-08-2026

### Added

- The overlay finds a game you started from a terminal, a script or the file manager, through the desktop entry that runs it.
- Games that are themselves Flatpaks appear on the Applications page, with their own icon.
- A game launched by the itch.io app is recognised.

### Fixed

- Five ways a game went undetected: launched by Heroic, started through a wrapper script, installed in a Wine prefix, started over D-Bus, and anything running on GNOME.
- A session with the gamescope layer switched off no longer counts every process in it as a game.
- Two games that share a process name no longer overwrite each other's row on the Applications page, and a game can no longer be held up at its first frame by a file left in the way.
- `vocem-why.sh` prints its answer and no shell errors.
- The settings window opens about two seconds faster. It used to build the list of the machine's fonts at startup, even if you never opened the Appearance page.
- The map of your display has the shape of the display the overlay is sized for, and a horizontal panel is previewed at the width the overlay draws it.
- The Debug section shows no warning when the daemon is simply stopped.
- The opacity settings no longer say "No background" while a faint one is still being drawn.
- A font file that is cut short is refused instead of crashing the game. A font the overlay cannot draw is refused once, instead of costing the game work on every frame.
- The daemon goes on doing its own work while a program on Discord's port floods it, and waits before trying again when one takes the connection and drops it.
- The package is built with the compiler flags the machine building it asks for, and the 32-bit libraries carry the same hardening as the 64-bit ones.

## [0.1.3] - 5-08-2026

### Added

- The voice panel can be laid out horizontally, as a line of people along an edge of the screen.
- The overlay can draw in any TrueType font installed on the machine, keeping its own Inter underneath for whatever the chosen font does not carry.

### Fixed

- The overlay's colours are right in games that render in sRGB. They were encoded a second time, which left the panel light grey instead of dark slate.
- The previews draw one placeholder for somebody with no picture, and it no longer has a dark rim under the shoulders.
- The message box can be turned down to no background at all.
- A message's text is drawn in the panel's idle grey, so it can be told from the sender's name above it.
- The Debug section carries the bottom bar every other page has, with a Clear button for the overlay's own journals.
- The list of past sessions scrolls.
- Several settings say in one line what took three.

## [0.1.2] - 3-08-2026

### Fixed

- The text of a message appears in a game that is itself a Flatpak.
- The daemon and the overlay log when a message's text goes missing, never the text itself.

## [0.1.1] - 3-08-2026

Games that are themselves Flatpaks.

### Added

- The overlay appears in a game installed as a Flatpak. It ships as a Vulkan layer extension carrying both architectures, and one branch covers Steam, Heroic, Sober, Lutris, Bottles and PrismLauncher.
- The daemon publishes the voice state, your settings and the cached pictures into the directory Flatpak shares with the host, and only while the overlay inside it is drawing.
- The OpenGL interposer can be preloaded inside a Flatpak with one `flatpak override`. The command is in the README.

### Known limitations

- A game rendering below its display's mode draws the overlay proportionally larger, since the overlay is sized by the display. Telling the game to render at native resolution is the remedy.

## [0.1.0] - 3-08-2026

First release.

### Added

- A Discord voice overlay drawn inside the game's own frame, where exclusive fullscreen cannot cover it. Vulkan through an implicit layer, OpenGL through a session-wide preload, both at 32 and 64 bits.
- Who is in the channel, who is speaking, and who is muted or deafened, with names, pictures, and emoji in colour.
- Direct messages as a message box. The text lives in a shared segment that exists only while the box is on screen.
- A settings window with ten sections and a map of your display, a tray icon showing your voice state, and `vocem` on the command line. Changes reach a running game within two seconds of Apply.
- An Applications page listing every process the overlay was loaded into, with what the detection went on for each and a switch that overrides it.
- A crash journal per drawing process, shown in the Debug section.
- HDR swapchains: the overlay's colours are encoded for the target instead of being blown out, with reference white at 203 nits.
- `vocemd` is the only component that talks to Discord. It connects only to 127.0.0.1, on ports 6463 to 6472, and checks that the process listening there belongs to you before it sends its token.
- `VOCEM_DISABLE=1` keeps the overlay out of one process.

### Known limitations

- A game the detection does not recognise draws nothing until you tick its box on the Applications page.
- A game inside a Flatpak sandbox is out of reach. (Lifted in 0.1.1.)
- A launch script that assigns `LD_PRELOAD` instead of appending to it takes the OpenGL overlay out of that game. Vulkan is not affected.
- Anti-cheat has not been tested. This injects into a game's process and hooks its rendering.
- The session's preload expands to `lib` and `lib32` only, so a Debian-style multiarch layout would need a different arrangement.
- The daemon authenticates as Discord's own Streamkit application, which is why the authorisation prompt names Streamkit.
