// cSC4MySimPool (i386 0x0054e108..0x00550242, SC4Advisors/source/SC4MySimPool.cpp): the sims a player can move into the city.
// The templates are the game's MySim exemplars (type 0x6534284a, group 0x6a297266, exemplar type 0x26) plus the sims imported
// earlier, listed in "<MySim directory>MySims.dat". Import (0x0054e72a) and FindTheSimsInstallation (0x00550242) bring sims over
// from an installation of The Sims; the Mac build finds it with LaunchServices (creator 'TSms'), the PC build in the registry.
#pragma once
#include <cstdint>
#include <vector>

#include "rz/cRZString.h"

namespace sc4 {

class cISCPropertyHolder;

// cISC4MySimPool::tMySimTemplate (0x20 bytes)
struct tMySimTemplate {
    std::uint32_t bitmapId = 0;   // +0x00 portrait image instance (group 0x46a006b0); 0 = the bitmap file below
    cRZString bitmapFile;         // +0x04 portrait file in the MySim directory (imported sims)
    cRZString name;               // +0x10
    bool male = false;            // +0x1c
    std::uint8_t zodiac = 0;      // +0x1d 0..11
};

// What the pool asks the game for.
class IMySimPoolHost {
public:
    virtual ~IMySimPoolHost() = default;
    virtual void MySimDirectory(cIGZString& out) = 0;   // SL::spSC4 vslot 0xec GetMySimDirectory
    // SC4LoadExemplarString(out, group, instance, property): the localized string the exemplar's property points at
    virtual bool LoadExemplarString(cIGZString& out, cISCPropertyHolder* exemplar, std::uint32_t property) = 0;
};
IMySimPoolHost* MySimPoolHost();
void SetMySimPoolHost(IMySimPoolHost* host);

class cSC4MySimPool {
public:
    cSC4MySimPool() = default;                                    // 0x0054e1c0
    virtual ~cSC4MySimPool() = default;                           // 0x0054e108
    // vslot 0x1c: the game's templates and the imported sims (read once), copied into `out`
    virtual void GetMySimTemplates(std::vector<tMySimTemplate>& out);   // 0x0054e6a6
    // vslot 0x20: imports the sims of The Sims; `count` = how many. NOT PORTED (no installation is found natively).
    virtual bool Import(std::uint32_t& count);
    // vslot 0x24: whether an installation of The Sims was found. Linux has none: false.
    virtual bool FindTheSimsInstallation();

    // the MySims.dat reader (0x0054e220): magic 0x4a32e4aa, then per sim: name (byte length + bytes), male byte, zodiac byte,
    // bitmap file name (byte length + bytes) + ".bmp"
    static void ReadImports(const std::vector<std::uint8_t>& bytes, std::vector<tMySimTemplate>& out);

private:
    void LoadImports();                                           // 0x0054e220
    bool loaded_ = false;                                         // +0x08
    std::vector<tMySimTemplate> templates_;                       // +0x0c
    cRZString simsPath_;                                          // +0x18 the installation The Sims is in
};

}  // namespace sc4
