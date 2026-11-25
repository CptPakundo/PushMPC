-- PushMPC for the Snap VLC: a VLC Lua interface script (lua-intf=pushmpc_boot, extraintf=luaintf in the snap's vlcrc).
-- It loads PushMPC's plugin from the snap's common folder and calls its bootstrap, which checks VLC's module layout and
-- hands VLC's postproc filter slot to PushMPC, or changes nothing. Off: remove this file, or PUSHMPC_SNAP_BOOT=0.
local base = os.getenv("SNAP_USER_COMMON")
if not base then
  vlc.msg.info("PushMPC boot: not in a snap, nothing to do")
  return
end
local so = base .. "/pushmpc/libpushmpc_plugin.so"
local boot, err = package.loadlib(so, "luaopen_pushmpc_boot")
if not boot then
  vlc.msg.err("PushMPC boot: cannot load " .. so .. ": " .. tostring(err))
  return
end
boot()
vlc.msg.info("PushMPC boot: bootstrap called")
