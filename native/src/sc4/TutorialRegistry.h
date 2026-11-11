// cSC4TutorialRegistry (Mac i386 0x0045212c..0x00452f74; SL::spSC4 vslot 0x178 hands it out): the tutorials the game ships,
// read from the Lua table `tutorial_registry` (script tutorial_registry.lua, group 0x4a5e8ef6), and which of them the player
// has finished. A finished tutorial's GUID sits in the preferences' list of 256 ints at +0xf34; every other one is "active".
//
// Item layout (cItem, 0x18 bytes in the original): +0 GUID, +4 order, +8 first task, +0xc task count, +0x10 active,
// +0x14 the region directory (lower case). The items are kept per region directory, sorted by order.
//
// What reads it here: the region map (the mayor line is hidden under an active tutorial; the arrow points at the next active
// tutorial city of the shown region).
#pragma once
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <string>

class cISCLua;

namespace sc4 {

class cSC4TutorialRegistry {
public:
    struct Item {
        int guid = 0;                // +0x00
        int order = 0;               // +0x04
        int taskFirst = 0;           // +0x08
        int taskCount = 0;           // +0x0c
        bool active = true;          // +0x10
        std::string regionDir;       // +0x14
    };
    static constexpr std::uint32_t kScriptGroup = 0x4a5e8ef6;
    static constexpr std::size_t kInactiveSlots = 0x100;   // preferences +0xf34

    // @ 0x004526f8. Reads the table (running the script first when the global is not a table yet), then marks the items
    // listed in `inactive` (the preferences' 256 ints) inactive and clears the slots that name no item. Answers false when
    // the table cannot be had. `listChanged` is the original's save flag: whether the last non-zero slot survived.
    bool Init(cISCLua* lua, std::int32_t* inactive, bool* listChanged = nullptr);
    void Shutdown();

    Item* GetItem(int guid);                                                   // @ 0x0045214c
    // @ 0x004521a2: false when there is no such item or nothing changes. Activating takes the GUID out of the list,
    // deactivating writes it into the first free slot; the caller then saves the preferences (spSC4 vslot 0x44).
    bool SetActive(int guid, bool active, std::int32_t* inactive);
    // @ 0x004523f0: the region's items from the highest order down; the walk stops at the first item that has a city
    // here and is finished, and answers the last active one it passed (null when none). `hasCity` stands for
    // GetRegionalCity(guid) != null.
    const Item* GetNextActive(const std::string& regionDir, const std::function<bool(int guid)>& hasCity) const;
    // @ 0x0045222e: the default region directory of expansion `id` (entry 0 when it has none). The nation opens it on a
    // first start.
    std::string GetTutorialRegionDefaultDirName(long id) const;
    const std::map<long, std::string>& DefaultRegions() const { return defaultRegions_; }
    const std::map<std::string, std::list<Item>>& Items() const { return items_; }

private:
    std::map<std::string, std::list<Item>> items_;   // +0x00 by region directory
    std::map<long, std::string> defaultRegions_;     // +0x18 expansion id -> region directory
};

}  // namespace sc4
