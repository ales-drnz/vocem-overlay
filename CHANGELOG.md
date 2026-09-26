## [0.1.10] - 2026-09-22

### Added

- The avatars of the people not talking are dimmed like their names, set by the new Quiet avatars slider on the Appearance page.

### Fixed

- A new colour emoji in a name or a message no longer freezes the game for up to 200 ms.
- The panel's first appearance costs a game about 50 ms instead of 180: the font atlas is built beside the game.
- A Vulkan game in a window builds the font atlas once, not twice.
- A Vulkan game no longer waits for the GPU when a face or an emoji arrives.
- People without an avatar share one upload of their default picture.
- The Vulkan layer waits for the game's frame before drawing over it, as the specification requires.
- An OpenGL game with a second window keeps that window's textures, and the overlay follows the game to its second context.
- A game that recreates its OpenGL context keeps its overlay and no longer rebuilds the font atlas for it.
- The font atlas takes 16 MB less memory in every game that draws the overlay.
- Stopping the overlay or switching it off hands back what every game was holding for it.
- The Vulkan layer builds its renderer only when there is something to draw.
- A game notices the overlay stopping within a second, whatever its frame rate.
- A Flatpak game with the overlay switched off is no longer sent the channel and its faces.
- Hiding a running game on the Applications page is written to the log, with the reason.
- Emoji sequences draw as one glyph in Flatpak games too.
- The Flatpak extension carries the licences of the fonts and of Dear ImGui.
- `vocem-run` works after an install under `~/.local`.
- A message's words and its sender leave every game and sandbox when the toast ends or the daemon stops, and the daemon's journal no longer names who sent it.
- The daemon stops within a second whatever the peer it is talking to is doing.
- The daemon no longer reconnects thousands of times a second when Discord restarts.
- A message's words are retired on time while the daemon is looking for Discord.
- The Flatpak bridge logs each refusal once instead of every second.
- The daemon refuses a message too large to parse safely instead of spending hundreds of megabytes on it.
- The settings window keeps the pinned display when a monitor is plugged in or removed.
- The window's previews draw the voice panel faint while its switch is off.
- A row's description is drawn as plain text.
- The overlay's switches no longer undo settings edited outside the window, and say so when a write fails.
- `vocem-why` says when it cannot look inside a process instead of reporting the overlay absent.
- The injected libraries export only their entry points in any build.

## [0.1.9] - 2026-09-08

### Fixed

- Emoji sequences draw as one glyph: the lime, the families, the flags, the keycaps and every other ligature of the font, 2546 glyphs in all. A name with 🍋‍🟩 in it drew a lemon beside a green square.

## [0.1.8] - 2026-09-07

### Fixed

- The Vulkan layer uploads its font atlas after the present is handed down, not inside it: the first drawn frame carried a queue wait on the present path.
- The Vulkan layer draws into a swapchain recreated in another format through a pipeline built for that swapchain, not the first swapchain's.
- A helper Vulkan device created and destroyed beside the presenting one no longer tears the overlay down and rebuilds it on the next frame.
- The Vulkan layer's fences are created unsignalled, and a present on a queue family without graphics, or not the command pool's, passes through untouched.
- The OpenGL overlay builds its font atlas texture once per backend. It built it twice on the first frame and orphaned the first copy, up to 64 MB, for the life of the game's context.
- The OpenGL overlay releases its backend only when the context it lives in is destroyed, not on any context's death.
- The OpenGL overlay restores the game's draw framebuffer binding alone and leaves its read framebuffer binding untouched.
- The daemon unlinks its segment, the message's words and every Flatpak mirror the moment it is told to stop, and aborts a download in flight rather than waiting for it.
- The daemon serves at most 32 Flatpak sandboxes and only directories named like a Flatpak application id. Two thousand asking directories used to exhaust its descriptors.
- The daemon's token file is read bounded and validated, and written through a temporary. A link, a FIFO or a byte that is not UTF-8 at that path could stall the daemon or end it on its first message.
- A newline or another control character in a nickname, a channel name or a message's title is removed before the text reaches the panel, the log or the journal.
- A peer that stops reading what the daemon sends is noticed and reconnected, and a WebSocket frame with a reserved bit set, or a binary message's fragments, no longer reach the text stream.
- A desktop entry is read only if it is a regular file of a plausible size. A FIFO or a link to /dev/zero named `.desktop` in a data directory could hold a game's first frame for ever.
- A journal is never created over a file already at its name, a sandbox never prunes journals whose processes it cannot see, and a crashed process's counters go with its journal.
- A record written from the environment is one line per field, so a launcher's value with a newline in it cannot add a row to the Applications page.
- Desktop entries three directories down, where wine installs a prefix's programs, are found.
- A display that is connected but switched off no longer sizes the overlay.
- The settings window writes its three instant switches on top of the file as it stands, picks up a file edited while it is open, and says so when the file cannot be written.
- The settings window's numeric setters are held to the same bounds the file is read with, from one table. Three of them had no bound.
- The row switch on the Applications page removes a rule written against the executable's name as well as one against the process name.
- A quoted value in the settings file loses both quotes, `True` and `on` are true, a corner that is not a number keeps the current corner, and `auto` is the whole word.
- `vocem --watch` follows a daemon that stops and one that starts again, and clears the screen only on a terminal.
- `vocem-why` is installed.
- The package declares `qt6-svg`, which the window's icons need, and the tagged package build no longer compiles the test suite.
- The Vulkan layer tells the daemon that a Flatpak sandbox with the master switch off is not drawing, so the daemon serves it a cleared state rather than the channel and every face.
- The daemon's token exchange ends within a second of a stop, instead of holding the stop for the connect timeout with the segment published.
- The settings window's first frame no longer waits on `systemctl`: the preload probe is asynchronous, and the Debug page says it is asking until the answer arrives.
- The settings window announces a state change only when one happened, asks the segment and the display tree once per sweep, and finds an application's icon through an index.
- Two launches of the settings window inside its own startup are one window: a lock file decides it before the socket does.
- Quit hides the window at once and ends the process when the daemon has stopped.
- The autostart entry quotes the window's path, so a space or a `%` in it does not break the entry.
- The settings window reads a desktop entry's `Exec` the way the overlay does, any word of it and not the first, so a game behind a wrapper gets its own icon.
- `VOCEM_LOG_FILE` is honoured by the Vulkan layer as well as the OpenGL overlay.

### Changed

- The daemon's source is split into its model, its protocol and its loops.
- The code the two overlay paths, the daemon, the window and the tests share is built once per width as a static library, and the bookkeeping the two paths kept alike is one object.
- The test suite is written in four CMake functions with one skip spelling, and the tests that run against the 32-bit tree look for it when they run.

## [0.1.7] - 2026-09-02

### Added

- The voice panel can draw its background behind the names alone, as a rounded box under each one, set by the new Box control on the Appearance page.
- A fifth preset, Pills, draws it that way, on the same surface and at the same opacity as Dark.

### Changed

- A new installation opens on that preset instead of on Transparent.
- A settings file from an earlier release keeps its colour and its opacity, and draws them in the new shape.

## [0.1.6] - 2026-08-18

### Fixed

- The symbols a server decorates its channel names with are drawn, instead of the empty boxes that stood in for them.
- A game whose systemd unit carries no random part in its name is found again. Everything after the first dash of the application id was being discarded.
- A monitor plugged in, unplugged or switched to another resolution resizes the overlay in running games even while Discord is closed. It used to wait for Discord to come back.
- The "Show for" slider offers every duration the settings file accepts. It stopped at 20 seconds against a limit of 30, and rewrote a longer setting the moment it was touched.
- A message toast fades in whole. The placeholder disc and the outline around the text arrived at full strength over a box that was still appearing.
- Reset restores the two preview-display settings, and their pages notice when everything on them is back to its default.
- The daemon stays bounded when messages arrive from a great many different senders, and an avatar that never appears says in the log which half of the work failed.
- A subscription Discord refuses is named in the log, instead of leaving a voice channel where nobody ever seems to speak.
- A participant left out because the panel is full is named in the log, with the channel and the count.
- The daemon tells Discord which release it is. The one string it shows a server it does not control said 0.1 whatever the package said.
- `vocem --help` prints what the tool accepts and succeeds; an unknown argument prints the same on the error output and fails. It used to print one snapshot and succeed either way.

## [0.1.5] - 2026-08-09

### Fixed

- The About page names the release that is installed, and the list of changes Discover and GNOME Software show reaches it too.
- The map of the display follows a monitor plugged in, unplugged or switched to another resolution while the settings window is open. It used to draw whatever was there when the window opened.
- The text of a direct message leaves shared memory when its toast ends, whatever Discord is doing. It used to stay for as long as Discord was away, and what a killed daemon leaves behind is cleared by the next one to start.
- A settings line is read and written whole at any length. Past 255 bytes it was cut without a word, which gave the overlay back to the applications hidden after the cut and lost them from the file at the next Apply.
- Colour emoji are drawn in colour in a Steam game, and in a game that is itself a Flatpak. The bank they come from was looked for at one path, which inside a container belongs to the container rather than to the machine.
- A keycap emoji is drawn by one typeface throughout, instead of a plain digit inside a coloured tile.
- Emoji stay coloured in a channel full of decorated or CJK names. The table of characters that have a colour glyph counted every unusual character, not only the emoji.
- `vocem-why.sh` reports the daemon's state on one line when the daemon is stopped.

## [0.1.4] - 2026-08-06

### Added

- The overlay finds a game started from a terminal, a script or the file manager, through the desktop entry that runs it.
- Games that are themselves Flatpaks appear on the Applications page, with their own icon.
- A game launched by the itch.io app is recognised.

### Fixed

- Five ways a game went undetected: launched by Heroic, started through a wrapper script, installed in a Wine prefix, started over D-Bus, and anything running on GNOME.
- A session with the gamescope layer switched off no longer counts every process in it as a game.
- Two games that share a process name no longer overwrite each other's row on the Applications page, and a game can no longer be held up at its first frame by a file left in the way.
- `vocem-why.sh` prints its answer and no shell errors.
- The settings window opens about two seconds faster. It used to build the list of the machine's fonts at startup, whether or not the Appearance page was ever opened.
- The map of the display has the shape of the display the overlay is sized for, and a horizontal panel is previewed at the width the overlay draws it.
- The Debug section shows no warning when the daemon is simply stopped.
- The opacity settings no longer say "No background" while a faint one is still being drawn.
- A font file that is cut short is refused instead of crashing the game. A font the overlay cannot draw is refused once, instead of costing the game work on every frame.
- The daemon goes on doing its own work while a program on Discord's port floods it, and waits before trying again when one takes the connection and drops it.
- The package is built with the compiler flags the machine building it asks for, and the 32-bit libraries carry the same hardening as the 64-bit ones.

## [0.1.3] - 2026-08-05

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

## [0.1.2] - 2026-08-03

### Fixed

- The text of a message appears in a game that is itself a Flatpak.
- The daemon and the overlay log when a message's text goes missing, never the text itself.

## [0.1.1] - 2026-08-03

Games that are themselves Flatpaks.

### Added

- The overlay appears in a game installed as a Flatpak. It ships as a Vulkan layer extension carrying both architectures, and one branch covers Steam, Heroic, Sober, Lutris, Bottles and PrismLauncher.
- The daemon publishes the voice state, the settings and the cached pictures into the directory Flatpak shares with the host, and only while the overlay inside it is drawing.
- The OpenGL interposer can be preloaded inside a Flatpak with one `flatpak override`. The command is in the README.

### Known limitations

- A game rendering below its display's mode draws the overlay proportionally larger, since the overlay is sized by the display. Telling the game to render at native resolution is the remedy.

## [0.1.0] - 2026-08-03

First release.

### Added

- A Discord voice overlay drawn inside the game's own frame, where exclusive fullscreen cannot cover it. Vulkan through an implicit layer, OpenGL through a session-wide preload, both at 32 and 64 bits.
- Who is in the channel, who is speaking, and who is muted or deafened, with names, pictures, and emoji in colour.
- Direct messages as a message box. The text lives in a shared segment that exists only while the box is on screen.
- A settings window with ten sections and a map of the display, a tray icon showing the current voice state, and `vocem` on the command line. Changes reach a running game within two seconds of Apply.
- An Applications page listing every process the overlay was loaded into, with what the detection went on for each and a switch that overrides it.
- A crash journal per drawing process, shown in the Debug section.
- HDR swapchains: the overlay's colours are encoded for the target instead of being blown out, with reference white at 203 nits.
- `vocemd` is the only component that talks to Discord. It connects only to 127.0.0.1, on ports 6463 to 6472, and checks that the process listening there belongs to the same user before it sends its token.
- `VOCEM_DISABLE=1` keeps the overlay out of one process.

### Known limitations

- A game the detection does not recognise draws nothing until its box is ticked on the Applications page.
- A game inside a Flatpak sandbox is out of reach. (Lifted in 0.1.1.)
- A launch script that assigns `LD_PRELOAD` instead of appending to it takes the OpenGL overlay out of that game. Vulkan is not affected.
- Anti-cheat has not been tested. This injects into a game's process and hooks its rendering.
- The session's preload expands to `lib` and `lib32` only, so a Debian-style multiarch layout would need a different arrangement.
- The daemon authenticates as Discord's own Streamkit application, which is why the authorisation prompt names Streamkit.
