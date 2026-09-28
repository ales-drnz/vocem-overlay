// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The window a GL probe draws into: override-redirect and parked at x=-4000,
// so the window manager never sees it, nothing appears on the desktop and no
// focus is taken -- the owner may be in a game while the suite runs. Under a
// compositor every window renders into a buffer of its own, so a front-buffer
// read-back still sees what was drawn.
//
// The visual is the caller's: each probe resolves glXChooseVisual its own way
// (through dlopen(RTLD_LOCAL) and dlsym, as a game does, or linked), and that
// door is what several of them are about (entry 38).

#ifndef VOCEM_TESTS_GL_WINDOW_H
#define VOCEM_TESTS_GL_WINDOW_H

#include <X11/Xlib.h>
#include <X11/Xutil.h>

namespace vocem_test {

inline Window offscreen_window(Display* display, XVisualInfo* visual, int width, int height) {
    XSetWindowAttributes attributes{};
    attributes.colormap =
        XCreateColormap(display, RootWindow(display, visual->screen), visual->visual, AllocNone);
    attributes.override_redirect = True;
    const Window window = XCreateWindow(display, RootWindow(display, visual->screen), -4000, 0,
                                        static_cast<unsigned>(width),
                                        static_cast<unsigned>(height), 0, visual->depth,
                                        InputOutput, visual->visual,
                                        CWColormap | CWOverrideRedirect, &attributes);
    XMapWindow(display, window);
    return window;
}

}  // namespace vocem_test

#endif  // VOCEM_TESTS_GL_WINDOW_H
