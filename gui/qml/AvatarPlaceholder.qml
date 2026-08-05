// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The picture of somebody whose picture has not arrived.
//
// The same silhouette the overlay draws, from the same four fractions of the
// radius, so a preview of a waiting row looks like the waiting row a game gets:
// a head above the lens where a larger circle below overlaps the disc.
// common/src/panel.cpp is the other half; the numbers here are its numbers, as
// fractions of the disc's radius rather than of the item's width, so the two
// files can be read side by side.
//
// **Painted, not masked and not a Shape.** Two earlier versions of this file are
// the reason it is a Canvas:
//
//   * two circles in a layer with a MultiEffect masking them to the disc -- four
//     antialiased edges meeting along the bottom of the figure, and invisible to
//     the offscreen harness, which draws no effects at all;
//   * a QtQuick.Shapes path -- correct under the software renderer this machine
//     can measure, and reported wrong by the owner on his own screen, which is
//     the GPU one. Its arcs carry three conventions (an angle's sign, a sweep's
//     direction, what `moveToStart: false` does to the second arc) and Shape has
//     two renderers that need not agree about them.
//
// Canvas rasterises through QPainter: one implementation, the same on every
// backend, and the same primitives panel.cpp uses -- a filled circle, a filled
// circle, and a path of two arcs. When the two halves of this project have to
// draw one shape, the way to keep them together is to give them the same
// arithmetic and as few layers of interpretation as possible.
//
// The measurement that cleared the game's half is placeholder_inside_disc() in
// tests/panel_geometry.cpp.

import QtQuick

Item {
    id: root

    // The disc's colour, and the mark's, both from the theme through the bridge.
    // The mark used to be blended here in JavaScript from a copy of the drawing's
    // formula; it is a token now (theme.h, avatar_mark_for), because a colour the
    // geometry measurement finds a shape by cannot live in two files.
    property color discColour: "#4f545c"
    property color markColour: "#9ea1a5"

    readonly property real radius: width / 2

    // panel.cpp: the head is 0.28 of the disc's radius, centred 0.28 above the
    // middle; the shoulders are a circle of 0.72 centred 0.87 below it, and the
    // two circles cross at y = 0.712, x = +-0.702.
    readonly property real headFactor: 0.28
    readonly property real shoulderFactor: 0.72
    readonly property real shoulderDrop: 0.87
    readonly property real meetX: 0.702
    readonly property real meetY: 0.712

    // The lower edge of the shoulders is drawn half a unit outside the disc, for
    // the reason panel.cpp gives at the same place: two antialiased fills that
    // share an edge do not add up to a solid one, and the seam reads as a dark
    // rim under the figure. Half a unit is under what antialiasing already does.
    readonly property real seam: 0.5

    onDiscColourChanged: canvas.requestPaint()
    onMarkColourChanged: canvas.requestPaint()

    Canvas {
        id: canvas

        anchors.fill: parent
        // The item is laid out at the overlay's reference size and the view
        // scales it whole (rule 24), so the canvas is painted once per size
        // rather than per frame.
        renderStrategy: Canvas.Cooperative
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()

        onPaint: {
            const context = getContext("2d");
            const r = root.radius;
            context.reset();
            if (r <= 0) {
                return;
            }

            context.fillStyle = root.discColour;
            context.beginPath();
            context.arc(r, r, r, 0, 2 * Math.PI);
            context.fill();

            // Below three pixels of radius the mark is one pixel of mud, which
            // is where panel.cpp stops drawing it as well.
            if (r < 3) {
                return;
            }

            context.fillStyle = root.markColour;
            context.beginPath();
            context.arc(r, r - r * root.headFactor, r * root.headFactor, 0, 2 * Math.PI);
            context.fill();

            // The shoulders: the lens where a larger circle below overlaps the
            // disc. Its upper edge is that circle's, through its top; its lower
            // edge is the disc's own, through its bottom. Angles are radians
            // measured from three o'clock and grow clockwise -- the canvas's
            // convention and atan2's, in a coordinate system whose y grows
            // downwards, which is why the same four fractions serve here and in
            // panel.cpp without a sign between them.
            const shoulderY = r + r * root.shoulderDrop;
            context.beginPath();
            context.arc(r, shoulderY, r * root.shoulderFactor,
                        Math.atan2(root.meetY - root.shoulderDrop, -root.meetX),
                        Math.atan2(root.meetY - root.shoulderDrop, root.meetX));
            context.arc(r, r, r + root.seam,
                        Math.atan2(root.meetY, root.meetX),
                        Math.atan2(root.meetY, -root.meetX));
            context.closePath();
            context.fill();
        }
    }
}
