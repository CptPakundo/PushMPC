// SPDX-License-Identifier: GPL-3.0-or-later
// PushMPC capture mode, CI test source (capture.yml, linux-x11): a window showing a 24 fps "clip" of a white square moving
// over grey, drawn straight with Xlib. Frame k goes up at k / 24 s on the monotonic clock with the square at
// (k * speed / 24) mod wrap: one thread that sleeps between frames, one XCopyArea per frame (one damage event, no half
// drawn picture). mpv on the loaded 4-core runner left out 2-4 of every 24 frames even with --framedrop=no (run
// 45973220987: 20.1-21.9 new pictures a second), so the motion check measured the player, not frame generation.
//   pmc-square <W> <H> <side> <speed px/s> <wrap px> <y> [seconds]
// The window is called pmc-square (title, WM_CLASS and the process name: capture finds it by --player pmc-square).
// Every 240 frames, and at the end, stderr says how many frames went up and how many were late by a whole frame or more
// (left out: the next one shows the clock's frame). Build: cc -O2 -o pmc-square square_x11.c -lX11
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int s) { (void)s; g_stop = 1; }

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static void sleep_until(double t)
{
    struct timespec ts;
    ts.tv_sec = (time_t)t;
    ts.tv_nsec = (long)((t - (double)ts.tv_sec) * 1e9);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR && !g_stop) {}
}

int main(int argc, char** argv)
{
    if (argc < 7) {
        fprintf(stderr, "usage: pmc-square <W> <H> <side> <speed px/s> <wrap px> <y> [seconds]\n");
        return 2;
    }
    const int w = atoi(argv[1]), h = atoi(argv[2]), side = atoi(argv[3]);
    const double speed = atof(argv[4]);
    const int wrap = atoi(argv[5]), y = atoi(argv[6]);
    const double seconds = argc > 7 ? atof(argv[7]) : 0;
    if (w < 16 || h < 16 || side < 1 || wrap < 1) { fprintf(stderr, "pmc-square: bad size\n"); return 2; }
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    Display* d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "pmc-square: no X display\n"); return 1; }
    const int scr = DefaultScreen(d);
    const unsigned long grey = 0x3c3c3c, white = 0xffffff;   // TrueColor 24-bit (Xvfb -screen 0 WxHx24)
    Window win = XCreateSimpleWindow(d, RootWindow(d, scr), 40, 40, (unsigned)w, (unsigned)h, 0, grey, grey);
    XStoreName(d, win, "pmc-square");
    XClassHint ch = { (char*)"pmc-square", (char*)"pmc-square" };
    XSetClassHint(d, win, &ch);
    XSelectInput(d, win, StructureNotifyMask);
    Pixmap pm = XCreatePixmap(d, win, (unsigned)w, (unsigned)h, (unsigned)DefaultDepth(d, scr));
    GC gc = XCreateGC(d, win, 0, NULL);
    XMapWindow(d, win);
    for (;;) { XEvent ev; XNextEvent(d, &ev); if (ev.type == MapNotify) break; }

    const double t0 = now_s();
    long k = 0, up = 0, late = 0;
    while (!g_stop) {
        const double due = t0 + (double)k / 24.0;
        sleep_until(due);
        if (g_stop) break;
        // late by a whole frame or more: show the clock's frame, as a player that drops would (counted)
        const long kc = (long)((now_s() - t0) * 24.0);
        if (kc > k) { late += kc - k; k = kc; }
        const int x = (int)((long)((double)k * speed / 24.0 + 0.5) % wrap);
        XSetForeground(d, gc, grey);
        XFillRectangle(d, pm, gc, 0, 0, (unsigned)w, (unsigned)h);
        XSetForeground(d, gc, white);
        XFillRectangle(d, pm, gc, x, y, (unsigned)side, (unsigned)side);
        XCopyArea(d, pm, win, gc, 0, 0, (unsigned)w, (unsigned)h, 0, 0);
        XFlush(d);
        while (XPending(d)) { XEvent ev; XNextEvent(d, &ev); }
        ++up;
        if (up % 240 == 0) fprintf(stderr, "pmc-square: %ld frames up, %ld left out (late)\n", up, late);
        ++k;
        if (seconds > 0 && (double)k / 24.0 >= seconds) break;
    }
    fprintf(stderr, "pmc-square: done, %ld frames up, %ld left out (late)\n", up, late);
    XFreePixmap(d, pm);
    XFreeGC(d, gc);
    XDestroyWindow(d, win);
    XCloseDisplay(d);
    return 0;
}
