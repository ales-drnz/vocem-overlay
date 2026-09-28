// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The picture of somebody whose picture has not arrived: the silhouette the
// overlay draws (common/src/panel.cpp), a head above the lens where a larger
// circle below overlaps the disc, from the same fractions of the disc's radius.
//
// A Canvas, because it rasterises through QPainter: one implementation on every
// backend, with the primitives panel.cpp uses. A MultiEffect mask is invisible
// to the offscreen harness, and a Shape has two renderers that need not agree
// about arcs (entry 89).

import QtQuick

Item {
    id: root

    // The disc's colour and the mark's, both theme tokens through the bridge
    // (theme.h, avatar_mark_for).
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

    // The shoulders' lower edge is drawn half a unit outside the disc, as in
    // panel.cpp: two antialiased fills sharing an edge leave a dark seam.
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
            // disc, upper edge that circle's, lower edge the disc's. Angles grow
            // clockwise from three o'clock with y downwards, as in panel.cpp, so
            // the same fractions serve both without a sign between them.
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
