#include "sc4/TutorialRegistry.h"

#include <cctype>

#include "lua/ISCLua.h"
#include "lua/SCLuaScripts.h"

namespace sc4 {
namespace {

// The original's double -> unsigned conversion (clamped to 0..4294967295), kept as the signed int the item stores.
int ClampedNumber(cISCLua* lua, int index) {
    double value = lua->ToNumber(index);
    if (value >= 4294967295.0) value = 4294967295.0;
    if (value <= 0.0) value = 0.0;
    return static_cast<int>(static_cast<std::uint32_t>(value));
}

std::string Lower(const char* text) {
    std::string out(text ? text : "");
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// Pushes table[field] of the table on top; the caller pops it.
void PushField(cISCLua* lua, const char* field) {
    lua->PushString(field);
    lua->GetTable(-2);
}

int NumberField(cISCLua* lua, const char* field) {
    PushField(lua, field);
    const int value = ClampedNumber(lua, -1);
    lua->Pop(1);
    return value;
}

// The `region_dir` string of the table on top, lower case; `fallback` when it is not a string.
std::string RegionDirField(cISCLua* lua, const std::string& fallback) {
    PushField(lua, "region_dir");
    std::string out = lua->IsString(-1) ? Lower(lua->ToString(-1)) : fallback;
    lua->Pop(1);
    return out;
}

}  // namespace

bool cSC4TutorialRegistry::Init(cISCLua* lua, std::int32_t* inactive, bool* listChanged) {
    if (listChanged) *listChanged = false;
    if (!lua) return false;
    const int top = lua->GetTop();   // nsIGZLua5::cStackPosSaver
    lua->GetGlobal("tutorial_registry");
    if (!lua->IsTable(-1)) {
        lua->Pop(1);
        if (SCLuaHelp::LoadResourceScript(lua, kScriptGroup, "tutorial_registry.lua") != 0) {
            lua->SetTop(top);
            return false;
        }
        lua->GetGlobal("tutorial_registry");
        if (!lua->IsTable(-1)) {
            lua->SetTop(top);
            return false;
        }
    }

    defaultRegions_.clear();
    defaultRegions_[0] = "maxisland";
    PushField(lua, "default_regions");
    lua->PushNil();
    while (lua->Next(-2) != 0) {
        const long id = ClampedNumber(lua, -2);
        if (lua->IsTable(-1)) {
            PushField(lua, "region_dir");
            if (lua->IsString(-1)) defaultRegions_[id] = Lower(lua->ToString(-1));
            lua->Pop(1);
        }
        lua->Pop(1);
    }
    lua->Pop(1);

    items_.clear();
    PushField(lua, "tutorials");
    lua->PushNil();
    while (lua->Next(-2) != 0) {
        Item item;
        item.guid = ClampedNumber(lua, -2);
        item.active = true;
        if (lua->IsTable(-1)) {
            item.order = NumberField(lua, "order");
            item.taskFirst = NumberField(lua, "task_first");
            item.taskCount = NumberField(lua, "task_count");
            item.regionDir = RegionDirField(lua, item.regionDir);
            // Sorted by order: in front when the list is empty or the front's order is not lower, at the back when the
            // back's is lower, otherwise before the first item whose order is not lower.
            std::list<Item>& list = items_[item.regionDir];
            if (list.empty() || item.order <= list.front().order) {
                list.push_front(item);
            } else if (list.back().order < item.order) {
                list.push_back(item);
            } else {
                for (auto it = list.begin(); it != list.end(); ++it)
                    if (item.order <= it->order) {
                        list.insert(it, item);
                        break;
                    }
            }
        }
        lua->Pop(1);
    }
    lua->Pop(1);

    bool changed = false;
    if (inactive) {
        for (std::size_t i = 0; i < kInactiveSlots; ++i) {
            if (inactive[i] == 0) continue;
            if (Item* item = GetItem(inactive[i]))
                item->active = false;
            else
                inactive[i] = 0;
            changed = inactive[i] != 0;
        }
    }
    if (listChanged) *listChanged = changed;
    lua->SetTop(top);
    return true;
}

std::string cSC4TutorialRegistry::GetTutorialRegionDefaultDirName(long id) const {
    auto found = defaultRegions_.find(id);
    if (found != defaultRegions_.end()) return found->second;
    found = defaultRegions_.find(0);
    return found != defaultRegions_.end() ? found->second : std::string();
}

void cSC4TutorialRegistry::Shutdown() {
    items_.clear();
    defaultRegions_.clear();
}

cSC4TutorialRegistry::Item* cSC4TutorialRegistry::GetItem(int guid) {
    for (auto& [dir, list] : items_)
        for (Item& item : list)
            if (item.guid == guid) return &item;
    return nullptr;
}

bool cSC4TutorialRegistry::SetActive(int guid, bool active, std::int32_t* inactive) {
    Item* item = GetItem(guid);
    if (!item || item->active == active) return false;
    item->active = active;
    if (inactive) {
        for (std::size_t i = 0; i < kInactiveSlots; ++i) {
            if (active) {
                if (inactive[i] == guid) {
                    inactive[i] = 0;
                    break;
                }
            } else if (inactive[i] == 0) {
                inactive[i] = guid;
                break;
            }
        }
    }
    return true;
}

const cSC4TutorialRegistry::Item* cSC4TutorialRegistry::GetNextActive(const std::string& regionDir,
                                                                      const std::function<bool(int guid)>& hasCity) const {
    auto found = items_.find(Lower(regionDir.c_str()));
    if (found == items_.end() || found->second.empty()) return nullptr;
    const Item* candidate = nullptr;
    for (auto it = found->second.rbegin(); it != found->second.rend(); ++it) {
        if (!hasCity || !hasCity(it->guid)) continue;
        if (!it->active) break;
        candidate = &*it;
    }
    return candidate;
}

}  // namespace sc4
