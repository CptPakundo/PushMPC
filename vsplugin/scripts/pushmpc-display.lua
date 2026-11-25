-- pushmpc-display.lua: tells PushMPC's VapourSynth filter (the vf labelled @pushmpc, pushmpc.vpy) what the display is:
-- HDR or SDR, its peak luminance and its size. mpv knows; a VapourSynth script does not. Goes into mpv's scripts folder
-- (~/.config/mpv/scripts on Linux, %APPDATA%\mpv\scripts on Windows; PushMPC's installer puts it there).
--
-- How: with --target-colorspace-hint-mode=target (set here when this mpv has it and the user has not chosen a mode),
-- mpv's video-target-params describe the display itself (Wayland's colour-management protocol, D3D11, winvk), not the
-- video: gamma pq/hlg = an HDR output, max-luma = its peak. display-width / display-height give the size. X11 has no
-- HDR output: target-params stay SDR there, which is right.
-- The values go into the filter's user-data (display_hdr, display_peak, and display_w / display_h, which mpv 0.36+ also
-- passes as display_res; any other keys
-- the user put there are kept) and the vf is re-applied only when they change (debounced; mpv reloads the script, the
-- plugin's engine stays warm). Script options (script-opts=pushmpc-display-...):
--   enable=yes        no: do nothing
--   set_hint_mode=yes no: never touch target-colorspace-hint-mode
--   label=pushmpc     the vf label to update
local mp = require "mp"
local opts = { enable = true, set_hint_mode = true, label = "pushmpc" }
require("mp.options").read_options(opts, "pushmpc-display")
if not opts.enable then return end

local msg = require "mp.msg"
local KEYS = { display_hdr = true, display_peak = true, display_w = true, display_h = true }

-- user-data "k=v,k=v" <-> table (order kept for the keys we do not own)
local function parse(ud)
    local t, order = {}, {}
    for kv in string.gmatch(ud or "", "[^,]+") do
        local k, v = string.match(kv, "^%s*([^=]-)%s*=%s*(.-)%s*$")
        if k and k ~= "" then
            if t[k] == nil then order[#order + 1] = k end
            t[k] = v
        end
    end
    return t, order
end
local function join(t, order)
    local out = {}
    for _, k in ipairs(order) do if t[k] ~= nil then out[#out + 1] = k .. "=" .. t[k] end end
    for _, k in ipairs({ "display_hdr", "display_peak", "display_w", "display_h" }) do
        local seen = false
        for _, o in ipairs(order) do if o == k then seen = true end end
        if not seen and t[k] ~= nil then out[#out + 1] = k .. "=" .. t[k] end
    end
    return table.concat(out, ",")
end

-- what the display is now (nil while mpv does not know yet)
local function display()
    local tp = mp.get_property_native("video-target-params")
    if type(tp) ~= "table" or not tp.gamma then return nil end
    local g = tostring(tp.gamma)
    local hdr = (g == "pq" or g == "hlg") and 1 or 0
    local d = { display_hdr = tostring(hdr) }
    if hdr == 1 and tonumber(tp["max-luma"]) and tonumber(tp["max-luma"]) >= 100 then d.display_peak = string.format("%d", math.floor(tonumber(tp["max-luma"]) + 0.5)) end
    local w, h = mp.get_property_number("display-width"), mp.get_property_number("display-height")
    if w and h and w > 0 and h > 0 then d.display_w = string.format("%d", w); d.display_h = string.format("%d", h) end
    return d
end

local last = nil
local function apply()
    local d = display()
    if not d then return end
    local key = (d.display_hdr or "") .. "/" .. (d.display_peak or "") .. "/" .. (d.display_w or "") .. "x" .. (d.display_h or "")
    if key == last then return end
    local vf = mp.get_property_native("vf")
    if type(vf) ~= "table" then return end
    local changed = false
    for _, f in ipairs(vf) do
        if f.label == opts.label and f.name == "vapoursynth" then
            f.params = f.params or {}
            local t, order = parse(f.params["user-data"])
            for k in pairs(KEYS) do t[k] = d[k] end
            local ud = join(t, order)
            if ud ~= (f.params["user-data"] or "") then f.params["user-data"] = ud; changed = true end
        end
    end
    last = key
    if changed then
        msg.info("display: HDR " .. d.display_hdr .. (d.display_peak and (", peak " .. d.display_peak .. " cd/m2") or "") ..
                 (d.display_w and (", " .. d.display_w .. "x" .. d.display_h) or "") .. ": PushMPC's filter updated")
        mp.set_property_native("vf", vf)
    end
end

-- the hint mode that makes video-target-params describe the display
if opts.set_hint_mode then
    local mode = mp.get_property("options/target-colorspace-hint-mode")
    if mode == nil then
        msg.verbose("this mpv has no target-colorspace-hint-mode: HDR output is not detected (set display_hdr in user-data)")
    elseif mode ~= "target" and mp.get_property("options/target-colorspace-hint") ~= "no" then
        mp.set_property("options/target-colorspace-hint-mode", "target")
    end
end

local timer = nil
local function later()   -- debounce: the target and the window settle together
    if timer then timer:kill() end
    timer = mp.add_timeout(0.5, apply)
end
mp.observe_property("video-target-params", "native", later)
mp.observe_property("display-width", "number", later)
mp.observe_property("display-height", "number", later)
mp.register_event("file-loaded", function() last = nil; later() end)
