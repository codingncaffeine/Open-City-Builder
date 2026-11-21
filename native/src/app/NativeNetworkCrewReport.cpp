// The network manager's side of the network construction crews (app/NativeNetworkConstructionCrew.cpp has the crews'
// services): registering a crew with the city's manager and the kit verb `crews`. A translation unit of its own because the
// network manager's header and the automata managers' cannot share one.
#include <cstdio>
#include <string>

#include "app/NativeCity.h"
#include "app/NativeDemolition.h"
#include "sc4/OccupantManager.h"
#include "sc4/cSC4City.h"
#include "sc4/NetworkConstructionCrew.h"
#include "sc4/NetworkManager.h"
#include "sc4/NetworkOccupant.h"

namespace native {
using namespace sc4;

// SL::NetworkManager() vslots 0x40 / 0x44.
void NativeRegisterNetworkCrew(cSC4NetworkConstructionCrew* crew) {
    if (cSC4NetworkManager* manager = CityNetworkManager()) manager->RegisterConstructionCrew(crew);
}
void NativeUnregisterNetworkCrew(cSC4NetworkConstructionCrew* crew) {
    if (cSC4NetworkManager* manager = CityNetworkManager()) manager->UnregisterConstructionCrew(crew);
}

// Kit verb `crews` (read-only): every running network construction crew, its crews' pieces, their vehicles and how many
// pieces of the line are usable.
// With a cell rectangle, every network piece in it too: usable or not, its network flags and the crews still on it.
void NativeConstructionCrewReport(int x0, int z0, int x1, int z1) {
    if (x0 >= 0 && z0 >= 0 && x1 >= x0 && z1 >= z0) {
        cSC4City* city = HostCity();
        cIGZUnknown* object = city ? city->GetComponent(CityComponent::OccupantManager) : nullptr;
        void* raw = nullptr;
        if (object && object->QueryInterface(kGZIID_cISC4OccupantManager, &raw) && raw) {
            auto* occupants = static_cast<cISC4OccupantManager*>(raw);
            const int a[2] = {x0, z0}, b[2] = {x1, z1};
            OccupantList list;
            int usable = 0, total = 0;
            if (occupants->GetOccupantsByStandardCityCells(list, a, b, kOccupantTypeNetwork, 0xffffffffu)) {
                for (cISC4Occupant* occupant : list) {
                    auto* piece = dynamic_cast<cSC4NetworkOccupant*>(occupant);
                    if (!piece) continue;
                    std::uint32_t cx = 0, cz = 0;
                    piece->GetOccupiedCell(cx, cz);
                    ++total;
                    usable += piece->IsUsable() ? 1 : 0;
                    std::printf("crews: piece %u,%u %s, flags %08x, crews on it %u\n", cx, cz, piece->IsUsable() ? "usable" : "not usable",
                                piece->GetNetworkFlag(), cSC4NetworkConstructionCrew::GetNetworkOccupantCrewCount(piece));
                }
                occupants->ReleaseOccupantList(list);
            }
            occupants->Release();
            std::printf("crews: %d of %d pieces usable in %d,%d..%d,%d\n", usable, total, x0, z0, x1, z1);
        }
    }
    cSC4NetworkManager* manager = CityNetworkManager();
    if (!manager) {
        std::printf("crews: no network manager\n");
        return;
    }
    const std::vector<INetworkManagerCrew*>& running = manager->ConstructionCrews();
    std::printf("crews: %zu running\n", running.size());
    int index = 0;
    for (INetworkManagerCrew* entry : running) {
        auto* crew = dynamic_cast<cSC4NetworkConstructionCrew*>(entry);
        if (!crew) {
            std::printf("crews: [%d] not a network construction crew\n", index++);
            continue;
        }
        std::printf("crews: [%d] inited %d ticking %d, piece time %u ms, speed %.1f units/s, %zu crews, %zu pending\n", index, crew->IsInited() ? 1 : 0,
                    crew->IsTicking() ? 1 : 0, crew->Duration(), crew->Speed() * 1e6f, crew->Crews().size(), crew->PendingPieces().size());
        int c = 0;
        for (const cSC4NetworkConstructionCrew::tConstructionCrewData& data : crew->Crews()) {
            int usable = 0;
            std::string cells;
            for (cSC4NetworkOccupant* piece : data.pieces) {
                std::uint32_t x = 0, z = 0;
                piece->GetOccupiedCell(x, z);
                const bool on = piece->IsUsable();
                usable += on ? 1 : 0;
                char buffer[32];
                std::snprintf(buffer, sizeof buffer, " %u,%u%s", x, z, on ? "+" : "-");
                cells += buffer;
            }
            std::uint32_t cx = 0, cz = 0;
            if (data.current) data.current->GetOccupiedCell(cx, cz);
            std::printf("crews: [%d.%d] count %d, at %s%u,%u, %d of %zu usable, %zu workers, effect %d, elapsed %u ms:%s\n", index, c, data.count,
                        data.current ? "" : "(none) ", cx, cz, usable, data.pieces.size(), data.workers.size(), data.effect ? 1 : 0,
                        data.timer.GetElapsedTimeU32(), cells.c_str());
            const auto vehicle = [&](const char* name, const cSC4NetworkConstructionCrew::tVehicleInfo& info) {
                if (!info.vehicle) {
                    std::printf("crews: [%d.%d] %s vehicle: none\n", index, c, name);
                    return;
                }
                float p[3] = {0, 0, 0};
                info.vehicle->GetPosition(p);
                std::uint32_t px = 0, pz = 0;
                if (info.piece) info.piece->GetOccupiedCell(px, pz);
                std::printf("crews: [%d.%d] %s vehicle model 0x%08x at (%.1f, %.1f, %.1f) cell %d,%d on piece %u,%u, end (%.1f, %.1f), arrived %d on path %d\n",
                            index, c, name, info.modelId, p[0], p[1], p[2], static_cast<int>(p[0] / 16.0f), static_cast<int>(p[2] / 16.0f), px, pz,
                            info.endPoint[0], info.endPoint[2], info.arrived ? 1 : 0, info.onPath ? 1 : 0);
            };
            vehicle("lead", data.lead);
            vehicle("follow", data.follow);
            ++c;
        }
        ++index;
    }
}

}  // namespace native
