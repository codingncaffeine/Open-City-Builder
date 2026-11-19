#include "sc4/OccupantGroupTemplate.h"

#include "sc4/SCPropertyHelp.h"

namespace sc4 {

namespace {
IOccupantGroupHost* g_host = nullptr;
}
IOccupantGroupHost* OccupantGroupHost() { return g_host; }
void SetOccupantGroupHost(IOccupantGroupHost* host) { g_host = host; }

// ---- cSC4OccupantGroupOccupantFilter ----------------------------------------------------------------------------------------
cSC4OccupantGroupOccupantFilter::cSC4OccupantGroupOccupantFilter(std::uint32_t groupId) {
    if (groupId != 0) groups_.push_back(groupId);
}

cSC4OccupantGroupOccupantFilter::cSC4OccupantGroupOccupantFilter(const std::vector<std::uint32_t>& groups) : groups_(groups) {}

bool cSC4OccupantGroupOccupantFilter::IsOccupantIncluded(cISC4Occupant* occupant) {
    if (!occupant) return false;
    for (std::uint32_t group : groups_) {
        if (occupant->IsOccupantGroup(group)) return true;
    }
    return false;
}

void cSC4OccupantGroupOccupantFilter::AddGroupId(std::uint32_t groupId) {
    if (groupId != 0) groups_.push_back(groupId);
}

void cSC4OccupantGroupOccupantFilter::SetGroupIdVector(const std::vector<std::uint32_t>& groups) {
    if (&groups_ != &groups) groups_ = groups;
}

// ---- cSC4OccupantGroupTemplate ----------------------------------------------------------------------------------------------
cSC4OccupantGroupTemplate::~cSC4OccupantGroupTemplate() = default;

bool cSC4OccupantGroupTemplate::IsOccupantGroup(std::uint32_t group) const {
    for (const cSC4OccupantGroupTemplate* t = this; t; t = t->parent) {
        if (t->groupId == group) return true;
    }
    return false;
}

bool cSC4OccupantGroupTemplate::GetAnimOffset(const cRZString& name, std::uint32_t& offset) const {
    auto it = animOffsets.find(std::string(name.ToChar(), name.Strlen()));
    if (it == animOffsets.end()) return false;
    offset = it->second;
    return true;
}

std::uint32_t cSC4OccupantGroupTemplate::GetClassID() const {
    for (const cSC4OccupantGroupTemplate* t = this; t; t = t->parent) {
        if (t->classId != 0) return t->classId;
    }
    return 0;
}

cRZVariant& cSC4OccupantGroupTemplate::GetGroupIDVariant() {
    if (groupVariant_.GetCount() == 0) {
        std::vector<std::uint32_t> ids;
        for (const cSC4OccupantGroupTemplate* t = this; t; t = t->parent) {
            if (t->groupId != 0) ids.push_back(t->groupId);
        }
        if (!ids.empty()) groupVariant_.RefUint32(ids.data(), static_cast<std::uint32_t>(ids.size()));
    }
    return groupVariant_;
}

bool cSC4OccupantGroupTemplate::IsOccupantIncluded(cISC4Occupant* occupant) {
    std::vector<std::uint32_t> groups;
    if (!cSCPropertyHelp::GetPropertyValue(occupant->AsPropertyHolder(), kPropertyOccupantGroups, groups) || groupId == 0) return false;
    for (std::uint32_t id : groups) {
        if (id != groupId) continue;
        if (chance_ < 1.0f) return static_cast<float>(g_host->RandomDouble()) <= chance_;   // compared as a float, as the original
        return true;
    }
    return false;
}

bool cSC4OccupantGroupTemplate::GetKeyEffect(std::uint8_t command, tKeyEffect& out) const {
    auto it = keyEffects.find(command);
    if (it == keyEffects.end()) return false;
    out.fx.Copy(it->second.fx);
    out.bone.Copy(it->second.bone);
    out.controller.Copy(it->second.controller);
    out.value = it->second.value;
    return true;
}

int cSC4OccupantGroupTemplate::GetGroupHierarchyDepth() const {
    int depth = 0;
    for (const cSC4OccupantGroupTemplate* t = this; t; t = t->parent) ++depth;
    return depth;
}

}  // namespace sc4
