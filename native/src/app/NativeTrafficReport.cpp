// The kit runner's `cars`: the vehicle manager's traffic automata (read-only). How many follow a network path, the states,
// behaviors and populate layers they are in, why a path-less one has none, and a few samples (the same automata on every
// call) with their network piece, path key, path index and position, so successive calls show them advancing.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <vector>

#include "app/NativeAutomataViews.h"
#include "sc4/AutomataManager.h"
#include "sc4/Automaton.h"
#include "sc4/PathInfo.h"
#include "sc4/TrafficAutomaton.h"
#include "sc4/Vehicle.h"

namespace native {
sc4::cSC4AutomataManager* NativeAutomataManagerForGroup(std::uint32_t group);   // app/NativeAutomataManagerHost.cpp

namespace {
using namespace sc4;

std::vector<cSC4TrafficAutomaton*> g_samples;   // followed across calls; only dereferenced while still held

bool OnPath(cSC4TrafficAutomaton* a) { return a->CurrentPath() && !a->CurrentPath()->empty() && a->GetNetworkOccupant(); }
}  // namespace

void NativeCarReport(int samples) {
    cSC4AutomataManager* manager = NativeAutomataManagerForGroup(0x4000);   // the vehicles' base group
    if (!manager) {
        std::printf("cars: no manager\n");
        return;
    }
    std::vector<cSC4TrafficAutomaton*> held;
    for (IAutomataAutomaton* view : manager->Automata())
        if (auto* a = view ? dynamic_cast<cSC4TrafficAutomaton*>(AutomatonOf(view)) : nullptr) held.push_back(a);
    int saved = 0;
    int total = 0, onPath = 0, moving = 0, movingOnPath = 0, ignorePaths = 0, noOccupant = 0, occupantNoPath = 0, removing = 0;
    std::map<int, std::pair<int, int>> byState, byLayer;               // id -> (all, on a path)
    std::map<std::uint32_t, std::pair<int, int>> byGroup;
    for (cSC4TrafficAutomaton* a : held) {
        ++total;
        saved += a->IsSavedFlagSet();
        const bool path = OnPath(a);
        const bool isMoving = a->GetCurrentSpeed() > 0.0f;
        onPath += path;
        moving += isMoving;
        movingOnPath += isMoving && path;
        if (a->CheckAutomataFlag(kAutomataFlagMarkedForRemove)) ++removing;
        if (!path) {
            if (a->CheckAutomataFlag(kAutomataFlagIgnorePaths)) ++ignorePaths;
            else if (!a->GetNetworkOccupant()) ++noOccupant;
            else ++occupantNoPath;
        }
        const tAutomataState* state = a->GetState();
        auto& s = byState[state ? static_cast<int>(state->stateType) : -1];
        ++s.first;
        s.second += path;
        auto& l = byLayer[static_cast<int>(a->PopulateLayer())];
        ++l.first;
        l.second += path;
        auto& g = byGroup[a->AutomataGroupId()];
        ++g.first;
        g.second += path;
    }
    std::printf("cars: %d traffic automata, %d on a path; %d moving, %d of them on a path; without a path: %d ignore-paths, %d no network piece, "
                "%d piece but no path; %d marked for removal; %d loaded from the save\n",
                total, onPath, moving, movingOnPath, ignorePaths, noOccupant, occupantNoPath, removing, saved);
    std::printf("cars: states");
    for (auto& [id, n] : byState) std::printf(" %d:%d/%d", id, n.second, n.first);
    std::printf("\ncars: layers");
    for (auto& [id, n] : byLayer) std::printf(" %d:%d/%d", id, n.second, n.first);
    std::printf("\ncars: groups");
    for (auto& [id, n] : byGroup) std::printf(" 0x%x:%d/%d", id, n.second, n.first);
    std::printf("\n");
    // The samples: keep the ones still held, refill with on-path automata (then any) up to `samples`.
    g_samples.erase(std::remove_if(g_samples.begin(), g_samples.end(),
                                   [&](cSC4TrafficAutomaton* a) { return std::find(held.begin(), held.end(), a) == held.end(); }),
                    g_samples.end());
    for (int pass = 0; pass < 2; ++pass)
        for (cSC4TrafficAutomaton* a : held) {
            if (static_cast<int>(g_samples.size()) >= samples) break;
            if (pass == 0 && !OnPath(a)) continue;
            if (std::find(g_samples.begin(), g_samples.end(), a) == g_samples.end()) g_samples.push_back(a);
        }
    for (cSC4TrafficAutomaton* a : g_samples) {
        float p[3] = {0, 0, 0};
        a->GetPosition(p);
        const tAutomataState* state = a->GetState();
        const std::vector<cS3DVector3>* path = a->CurrentPath();
        std::printf("cars: sample %p group 0x%x layer %u state %d behavior 0x%x at (%.1f, %.1f, %.1f) speed %.4f piece %p key 0x%08x index %u/%zu",
                    static_cast<const void*>(a), a->AutomataGroupId(), a->PopulateLayer(), state ? static_cast<int>(state->stateType) : -1,
                    a->GetBehavior(), p[0], p[1], p[2], a->GetCurrentSpeed(), static_cast<void*>(a->GetNetworkOccupant()), a->GetPathKey(),
                    a->PathIndex(), path ? path->size() : 0u);
        if (path && !path->empty())
            std::printf(" path (%.1f, %.1f)->(%.1f, %.1f)", path->front().x, path->front().z, path->back().x, path->back().z);
        const cS3DVector3& d = a->GetDispatchDestination();
        std::printf(" dest (%.1f, %.1f) occupant flags 0x%x saved %d init %d self %d reached %d flags 0x%x\n", d.x, d.z, a->GetFlags(), a->IsSavedFlagSet() ? 1 : 0, a->IsInitialized() ? 1 : 0,
                    a->GetSelfControlling() ? 1 : 0, a->GetReachedDestination() ? 1 : 0, static_cast<unsigned>(a->AutomataFlags()));
    }
}

}  // namespace native
