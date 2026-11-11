// cSC4TutorialRegistry on the real Lua 5.0 interpreter: the shipped tutorial_registry.lua layout (GUIDs and orders of the
// game's own script), the finished list from the preferences, the next-active walk the region map's arrow uses, SetActive.
#include <cstdint>
#include <cstdio>
#include <string>

#include "lua/SCLuaInstance.h"
#include "lua/SCLuaScripts.h"
#include "sc4/TutorialRegistry.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

using sc4::cSC4TutorialRegistry;

namespace {

constexpr int kGettingStarted = 0xabfad020, kTerraforming = static_cast<int>(0xea5d6dc8u), kMakingMoney = 0x0bfdb71b,
              kBigCity = static_cast<int>(0xcc02d3a3u), kWhatsNew = 0x2c12d0e9;

// The table part of the shipped script, then its registrations in the shipped order (not sorted by order on purpose).
const char* kScript = R"(
tutorial_registry = {
   default_regions = {},
   tutorials = {},
   add_tutorial = function (self, _guid, _order, _region_dir)
      self.tutorials[_guid] = { order = _order, region_dir = _region_dir, task_first = 0, task_count = 0, task_guid = _guid }
   end,
   add_default_region = function (self, _exp_pack_id, _region_dir)
      self.default_regions[_exp_pack_id] = { region_dir = _region_dir }
   end,
}
function hex2dec(hex_string) return tonumber(hex_string, 16) end
tutorial_registry:add_default_region(0, 'timbuktu')
tutorial_registry:add_default_region(1, 'TimBuktu')
tutorial_registry:add_tutorial(hex2dec('abfad020'), 0, 'timbuktu')
tutorial_registry:add_tutorial(hex2dec('ea5d6dc8'), 1, 'timbuktu')
tutorial_registry:add_tutorial(hex2dec('cc02d3a3'), 3, 'Timbuktu')
tutorial_registry:add_tutorial(hex2dec('0bfdb71b'), 2, 'timbuktu')
tutorial_registry:add_tutorial(hex2dec('2c12d0e9'), 4, 'timbuktu')
tutorial_registry.tutorials[hex2dec('0bfdb71b')].task_first = 12
tutorial_registry.tutorials[hex2dec('0bfdb71b')].task_count = 9
)";

cISCLua* NewLua() {
    cISCLua* lua = lua5::CreateSCLua();
    CHECK(lua->Open(0x7e));
    return lua;
}

bool AllCities(int) { return true; }

}  // namespace

int main() {
    cISCLua* lua = NewLua();
    CHECK(lua->DoString(kScript) == 0);
    std::int32_t inactive[cSC4TutorialRegistry::kInactiveSlots] = {};

    {   // read: one region list, sorted by order, region names lower case, the default regions with "maxisland" at 0
        cSC4TutorialRegistry registry;
        bool changed = true;
        CHECK(registry.Init(lua, inactive, &changed));
        CHECK(!changed);
        CHECK(lua->GetTop() == 0);   // the stack is left as it was
        CHECK(registry.Items().size() == 1 && registry.Items().count("timbuktu") == 1);
        std::string orders;
        for (const auto& item : registry.Items().at("timbuktu")) orders += std::to_string(item.order);
        CHECK(orders == "01234");
        CHECK(registry.DefaultRegions().size() == 2);
        CHECK(registry.DefaultRegions().at(0) == "timbuktu" && registry.DefaultRegions().at(1) == "timbuktu");
        CHECK(registry.GetTutorialRegionDefaultDirName(1) == "timbuktu");
        CHECK(registry.GetTutorialRegionDefaultDirName(7) == "timbuktu");   // no entry 7: entry 0
        const cSC4TutorialRegistry::Item* money = registry.GetItem(kMakingMoney);
        CHECK(money && money->taskFirst == 12 && money->taskCount == 9 && money->active);
        CHECK(registry.GetItem(12345) == nullptr);
        // nothing finished: the arrow goes to the first tutorial
        const cSC4TutorialRegistry::Item* next = registry.GetNextActive("Timbuktu", AllCities);
        CHECK(next && next->guid == kGettingStarted);
        CHECK(registry.GetNextActive("london", AllCities) == nullptr);
        // a tutorial without a city in the region is passed over
        next = registry.GetNextActive("timbuktu", [](int guid) { return guid != kGettingStarted; });
        CHECK(next && next->guid == kTerraforming);
    }

    {   // the finished list: Getting Started done -> inactive; an unknown GUID's slot is cleared; the walk stops at the
        // finished one and answers the active tutorial after it
        inactive[3] = kGettingStarted;
        inactive[7] = 0x1234;
        cSC4TutorialRegistry registry;
        bool changed = false;
        CHECK(registry.Init(lua, inactive, &changed));
        CHECK(!changed);   // the last non-zero slot was the unknown one, now cleared (the original's flag)
        CHECK(inactive[3] == kGettingStarted && inactive[7] == 0);
        CHECK(!registry.GetItem(kGettingStarted)->active);
        const cSC4TutorialRegistry::Item* next = registry.GetNextActive("timbuktu", AllCities);
        CHECK(next && next->guid == kTerraforming);
        // Big City finished too: the walk from the top stops there, so the next active is What's New
        CHECK(registry.SetActive(kBigCity, false, inactive));
        CHECK(inactive[0] == kBigCity);   // the first free slot
        next = registry.GetNextActive("timbuktu", AllCities);
        CHECK(next && next->guid == kWhatsNew);
        CHECK(!registry.SetActive(kBigCity, false, inactive));   // no change
        CHECK(registry.SetActive(kBigCity, true, inactive));
        CHECK(inactive[0] == 0 && registry.GetItem(kBigCity)->active);
        CHECK(!registry.SetActive(0x1234, true, inactive));      // no such item
        bool changedAgain = false;
        cSC4TutorialRegistry again;
        CHECK(again.Init(lua, inactive, &changedAgain));
        CHECK(changedAgain);   // the last non-zero slot names an item
        (void)kWhatsNew;
    }

    {   // no table and no script resource: Init fails and leaves the stack alone
        cISCLua* empty = NewLua();
        cSC4TutorialRegistry registry;
        CHECK(!registry.Init(empty, nullptr));
        CHECK(empty->GetTop() == 0);
        CHECK(registry.GetNextActive("timbuktu", AllCities) == nullptr);
    }

    std::printf(failures ? "FAILED\n" : "sc4_tutorialregistry ok\n");
    return failures != 0;
}
