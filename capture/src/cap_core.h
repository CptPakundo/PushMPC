// SPDX-License-Identifier: GPL-3.0-or-later
// PushMPC capture mode for Linux and macOS: the parts every OS shares (pure C++, no OS headers; capture_test checks them
// on any OS, Windows included).
//
//   Timeline   the player's own frame times. A player shows a 24 fps video on a 60 Hz screen in a 3:2 cadence, so the
//              captured frames arrive 50 / 33 ms apart. Capture mode fits a line through the arrival times of the last
//              frames (index -> time) and plays the frames back on that line, a fixed delay later: every screen refresh
//              shows the picture of its own moment (a generated frame between two real ones), so motion is even again.
//   Pacer      per screen refresh: which two frames and where between them (t), from the timeline; Smoother Auto = any t
//              (every refresh its own frame), x2..x8 = t on a k-step grid, off = the real frame of that moment.
//   Converter  BGRx / RGBx to YUV 4:2:0 (BT.709, limited range) for the engines that take frames in host memory.
//   Protected  copy-protected video: the system hands capture a black picture. Capture mode then steps aside (the
//              player's own picture shows) and says so once, in plain words.
//   AudioDelay the lip-sync part of a player's mpv.conf (players built on mpv), written while capture mode runs.
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace pmc {

double Now();   // seconds, the monotonic clock the capture timestamps use (CLOCK_MONOTONIC, mach_absolute_time on macOS)

// ------------------------------------------------------------------------------------------------ frames
enum class PixFmt { BGRX, RGBX };   // 32-bit pixels: B (or R) in the lowest byte

// a planar YUV 4:2:0 picture, 8 bit, rows packed
struct I420 {
    int w = 0, h = 0;
    std::vector<uint8_t> y, u, v;
    int cw() const { return (w + 1) / 2; }
    int ch() const { return (h + 1) / 2; }
    void Resize(int width, int height);
};
// BT.709 limited range; the chroma of each 2 x 2 block averaged (centre sited). threads: 0 = hardware threads (<= 8)
void ToI420(const uint8_t* px, ptrdiff_t pitch, int w, int h, PixFmt f, I420& out, int threads = 0);
// back to 32-bit pixels (tests and the dump)
void FromI420(const I420& in, uint8_t* px, ptrdiff_t pitch, PixFmt f);

// a fingerprint of the picture on a sparse grid (duplicates: a player that draws the same frame again)
uint64_t Fingerprint(const uint8_t* px, ptrdiff_t pitch, int w, int h);

// ------------------------------------------------------------------------------------------------ the timeline
struct Timeline {
    // fit over this many frames; the delay's margin above the fitted lateness
    int window = 32;
    double margin = 0.004;
    // the frame rate before the fit has enough points (and the clamp of the fit)
    double minInterval = 1.0 / 240, maxInterval = 1.0 / 5;
    // a gap longer than this (seconds, or intervals) is a pause / seek: the line starts again
    double pauseSeconds = 0.25, pauseIntervals = 4;
    // how much of the gap to the fitted line the shown position takes per refresh
    double follow = 0.05;

    // a new (changed) frame captured at time t; returns its index on the line (indices skip over frames the capture
    // missed). discontinuity: set when the line started again with this frame (no pair across it)
    uint64_t Add(double t, bool& discontinuity);
    bool Valid() const { return n_ > 0; }
    double Interval() const { return d_; }      // seconds per source frame (the fit)
    double Fps() const { return d_ > 0 ? 1.0 / d_ : 0; }
    double Delay() const { return delay_; }     // how long after its time on the line a frame is shown whole
    double Lateness() const { return late_; }   // the latest arrival above the line within the window
    // the fractional frame index to show at time T (never goes back, except after a restart)
    double Position(double T);
    uint64_t Newest() const { return lastIdx_; }
    uint64_t SegmentStart() const { return segStart_; }   // the first index after the last restart
    double TimeOf(double idx) const { return a_ + d_ * idx; }

private:
    void Fit();
    std::deque<std::pair<double, double>> pts_;   // (index, time)
    double a_ = 0, d_ = 1.0 / 24, delay_ = 0, late_ = 0, lastTime_ = 0, lastPos_ = -1e18, lastT_ = 0;
    uint64_t lastIdx_ = 0, segStart_ = 0;
    int n_ = 0;
    bool restarted_ = false;
};

// ------------------------------------------------------------------------------------------------ pacing
// what one screen refresh shows: the real frame a (t = 0), or a frame between a and b at t
struct Pick {
    bool valid = false;
    uint64_t a = 0, b = 0;
    float t = 0;          // 0 = a itself; (0, 1) = generated; never 1 (then a = b's index)
    bool generated() const { return valid && t > 0.f && a != b; }
};
// smoother: 0 Auto (any t), 1 off (real frames only), 2..8 a fixed k-step grid. frames: the indices the engine has
// (ascending). discontinuities: indices that start a new segment (no pair ending in them).
Pick Choose(double pos, int smoother, const std::vector<uint64_t>& frames, const std::vector<uint64_t>& discontinuities);

// ------------------------------------------------------------------------------------------------ copy protection
struct Protected {
    double hideAfter = 1.5;    // seconds of a black picture before capture mode steps aside
    double tellAfter = 4.0;    // ... and before it says why (once per run)
    // one captured frame: 1 = hide the overlay now, 0 = no change, -1 = show it again; told = the message is due now
    int Feed(const uint8_t* px, ptrdiff_t pitch, int w, int h, double t, bool& told);
    bool Hidden() const { return hidden_; }
    static bool Black(const uint8_t* px, ptrdiff_t pitch, int w, int h);   // >= 90 % of a sparse grid exactly black
private:
    double since_ = -1;
    bool hidden_ = false, told_ = false;
};
extern const char* const kProtectedMessage;

// ------------------------------------------------------------------------------------------------ lip sync
// the marked part of an mpv.conf (audio-delay=+0.080); an existing one is replaced; false + why when not writable
bool SetAudioDelay(const std::string& conf, int ms, std::string& why);
bool ClearAudioDelay(const std::string& conf);
std::string SetAudioDelayText(const std::string& text, int ms);   // the file's new text (tests)
std::string ClearAudioDelayText(const std::string& text);
extern const char* const kDelayBegin;
extern const char* const kDelayEnd;

// ------------------------------------------------------------------------------------------------ options
struct Options {
    std::string window;          // X11 window id / macOS window number
    std::string player;          // the player's process / app name
    bool pick = false, list = false, portal = false, x11 = false, monitor = false;
    std::string engine = "auto"; // auto | gpu | cpu | ml | apple | nvl
    int smoother = -1;           // -1 = the setting (Smoother); 0 Auto, 1 off, 2..8
    std::string upscaler;        // auto | nis | fsr | off ("" = the setting)
    bool fullscreen = false;
    double displayHz = 0;
    bool present = true;
    long frames = 0;             // stop after this many output frames (tests)
    double seconds = 0;
    std::string dump;            // .y4m of the output frames (tests)
    std::string audioConf;       // an mpv.conf for the lip-sync part
    int audioDelayMs = -1;       // -1 = measured
    std::string vulkanDevice;
    bool allowCpuVulkan = false;
    std::string log;
    bool selftest = false, help = false, verbose = false;
    std::string restoreToken;    // the portal's restore token file
};
// false + why on a bad argument
bool ParseOptions(int argc, char** argv, Options& o, std::string& why);
extern const char* const kUsage;

} // namespace pmc
