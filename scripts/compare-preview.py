#!/usr/bin/env python3
# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
"""What the overlay draws against what the configuration window shows.

The previews had been corrected three times from screenshots, and every pass
fixed some numbers and broke others, because "that gap looks too big" is not a
measurement. This puts the two geometries side by side as figures.

Both sides are measured, not derived. `vocem_panel_geometry` runs the real
build_panel() against a null backend and reads the geometry back out of the
vertices ImGui produced; `vocem-config` walks its own scene graph and prints
where each preview item actually ended up. Everything is then expressed in the
overlay's own unit -- pixels at 1080 lines of display height, where the text is
16 pixels -- so the two are directly comparable whatever scale each view happens
to draw at.

    scripts/compare-preview.py [--sections 0,1] [setting=value ...]

Every figure is in the overlay's reference unit: a pixel at 1080 lines of display
height with the size settings at 1. A difference of up to a pixel is the rounding
each side does in its own units, and the rows measured to a piece of text also
carry the first glyph's side bearing.

The window runs offscreen and only walks the sections that will be read, so this
takes a couple of seconds and never appears on the desktop. VOCEM_PREVIEW_VISIBLE=1
puts it back on screen, which is worth doing when the question is what the window
looks like rather than what it measures -- and note that it will look wrong when it
does, because the window is given an empty XDG_CONFIG_HOME and the Plasma style has
no kdeglobals there to read a colour scheme out of. That has never reached these
figures: the previews take their colours from the overlay's own theme and their
sizes from their own layout, neither of which is the desktop's business.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

# The sections whose preview stands for something the overlay draws, and the stage
# each one uses. The dump numbers its sections in the order the window's sidebar
# does; the ones missing here -- Appearance, Spacing and People -- carry no model
# of a display to compare against. Each of the two that do stands for one box: the
# position map draws the voice panel alone, the message map the message alone, so
# the rows for the other one are simply absent from that section's table.
STAGES = {
    0: "positionStage",
    1: "cornerStage",
}

# Antialiasing puts a vertex about half a pixel outside the shape at each end, so a
# disc measured from the vertices is about a pixel wider than its diameter and its
# edges are half a pixel out on each side.
AA = 1.0
EDGE = AA / 2

# Distances that end at a piece of text are measured to its ink on the overlay side
# and to its box on the preview side, so they carry the first glyph's left side
# bearing -- a fraction of a pixel at the reference size, and not a divergence.


# The window's previews draw a fixed roster of four, so the probe is asked for the
# same four. Anything else would be comparing two different channels.
USERS = 4


def run_overlay(build, settings, width, height):
    binary = os.path.join(build, "tests", "vocem_panel_geometry")
    command = [binary, f"width={width}", f"height={height}", f"users={USERS}"]
    command += [f"{key}={value}" for key, value in settings.items()]
    output = subprocess.run(command, capture_output=True, text=True, check=True).stdout
    return json.loads(output)


def run_window(build, settings, config_home, sections, width, height):
    """The configuration window's own geometry, one dump per section."""
    directory = os.path.join(config_home, "vocem")
    os.makedirs(directory, exist_ok=True)
    with open(os.path.join(directory, "config.ini"), "w") as handle:
        handle.write("# written by compare-preview.py\n")
        for key, value in settings.items():
            handle.write(f"{key} = {value}\n")

    dump = os.path.join(config_home, "geometry.jsonl")
    environment = dict(os.environ)
    environment["XDG_CONFIG_HOME"] = config_home
    environment["VOCEM_CONFIG_GEOMETRY"] = dump
    # Off the screen, unless somebody wants to watch it. This walks the window
    # through its sections one at a time, and it used to do that in a real window
    # on the real desktop: eight pages flicking past over nine seconds, on top of
    # whatever the person running it was doing. Every distance it measures comes
    # out identical offscreen, because they are all laid out rather than rendered.
    #
    # VOCEM_PREVIEW_VISIBLE=1 puts the window back, which is what to do when the
    # question is what the window looks like rather than what it measures.
    if not os.environ.get("VOCEM_PREVIEW_VISIBLE"):
        environment["QT_QPA_PLATFORM"] = "offscreen"
    # The one figure that is not laid out but discovered: the shape of the display
    # the maps stand for, which the window otherwise takes from whatever screen it
    # is on. Pinned to the display the overlay side was measured at, so the two are
    # comparing the same screen -- and so the same command gives the same numbers on
    # somebody else's monitor, which it did not before.
    environment["VOCEM_CONFIG_SCREEN"] = f"{width}x{height}"
    # Only the sections that will be read. The other six are a page switch and a
    # render each, for a dump nothing looks at.
    environment["VOCEM_CONFIG_SECTIONS"] = str(max(sections))
    # Qt writes to the journal when JOURNAL_STREAM is set, which hides QML errors
    # completely. Clearing it puts them back on stderr where they can be seen.
    environment["JOURNAL_STREAM"] = ""
    # Nothing has to be kept away from the window any more: its previews draw a
    # fixed roster and never the live channel, so a daemon connected to a busy
    # server changes nothing in the dump. This used to run under bwrap with a
    # private /dev/shm and a synthetic channel published into it.
    command = [os.path.join(build, "gui", "vocem-config")]
    result = subprocess.run(command, env=environment, capture_output=True, text=True,
                            timeout=120)
    if result.returncode != 0:
        sys.stderr.write(result.stderr)
        raise SystemExit("vocem-config failed")
    if result.stderr.strip():
        sys.stderr.write(result.stderr)

    dumped = {}
    current = None
    with open(dump) as handle:
        for line in handle:
            entry = json.loads(line)
            if "section" in entry:
                current = dumped.setdefault(entry["section"], {})
                continue
            # The dump also records every top-level window (the crash report has
            # to be a window of its own); those lines carry no item.
            if "item" not in entry:
                continue
            # The path ends with the item's name; what matters is the stage it is
            # under and what it is.
            path = entry["item"].split("/")
            for depth in range(len(path)):
                if path[depth] in STAGES.values():
                    current["/".join(path[depth:])] = entry
                    break
    return dumped


def compare(overlay, window, section, verbose, settings):
    """One table, in the overlay's reference units.

    A reference unit is a pixel at 1080 lines of display height with the size
    settings at 1, which is what both sides are laid out in before anything scales
    them. The overlay's own scale for the voice panel is ui_scale, the message has
    that times its own size setting, and each preview item carries the factor its
    view drew it at -- so every figure below is a distance divided by the scale the
    side that produced it was drawn at.
    """
    stage_name = STAGES[section]

    items = window[section]
    stage = items.get(stage_name)
    if stage is None or not stage.get("overlayScale"):
        raise SystemExit(f"section {section}: no {stage_name} in the dump")

    def item(name):
        """The item under this stage, and the first visible one where a view draws
        several of the same thing."""
        exact = items.get(f"{stage_name}/{name}")
        if exact is not None and exact.get("visible"):
            return exact
        prefix = f"{stage_name}/{name}#"
        for key in sorted(items):
            if key.startswith(prefix) and items[key].get("visible"):
                return items[key]
        # Nothing visible: the view does not draw this at all -- each map stands
        # for one box -- and there is nothing to compare.
        return None

    panel = item("panel")
    avatar = item("panel/avatar")
    avatar2 = item("panel/avatar#1")
    name = item("panel/name")
    # The box behind that name, where the panel draws one there. Absent from the
    # dump in the other mode -- the item is not visible -- and absent from the
    # table with it, which is the honest shape: there is no pill to compare.
    name_box = item("panel/nameBox")
    message = item("message")
    message_avatar = item("message/messageAvatar")
    message_title = item("message/messageTitle")
    message_body = item("message/messageBody")

    # What each side was drawn at.
    panel_unit = panel["factor"] if panel and panel.get("factor") else stage["overlayScale"]
    message_unit = (message["factor"] if message and message.get("factor")
                    else panel_unit)
    overlay_unit = overlay["ui_scale"]
    overlay_message_unit = overlay["message_scale"]

    rows = []
    # Kept apart because they are percentages of the display rather than distances,
    # and averaging the two into one "worst" figure would say nothing.
    shares = []

    def row(what, real, shown, unit=None, preview_unit=None):
        if real is None or shown is None:
            return
        real = real / (unit or overlay_unit)
        shown = shown / (preview_unit or panel_unit)
        difference = shown - real
        percent = (difference / real * 100.0) if abs(real) > 1e-6 else float("inf")
        rows.append((what, real, shown, difference, percent))

    o_panel = overlay["panel"]
    o_avatar = overlay["avatar"]
    o_avatar2 = overlay["second_avatar"]
    o_name = overlay["first_name"]
    # The surface behind that name, whatever shape it is in: the window's
    # background where the box is around everything, the name's own pill where it
    # is behind the names. Compared only in the second case, which is the one the
    # preview draws a separate item for.
    o_name_box = overlay["first_surface"]
    o_toast = overlay["toast"]
    o_toast_avatar = overlay["toast_avatar"]
    o_toast_title = overlay["toast_title"]
    o_toast_body = overlay["toast_body"]

    # How much of the display each box covers. Every other row divides both sides by
    # the scale the view drew at, which validates what is inside a box but can never
    # catch a view that chose the wrong scale for the box itself -- the corner picker
    # drew the message at a size of its own and no figure below moved.
    def share(what, real, real_of, shown, shown_of):
        if real is None or shown is None:
            return
        shares.append((what, real / real_of * 100.0, shown / shown_of * 100.0,
                       shown / shown_of * 100.0 - real / real_of * 100.0, 0.0))

    share("panel: share of the screen width, %", o_panel["w"], overlay["output"]["w"],
          panel["w"] if panel else None, stage["w"])
    share("message: share of the screen width, %", o_toast["w"], overlay["output"]["w"],
          message["w"] if message else None, stage["w"])
    row("panel: box width", o_panel["w"], panel["w"] if panel else None)
    row("panel: inset from the screen edge", o_panel["x"],
        panel["x"] - stage["x"] if panel else None)
    row("panel: box height", o_panel["h"], panel["h"] if panel else None)
    row("panel: box edge to the picture", o_avatar["x"] - o_panel["x"] + EDGE,
        avatar["x"] - panel["x"] if avatar and panel else None)
    row("panel: picture diameter", o_avatar["w"] - AA, avatar["w"] if avatar else None)
    row("panel: top of the box to the first picture", o_avatar["y"] - o_panel["y"] + EDGE,
        avatar["y"] - panel["y"] if avatar and panel else None)
    row("panel: picture to name (to the ink)", o_name["x"] - (o_avatar["x"] + o_avatar["w"]) + EDGE,
        name["x"] - avatar["x"] - avatar["w"] if name and avatar else None)
    row("panel: name box width", (o_name_box["w"] - AA) if name_box else None,
        name_box["w"] if name_box else None)
    row("panel: name box height", (o_name_box["h"] - AA) if name_box else None,
        name_box["h"] if name_box else None)
    row("panel: box edge to the name box",
        (o_name_box["x"] - o_panel["x"] + EDGE) if name_box else None,
        name_box["x"] - panel["x"] if name_box and panel else None)
    row("panel: one row to the next",
        (o_avatar2["y"] - o_avatar["y"]) if o_avatar2 else None,
        avatar2["y"] - avatar["y"] if avatar2 and avatar else None)

    row("message: box width", o_toast["w"], message["w"] if message else None,
        overlay_message_unit, message_unit)
    # The distance from the edge belongs to the display, so it is in the panel's
    # unit on both sides -- and measured to whichever edge the box is against, since
    # the corner is a setting and the message map draws the one it is asked for.
    row("message: inset from the screen edge",
        min(o_toast["x"], overlay["output"]["w"] - (o_toast["x"] + o_toast["w"])),
        min(message["x"] - stage["x"],
            stage["x"] + stage["w"] - message["x"] - message["w"]) if message else None)
    row("message: box height", o_toast["h"], message["h"] if message else None,
        overlay_message_unit, message_unit)
    row("message: picture diameter", o_toast_avatar["w"] - AA,
        message_avatar["w"] if message_avatar else None, overlay_message_unit, message_unit)
    row("message: box edge to the picture", o_toast_avatar["x"] - o_toast["x"] + EDGE,
        message_avatar["x"] - message["x"] if message_avatar and message else None,
        overlay_message_unit, message_unit)
    row("message: picture to text (to the ink)",
        o_toast_title["x"] - (o_toast_avatar["x"] + o_toast_avatar["w"]) + EDGE,
        message_title["x"] - message_avatar["x"] - message_avatar["w"]
        if message_title and message_avatar else None, overlay_message_unit, message_unit)
    row("message: sender to body", o_toast_body["y"] - o_toast_title["y"],
        message_body["y"] - message_title["y"] if message_body and message_title else None,
        overlay_message_unit, message_unit)

    # The overlay keeps its text between 11 and 64 pixels, and that clamp depends on
    # the output's real height in device pixels -- which the window has no reliable
    # way to know on a fractionally scaled session, and which is the game's output
    # rather than the window's in any case. Where it bites, the preview is drawing
    # the unclamped size and the difference is not a divergence to fix.
    unclamped = (float(settings.get("font_size", 16.0)) * overlay["output"]["h"] / 1080.0 *
                 float(settings.get("scale", 1.0)))
    if abs(unclamped - overlay["font_pixels"]) > 0.6:
        print(f"    note: at {int(overlay['output']['w'])}x{int(overlay['output']['h'])} the "
              f"overlay clamps its text to {overlay['font_pixels']:.0f}px where the setting asks "
              f"for {unclamped:.1f}px; the preview draws the unclamped size")

    print(f"--- section {section} ({stage_name}), 1 unit = 1 overlay pixel at 1080 lines, "
          f"size settings at 1")
    print(f"{'':44} {'overlay':>9} {'preview':>9} {'diff':>8} {'':>7}")
    for what, real, shown, difference, _ in shares:
        flag = "   " if abs(difference) <= 1.0 else " ! "
        print(f"{flag}{what:41} {real:9.2f} {shown:9.2f} {difference:+8.2f}   points")
    for what, real, shown, difference, percent in rows:
        flag = "   " if abs(difference) <= 1.0 else ("!! " if abs(percent) > 10 else " ! ")
        print(f"{flag}{what:41} {real:9.2f} {shown:9.2f} {difference:+8.2f} {percent:+6.1f}%")
    worst = max((abs(difference) for _, _, _, difference, _ in rows), default=0.0)
    worst_share = max((abs(difference) for _, _, _, difference, _ in shares), default=0.0)
    print(f"    worst divergence {worst:.2f} units, {worst_share:.2f} points of the display")
    if verbose:
        print()
        for key, entry in sorted(items.items()):
            print(f"    {key:46} x {entry['x']:8.2f} y {entry['y']:8.2f} "
                  f"w {entry['w']:8.2f} h {entry['h']:8.2f}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build")
    parser.add_argument("--width", type=int, default=1920)
    parser.add_argument("--height", type=int, default=1080)
    parser.add_argument("--sections", default="0")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("settings", nargs="*", metavar="key=value")
    arguments = parser.parse_args()

    settings = {}
    for setting in arguments.settings:
        key, _, value = setting.partition("=")
        settings[key] = value

    sections = [int(part) for part in arguments.sections.split(",")]
    overlay = run_overlay(arguments.build, settings, arguments.width, arguments.height)
    home = tempfile.mkdtemp(prefix="vocem-compare-")
    # The geometry harness always publishes a toast WITH a body, while the
    # preview follows notification_show_body and the default withholds it -- a
    # default-settings comparison would measure the privacy switch rather than
    # a divergence. The window side gets the body switched on so the two draw
    # the same toast; the harness has no such key, its fixture IS the body.
    window_settings = {"notification_show_body": "true", **settings}
    try:
        window = run_window(arguments.build, window_settings, home, sections,
                            arguments.width, arguments.height)
        for section in sections:
            compare(overlay, window, section, arguments.verbose, settings)
    finally:
        shutil.rmtree(home, ignore_errors=True)


if __name__ == "__main__":
    main()
