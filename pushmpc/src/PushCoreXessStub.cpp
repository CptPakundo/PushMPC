// pushcore built without the XeSS SDK headers: XeFG and XeSS SR report themselves unavailable (PushCoreXess.h).
#include "PushCoreXess.h"
#include "veyra/RuntimePaths.h"

namespace pushmpc::xess {
namespace { const char* kWhy = "this pushcore was built without the Intel XeSS SDK"; }
std::filesystem::path runtimeFolder() { return veyra::runtime::localDataDirectory() / L"intel" / L"experimental"; }
FileCheck checkFile(const std::filesystem::path&) { FileCheck c; c.why = kWhy; return c; }
bool fgFilesUsable(std::string* why) { if (why) *why = kWhy; return false; }
bool srFileUsable(std::string* why) { if (why) *why = kWhy; return false; }
struct XefgStage::Impl {};
XefgStage::XefgStage() = default;
XefgStage::~XefgStage() = default;
bool XefgStage::init(const XefgConfig&, std::string& why) { why = kWhy; return false; }
void XefgStage::shutdown() {}
bool XefgStage::submit(ID3D12Resource*, ID3D12Resource*, bool, float, int64_t, uint64_t, std::string& why) { why = kWhy; return false; }
bool XefgStage::take(Output&) { return false; }
void XefgStage::release(uint32_t, ID3D12Fence*, uint64_t) {}
bool XefgStage::canSubmit() const { return false; }
uint64_t XefgStage::pending() { return 0; }
void XefgStage::flush() {}
uint32_t XefgStage::slotCount() const { return 0; }
ID3D12Resource* XefgStage::slotResource(uint32_t) const { return nullptr; }
XefgStage::Counters XefgStage::counters() const { return {}; }
XefgSelfTestResult xefgSelfTest(uint64_t, bool, uint32_t, uint32_t, uint32_t, uint32_t) { XefgSelfTestResult r; r.note = kWhy; return r; }
const void* srProvider(uint32_t) { return nullptr; }
bool srProviderBuilt() { return false; }
XessSrSelfTestResult xessSrSelfTest(uint64_t, bool, uint32_t, uint32_t, uint32_t) { XessSrSelfTestResult r; r.note = kWhy; return r; }
}  // namespace pushmpc::xess
