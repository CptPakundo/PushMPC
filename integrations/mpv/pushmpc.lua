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
-- authors publish them, fetched and checked by Setup): used instead of the set above for drawn animation MEASURED in the
-- picture (never from the file name: the image model in pushmpc-content, see below; without it mpv's fingerprint filter
-- counts frames that repeat the drawing before them, on at 20 % of the last ~4 s, off again below 10 % for ~8 s), only
-- when the picture is enlarged (their //!WHEN guard); the "-ds" (denoise + sharpen) variants are never picked
-- automatically (content-helper: the helper's path when it is not next to this script). After a few
-- seconds their measured render time
-- (vo-passes) must stay within anime-share of a frame's time, else the next lighter one takes over. They are compute
-- shaders: renderers without compute (OpenGL below 4.3) keep the fragment set.
local opts = { vpy = "", buffered = "4", concurrent = "2", ["drop-d3d11vpp"] = "yes", mode = "vs", scaling = "nvidia",
               shaders = "", ["shaders-when"] = "fallback", native = "auto", ["native-opts"] = "", ["anime-shaders"] = "", ["anime-share"] = "0.35",
               ["content-helper"] = "" }
options.read_options(opts, "pushmpc")

local shaders_on = false
local anime = { list = {}, at = 0, path = nil, timer = nil }
for e in opts["anime-shaders"]:gmatch("[^;]+") do
    local id, path = e:match("^([^=]+)=(.+)$")
    if id then anime.list[#anime.list + 1] = { id = id, path = path } end
end
-- the source frame rate from the frames' timestamps (estimated-vf-fps), the container's tag only until it exists
local function source_fps()
    local e = mp.get_property_number("estimated-vf-fps", 0)
    if e and e > 1 then return e end
    return mp.get_property_number("container-fps", 24)
end
-- the fallback measure (whole-frame holds, as pushcore's held share) where neither a PushMPC engine nor the image model
-- runs: mpv's own fingerprint
-- filter (an 8x8 gray thumbnail per frame, passed through unchanged) and a comparison of consecutive thumbnails. The
-- fingerprint filter reads only CPU frames (vf_fingerprint.c has no hardware path), so with a hardware decoder this
-- file switches to the same decoder's copy-back mode once at the start (d3d11va -> d3d11va-copy: one NV12 copy per
-- frame, no software tap of the full picture).
local meter = { on = false, held = {}, anim = false, since = 0, timer = nil, last = nil, copied = false }
local function meter_stop()
    if meter.timer then meter.timer:kill() end
    if meter.on then mp.commandv("vf", "remove", "@pmcd") end
    meter.on, meter.held, meter.anim, meter.since, meter.timer, meter.last = false, {}, false, 0, nil, nil
end
local function fp_diff(a, b)
    if not a or not b or #a ~= #b then return 255 end
    local s = 0
    for i = 1, #a, 2 do s = s + math.abs(tonumber(a:sub(i, i + 1), 16) - tonumber(b:sub(i, i + 1), 16)) end
    return s / (#a / 2)
end
local function meter_start(on_change)
    if meter.on then return true end
    local hw = mp.get_property("hwdec-current") or "no"
    -- copy-back only up to 1080p (one NV12 copy of at most 3 MB a frame); above that the hardware path stays as it is and
    -- nothing is measured here (the live-action set stays: the safe side), so 4K playback never gets heavier
    local vp = mp.get_property_native("video-params") or {}
    if hw ~= "no" and hw ~= "" and not hw:find("%-copy$") and (vp.h or 0) > 1088 then
        msg.info("animation measure: " .. tostring(vp.w) .. "x" .. tostring(vp.h) .. " on " .. hw .. ": not measured (no copy-back above 1080p)")
        return false
    end
    if hw ~= "no" and hw ~= "" and not hw:find("%-copy$") and not meter.copied then
        meter.copied = true
        mp.set_property("file-local-options/hwdec", hw .. "-copy")
        msg.info("animation measure: " .. hw .. " -> " .. hw .. "-copy for this file (the fingerprint filter needs CPU frames)")
    end
    if not mp.commandv("vf", "append", "@pmcd:fingerprint=type=gray-hex-8x8:clear-on-query=yes") then return false end
    meter.on = true
    meter.timer = mp.add_periodic_timer(0.1, function()
        local md = mp.get_property_native("vf-metadata/pmcd")
        if not md then return end
        for i = 0, 9 do
            local hx = md["fp" .. i .. ".hex"]
            if not hx then break end
            if meter.last then
                local h = meter.held
                h[#h + 1] = fp_diff(hx, meter.last) <= 0.75 and 1 or 0
                if #h > 96 then table.remove(h, 1) end
            end
            meter.last = hx
        end
        local h = meter.held
        if #h < 48 then return end
        local n = 0; for _, v in ipairs(h) do n = n + v end
        local share, now = n / #h, mp.get_time()
        local want = meter.anim and share >= 0.10 or (not meter.anim and share >= 0.20)
        if want ~= meter.anim then
            if meter.since == 0 then meter.since = now end
            if now - meter.since >= (want and 4 or 8) then
                meter.anim, meter.since = want, 0
                msg.info(string.format("animation measured: %s (%.0f %% of frames repeat the drawing)", want and "yes" or "no", 100 * share))
                on_change(want)
            end
        else meter.since = 0 end
    end)
    return true
end
-- measured animation through the image model where no PushMPC engine runs (the fingerprint measure above is only the
-- fallback, used when the model cannot run): pushmpc-content (next to this script, or script-opts pushmpc-content-helper)
-- is started once per mpv and ends with it (and by itself once this mpv is gone). It loads the content model once
-- (CLIP's image encoder through pmml). About once a second this script takes "screenshot-raw video": the VO renders
-- the frame it shows (from the decoder's own frame, so hardware decoding stays as it is, at any resolution) and hands
-- back bgr0 at the video's size (mpv has no smaller size to ask for). Only the rows the model's 224 x 224 centre square
-- samples go to the helper (a file in the temp folder, renamed into place: mpv's JSON IPC cannot carry bytes). The
-- helper integrates the evidence with ContentDetect's tracker and writes the class, the CG flag (rendered 3D CG among
-- the drawn looks) and the policy into its status file, which this script follows. The time a screenshot costs is
-- measured: the interval grows (up to 2.5 s) so that it stays within 2 % of the player's time.
local cd = { state = "off", why = "", proc = nil, timer = nil, on_change = nil, anim = false, ds = true, seq = 0, sent = -1,
             cut = 2, next = 0, interval = 1, fails = 0, started = 0, model = "", first = 1 }
local is_windows = package.config:sub(1, 1) == "\\"
local function cd_base()   -- the helper uses the same folder: TEMP / TMP on Windows, TMPDIR or /tmp elsewhere
    local dir
    if is_windows then dir = os.getenv("TEMP") or os.getenv("TMP") or "." else dir = os.getenv("TMPDIR") or "/tmp" end
    if dir == "" then dir = is_windows and "." or "/tmp" end
    local sep = package.config:sub(1, 1)
    local tag = (mp.get_script_name() or ""):gsub("[^%w%-_]", "")
    return dir .. (dir:sub(-1) == sep and "" or sep) .. "pushmpc-cd-" .. tostring(mp.get_property_number("pid", 0)) .. (tag ~= "" and ("-" .. tag) or "")
end
-- the helper: script-opts content-helper, else next to this script, else in the mpv route's install folder (copies of
-- this script that ship elsewhere, e.g. with PushMPC's own mpv build, use the one Setup installed)
local function cd_helper_path()
    local exe = "pushmpc-content" .. (is_windows and ".exe" or "")
    local list = {}
    if opts["content-helper"] ~= "" then list[1] = opts["content-helper"] end
    local dir = (debug.getinfo(1, "S").source or ""):match("^@(.*)[/\\][^/\\]*$")
    if dir then list[#list + 1] = dir .. "/" .. exe end
    if is_windows then
        if os.getenv("LOCALAPPDATA") then list[#list + 1] = os.getenv("LOCALAPPDATA") .. "/PushMPC/vsplugin/" .. exe end
    else
        local data = os.getenv("XDG_DATA_HOME") or ((os.getenv("HOME") or "") .. "/.local/share")
        list[#list + 1] = data .. "/pushmpc/vsplugin/" .. exe
    end
    for _, p in ipairs(list) do
        local f = io.open(p, "rb")
        if f then f:close(); return p end
    end
    return nil
end
local function cd_status()
    local f = io.open(cd_base() .. ".st", "rb")
    if not f then return nil end
    local t = f:read("*a"); f:close()
    local s = {}
    for line in t:gmatch("[^\n]+") do
        local k, v = line:match("^([^=]+)=(.*)$")
        if k then s[k] = v end
    end
    return s["pushmpc-content"] and s or nil
end
local function cd_quit()
    cd.state = "quit"
    if cd.timer then cd.timer:kill(); cd.timer = nil end
    if cd.proc then mp.abort_async_command(cd.proc); cd.proc = nil end
    local b = cd_base()
    os.remove(b .. ".in"); os.remove(b .. ".in.tmp"); os.remove(b .. ".st")
end
mp.register_event("shutdown", cd_quit)
-- the helper failed or is absent: the fingerprint measure for this file and the next ones
local function cd_fallback(why)
    local active = cd.timer ~= nil   -- a file is being measured now
    if cd.timer then cd.timer:kill(); cd.timer = nil end
    if cd.state == "failed" or cd.state == "quit" then return end
    cd.state, cd.why = "failed", why
    if cd.proc then mp.abort_async_command(cd.proc); cd.proc = nil end
    msg.info("animation measure: fingerprint held share (fallback: " .. why .. ")")
    if active and cd.on_change then meter_start(cd.on_change) end
end
-- one picture to the helper: the source rows (bgr0, the centre columns plus one on each side) its 224 x 224 centre
-- square samples bilinearly, two per output row, with ClipInput's sample positions (content_helper.h: the protocol)
local function cd_sample()
    local t0 = mp.get_time()
    local r = mp.command_native({ "screenshot-raw", "video" })
    if type(r) ~= "table" or type(r.data) ~= "string" or (r.format and r.format ~= "bgr0") or not r.w or r.w < 16 or r.h < 16 then
        cd.fails = cd.fails + 1
        if cd.fails >= 5 then cd_fallback("mpv returned no picture for screenshot-raw") end
        return
    end
    cd.fails = 0
    local w, h, stride, data = r.w, r.h, r.stride, r.data
    r = nil
    local side = math.min(w, h)
    local x0, y0 = math.floor((w - side) / 2), math.floor((h - side) / 2)
    local cx0 = math.max(0, x0 - 1)
    local ncol = math.min(w - 1, x0 + side) - cx0 + 1
    cd.seq = cd.seq + 1
    local parts = { string.format("PMCD1 %d %d %d %d %d %d %d %.3f %d %d\n", w, h, side, x0, y0, cx0, ncol,
                                  mp.get_property_number("time-pos", 0), cd.cut, cd.seq) }
    for j = 0, 223 do
        local iy = math.floor(y0 + (j + 0.5) * side / 224 - 0.5)
        for k = 0, 1 do
            local row = math.max(0, math.min(h - 1, iy + k))
            local off = row * stride + cx0 * 4
            parts[#parts + 1] = data:sub(off + 1, off + ncol * 4)
        end
    end
    data = nil
    local b = cd_base()
    local f = io.open(b .. ".in.tmp", "wb")
    if f then
        f:write(table.concat(parts)); f:close()
        if os.rename(b .. ".in.tmp", b .. ".in") then cd.cut, cd.sent = 0, cd.seq else os.remove(b .. ".in.tmp") end
    end
    parts = nil
    collectgarbage()   -- the picture string (w * h * 4 bytes) goes now, not at the next full cycle
    local dt = mp.get_time() - t0
    cd.interval = math.max(1, math.min(2.5, dt / 0.02))
    cd.next = mp.get_time() + cd.interval
    msg.verbose(string.format("animation measure: picture %d (%dx%d) took %.1f ms here, next in %.1f s", cd.seq, w, h, dt * 1000, cd.interval))
end
local function cd_tick()
    local s = cd_status()
    if s and s.state == "failed" then cd_fallback(s.reason ~= "" and s.reason or "the image model failed"); return end
    if cd.state == "loading" then
        if s and s.state == "ready" then
            cd.state, cd.model = "ready", s.model or ""
            msg.info("animation measure: the image model (" .. cd.model .. "), hardware decoding unchanged")
        elseif mp.get_time() - cd.started > 90 then
            cd_fallback("the image model did not load within 90 s"); return
        end
    end
    -- (only answers for this file's pictures)
    if s and tonumber(s.seq or "") and tonumber(s.seq) >= cd.first then
        local anim = s.class == "drawn" and s.animeUpscaler == "1"
        local ds = s.animeUpscalerDs == "1"
        if anim ~= cd.anim or (anim and ds ~= cd.ds) then
            cd.anim, cd.ds = anim, ds
            msg.info(string.format("animation measured by the image model: %s%s (p %.2f, CG %s, %.0f ms)", anim and "drawn" or "not drawn",
                anim and (s.cg == "1" and ", rendered 3D CG" or ", hand-drawn") or "", tonumber(s.pDrawn) or 0, s.pCg or "-", tonumber(s.ms) or 0))
            if cd.on_change then cd.on_change(anim, ds) end
        end
    end
    -- the next picture once the model is loaded and the helper took the last one, while the video plays
    if cd.state ~= "ready" or mp.get_time() < cd.next or mp.get_property_native("pause") or mp.get_property_native("seeking") then return end
    if not mp.get_property_native("video-params") then return end
    local f = io.open(cd_base() .. ".in", "rb")
    if f then f:close(); return end
    cd_sample()
end
-- the measure for this file: the image model when the helper is there, else the fingerprint held share
local function detect_start(on_change)
    cd.on_change, cd.anim, cd.ds, cd.first = on_change, false, true, cd.seq + 1
    if cd.state == "failed" then return meter_start(on_change) end
    if cd.state == "off" then
        local p = cd_helper_path()
        if not p then
            cd.state = "failed"
            msg.info("animation measure: fingerprint held share (fallback: no pushmpc-content next to this script)")
            return meter_start(on_change)
        end
        local b = cd_base()
        os.remove(b .. ".in"); os.remove(b .. ".st")
        cd.state, cd.started = "loading", mp.get_time()
        cd.proc = mp.command_native_async({ name = "subprocess", playback_only = false, capture_stdout = true, capture_stderr = true,
            args = { p, "--pid", tostring(mp.get_property_number("pid", 0)), "--tag", mp.get_script_name() or "" } },
            function(ok, res)
                cd.proc = nil
                if cd.state ~= "failed" and cd.state ~= "quit" then
                    local s = cd_status()
                    cd_fallback(s and s.state == "failed" and s.reason ~= "" and s.reason or
                        ("pushmpc-content ended (" .. tostring(res and (res.error_string ~= "" and res.error_string or res.status)) .. ")"))
                end
            end)
        msg.verbose("animation measure: starting " .. p)
    end
    if cd.timer then cd.timer:kill() end
    cd.next = 0
    cd.timer = mp.add_periodic_timer(0.25, cd_tick)
    return true
end
local function detect_stop()
    if cd.timer then cd.timer:kill(); cd.timer = nil end
    cd.anim, cd.cut = false, 2
    meter_stop()
end
mp.register_event("playback-restart", function() if cd.cut == 0 then cd.cut = 1 end end)
mp.register_event("file-loaded", function() cd.cut = 2 end)
local function low_bitrate()
    local br, vp, fps = mp.get_property_number("video-bitrate", 0), mp.get_property_native("video-params"), source_fps()
    if br <= 0 or not vp or not vp.w or vp.w <= 0 then return false end
    return br / (vp.w * vp.h * math.max(fps, 1)) <= 0.05   -- bits per pixel per frame (H.264-class low bitrate)
end
local function set_anime(i)
    if anime.path then mp.commandv("change-list", "glsl-shaders", "remove", anime.path); anime.path = nil end
    local e = anime.list[i]
    if not e then return false end
    -- the -ds variant only where the measured policy allows it: today never automatically (measured worse than the plain
    -- model on clean pictures and on release encodes alike); it stays selectable by hand (pushmpc-anime-shaders order)
    if anime.ds == true and low_bitrate() and not e.id:find("%-ds$") then
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
        local fps = math.max(mp.get_property_number("display-fps", 0), source_fps())
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
local function live_set(add)
    if opts.shaders == "" then return end
    local have = {}
    for _, p in ipairs(mp.get_property_native("glsl-shaders") or {}) do have[p] = true end
    for p in opts.shaders:gmatch("[^;]+") do
        if add and not have[p] then mp.commandv("change-list", "glsl-shaders", "append", p) end
        if not add and have[p] then mp.commandv("change-list", "glsl-shaders", "remove", p) end
    end
end
local function shaders(why)
    if shaders_on then return end
    shaders_on = true
    live_set(true)
    if opts.shaders ~= "" then msg.info("upscaling shaders added (" .. why .. "): " .. opts.shaders) end
    -- the anime set replaces it while the picture is measured as animation (ds: the policy allows the -ds variant;
    -- the fingerprint fallback has no CG flag and keeps the hand-drawn policy)
    if #anime.list > 0 then
        detect_start(function(anim, ds)
            anime.ds = ds ~= false
            if anim then
                local first = 1
                while anime.list[first] and anime.list[first].id:find("%-ds$") do first = first + 1 end
                if anime.path and anime.at > 0 then first = anime.at end   -- the CG flag changed: the same model, -ds or not
                if set_anime(first) then live_set(false) end
            else
                if anime.timer then anime.timer:kill() end
                if anime.path then mp.commandv("change-list", "glsl-shaders", "remove", anime.path); anime.path = nil end
                live_set(true)
            end
        end)
    end
end
mp.register_event("end-file", function() detect_stop(); meter.copied = false; anime.at = 0; if anime.timer then anime.timer:kill() end; if anime.path then mp.commandv("change-list", "glsl-shaders", "remove", anime.path); anime.path = nil end; shaders_on = false end)

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
-- the VapourSynth filter measures the class and the real frame rate on the frames (vsplugin/src/vs_content.h) and writes
-- them to <temp>/pushmpc-vs-<pid>.txt; when the running instance was created with another rate or class (reload=1),
-- the filter is added again once (as a seek does: mpv reloads the script) and the new instance starts with them.
-- A re-add drops the frames in flight (about 9: mpv's buffer, the concurrent requests, Dedup's lookahead): never while
-- encoding (--o: every frame counts; the encode keeps the instance it started with). The status file is removed once
-- acted on: the new instance writes it again only when its own measurement differs (a stale reload=1 left by the
-- previous instance re-added the filter a second time)
local vs_watch = { timer = nil, reloads = 0 }
local function vs_status_path()
    local dir = os.getenv("TEMP") or os.getenv("TMPDIR") or "/tmp"
    local sep = package.config:sub(1, 1)
    return dir .. (dir:sub(-1) == sep and "" or sep) .. "pushmpc-vs-" .. tostring(mp.get_property_number("pid", 0)) .. ".txt"
end
mp.register_event("end-file", function() if vs_watch.timer then vs_watch.timer:kill() end; vs_watch.reloads = 0; os.remove(vs_status_path()); os.remove(vs_status_path() .. ".taken") end)
function vs_status_watch()
    if vs_watch.timer then vs_watch.timer:kill() end
    vs_watch.timer = mp.add_periodic_timer(2, function()
        local f = io.open(vs_status_path(), "r")
        if not f then return end
        local t = f:read("*a"); f:close()
        if not t:find("reload=1", 1, true) or vs_watch.reloads >= 2 then return end
        if (mp.get_property("o", "") or "") ~= "" then return end   -- encoding: keep every frame
        -- claimed by renaming (atomic): with the script loaded twice (mpv.conf read twice), only one copy re-adds
        local taken = vs_status_path() .. ".taken"
        os.remove(taken)
        if not os.rename(vs_status_path(), taken) then return end
        os.remove(taken)
        vs_watch.reloads = vs_watch.reloads + 1
        msg.info("PushMPC: measured on the frames: " .. t:gsub("\n", " ") .. "-> filter added again with the measurement")
        mp.commandv("vf", "remove", "@pushmpc")
        mp.commandv("vf", "append", spec)
    end)
end
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
        vs_status_watch()
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
