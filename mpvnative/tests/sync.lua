-- mpv CI helper for the native filter: after ~3 s of playback, print what mpv's frame timing sees, seek once (mpv
-- rebuilds the filter graph), print again, quit. With --vo=null --vo-null-fps=<display> and --video-sync=display-resample
-- this shows whether display-resample locks onto the filter's output rate.
local step = 0
local function report(tag)
    local p = function(n) return tostring(mp.get_property(n, "?")) end
    mp.msg.info(string.format("pushmpc-ci %s: estimated-vf-fps=%s container-fps=%s display-fps=%s vsync-ratio=%s video-speed-correction=%s frame-drop-count=%s mistimed-frame-count=%s vo-delayed-frame-count=%s",
        tag, p("estimated-vf-fps"), p("container-fps"), p("display-fps"), p("vsync-ratio"), p("video-speed-correction"),
        p("frame-drop-count"), p("mistimed-frame-count"), p("vo-delayed-frame-count")))
end
mp.add_periodic_timer(1.0, function()
    step = step + 1
    if step == 3 then report("before-seek"); mp.commandv("seek", "1", "absolute")
    elseif step == 6 then report("after-seek"); mp.commandv("quit")
    end
end)
