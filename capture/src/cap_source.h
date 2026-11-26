// SPDX-License-Identifier: GPL-3.0-or-later
// PushMPC capture mode: where the player's picture comes from, and where it is shown.
//   Sources     X11: XComposite + XDamage + MIT-SHM (source_x11.cpp); Linux portal: xdg-desktop-portal ScreenCast +
//               PipeWire, shared memory or dma-buf (source_portal.cpp); macOS: ScreenCaptureKit (source_sck.mm)
//   Presenters  X11: an override-redirect window over the player with an empty input shape (clicks go through);
//               Wayland: a layer-shell overlay (KDE, wlroots, Hyprland, COSMIC), else a full-screen window (GNOME), both
//               with an empty input region; macOS: a borderless window above the player that ignores the mouse
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "cap_core.h"
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include "volk.h"

namespace pmc {

struct SourceFrame {
    int width = 0, height = 0;          // the visible picture
    PixFmt fmt = PixFmt::BGRX;
    const uint8_t* ptr = nullptr;       // host memory: the first visible pixel (null for a dma-buf that is not mapped)
    ptrdiff_t pitch = 0;                // bytes per row
    int dmabufFd = -1;                  // Linux: a linear dma-buf, the visible picture at dmabufOffset
    uint64_t dmabufOffset = 0, dmabufSize = 0, dmabufInode = 0;
    double time = 0;                    // when the player showed it (the compositor's time when known), Now()'s clock
    std::function<void()> release;      // gives the buffer back; call once, after the GPU / CPU has read it
    void Release() { if (release) { auto r = std::move(release); release = nullptr; r(); } }
};

class Source {
public:
    virtual ~Source() = default;
    virtual bool Start(std::string& err) = 0;
    // the next new frame, in order (at most a few are kept; older ones are dropped), waiting up to timeoutMs
    virtual bool Next(SourceFrame& f, int timeoutMs) = 0;
    // false + why: the window closed, the stream ended
    virtual bool Alive(std::string& why) = 0;
    virtual std::string Describe() const = 0;
    virtual std::string PlayerName() const { return {}; }   // the player's process / app, when known
    virtual uint64_t Window() const { return 0; }           // X11 window / macOS window number, when known
    virtual bool DmabufCapable() const { return false; }
    // HDR capture (frames then come as PixFmt::RGB10, BT.2020 PQ): true when the source switched (macOS ScreenCaptureKit)
    virtual bool SetHdr(bool on) { (void)on; return false; }
};

struct WindowInfo { uint64_t id = 0; std::string title, owner; int x = 0, y = 0, w = 0, h = 0; };

class Presenter {
public:
    virtual ~Presenter() = default;
    virtual std::vector<const char*> InstanceExtensions() const = 0;
    virtual bool Create(std::string& err) = 0;
    virtual VkSurfaceKHR Surface(VkInstance instance, std::string& err) = 0;
    virtual void Size(int& w, int& h) const = 0;
    // window events and the player's position; false = the user closed capture mode. resized: the size changed
    virtual bool Pump(bool& resized) = 0;
    virtual void SetVisible(bool on) = 0;
    virtual double RefreshHz() const = 0;   // 0 = unknown
    virtual std::string Describe() const = 0;
    // whether this window can show HDR (the swapchain then decides: Screen::Init with hdr); why: when not
    virtual bool HdrCapable(std::string& why) const { why = "X11 overlays stay SDR"; return false; }
    // the swapchain became HDR10 (macOS: the layer's extended dynamic range on)
    virtual void SetHdr(bool on) { (void)on; }
};

} // namespace pmc
