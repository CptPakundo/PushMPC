-- pushmpc-native.lua: drives the native (libavfilter) route in mpv. The filter cannot see mpv's display, so this
-- script tells it: on file load and whenever display-fps changes, it (re)sets the @pushmpc filter with the display rate.
--   script-opts:
--     pushmpc-engine=pushmpc   vf_pushmpc (our FFmpeg / the runtime hook): pushcore on D3D11, portable engines
--     pushmpc-engine=fruc      FFmpeg's fruc_vulkan (NVIDIA optical flow, RTX 30+; needs hwdec=vulkan, gpu-api=vulkan):
--                              target = the largest whole multiple of the source rate that fits the display
--     pushmpc-opts=...         extra vf_pushmpc options, e.g. smoother=4:sharper=off
-- Setup writes the matching mpv.conf block (hwdec, video-sync=display-resample, interpolation=no) and picks the
-- engine: fruc only when `mpv --vf=help` lists fruc_vulkan and the GPU is RTX 30 or newer, and only where it beats the
-- VapourSynth / pushcore route (Auto's benchmark).
local opts = { engine = "pushmpc", opts = "" }
require("mp.options").read_options(opts, "pushmpc")

local last = nil

local function multiple(src, disp)
    if not src or src <= 0 or not disp or disp <= 0 then return 2 end
    local k = math.floor(disp / src + 0.02)
    if k < 1 then k = 1 end
    if k > 16 then k = 16 end   -- PushMPC's ceiling (PUSHCORE_MAX_MULTIPLIER)
    return k
end

local function apply()
    local disp = mp.get_property_number("display-fps", 0)
    local src = mp.get_property_number("container-fps", 0)
    local spec
    if opts.engine == "fruc" then
        local k = multiple(src, disp)
        if k <= 1 then spec = nil else spec = string.format("@pushmpc:fruc_vulkan=fps=source_fps*%d", k) end
    else
        spec = "@pushmpc:pushmpc"
        local o = {}
        if disp > 0 then o[#o + 1] = string.format("display_fps=%.3f", disp) end
        -- full screen (Auto: no window composition per shown frame); through "extra", which every vf_pushmpc knows
        if not string.find(opts.opts, "extra=", 1, true) then
            o[#o + 1] = "extra=[display_fs=" .. (mp.get_property_native("fullscreen") and "1" or "0") .. "]"
        end
        if opts.opts ~= "" then o[#o + 1] = opts.opts end
        if #o > 0 then spec = spec .. "=" .. table.concat(o, ":") end
    end
    if spec == last then return end
    if last then mp.commandv("vf", "remove", "@pushmpc") end
    if spec then
        local ok = mp.commandv("vf", "append", spec)
        mp.msg.info((ok and "set " or "could not set ") .. spec)
    end
    last = spec
end

mp.register_event("file-loaded", apply)
mp.observe_property("display-fps", "number", function() if last ~= nil or mp.get_property("path") then apply() end end)
mp.observe_property("fullscreen", "bool", function() if last ~= nil then apply() end end)
