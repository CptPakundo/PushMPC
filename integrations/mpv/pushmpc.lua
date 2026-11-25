-- pushmpc.lua: adds PushMPC's VapourSynth filter (pushmpc.Enhance through pushmpc.vpy) to mpv's video filters, and
-- keeps it there. mpv.conf names this script (scripts-append) instead of carrying a vf= line, for two reasons:
--   * A vf= line whose filter cannot start (an mpv or libmpv without VapourSynth, a missing runtime) makes mpv refuse
--     to play every file. The "vf append" command used here fails softly instead: mpv keeps playing without the filter
--     and this script logs why. Players that share ~/.config/mpv or %APPDATA%\mpv (legacy Emby Theater) stay safe.
--   * Hosts that set the vf property themselves (Stremio resets it whenever its display state changes) would drop a
--     configured filter; the filter is added again after every change.
-- When PushMPC's filter is active, a d3d11vpp entry (Stremio's own RTX VSR) is removed: it needs D3D11 frames, which
-- cannot follow a VapourSynth filter, and PushMPC does the upscaling itself.
-- Options (script-opts, prefix pushmpc-): vpy (path of pushmpc.vpy), buffered (4), concurrent (2),
-- drop-d3d11vpp (yes).
-- mode=vsr (players whose libmpv has no VapourSynth, e.g. Plex for Windows / Plex HTPC): RTX Video Super Resolution
-- (or Intel's) through mpv's own d3d11vpp, scaling-mode=<scaling> (nvidia | intel), the scale from the display and
-- the video height, set again when either changes; when d3d11vpp cannot start (no D3D11 decoding, another GPU), the
-- shaders listed in pushmpc-shaders (";" between paths) are added instead, if any. Failures never stop playback.
local mp = require 'mp'
local msg = require 'mp.msg'
local options = require 'mp.options'

local opts = { vpy = "", buffered = "4", concurrent = "2", ["drop-d3d11vpp"] = "yes", mode = "vs", scaling = "nvidia", shaders = "" }
options.read_options(opts, "pushmpc")

if opts.mode == "vsr" then
    local shaders_on, failed_for, last = false, nil, nil
    local function shaders()
        if shaders_on or opts.shaders == "" then return end
        for p in opts.shaders:gmatch("[^;]+") do mp.commandv("change-list", "glsl-shaders", "append", p) end
        shaders_on = true
        msg.info("upscaling shaders added (d3d11vpp unavailable)")
    end
    local function vsr()
        local vp = mp.get_property_native("video-params")
        if vp == nil or not vp.h or vp.h <= 0 then return end
        local path = mp.get_property("path")
        if failed_for ~= nil and failed_for == path then return end
        local dh = mp.get_property_number("display-height", 0)
        if dh <= 0 then dh = mp.get_property_number("osd-height", 0) end
        local scale = dh > 0 and dh / vp.h or 2
        if scale <= 1.01 then return end            -- the video already fills the display: nothing to upscale
        if scale > 4 then scale = 4 end
        local spec = string.format("@pushmpc-vsr:d3d11vpp=scaling-mode=%s:scale=%.3f", opts.scaling, scale)
        if spec == last then return end
        for _, f in ipairs(mp.get_property_native("vf") or {}) do
            if f.label == "pushmpc-vsr" then mp.commandv("vf", "remove", "@pushmpc-vsr") end
        end
        local ok, err = mp.commandv("vf", "append", spec)
        if ok then last = spec; msg.info("RTX/Intel video super resolution: " .. spec)
        else failed_for = path; last = nil; msg.warn("d3d11vpp could not start (" .. tostring(err) .. ")"); shaders() end
    end
    mp.register_event("file-loaded", function() failed_for = nil; last = nil end)
    mp.observe_property("video-params", "native", function() vsr() end)
    mp.observe_property("display-height", "native", function() vsr() end)
    return
end

if opts.vpy == "" then
    msg.warn("no pushmpc-vpy in script-opts: nothing to do")
    return
end

-- mpv's [...] quoting keeps ':' and spaces; a path with ']' needs %n% (byte length) quoting
local function quote(p)
    p = p:gsub("\\", "/")
    if p:find("]", 1, true) then return "%" .. #p .. "%" .. p end
    return "[" .. p .. "]"
end
local spec = "@pushmpc:vapoursynth=file=" .. quote(opts.vpy) .. ":buffered-frames=" .. opts.buffered ..
             ":concurrent-frames=" .. opts.concurrent

local failed_for = nil   -- the file the filter failed on: no retry until the next file
local busy = false

local function ensure()
    if busy then return end
    if mp.get_property_native("video-params") == nil then return end   -- only once video runs: soft failure
    local path = mp.get_property("path")
    if failed_for ~= nil and failed_for == path then return end
    local vf = mp.get_property_native("vf") or {}
    local ours = false
    for _, f in ipairs(vf) do if f.label == "pushmpc" then ours = true end end
    busy = true
    if not ours then
        local ok, err = mp.commandv("vf", "append", spec)
        if not ok then
            failed_for = path
            msg.warn("PushMPC's filter could not start (" .. tostring(err) .. "): playing without it")
            busy = false
            return
        end
        msg.info("PushMPC's filter added")
    end
    if opts["drop-d3d11vpp"] == "yes" then
        local list, dropped = {}, false
        for _, f in ipairs(mp.get_property_native("vf") or {}) do
            if f.name == "d3d11vpp" then dropped = true else list[#list + 1] = f end
        end
        if dropped then
            mp.set_property_native("vf", list)
            msg.info("d3d11vpp removed: PushMPC's filter is active (it gets CPU frames and upscales itself)")
        end
    end
    busy = false
end

mp.register_event("file-loaded", function() failed_for = nil end)
mp.observe_property("video-params", "native", function() ensure() end)
mp.observe_property("vf", "native", function() ensure() end)
