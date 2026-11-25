// Minimal inputstream add-on that shows Kodi calling DemuxSeekTime with startpts == nullptr.
// It claims .mkv files (addon.xml extension="mkv"), offers one H.264 video stream, delivers no packets, and reports
// IDEMUX | SEEK (no IPOSTIME), so CInputStreamAddon::SeekTime forwards the caller's startpts pointer to the add-on.
// kodi-dev-kit's ADDON_DemuxSeekTime turns that pointer into a reference (*startpts); writing it, as any add-on may,
// faults when the caller passed nullptr (DVDFileInfo thumbnail / stream details extraction).
#include <kodi/addon-instance/Inputstream.h>

class ATTR_DLL_LOCAL CReproStream : public kodi::addon::CInstanceInputStream
{
public:
  explicit CReproStream(const kodi::addon::IInstanceInfo& instance) : CInstanceInputStream(instance) {}

  bool Open(const kodi::addon::InputstreamProperty& props) override
  {
    kodi::Log(ADDON_LOG_INFO, "startptsrepro: Open(%s)", props.GetURL().c_str());
    return true;
  }
  void Close() override {}
  void GetCapabilities(kodi::addon::InputstreamCapabilities& caps) override
  {
    caps.SetMask(INPUTSTREAM_SUPPORTS_IDEMUX | INPUTSTREAM_SUPPORTS_SEEK);
  }
  bool GetStreamIds(std::vector<unsigned int>& ids) override
  {
    ids.push_back(1);
    return true;
  }
  bool GetStream(int streamid, kodi::addon::InputstreamInfo& info) override
  {
    info.SetStreamType(INPUTSTREAM_TYPE_VIDEO);
    info.SetCodecName("h264");
    info.SetWidth(640);
    info.SetHeight(360);
    info.SetFpsRate(24);
    info.SetFpsScale(1);
    info.SetAspect(16.0f / 9.0f);
    info.SetPhysicalIndex(1);
    return streamid == 1;
  }
  void EnableStream(int, bool) override {}
  bool OpenStream(int) override { return true; }
  DEMUX_PACKET* DemuxRead() override { return nullptr; }
  bool DemuxSeekTime(double time, bool backwards, double& startpts) override
  {
    kodi::Log(ADDON_LOG_INFO, "startptsrepro: DemuxSeekTime(time=%f, backwards=%d, &startpts=%p)", time,
              backwards ? 1 : 0, static_cast<void*>(&startpts));
    startpts = 0; // what an add-on is allowed to do with its reference
    return true;
  }
};

class ATTR_DLL_LOCAL CReproAddon : public kodi::addon::CAddonBase
{
public:
  ADDON_STATUS CreateInstance(const kodi::addon::IInstanceInfo& instance,
                              KODI_ADDON_INSTANCE_HDL& hdl) override
  {
    if (!instance.IsType(ADDON_INSTANCE_INPUTSTREAM))
      return ADDON_STATUS_NOT_IMPLEMENTED;
    hdl = new CReproStream(instance);
    return ADDON_STATUS_OK;
  }
};

ADDONCREATOR(CReproAddon)
