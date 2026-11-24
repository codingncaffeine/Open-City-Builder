#include "sc4/MySimPool.h"

#include <cstring>
#include <string>

#include "gz/cGZPersistResourceKey.h"
#include "gz/cIGZPersistResourceKeyList.h"
#include "gz/cIGZPersistResourceManager.h"
#include "platform/Unwired.h"
#include "rz/cRZFile.h"
#include "rz/cRZSysServPtr.h"
#include "sc4/Exemplar.h"
#include "sc4/SCPropertyHelp.h"

namespace sc4 {
namespace {
using ResourceManagerPtr = cRZSysServPtr<cIGZPersistResourceManager, 0x056b906e, 0x656b8efc>;
IMySimPoolHost* g_host = nullptr;

constexpr std::uint32_t kExemplarType = 0x6534284a, kMySimGroup = 0x6a297266;
constexpr std::uint32_t kPropExemplarType = 0x10, kExemplarTypeMySim = 0x26;
constexpr std::uint32_t kPropBitmap = 0xea296f8d, kPropNameString = 0xca416b3f, kPropName = 0xea296f8f;
constexpr std::uint32_t kPropMale = 0xea296f91, kPropZodiac = 0xea296f90;
constexpr std::uint32_t kImportsMagic = 0x4a32e4aa;

// 0x0054e524: a MySim exemplar of the game's packages becomes a template (bitmap, localized name or the plain name, sex,
// zodiac sign).
void EnumKeysCallback(const cGZPersistResourceKey& key, void* context) {
    if (key.group != kMySimGroup) return;
    auto* templates = static_cast<std::vector<tMySimTemplate>*>(context);
    ResourceManagerPtr manager;
    if (!manager.get()) return;
    void* raw = nullptr;
    if (!manager->GetResource(key, kGZIID_cISCResExemplar, &raw, 0, nullptr) || !raw) return;
    auto* exemplar = static_cast<cISCResExemplar*>(raw);
    cISCPropertyHolder* holder = exemplar->AsISCPropertyHolder();
    std::uint32_t type = 0;
    if (holder && cSCPropertyHelp::GetPropertyValue(holder, kPropExemplarType, type) && type == kExemplarTypeMySim) {
        tMySimTemplate sim;
        sim.bitmapFile = cRZString("");
        cSCPropertyHelp::GetPropertyValue(holder, kPropBitmap, sim.bitmapId);
        if (!g_host || !g_host->LoadExemplarString(sim.name, holder, kPropNameString))
            cSCPropertyHelp::GetPropertyValue(holder, kPropName, sim.name);
        bool male = false;
        cSCPropertyHelp::GetPropertyValue(holder, kPropMale, male);
        sim.male = male;
        cSCPropertyHelp::GetPropertyValue(holder, kPropZodiac, sim.zodiac);
        templates->push_back(sim);
    }
    exemplar->Release();
}
}  // namespace

IMySimPoolHost* MySimPoolHost() { return g_host; }
void SetMySimPoolHost(IMySimPoolHost* host) { g_host = host; }

void cSC4MySimPool::GetMySimTemplates(std::vector<tMySimTemplate>& out) {
    if (!loaded_) {
        ResourceManagerPtr manager;
        cIGZPersistResourceKeyList* keys = nullptr;
        if (manager.get() && manager->GetAvailableResourceListForType(&keys, kExemplarType) != 0 && keys) {
            keys->EnumKeys(&EnumKeysCallback, &templates_);
        }
        if (keys) keys->Release();
        LoadImports();
        loaded_ = true;
    }
    out = templates_;
}

void cSC4MySimPool::ReadImports(const std::vector<std::uint8_t>& bytes, std::vector<tMySimTemplate>& out) {
    std::size_t at = 0;
    auto read = [&](void* to, std::size_t n) {
        if (at + n > bytes.size()) return false;
        if (n) std::memcpy(to, bytes.data() + at, n);
        at += n;
        return true;
    };
    std::uint32_t magic = 0;
    if (!read(&magic, 4) || magic != kImportsMagic) return;
    tMySimTemplate sim;   // one record reused, as the original (its bitmap id stays 0)
    for (;;) {
        std::uint8_t length = 0, flag = 0;
        char text[256];
        if (!read(&length, 1) || !read(text, length)) break;
        sim.name = cRZString(std::string(text, length).c_str());
        if (!read(&flag, 1)) break;
        sim.male = flag != 0;
        if (!read(&flag, 1)) break;
        sim.zodiac = flag;
        if (!read(&length, 1) || !read(text, length)) break;
        sim.bitmapFile = cRZString(std::string(text, length).c_str());
        sim.bitmapFile.AppendString(cRZString(".bmp"));
        out.push_back(sim);
    }
}

void cSC4MySimPool::LoadImports() {
    if (!g_host) return;
    cRZString directory;
    g_host->MySimDirectory(directory);
    std::string path(directory.ToChar());
    path += "MySims.dat";
    cRZFile file{cRZString(path.c_str())};
    if (!file.Open(1, 2, 1)) return;
    std::vector<std::uint8_t> bytes(file.Length());
    if (!bytes.empty() && !file.Read(bytes.data(), static_cast<std::uint32_t>(bytes.size()))) bytes.clear();
    file.Close();
    ReadImports(bytes, templates_);
}

bool cSC4MySimPool::Import(std::uint32_t& count) {
    count = 0;
    SC4_UNWIRED("cSC4MySimPool::Import: importing sims from The Sims is not ported");
    return false;
}

// 0x00550242 asks LaunchServices for the application with creator 'TSms' (the PC build reads HKLM\SOFTWARE\Maxis\The Sims);
// there is no such installation on Linux.
bool cSC4MySimPool::FindTheSimsInstallation() {
    simsPath_ = cRZString("");
    return false;
}

}  // namespace sc4
