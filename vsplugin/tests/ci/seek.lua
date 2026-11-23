-- mpv CI helper: after playback starts, seek twice (each seek reloads the VapourSynth script), then quit.
-- The plugin log must then show the warm core reused ("flush (warm core reused)") for the reloads.
local step = 0
mp.add_periodic_timer(0.7, function()
    step = step + 1
    if step == 1 then mp.commandv("seek", "1", "absolute")
    elseif step == 2 then mp.commandv("seek", "0.5", "absolute")
    elseif step >= 4 then
        mp.msg.info("pushmpc-ci: estimated-vf-fps=" .. tostring(mp.get_property_number("estimated-vf-fps", 0)))
        mp.commandv("quit")
    end
end)
