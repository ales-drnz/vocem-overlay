#!/usr/bin/env python3
# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# The window's paddings, as numbers.
#
# Every card in the window carries objectName "card" and every padded block
# inside one "cardContent", so the geometry dump the window writes for
# VOCEM_CONFIG_GEOMETRY says where they all landed. Checked per section and per
# window size:
#
#   * every block is inset from its card by the same amount on the left and on
#     the right -- the amount is whatever the window agrees on most often, so
#     this reports disagreement rather than enforcing a taste;
#   * a card's height is exactly the sum of its blocks' heights plus their
#     paddings: an item in a card that is not a padded block (a loose Label
#     flush against the card's edge) adds height nothing accounts for;
#   * every row's way back to its default ("rowReset") ends at one right edge
#     per page, and is in the keyboard's tab chain;
#   * the room under a page's content ("pageContent") is the same on every page,
#     whether what ends the page is the Apply bar ("pageBar") or the window's
#     own edge;
#   * every group heading -- a card's own title ("cardTitle") and the foldable
#     headers on the Applications page ("groupHeading") -- starts at the left
#     edge the blocks under it start at;
#   * a scrolling page that carries a card ("pageColumn") is exactly as tall as
#     its contents: a column told to fill the page hands the slack to every
#     child that is itself a layout, a Card included.
#
# Blocks are attributed to cards by geometry rather than by name: the dump's
# paths are built from named ancestors and do not distinguish the second card
# on a page from the first.

import argparse
import collections
import json
import sys

TOLERANCE = 1.0


def load(path):
    """One dump file, as {section: [records]} plus the window size seen."""
    sections = collections.OrderedDict()
    window = None
    current = None
    for line in open(path, encoding="utf-8"):
        line = line.strip()
        if not line:
            continue
        entry = json.loads(line)
        if "section" in entry:
            current = entry["section"]
            sections[current] = []
            window = entry["window"]
        elif "item" in entry and current is not None:
            sections[current].append(entry)
    return sections, window


def leaf(name):
    """The last path component, without the dump's duplicate suffix."""
    return name.rsplit("/", 1)[-1].split("#", 1)[0]


def contains(outer, inner):
    centre_x = inner["x"] + inner["w"] / 2.0
    centre_y = inner["y"] + inner["h"] / 2.0
    return (outer["x"] - TOLERANCE <= centre_x <= outer["x"] + outer["w"] + TOLERANCE and
            outer["y"] - TOLERANCE <= centre_y <= outer["y"] + outer["h"] + TOLERANCE)


def cards_of(records):
    cards = [r for r in records if leaf(r["item"]) == "card" and r["visible"] and r["h"] > 0]
    blocks = [r for r in records if leaf(r["item"]) == "cardContent" and r["visible"]]
    owned = collections.defaultdict(list)
    for block in blocks:
        holders = [c for c in cards if contains(c, block)]
        if not holders:
            continue
        # The smallest containing card, so a card nested in another is not
        # credited with its parent's blocks.
        holders.sort(key=lambda c: c["w"] * c["h"])
        owned[id(holders[0])].append(block)
    return cards, owned


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dumps", nargs="+", help="geometry dumps written by VOCEM_CONFIG_GEOMETRY")
    parser.add_argument("--quiet", action="store_true", help="report only the disagreements")
    arguments = parser.parse_args()

    insets = collections.Counter()
    rows = []
    resets = []
    clearances = []
    headings = []
    columns = []
    for path in arguments.dumps:
        sections, window = load(path)
        for section, records in sections.items():
            size = "%dx%d" % (window["w"], window["h"]) if window else "?"

            visible_resets = [r for r in records
                              if leaf(r["item"]) == "rowReset" and r["visible"]]
            if visible_resets:
                # A window that does not print activeFocusOnTab at all cannot
                # be judged: "absent" must not be read as "false".
                measured = [r for r in visible_resets if "activeFocusOnTab" in r]
                resets.append((size, section,
                               sorted({round(r["x"] + r["w"], 1) for r in visible_resets}),
                               sum(1 for r in measured if not r["activeFocusOnTab"])
                               if measured else None))

            content = [r for r in records if leaf(r["item"]) == "pageContent" and r["visible"]]
            bar = [r for r in records if leaf(r["item"]) == "pageBar" and r["visible"]]
            if content and window:
                ends = bar[0]["y"] if bar else window["h"]
                clearances.append((size, section, "bar" if bar else "window edge",
                                   round(ends - (content[0]["y"] + content[0]["h"]), 1)))

            cards, owned = cards_of(records)

            # A page that carries a card must be exactly as tall as what is on
            # it. A page showing nothing but a placeholder message is allowed to
            # fill the viewport, which is how the placeholder ends up centred.
            for record in records:
                if leaf(record["item"]) == "pageColumn" and record["visible"] and cards:
                    columns.append((size, section, round(record["h"], 1),
                                    round(record.get("implicitHeight", record["h"]), 1)))

            # Where the blocks in this page's cards start, which is the left
            # edge every heading over them is supposed to share.
            block_left = collections.Counter(
                round(b["x"], 1) for blocks in owned.values() for b in blocks)
            if block_left:
                left = block_left.most_common(1)[0][0]
                for record in records:
                    if leaf(record["item"]) in ("cardTitle", "groupHeading") and record["visible"]:
                        headings.append((size, section, leaf(record["item"]),
                                         round(record["x"], 1), left))

            for card in cards:
                blocks = sorted(owned[id(card)], key=lambda b: b["y"])
                left = [round(b["x"] - card["x"], 1) for b in blocks]
                right = [round((card["x"] + card["w"]) - (b["x"] + b["w"]), 1) for b in blocks]
                insets.update(left)
                insets.update(right)
                rows.append({
                    "file": path,
                    "size": size,
                    "section": section,
                    "card": card,
                    "blocks": blocks,
                    "left": left,
                    "right": right,
                })

    if not rows:
        print("no cards in the dump: the window was not measured", file=sys.stderr)
        return 1
    if not insets:
        print("no padded blocks in any card: nothing to compare", file=sys.stderr)
        return 1

    padding = insets.most_common(1)[0][0]
    problems = []

    print("padding taken to be %g, the inset %d of %d card edges agree on"
          % (padding, insets[padding], sum(insets.values())))
    for row in rows:
        card = row["card"]
        explained = sum(b["h"] + padding * 2 for b in row["blocks"])
        unexplained = card["h"] - explained
        odd = sorted({v for v in row["left"] + row["right"] if abs(v - padding) > TOLERANCE})
        state = "ok"
        if odd:
            state = "inset " + ", ".join("%g" % v for v in odd)
            problems.append("%s section %d: a block is inset %s where the window uses %g"
                            % (row["size"], row["section"], state[6:], padding))
        if abs(unexplained) > TOLERANCE:
            state = ("%s; %+.1f unexplained" % (state, unexplained)) if odd \
                    else "%+.1f unexplained" % unexplained
            problems.append(
                "%s section %d: a card at x=%.0f y=%.0f is %.1f taller than the %d padded "
                "block(s) in it -- something in that card is not taking the card's padding"
                % (row["size"], row["section"], card["x"], card["y"], unexplained,
                   len(row["blocks"])))
        if not arguments.quiet or state != "ok":
            print("  %-9s section %d  card x=%6.1f y=%6.1f w=%6.1f h=%6.1f  blocks=%d  %s"
                  % (row["size"], row["section"], card["x"], card["y"], card["w"], card["h"],
                     len(row["blocks"]), state))

    # ---- one right edge per page for the way back to a default, and a
    #      keyboard that can reach it.
    for size, section, edges, unreachable in resets:
        if not arguments.quiet or len(edges) > 1 or unreachable:
            print("  %-9s section %d  reset column at %s, %s out of the tab chain"
                  % (size, section, ", ".join("%g" % e for e in edges),
                     "not reported by this window" if unreachable is None else unreachable))
        if len(edges) > 1:
            problems.append(
                "%s section %d: the rows' reset buttons end at %s -- the way back to a "
                "default is meant to be one column down the page"
                % (size, section, " and ".join("%g" % e for e in edges)))
        if unreachable:
            problems.append(
                "%s section %d: %d of the rows' reset buttons are not in the tab chain -- "
                "a control the keyboard cannot reach is a control somebody cannot use"
                % (size, section, unreachable))

    # ---- one left edge for every heading over a group of rows.
    for size, section, kind, x, left in headings:
        off = abs(x - left) > TOLERANCE
        if not arguments.quiet or off:
            print("  %-9s section %d  %s at x=%g, blocks at x=%g"
                  % (size, section, kind, x, left))
        if off:
            problems.append(
                "%s section %d: a %s starts at %g while the rows under it start at %g -- "
                "two kinds of heading on one page, at two left edges"
                % (size, section, kind, x, left))

    # ---- a page with a card on it is as tall as what is on it.
    for size, section, height, implicit in columns:
        stretched = height - implicit > TOLERANCE
        if not arguments.quiet or stretched:
            print("  %-9s section %d  column h=%g, its contents %g"
                  % (size, section, height, implicit))
        if stretched:
            problems.append(
                "%s section %d: the page is %g tall where its contents are %g -- the "
                "slack goes to every card and group on it, which pushes them apart"
                % (size, section, height, implicit))

    # ---- one clearance under the content, whatever ends the page.
    room = collections.Counter(gap for _, _, _, gap in clearances)
    if room:
        expected = room.most_common(1)[0][0]
        print("clearance under a page's content taken to be %g, which %d of %d pages keep"
              % (expected, room[expected], sum(room.values())))
        for size, section, ends_with, gap in clearances:
            if not arguments.quiet or abs(gap - expected) > TOLERANCE:
                print("  %-9s section %d  %.0f above the %s" % (size, section, gap, ends_with))
            if abs(gap - expected) > TOLERANCE:
                problems.append(
                    "%s section %d: its content stops %g above the %s where the rest of the "
                    "window stops %g" % (size, section, gap, ends_with, expected))

    if problems:
        print()
        for problem in dict.fromkeys(problems):
            print("DISAGREEMENT: " + problem)
        return 1
    print("every card in every section is padded the same, nothing in one is unaccounted for, "
          "every heading starts where its rows start, the reset column is one column, and "
          "every page keeps the same room under it")
    return 0


if __name__ == "__main__":
    sys.exit(main())
