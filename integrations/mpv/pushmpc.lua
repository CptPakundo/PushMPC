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

-- Upscaling shaders (pushmpc-shaders, ";" between paths; PushMPC ships ArtCNN fragment shaders): shaders-when=always
-- adds them for every video (no better upscaler on this PC), =fallback only when PushMPC's filter or d3d11vpp could not
-- start. They carry a //!WHEN guard, so they run only when the video is shown at least 1.3x its size.
-- Anime upscaling shaders (pushmpc-anime-shaders: "id=path;..." best first; ArtCNN (MIT) and CuNNy (LGPL-3.0) as their
-- authors publish them, fetched and checked by Setup): used instead of the set above for animation (the file's path
-- names it: anime / animation / animated / cartoon(s) as whole words), only when the picture is enlarged (their
-- //!WHEN guard), the "-ds" (denoise + sharpen) variant at a low bitrate. After a few seconds their measured render time
-- (vo-passes) must stay within anime-share of a frame's time, else the next lighter one takes over. They are compute
-- shaders: renderers without compute (OpenGL below 4.3) keep the fragment set.
local opts = { vpy = "", buffered = "4", concurrent = "2", ["drop-d3d11vpp"] = "yes", mode = "vs", scaling = "nvidia",
               shaders = "", ["shaders-when"] = "fallback", native = "auto", ["native-opts"] = "", ["anime-shaders"] = "", ["anime-share"] = "0.35" }
options.read_options(opts, "pushmpc")

local shaders_on = false
local anime = { list = {}, at = 0, path = nil, timer = nil }
for e in opts["anime-shaders"]:gmatch("[^;]+") do
    local id, path = e:match("^([^=]+)=(.+)$")
    if id then anime.list[#anime.list + 1] = { id = id, path = path } end
end
local function is_animation()
    local p = (mp.get_property("path") or ""):lower():gsub("\\", "/")
    for _, w in ipairs({ "anime", "animation", "animations", "animated", "cartoon", "cartoons" }) do
        if p:find("%f[%w]" .. w .. "%f[^%w]") then return true end
    end
    return false
end
local function low_bitrate()
    local br, vp, fps = mp.get_property_number("video-bitrate", 0), mp.get_property_native("video-params"), mp.get_property_number("container-fps", 24)
    if br <= 0 or not vp or not vp.w or vp.w <= 0 then return false end
    return br / (vp.w * vp.h * math.max(fps, 1)) <= 0.05   -- bits per pixel per frame (H.264-class low bitrate)
end
local function set_anime(i)
    if anime.path then mp.commandv("change-list", "glsl-shaders", "remove", anime.path); anime.path = nil end
    local e = anime.list[i]
    if not e then return false end
    if low_bitrate() and not e.id:find("%-ds$") then
        for _, x in ipairs(anime.list) do if x.id == e.id .. "-ds" then e = x end end
    end
    mp.commandv("change-list", "glsl-shaders", "append", e.path)
    anime.at, anime.path = i, e.path
    msg.info("anime upscaling shader: " .. e.id)
    -- the measured cost: the shader's passes in vo-passes (average ns) against the share of a frame's time
    if anime.timer then anime.timer:kill() end
    anime.timer = mp.add_timeout(4, function()
        local passes, ns = mp.get_property_native("vo-passes"), 0
        for _, kind in ipairs({ "fresh" }) do
            for _, ps in ipairs((passes and passes[kind]) or {}) do
                if ps.desc and (ps.desc:find("ArtCNN") or ps.desc:find("CuNNy")) then ns = ns + (ps.avg or 0) end
            end
        end
        local fps = math.max(mp.get_property_number("display-fps", 0), mp.get_property_number("container-fps", 24))
        local budget = tonumber(opts["anime-share"]) * 1e9 / math.max(fps, 1)
        msg.info(string.format("anime upscaling shader %s: %.2f ms a frame (%.2f allowed)", e.id, ns / 1e6, budget / 1e6))
        if ns > budget then
            local j = i + 1
            while anime.list[j] and anime.list[j].id:find("%-ds$") do j = j + 1 end
            if anime.list[j] then set_anime(j) else msg.warn("no anime shader fits this frame time: keeping " .. e.id) end
        end
    end)
    return true
end
local function shaders(why)
    if shaders_on then return end
    if #anime.list > 0 and is_animation() then
        local first = 1
        while anime.list[first] and anime.list[first].id:find("%-ds$") do first = first + 1 end
        if set_anime(first) then shaders_on = true; return end
    end
    if opts.shaders == "" then return end
    local have = {}
    for _, p in ipairs(mp.get_property_native("glsl-shaders") or {}) do have[p] = true end
    for p in opts.shaders:gmatch("[^;]+") do
        if not have[p] then mp.commandv("change-list", "glsl-shaders", "append", p) end
    end
    shaders_on = true
    msg.info("upscaling shaders added (" .. why .. "): " .. opts.shaders)
end
mp.register_event("end-file", function() if anime.timer then anime.timer:kill() end; if anime.path then mp.commandv("change-list", "glsl-shaders", "remove", anime.path); anime.path = nil end; shaders_on = false end)

if opts.mode == "vsr" then
    local failed_for, last = nil, nil
    local function vsr()
        if opts.scaling == "" then shaders("no d3d11vpp super resolution on this GPU"); return end
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
        else failed_for = path; last = nil; msg.warn("d3d11vpp could not start (" .. tostring(err) .. ")"); shaders("d3d11vpp unavailable") end
    end
    mp.register_event("file-loaded", function() failed_for = nil; last = nil end)
    mp.observe_property("video-params", "native", function() vsr() end)
    mp.observe_property("display-height", "native", function() vsr() end)
    return
end

-- One copy of this script per player: a libmpv that PushMPC swapped in loads the block itself (pushmpc-libmpv.ps1),
-- and the player's own config may name the script too
do
    local owner = mp.get_property("user-data/pushmpc/script", "")
    if owner ~= "" and owner ~= mp.get_script_name() then
        msg.verbose("already running as " .. owner .. ": this copy does nothing")
        return
    end
    mp.set_property("user-data/pushmpc/script", mp.get_script_name())
end

-- native (pushmpc-native=auto | no): PushMPC's own libmpv has the "pushmpc" filter itself (vf_pushmpc: decoder textures
-- straight into PushMPC's engine, no copy to the CPU and back). Then it is used instead of VapourSynth, with D3D11
-- decoding (hwdec=d3d11va when the block's auto-copy or libmpv's default is in effect; software frames get an upload
-- to the GPU in front of the filter). Any other libmpv refuses the name and the VapourSynth route below runs as before.
-- (PushMPC's builds carry PUSHMPC_BUILD in mpv-configuration: other libmpvs are not probed, so they log no error)
local native = false
if opts.native == "yes" or (opts.native ~= "no" and mp.get_property("mpv-configuration", ""):find("PUSHMPC_BUILD", 1, true)) then
    local ok = mp.commandv("vf", "add", "@pushmpc-probe:pushmpc")
    if ok then
        mp.commandv("vf", "remove", "@pushmpc-probe")
        native = true
        local hw = mp.get_property("options/hwdec", "")
        -- (libmpv starts with "no": a player that sets nothing gets D3D11 decoding, the zero-copy input)
        if hw == "auto-copy" or hw == "" or hw == "no" then mp.set_property("options/hwdec", "d3d11va,auto") end
        msg.info("PushMPC's own libmpv: the native filter (vf_pushmpc) is used")
    end
end

if not native and opts.vpy == "" then
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

-- the native filter's list: vf_pushmpc with the display rate when mpv knows it, and on software frames an upload to
-- the player's D3D11 device first
local function native_specs()
    local o = {}
    local disp = mp.get_property_number("display-fps", 0)
    if disp and disp > 0 then o[#o + 1] = string.format("display_fps=%.3f", disp) end
    if opts["native-opts"] ~= "" then o[#o + 1] = opts["native-opts"] end
    local s = "@pushmpc:pushmpc" .. (#o > 0 and ("=" .. table.concat(o, ":")) or "")
    if mp.get_property("hwdec-current", "no") == "d3d11va" then return { s } end
    local vp = mp.get_property_native("video-params") or {}
    local fmt = tostring(vp.pixelformat or ""):match("10") and "p010" or "nv12"
    return { "@pushmpc-fmt:format=" .. fmt, "@pushmpc-up:hwupload", s }
end
local native_last = nil
local native_up_failed = nil   -- the file on which the upload could not start: vf_pushmpc on CPU frames then

local failed_for = nil   -- the file the filter failed on: no retry until the next file
local busy = false

local function remove_native()
    for _, f in ipairs(mp.get_property_native("vf") or {}) do
        if f.label == "pushmpc" or f.label == "pushmpc-fmt" or f.label == "pushmpc-up" then mp.commandv("vf", "remove", "@" .. f.label) end
    end
end

local function ensure()
    if busy then return end
    if mp.get_property_native("video-params") == nil then return end   -- only once video runs: soft failure
    local path = mp.get_property("path")
    if failed_for ~= nil and failed_for == path then return end
    local vf = mp.get_property_native("vf") or {}
    local ours = false
    for _, f in ipairs(vf) do if f.label == "pushmpc" then ours = true end end
    busy = true
    if native then
        local list = native_specs()
        if native_up_failed == path then list = { list[#list] } end
        local want = table.concat(list, ",")
        if not ours or want ~= native_last then
            if ours then remove_native() end
            local ok, err = true, nil
            for _, s in ipairs(list) do
                ok, err = mp.commandv("vf", "append", s)
                if not ok then break end
            end
            if not ok and #list > 1 then
                -- no D3D11 device to upload to (a player without one): vf_pushmpc on the CPU frames
                remove_native(); native_up_failed = path
                list = { list[#list] }; want = list[1]
                ok, err = mp.commandv("vf", "append", want)
            end
            if not ok then
                remove_native()
                failed_for = path; native_last = nil
                msg.warn("PushMPC's native filter could not start (" .. tostring(err) .. "): playing without it")
                shaders("PushMPC's filter unavailable")
                busy = false
                return
            end
            native_last = want
            msg.info("PushMPC's native filter added: " .. want)
        end
    elseif not ours then
        local ok, err = mp.commandv("vf", "append", spec)
        if not ok then
            failed_for = path
            msg.warn("PushMPC's filter could not start (" .. tostring(err) .. "): playing without it")
            shaders("PushMPC's filter unavailable")
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

mp.register_event("file-loaded", function()
    failed_for = nil
    if opts["shaders-when"] == "always" then shaders("no better upscaler on this PC") end
end)
mp.observe_property("video-params", "native", function() ensure() end)
mp.observe_property("vf", "native", function() ensure() end)
