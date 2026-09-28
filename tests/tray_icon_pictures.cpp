// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The tray's five state icons, as pictures: each file given is parsed and
// drawn by Qt's own SVG renderer at 480 px, and the pixels are measured.
//
//   * it renders at all (a file the parser refuses draws nothing in a tray);
//   * it is one circle, centred, of the same radius as the other four -- the
//     radius of a filled disc is its edge, of a ring its stroke's middle, so
//     five icons that drift apart make the tray twitch as the state changes;
//   * nothing is drawn outside it (no box, frame or backdrop behind the state);
//   * the circle carries its state's token colour.
//
// Usage: vocem_tray_icon_pictures <file.svg>=<#rrggbb> ... (five of them).
// tests/tray_icons.cmake runs it on the icons an install put down.

#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>

#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "vocem_check.h"

using vocem_test::check;
using vocem_test::failures;

namespace {

constexpr int kSize = 480;

struct Measured {
    bool rendered = false;
    double centre_x = 0, centre_y = 0;
    double radius = 0;        // the edge of a disc, the middle of a ring's stroke
    double outer = 0;         // the outermost opaque pixel's distance
    long outside = 0;         // opaque pixels beyond outer + 3 px
    QColor sample;            // the circle's own colour, just inside its line
};

bool opaque(const QImage& image, int x, int y) {
    return qAlpha(image.pixel(x, y)) >= 128;
}

// Opaque runs along one line through the middle: the first and last opaque
// pixel, and for a ring the first and last transparent pixel inside them.
void edges(const QImage& image, bool horizontal, double* outer_a, double* outer_b,
           double* inner_a, double* inner_b) {
    const int mid = kSize / 2;
    auto at = [&](int i) { return horizontal ? opaque(image, i, mid) : opaque(image, mid, i); };
    int first = -1, last = -1;
    for (int i = 0; i < kSize; ++i) {
        if (at(i)) {
            if (first < 0) first = i;
            last = i;
        }
    }
    *outer_a = first;
    *outer_b = last;
    *inner_a = *inner_b = -1;
    int i = first;
    while (i >= 0 && i <= last && at(i)) ++i;
    if (i < last) {  // a gap: a ring
        int j = last;
        while (j >= first && at(j)) --j;
        *inner_a = i;
        *inner_b = j;
    }
}

Measured measure(const std::string& path) {
    Measured m;
    QSvgRenderer renderer(QString::fromStdString(path));
    if (!renderer.isValid()) {
        return m;
    }
    QImage image(kSize, kSize, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        renderer.render(&painter, QRectF(0, 0, kSize, kSize));
    }
    m.rendered = true;
    double la, lb, li, ri, ta, tb, ti, bi;
    edges(image, true, &la, &lb, &li, &ri);
    edges(image, false, &ta, &tb, &ti, &bi);
    if (la < 0 || ta < 0) {
        m.rendered = false;
        return m;
    }
    m.centre_x = (la + lb + 1) / 2.0;
    m.centre_y = (ta + tb + 1) / 2.0;
    m.outer = (lb - la + 1) / 2.0;
    const bool ring = li >= 0;
    m.radius = ring ? (m.outer + (ri - li) / 2.0) / 2.0 : m.outer;
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const double d = std::hypot(x + 0.5 - m.centre_x, y + 0.5 - m.centre_y);
            if (d > m.outer + 3 && opaque(image, x, y)) {
                ++m.outside;
            }
        }
    }
    // A ring's colour is on its line; a disc's just inside its edge, clear of
    // any glyph drawn in its middle.
    const int x = static_cast<int>(m.centre_x - (ring ? m.radius : m.radius * 0.93));
    m.sample = QColor::fromRgba(image.pixel(x, kSize / 2));
    return m;
}

bool near(const QColor& a, const QColor& b) {
    return std::abs(a.red() - b.red()) <= 12 && std::abs(a.green() - b.green()) <= 12 &&
           std::abs(a.blue() - b.blue()) <= 12 && a.alpha() >= 240;
}

}  // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    setlocale(LC_NUMERIC, "C");  // the figures below in one spelling
    if (argc < 2) {
        printf("FAIL usage: %s <file.svg>=<#rrggbb> ...\n", argv[0]);
        return 1;
    }
    std::vector<double> radii;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const size_t equals = arg.rfind('=');
        const std::string path = arg.substr(0, equals);
        const QColor token(QString::fromStdString(arg.substr(equals + 1)));
        const std::string name = path.substr(path.rfind('/') + 1);
        const Measured m = measure(path);
        check(m.rendered, name + " renders");
        if (!m.rendered) {
            continue;
        }
        printf("--  %s: centre %.1f,%.1f radius %.1f outer %.1f, %ld px outside, colour %s\n",
               name.c_str(), m.centre_x, m.centre_y, m.radius, m.outer, m.outside,
               m.sample.name(QColor::HexArgb).toUtf8().constData());
        check(std::abs(m.centre_x - kSize / 2.0) <= 2 && std::abs(m.centre_y - kSize / 2.0) <= 2,
              name + " is centred");
        check(m.outside == 0, name + " draws nothing outside its circle");
        check(near(m.sample, token),
              name + " carries its state's colour " + token.name().toStdString());
        radii.push_back(m.radius);
    }
    if (!radii.empty()) {
        double low = radii[0], high = radii[0];
        for (double r : radii) {
            low = std::min(low, r);
            high = std::max(high, r);
        }
        printf("--  radii %.1f..%.1f px at %d px\n", low, high, kSize);
        check(high - low <= 2.0 && low > kSize / 4.0,
              "every icon is one circle of one radius (the tray does not twitch)");
    }
    check(static_cast<int>(radii.size()) == argc - 1, "every icon was measured");
    return vocem_test::finish();
}
