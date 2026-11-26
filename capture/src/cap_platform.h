// SPDX-License-Identifier: GPL-3.0-or-later
// PushMPC capture mode: the sources and presenters each OS build has (capture.cmake defines PMC_HAVE_X11,
// PMC_HAVE_WAYLAND, PMC_HAVE_PORTAL by what it finds; macOS always has ScreenCaptureKit and its window).
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "cap_core.h"
#include "cap_source.h"

namespace pmc {

#ifdef PMC_HAVE_X11
// XComposite + XDamage + MIT-SHM capture of one X11 window (--window, --player, --pick)
std::unique_ptr<Source> MakeX11Source(const Options& o);
// an override-redirect window over followWindow (0 or o.fullscreen: the whole monitor), clicks go through
std::unique_ptr<Presenter> MakeX11Presenter(const Options& o, uint64_t followWindow);
bool ListX11Windows(std::vector<WindowInfo>& out, std::string& err);
#endif
#ifdef PMC_HAVE_PORTAL
// xdg-desktop-portal ScreenCast (the desktop's own window picker) + PipeWire
std::unique_ptr<Source> MakePortalSource(const Options& o);
#endif
#ifdef PMC_HAVE_WAYLAND
// a layer-shell overlay over the whole output (KDE, wlroots, Hyprland, COSMIC), else a full-screen window (GNOME)
std::unique_ptr<Presenter> MakeWaylandPresenter(const Options& o);
#endif
#ifdef __APPLE__
std::unique_ptr<Source> MakeSckSource(const Options& o);
std::unique_ptr<Presenter> MakeMacPresenter(const Options& o, uint64_t followWindow);
bool ListMacWindows(std::vector<WindowInfo>& out, std::string& err);
// runs the application on the main thread (AppKit needs it) and loop() on a worker thread; loop's result
int MacMain(const std::function<int()>& loop);
#endif

// the plain message when macOS has not given the Screen Recording permission yet
extern const char* const kMacPermissionMessage;

} // namespace pmc
