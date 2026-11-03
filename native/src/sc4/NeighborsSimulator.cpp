#include "sc4/NeighborsSimulator.h"

#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

#include "rz/cRZRandom.h"
#include "rz/cRZString.h"
#include "sc4/DBSegment.h"
#include "sc4/SCPropertyHelp.h"

namespace sc4 {

namespace {

INeighborsHost* g_host = nullptr;

// cISC4RegionalCity::FindConnection / GetAllConnections reached through the host (the partial interface does
// not declare them). `City` is never null here.
cISC4RegionalCity* OtherCity(cISC4NeighborConnection* connection) {
    return static_cast<cISC4RegionalCity*>(connection ? connection->GetCityOnOtherSideOfConnection() : nullptr);
}
cISC4RegionalCity* OtherCity(cISC4NeighborDeal* deal) {
    return static_cast<cISC4RegionalCity*>(deal ? deal->GetCityOnOtherSideOfConnection() : nullptr);
}

// Insert at the end when the city is not in the list yet (the original's linear scan).
void AppendUnique(std::list<cRZAutoRefCount<cISC4RegionalCity>>& list, cISC4RegionalCity* city) {
    if (!city) return;
    for (const auto& ref : list) {
        if (ref.get() == city) return;
    }
    list.push_back(cRZAutoRefCount<cISC4RegionalCity>(city));
}

// floor division by 64 with the sign correction the original uses ((x + (x >> 31 ? 63 : 0)) >> 6 with the
// compiler's 32-bit bias trick). Only used on cells that can be negative by a few units.
int DivideByCellWidth(int value) { return (value + ((value >> 31) & 63)) >> 6; }

}  // namespace

INeighborsHost* NeighborsHost() { return g_host; }
void SetNeighborsHost(INeighborsHost* host) { g_host = host; }

//---------------------------------------------------------------------------------------------------------------------
// construction / COM
//---------------------------------------------------------------------------------------------------------------------

// @ 0x10006e61e. The constructor stores a 32-bit 1 at +0xa0, so "deals dirty" starts true.
cSC4NeighborsSimulator::cSC4NeighborsSimulator(INeighborsHost* host) : host_(host) {}

// @ 0x10006e71c. Frees the response curve and releases the tuning exemplar; the city and the message server
// references are intentionally left alone (the original destructor leaks them — Shutdown drops them).
cSC4NeighborsSimulator::~cSC4NeighborsSimulator() {
    if (tuningExemplar_) {
        tuningExemplar_->Release();
        tuningExemplar_ = nullptr;
    }
    pendingRemovals_.clear();
    deals_.clear();
    connections_.clear();
}

// @ 0x10006e7fc. iid 1 and 0x0773fef0 give the primary view, 0x452294aa the message target, 0xe4fda3d4 the
// serializable view. The original adds a reference before answering.
bool cSC4NeighborsSimulator::QueryInterface(std::uint32_t iid, void** out) {
    if (!out) return false;
    if (iid == 1 || iid == kGZIID_cISC4NeighborsSimulator) {
        *out = static_cast<cISC4NeighborsSimulator*>(this);
    } else if (iid == 0x452294aa) {
        *out = static_cast<cIGZMessageTarget2*>(this);
    } else if (iid == 0xe4fda3d4) {
        *out = static_cast<cIGZSerializable*>(this);
    } else {
        return false;
    }
    AddRef();
    return true;
}

// @ 0x10006e88c. Releasing the last reference calls Shutdown and then the deleting destructor.
std::uint32_t cSC4NeighborsSimulator::Release() {
    if (refCount_ < 2) {
        Shutdown();
        delete this;
        return 0;
    }
    return --refCount_;
}

//---------------------------------------------------------------------------------------------------------------------
// lifecycle
//---------------------------------------------------------------------------------------------------------------------

// @ 0x10006e944. Loads the tuning exemplar (the response curve is its property 0xa469353), registers as a
// simulator agent for the 6 (month), 4 (day) and 10 (idle) intervals, subscribes to the seven messages,
// retakes the city and message-server references, repairs every connection against the current region city,
// reloads the region cities around this one and re-dates the local deals they carry.
bool cSC4NeighborsSimulator::Init() {
    if (initialized_) return true;
    initialized_ = true;
    INeighborsHost* h = Host();
    if (!h) return true;

    if (!tuningExemplar_) {
        tuningExemplar_ = h->TuningExemplar();
        if (tuningExemplar_) {
            const cISCPropertyHolder* holder = tuningExemplar_->AsISCPropertyHolder();
            if (holder) {
                std::vector<float> values;
                if (cSCPropertyHelp::GetPropertyValue(holder, kNeighborsPropertyResponseCurve, values)) {
                    curve_.SetResponseCurveData(values.data(), values.size());
                }
            }
        }
    }

    mbStaticInitialized = true;   // 0x100b6ddd0

    static constexpr std::uint32_t kAgentIntervals[3] = {6, 4, 10};
    for (std::uint32_t interval : kAgentIntervals) {
        h->AddSimulatorAgent(static_cast<cIGZMessageTarget2*>(this), static_cast<int>(interval),
                             "cSC4NeighborsSimulator");
    }

    if (!messages_) {
        messages_ = h->MessageServer();
        if (messages_) {
            messages_->AddRef();
            static constexpr std::uint32_t kMessages[7] = {
                kNeighborsMessageOccupantGone, kNeighborsMessageTerrainChanged, kNeighborsMessageDealStatus,
                kNeighborsMessageCityEstablished, kNeighborsMessageOccupantRemoved,
                kNeighborsMessageCityReset2, kNeighborsMessageCityReset};
            for (std::uint32_t id : kMessages) {
                messages_->AddNotification(static_cast<cIGZMessageTarget2*>(this), id);
            }
        }
    }

    if (!city_) {
        city_ = h->City();
        if (city_) city_->AddRef();
    }

    cISC4RegionalCity* current = h->CurrentRegionalCity();
    for (auto& ref : connections_) {
        if (ref.get()) ref->Repair(current);
    }

    // First pass over the adjacent cities: reload each one from its region cell (region GetCity + ReloadCity).
    std::list<cRZAutoRefCount<cISC4RegionalCity>> cities;
    GetAdjacentCities(cities, false);
    for (auto& ref : cities) {
        cISC4RegionalCity* city = ref.get();
        if (!city) continue;
        city->AddRef();
        int x = 0, z = 0;
        h->CityPosition(city, x, z);
        h->ReloadCityAt(x, z);
        city->Release();
    }
    cities.clear();

    // Second pass: take the local deals of every adjacent city except the current one, clone them, shift their
    // four dates by (sim date - deal current date) and keep them.
    GetAdjacentCities(cities, false);
    if (current) {
        h->CityUpdateLocalDeals(current);
        deals_.clear();
        for (auto& ref : cities) {
            cISC4RegionalCity* city = ref.get();
            if (!city || city == current) continue;
            h->CityUpdateLocalDeals(city);
            DealList local;
            h->CityGetLocalDeals(city, local);
            for (auto& dealRef : local) {
                cISC4NeighborDeal* source = dealRef.get();
                if (!source) continue;
                cISC4NeighborDeal* deal = nullptr;
                const bool cloned = source->Clone(&deal, false) && deal;
                if (std::getenv("SC4_NEIGHBORDEBUG"))
                    std::fprintf(stderr, "neighbors: local deal %08x of %p cloned %d other city %p\n", source->GetType(), static_cast<void*>(city),
                                 cloned, cloned ? static_cast<void*>(OtherCity(deal)) : nullptr);
                if (!cloned) continue;
                if (OtherCity(deal)) {
                    deal->SetFlags(kDealFlagSilent, false);
                    deals_.push_back(cRZAutoRefCount<cISC4NeighborDeal>(deal));
                    cGZDate now;
                    h->SimDate(now);
                    cGZDate date;
                    deal->GetCurrentDate(date);
                    const int delta = static_cast<int>(now.DayNumber()) - static_cast<int>(date.DayNumber());
                    const auto shifted = [delta](cGZDate& value) {
                        value = cGZDate(value.DayNumber() + static_cast<std::uint32_t>(delta));
                    };
                    deal->GetStartDate(date);
                    shifted(date);
                    deal->SetStartDate(date);
                    deal->GetExpirationDate(date);
                    shifted(date);
                    deal->SetExpirationDate(date);
                    deal->GetSuspensionDate(date);
                    shifted(date);
                    deal->SetSuspensionDate(date);
                    deal->GetCurrentDate(date);
                    shifted(date);
                    deal->SetCurrentDate(date);
                }
                deal->Release();
            }
        }
        update_deals_in_regional_cities(true, true);
        cities.clear();
    }
    return true;
}

// @ 0x10006f258. Pushes the deals and/or the import/export statistics to the adjacent cities and to the
// current region city, then clears the dirty flags.
void cSC4NeighborsSimulator::update_deals_in_regional_cities(bool deals, bool connections) {
    if (!deals && !connections) return;
    INeighborsHost* h = Host();
    if (!h) return;
    std::list<cRZAutoRefCount<cISC4RegionalCity>> cities;
    GetAdjacentCities(cities, false);
    cISC4RegionalCity* current = h->CurrentRegionalCity();
    for (auto& ref : cities) {
        cISC4RegionalCity* city = ref.get();
        if (!city) continue;
        if (deals) h->CitySetLocalDeals(city, deals_);
        if (connections) h->CityUpdateImportExport(city);
    }
    if (current) {
        if (deals) h->CitySetLocalDeals(current, deals_);
        if (connections) h->CityUpdateImportExport(current);
    }
    if (deals) dealsDirty_ = false;
    if (connections) connectionsDirty_ = false;
}

// @ 0x10006f388. Unsubscribes, drops the city / message server / exemplar references, removes the simulator
// agent and clears both record lists.
bool cSC4NeighborsSimulator::Shutdown() {
    if (!initialized_) return true;
    initialized_ = false;
    INeighborsHost* h = Host();
    if (city_) {
        city_->Release();
        city_ = nullptr;
    }
    if (messages_) {
        static constexpr std::uint32_t kMessages[7] = {
            kNeighborsMessageOccupantGone, kNeighborsMessageTerrainChanged, kNeighborsMessageDealStatus,
            kNeighborsMessageCityEstablished, kNeighborsMessageOccupantRemoved,
            kNeighborsMessageCityReset2, kNeighborsMessageCityReset};
        for (std::uint32_t id : kMessages) {
            messages_->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), id);
        }
        messages_->Release();
        messages_ = nullptr;
    }
    if (h) h->RemoveSimulatorAgent(static_cast<cIGZMessageTarget2*>(this));
    connections_.clear();
    deals_.clear();
    if (tuningExemplar_) {
        tuningExemplar_->Release();
        tuningExemplar_ = nullptr;
    }
    return true;
}

cISCPropertyHolder* cSC4NeighborsSimulator::GetTuningExemplar() const {   // @ 0x10006f4ac
    return tuningExemplar_ ? tuningExemplar_->AsISCPropertyHolder() : nullptr;
}

//---------------------------------------------------------------------------------------------------------------------
// serialization
//---------------------------------------------------------------------------------------------------------------------

// @ 0x10006f4b6: uint16 version 2, uint32 count, then each connection record.
bool cSC4NeighborsSimulator::Write(cIGZOStream& stream) const {
    stream.SetUint16(2);
    stream.SetUint32(static_cast<std::uint32_t>(connections_.size()));
    for (const auto& ref : connections_) {
        if (ref.get()) SC4WriteGZSerializable(stream, static_cast<const cIGZUnknown*>(ref.get()));
    }
    return !stream.GetError();
}

// @ 0x10006f528. Only version 2 is accepted; the list is appended to (the original does not clear it).
bool cSC4NeighborsSimulator::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != 2) return false;
    std::uint32_t count = 0;
    stream.GetUint32(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        void* raw = nullptr;
        if (SC4ReadGZSerializable(stream, kGZIID_cISC4NeighborConnection, &raw, false)) {
            auto* connection = static_cast<cISC4NeighborConnection*>(raw);
            if (connection) {
                connections_.push_back(cRZAutoRefCount<cISC4NeighborConnection>(connection));
                connection->Release();   // the reference the read handed us
            }
        }
    }
    return !stream.GetError();
}

//---------------------------------------------------------------------------------------------------------------------
// messages
//---------------------------------------------------------------------------------------------------------------------

// @ 0x10006f6b4.
bool cSC4NeighborsSimulator::DoMessage(cIGZMessage2* message) {
    if (!message) return true;
    // The payload accessors live on cIGZMessage2Standard; the original reaches them through the concrete
    // object's vtable (every message the server delivers is a cRZMessage2Standard).
    auto* standard = dynamic_cast<cIGZMessage2Standard*>(message);
    if (!standard) return true;
    const std::uint32_t type = message->GetType();

    if (type == kNeighborsMessageOccupantRemoved) {   // 0x99ef1143: only for the power-pole occupant type
        auto* occupant = static_cast<cISC4Occupant*>(standard->GetVoid1());
        if (!occupant || Host()->OccupantType(occupant) != kOccupantTypePowerPole) return true;
        if (!Host()->OccupantRetained(occupant)) DeleteOccupantConnection(occupant);
        return true;
    }
    if (type == kNeighborsMessageOccupantGone) {      // 0xaa7a014d
        DeleteOccupantConnection(static_cast<cISC4Occupant*>(standard->GetVoid1()));
        return true;
    }
    if (type == kNeighborsMessageDealStatus) {        // 0xea418d8f
        const std::uint64_t status = standard->GetData1();
        if ((status | 8u) == 8u) {
            connectionsDirty_ = true;
        } else {
            dealsDirty_ = true;
        }
        simulate_deals();
        return true;
    }
    if (type == kNeighborsMessageCityReset || type == kNeighborsMessageCityReset2) {   // 0x26d31ec4 / c5
        deals_.clear();
        update_deals_in_regional_cities(true, true);
        return true;
    }
    if (type == kNeighborsMessageTerrainChanged) {    // 0x278e6003
        const std::uint32_t data1 = static_cast<std::uint32_t>(standard->GetData1());
        const std::uint32_t data2 = static_cast<std::uint32_t>(standard->GetData2());
        SC4RectInt32 rect;
        rect.x0 = static_cast<int>((data1 >> 16) & 0xffffu) - 1;
        rect.z0 = static_cast<int>((data2 >> 16) & 0xffffu) - 1;
        const int x1 = static_cast<int>(data1 & 0xffffu);
        const int z1 = static_cast<int>(data2 & 0xffffu);
        const int cellsX = Host()->CityCellCountX();
        const int cellsZ = Host()->CityCellCountZ();
        if ((data1 & 0xffff0000u) == 0) rect.x0 = 0;
        if ((data2 & 0xffff0000u) == 0) rect.z0 = 0;
        rect.x1 = cellsX <= x1 ? cellsX - 1 : x1;
        rect.z1 = cellsZ <= z1 ? cellsZ - 1 : z1;
        DoTerrainChanged(rect);
        return true;
    }
    if (type == kNeighborsMessageCityEstablished) {   // 0x6ac284f3
        cityEstablished_ = true;
        return true;
    }
    if (type == kNeighborsMessageMonth) {             // 0x66956816
        simulate_deals();
        return true;
    }
    if (type == kNeighborsMessageDay || type == kNeighborsMessageIdleTime) {   // 0x66956814 / 0x6695681a
        update_deals_in_regional_cities(dealsDirty_, connectionsDirty_);
        return true;
    }
    return true;
}

// @ 0x10006f8fe. Re-places the connection occupants after a terrain change: the connection cell is clamped
// onto the city and, when it lies in the changed rectangle, every occupant tagged as a connection occupant
// gets the new terrain altitude (+15.5 for the raised connection types) and is deactivated.
void cSC4NeighborsSimulator::DoTerrainChanged(const SC4RectInt32& rect) {
    INeighborsHost* h = Host();
    if (!h) return;
    const int cellsX = h->CityCellCountX();
    const int cellsZ = h->CityCellCountZ();
    for (auto& ref : connections_) {
        cISC4NeighborConnection* connection = ref.get();
        if (!connection) continue;
        int x = 0, z = 0;
        connection->GetConnectionCell(x, z);
        if (x < 0) {
            x = 0;
        } else if (cellsX <= x) {
            x = cellsX - 1;
        }
        if (z < 0) {
            z = 0;
        } else if (cellsZ <= z) {
            z = cellsZ - 1;
        }
        if (!(rect.x0 <= x && rect.z0 <= z && x <= rect.x1 && z <= rect.z1)) continue;

        float positionX = 0.0f, positionZ = 0.0f;
        h->CityCellCenterToPosition(x, z, positionX, positionZ);
        float positionY = h->TerrainAltitudeAtCell(x, z);
        const std::uint32_t connectionType = connection->GetType();
        if (connectionType < 0xc && ((kRaisedConnectionTypeMask >> (connectionType & 0x1f)) & 1u) != 0) {
            positionY += 15.5f;
        }
        std::vector<cISC4Occupant*> occupants;
        h->OccupantsInRect(x, z, x, z, occupants);
        for (cISC4Occupant* occupant : occupants) {
            if (!occupant) continue;
            bool isConnectionOccupant = false;
            cSCPropertyHelp::GetPropertyValue(h->OccupantProperties(occupant), kNeighborsPropertyConnectionFlag,
                                              isConnectionOccupant);
            if (!isConnectionOccupant) continue;
            const float xyz[3] = {positionX, positionY, positionZ};
            h->SetOccupantPosition(occupant, xyz);
            h->DeactivateOccupant(occupant);
        }
        h->ReleaseOccupantList(occupants);
    }
}

// @ 0x10006fb6a. The monthly deal pass.
void cSC4NeighborsSimulator::simulate_deals() {
    if (!cityEstablished_) return;
    INeighborsHost* h = Host();
    if (!h) return;
    if (inSimulateDeals_) {
        // The original posts data1 = 8 to itself, which marks the connections dirty and retries the pass.
        h->PostMessage(kNeighborsMessageDealStatus, 8, 0, 0, 0, nullptr);
        return;
    }
    inSimulateDeals_ = true;
    UpdateCommodityStats(h->City());

    if (!uiDisableFlag_) {
        pendingRemovals_.clear();
        const cISCPropertyHolder* properties = GetTuningExemplar();
        if (!properties) {
            // The original dereferences the exemplar unchecked; a simulator without one just flushes the flags.
            update_deals_in_regional_cities(dealsDirty_, connectionsDirty_);
            inSimulateDeals_ = false;
            return;
        }
        float dealYears = 0.0f;
        cSCPropertyHelp::GetPropertyValue(properties, kNeighborsPropertyDealYears, dealYears);
        const float dealMonths = static_cast<float>(static_cast<double>(dealYears) * 12.0);
        int landfillStart = 0, landfillEnd = 0;
        h->LandfillRange(landfillStart, landfillEnd);
        const unsigned capacity = static_cast<unsigned>(landfillEnd - landfillStart);
        const cISC4RegionalCity* current = h->CurrentRegionalCity();
        double exported = current ? h->CityGetExported(current, 3) : 0.0;
        double available = current ? h->CityGetImported(current, 3) : 0.0;
        const double produced = current ? h->CityGetProduced(current, 3) : 0.0;
        if (!deals_.empty()) {
            // (1 - 1/(months + 0.000107)) * capacity + imported + produced
            const double term = (1.0 - 1.0 / (static_cast<double>(dealMonths) + 0.000107)) *
                                static_cast<double>(static_cast<float>(capacity));
            available = term + available + produced;
            for (auto it = deals_.begin(); it != deals_.end(); ++it) {
                cISC4NeighborDeal* deal = it->get();
                if (!deal) continue;
                // Quirk kept: the original spins here without advancing the iterator when the deal is silent.
                while (deal->CheckFlagsOr(kDealFlagSilent)) {
                }
                deal->UpdateState();

                const std::uint32_t dealType = deal->GetType();
                if (exported > available && dealType == kDealTypeGarbageSell) {
                    exported -= static_cast<double>(deal->GetAmount());
                    deal->SetFlags(kDealFlagBudgetExceeded, true);
                }

                if (deal->CheckFlagsOr(kDealFlagCancelled | kDealFlagAffordable | kDealFlagUnaffordable |
                                       kDealFlagBudgetExceeded)) {
                    if (deal->CheckFlagsOr(kDealFlagCancelled)) {
                        pendingRemovals_.push_front(it);
                        h->PostDealStatusMessage(2, static_cast<cIGZUnknown*>(deal));
                        deal->SetAmount(0);
                    } else if (deal->CheckFlagsOr(kDealFlagFrozenBit)) {
                        int status = 0;
                        if (deal->CheckFlagsOr(kDealFlagAffordable)) {
                            status = 3;
                        } else if (deal->CheckFlagsOr(kDealFlagUnaffordable)) {
                            status = 4;
                        } else if (deal->CheckFlagsOr(kDealFlagBudgetExceeded)) {
                            status = 5;
                        } else {
                            continue;
                        }
                        pendingRemovals_.push_front(it);
                        h->PostDealStatusMessage(static_cast<std::uint32_t>(status),
                                                 static_cast<cIGZUnknown*>(deal));
                        deal->SetAmount(0);
                    } else {
                        deal->SetFlags(kDealFlagLockedState, true);
                        deal->SetFlags(kDealStateMask, false);
                        cGZDate now;
                        h->SimDate(now);
                        deal->SetSuspensionDate(now);
                    }
                } else if (deal->CheckFlagsOr(kDealFlagProposed)) {
                    if (deal->Activate()) {
                        cGZDate now;
                        h->SimDate(now);
                        deal->SetCurrentDate(now);
                        h->PostDealStatusMessage(1, static_cast<cIGZUnknown*>(deal));
                        connectionsDirty_ = true;
                    }
                } else if (deal->CheckFlagsOr(kDealFlagActive)) {
                    cGZDate now;
                    h->SimDate(now);
                    cGZDate currentDate;
                    deal->GetCurrentDate(currentDate);
                    cGZDate expiration;
                    deal->GetExpirationDate(expiration);

                    if (deal->CheckFlagsOr(kDealFlagLockedState)) {
                        float suspendDays = 0.0f;
                        cSCPropertyHelp::GetPropertyValue(properties, kNeighborsPropertySuspendDays, suspendDays);
                        cGZDate today;
                        cGZDate suspension;
                        deal->GetSuspensionDate(suspension);
                        // Quirk kept: the duration is added to a temporary, the test uses the raw suspension date.
                        today += static_cast<int>(suspendDays);
                        if (now >= suspension) {
                            deal->SetFlags(kDealFlagLockedState, false);
                            deal->SetFlags(kDealFlagFrozenBit, true);
                        }
                    } else if (deal->CheckFlagsOr(kDealFlagFrozenBit)) {
                        float freezeDays = 0.0f, suspendDays = 0.0f;
                        cSCPropertyHelp::GetPropertyValue(properties, kNeighborsPropertyFreezeDays, freezeDays);
                        cSCPropertyHelp::GetPropertyValue(properties, kNeighborsPropertySuspendDays, suspendDays);
                        cGZDate today;
                        cGZDate suspension;
                        deal->GetSuspensionDate(suspension);
                        today += static_cast<int>(static_cast<float>(suspendDays + freezeDays));
                        if (now >= suspension) deal->SetFlags(kDealFlagFrozenBit, false);
                    } else if (deal->CheckFlagsAnd(kDealFlagExpired)) {
                        deal->SetFlags(kDealFlagExpired, false);
                        const std::uint32_t type = deal->GetType();
                        const cISC4RegionalCity* budgetCity = current;
                        if ((type + 0xb88c001fu) < 5u && ((0x15u >> ((type + 0xb88c001fu) & 0x1f)) & 1u) != 0) {
                            if (cISC4RegionalCity* other = OtherCity(deal)) budgetCity = other;
                        }
                        deal->UpdateCost();
                        const std::int64_t cost = deal->GetCost();
                        const double funds = budgetCity ? h->CityGetBudget(budgetCity) : 0.0;
                        if (static_cast<double>(cost) <= funds) {
                            cGZDate today1, today2;
                            cGZDate simNow;
                            h->SimDate(simNow);
                            cGZDate previousCurrent;
                            deal->GetCurrentDate(previousCurrent);
                            cGZDate start;
                            deal->GetStartDate(start);
                            cGZDate oldExpiration;
                            deal->GetExpirationDate(oldExpiration);
                            const int dayA = static_cast<int>(today2.DayNumber());
                            const int dayB = static_cast<int>(today1.DayNumber());
                            const int dayNow = static_cast<int>(simNow.DayNumber());
                            deal->SetStartDate(previousCurrent);
                            const int newExpirationDay = (dayA - dayB) + dayNow;
                            deal->SetExpirationDate(cGZDate(static_cast<std::uint32_t>(newExpirationDay)));
                            h->PostDealStatusMessage(7, static_cast<cIGZUnknown*>(deal));
                        } else {
                            deal->SetFlags(kDealFlagUnaffordable, true);
                        }
                    } else if (now >= expiration) {
                        deal->SetFlags(kDealFlagExpired, true);
                        h->PostDealStatusMessage(6, static_cast<cIGZUnknown*>(deal));
                    }
                } else {
                    deal->Cancel();
                }
            }
        }
        dealsDirty_ = dealsDirty_ || !pendingRemovals_.empty();
        for (const auto& it : pendingRemovals_) deals_.erase(it);
        pendingRemovals_.clear();
        UpdateCommodityStats(h->City());
    }
    update_deals_in_regional_cities(dealsDirty_, connectionsDirty_);
    inSimulateDeals_ = false;
}

//---------------------------------------------------------------------------------------------------------------------
// connection geometry
//---------------------------------------------------------------------------------------------------------------------

// @ 0x100070670. True for a cell on exactly one edge of the city: x in {-1, sizeX*64} xor z in {-1, sizeZ*64},
// with the other coordinate strictly inside.
bool cSC4NeighborsSimulator::IsValidConnectionCell(int x, int z, cISC4RegionalCity* city) {
    INeighborsHost* h = NeighborsHost();
    if (!h) return false;
    if (!city) city = h->CurrentRegionalCity();
    if (!city) return false;
    int width = 0, height = 0;
    h->CitySize(city, width, height);
    const int edgeX = width << 6;
    const int edgeZ = height << 6;
    // Quirk kept: the original does not range-check the coordinate that is not on an edge.
    const bool onX = x == -1 || x == edgeX;
    const bool onZ = z == -1 || z == edgeZ;
    if (onX) return !onZ;
    return onZ;
}

// @ 0x1000706fa. A connection cell (just outside the city in city cells) to the region cell it touches.
void cSC4NeighborsSimulator::ConvertConnectionCellToNeighboringRegionCell(int x, int z, int& outX, int& outZ,
                                                                         cISC4RegionalCity* city) {
    INeighborsHost* h = NeighborsHost();
    if (!h) return;
    if (!city) city = h->CurrentRegionalCity();
    if (!city) return;
    int width = 0, height = 0;
    h->CitySize(city, width, height);
    if (x < 0) {
        h->CityPosition(city, outX, outZ);
        outX -= 1;
        outZ += DivideByCellWidth(z);
    } else if (z < 0) {
        h->CityPosition(city, outX, outZ);
        outX += x >> 6;
        outZ += -1;
    } else if (x < (width << 6)) {
        h->CityPosition(city, outX, outZ);
        if (z < (height << 6)) return;
        outX += x >> 6;
        outZ += height;
    } else {
        h->CityPosition(city, outX, outZ);
        outX += width;
        outZ += z >> 6;
    }
}

// @ 0x1000707f4. A connection cell to the equivalent cell in the neighbouring city (false when there is no
// city on the other side, or true with the city's first cell when the connection cell is inside a city).
bool cSC4NeighborsSimulator::ConvertConnectionCellToNeighborCell(int x, int z, int& outCellX, int& outCellZ,
                                                                cISC4RegionalCity** outCity, int& neighborCellX,
                                                                int& neighborCellZ) {
    INeighborsHost* h = NeighborsHost();
    if (!h) return false;
    *outCity = nullptr;
    outCellX = 0;
    outCellZ = 0;
    neighborCellX = 0;
    neighborCellZ = 0;
    int side = 0;   // 0 north (z < 0), 1 east (x >= width), 2 south (z >= height), 3 west (x < 0), -1 inside
    const int cityWidth = h->CityCellCountX();
    const int cityHeight = h->CityCellCountZ();
    if (x < 0) {
        side = 3;
    } else if (z >= 0) {
        side = 1;
        if (x < cityWidth) {
            side = 2;
            if (z < cityHeight) side = -1;
        }
    }
    cISC4RegionalCity* current = h->CurrentRegionalCity();
    if (!current) return false;
    h->CityPosition(current, outCellX, outCellZ);
    int width = 0, height = 0;
    h->CitySize(current, width, height);
    if (side < 4) {
        switch (side) {
            case 0:
                outCellX += x >> 6;
                outCellZ += -1;
                break;
            case 1:
                outCellX += width;
                outCellZ += z >> 6;
                break;
            case 2:
                outCellX += x >> 6;
                outCellZ += height;
                break;
            default:
                outCellX += -1;
                outCellZ += z >> 6;
                break;
        }
    }
    cISC4RegionalCity* other = h->RegionCityAt(outCellX, outCellZ);
    if (!other) return false;
    *outCity = other;
    int otherX = 0, otherZ = 0, otherWidth = 0, otherHeight = 0;
    h->CityPosition(other, otherX, otherZ);
    h->CitySize(other, otherWidth, otherHeight);
    if (side > 3) return true;
    const int localX = x - (x / 64) * 64;
    int resultX = 0, resultZ = 0;
    switch (side) {
        case 0:
            resultX = (outCellX - otherX) * 64 + localX;
            resultZ = otherHeight << 6;
            break;
        case 1:
            resultX = -1;
            resultZ = (outCellZ - otherZ) * 64 + (z - (z / 64) * 64);
            break;
        case 2:
            resultX = (outCellX - otherX) * 64 + localX;
            resultZ = -1;
            break;
        default:
            resultX = otherWidth << 6;
            resultZ = (outCellZ - otherZ) * 64 + (z - (z / 64) * 64);
            break;
    }
    neighborCellX = resultX;
    neighborCellZ = resultZ;
    return true;
}

// @ 0x100070a70. The inverse: a cell in a neighbouring city to the connection cell in this city (false when
// that side is not the current city).
bool cSC4NeighborsSimulator::ConvertNeighborCellToConnectionCell(cISC4RegionalCity* city, int x, int z,
                                                                int& outX, int& outZ) {
    INeighborsHost* h = NeighborsHost();
    outX = 0;
    outZ = 0;
    if (!h || !city) return false;
    cISC4RegionalCity* current = h->CurrentRegionalCity();
    if (!current) return false;
    int width = 0, height = 0;
    h->CitySize(city, width, height);
    int side = 0;
    if (x < 0) {
        side = 3;
    } else if (z >= 0) {
        side = 1;
        if (x < (width << 6)) {
            side = 2;
            if (z < (height << 6)) side = -1;
        }
    }
    int regionX = 0, regionZ = 0;
    h->CityPosition(city, regionX, regionZ);
    if (side < 4) {
        switch (side) {
            case 0:
                regionX += x >> 6;
                regionZ += -1;
                break;
            case 1:
                regionX += width;
                regionZ += z >> 6;
                break;
            case 2:
                regionX += x >> 6;
                regionZ += height;
                break;
            default:
                regionX += -1;
                regionZ += z >> 6;
                break;
        }
    }
    cISC4RegionalCity* atCell = h->RegionCityAt(regionX, regionZ);
    if (atCell != current) return false;
    int currentX = 0, currentZ = 0, currentWidth = 0, currentHeight = 0;
    h->CityPosition(current, currentX, currentZ);
    h->CitySize(current, currentWidth, currentHeight);
    if (side > 3) return true;
    const int localX = x - (x / 64) * 64;
    int resultX = 0, resultZ = 0;
    switch (side) {
        case 0:
            resultX = (regionX - currentX) * 64 + localX;
            resultZ = currentHeight << 6;
            break;
        case 1:
            resultX = -1;
            resultZ = (regionZ - currentZ) * 64 + (z - (z / 64) * 64);
            break;
        case 2:
            resultX = (regionX - currentX) * 64 + localX;
            resultZ = -1;
            break;
        default:
            resultX = currentWidth << 6;
            resultZ = (regionZ - currentZ) * 64 + (z - (z / 64) * 64);
            break;
    }
    outX = resultX;
    outZ = resultZ;
    return true;
}

//---------------------------------------------------------------------------------------------------------------------
// connection queries
//---------------------------------------------------------------------------------------------------------------------

// @ 0x100070ca0.
cISC4NeighborConnection* cSC4NeighborsSimulator::FindConnection(std::uint32_t type, int x, int z) {
    for (auto& ref : connections_) {
        cISC4NeighborConnection* connection = ref.get();
        if (!connection || connection->GetType() != type) continue;
        int cellX = 0, cellZ = 0;
        connection->GetConnectionCell(cellX, cellZ);
        if (cellX == x && cellZ == z) return connection;
    }
    return nullptr;
}

// @ 0x100070d18.
cISC4NeighborConnection* cSC4NeighborsSimulator::FindUnvisitedConnection(std::uint32_t type,
                                                                        const cISC4RegionalCity* city) {
    for (auto& ref : connections_) {
        cISC4NeighborConnection* connection = ref.get();
        if (!connection || connection->GetType() != type) continue;
        if (connection->GetVisitedThisCycle()) continue;
        if (OtherCity(connection) == city) return connection;
    }
    return nullptr;
}

bool cSC4NeighborsSimulator::HasConnection(std::uint32_t type, int x, int z) const {   // @ 0x100070d88
    return const_cast<cSC4NeighborsSimulator*>(this)->FindConnection(type, x, z) != nullptr;
}

// @ 0x100070d9a.
cISC4NeighborConnection* cSC4NeighborsSimulator::FindConnectionOnOtherSide(std::uint32_t type, int x, int z) {
    INeighborsHost* h = Host();
    int cellX = 0, cellZ = 0, neighborX = 0, neighborZ = 0;
    cISC4RegionalCity* city = nullptr;
    if (!ConvertConnectionCellToNeighborCell(x, z, cellX, cellZ, &city, neighborX, neighborZ)) return nullptr;
    if (!h || !city) return nullptr;
    return h->CityFindConnection(city, static_cast<int>(type), neighborX, neighborZ);
}

// @ 0x100070dfe.
cISC4NeighborConnection* cSC4NeighborsSimulator::FindConnectionOnOtherSide(cISC4NeighborConnection* connection) {
    if (!connection) return nullptr;
    int x = 0, z = 0;
    connection->GetConnectionCell(x, z);
    return FindConnectionOnOtherSide(connection->GetType(), x, z);
}

// @ 0x100071d38.
cISC4RegionalCity* cSC4NeighborsSimulator::FindCityOnOtherSideOfConnection(int x, int z) const {
    INeighborsHost* h = Host();
    int regionX = 0, regionZ = 0;
    ConvertConnectionCellToNeighboringRegionCell(x, z, regionX, regionZ, nullptr);
    return h ? h->RegionCityAt(regionX, regionZ) : nullptr;
}

void cSC4NeighborsSimulator::GetAllConnections(ConnectionList& out) const {   // @ 0x100071d94
    out.clear();
    out.insert(out.end(), connections_.begin(), connections_.end());
}

void cSC4NeighborsSimulator::GetConnectionsOfType(std::uint32_t type, ConnectionList& out) const {   // 0x100071f3c
    out.clear();
    for (const auto& ref : connections_) {
        if (ref.get() && ref->GetType() == type) out.push_back(ref);
    }
}

void cSC4NeighborsSimulator::GetConnectionsOfTypeToCity(std::uint32_t type, const cISC4RegionalCity* city,
                                                        ConnectionList& out) const {   // @ 0x100071fe0
    out.clear();
    for (const auto& ref : connections_) {
        if (!ref.get() || ref->GetType() != type) continue;
        if (OtherCity(ref.get()) == city) out.push_back(ref);
    }
}

void cSC4NeighborsSimulator::GetConnectionsToCity(const cISC4RegionalCity* city,
                                                  ConnectionList& out) const {   // @ 0x100072096
    out.clear();
    for (const auto& ref : connections_) {
        if (ref.get() && OtherCity(ref.get()) == city) out.push_back(ref);
    }
}

//---------------------------------------------------------------------------------------------------------------------
// adjacent / connected cities
//---------------------------------------------------------------------------------------------------------------------

// @ 0x100072144. Every city that owns a region cell bordering this city's rectangle.
void cSC4NeighborsSimulator::GetAdjacentCities(std::list<cRZAutoRefCount<cISC4RegionalCity>>& out,
                                              bool includeUnestablished) const {
    out.clear();
    INeighborsHost* h = Host();
    if (!h) return;
    cISC4RegionalCity* current = h->CurrentRegionalCity();
    if (!current) return;
    int cityX = 0, cityZ = 0, width = 0, height = 0;
    h->CityPosition(current, cityX, cityZ);
    h->CitySize(current, width, height);
    std::set<cISC4RegionalCity*> unique;
    const auto consider = [&](int x, int z) {
        cISC4RegionalCity* city = h->RegionCityAt(x, z);
        if (!city) return;
        if (!includeUnestablished && !h->CityEstablished(city)) return;
        unique.insert(city);
    };
    for (int i = 0; i < width; ++i) {
        consider(cityX + i, cityZ - 1);
        consider(cityX + i, cityZ + height);
    }
    for (int i = 0; i < height; ++i) {
        consider(cityX - 1, cityZ + i);
        consider(cityX + width, cityZ + i);
    }
    for (cISC4RegionalCity* city : unique) out.push_back(cRZAutoRefCount<cISC4RegionalCity>(city));
}

// @ 0x100072980. The cities directly reachable from this city's connections.
void cSC4NeighborsSimulator::GetConnectedCities(std::list<cRZAutoRefCount<cISC4RegionalCity>>& out,
                                               bool includeUnestablished) const {
    out.clear();
    INeighborsHost* h = Host();
    if (!h) return;
    ConnectionList all;
    GetAllConnections(all);
    for (const auto& ref : all) {
        cISC4NeighborConnection* connection = ref.get();
        if (!connection) continue;
        const std::uint32_t type = connection->GetType();
        if (type >= 0xd || ((kConnectedCityTypeMask >> (type & 0x1f)) & 1u) == 0) continue;
        cISC4RegionalCity* city = OtherCity(connection);
        if (!city) continue;
        if (!includeUnestablished && !h->CityEstablished(city)) continue;
        AppendUnique(out, city);
    }
}

// @ 0x1000726ce. Breadth-first walk of the connection graph, then the current city is dropped from the front
// and (unless `includeUnestablished`) unestablished cities are removed.
void cSC4NeighborsSimulator::GetAllConnectedCities(std::list<cRZAutoRefCount<cISC4RegionalCity>>& out,
                                                  bool includeUnestablished) const {
    out.clear();
    INeighborsHost* h = Host();
    if (!h) return;
    cISC4RegionalCity* current = h->CurrentRegionalCity();
    if (!current) return;
    out.push_back(cRZAutoRefCount<cISC4RegionalCity>(current));
    for (auto it = out.begin(); it != out.end(); ++it) {
        cISC4RegionalCity* city = it->get();
        if (!city) continue;
        ConnectionList list;
        if (city == current) {
            GetAllConnections(list);
        } else {
            h->CityAllConnections(city, list);
        }
        for (const auto& ref : list) {
            cISC4NeighborConnection* connection = ref.get();
            if (!connection) continue;
            const std::uint32_t type = connection->GetType();
            if (type >= 0xd || ((kConnectedCityTypeMask >> (type & 0x1f)) & 1u) == 0) continue;
            AppendUnique(out, OtherCity(connection));
        }
    }
    if (!out.empty()) out.pop_front();
    if (!includeUnestablished) {
        out.remove_if([h](const cRZAutoRefCount<cISC4RegionalCity>& ref) {
            return !ref.get() || !h->CityEstablished(ref.get());
        });
    }
}

//---------------------------------------------------------------------------------------------------------------------
// connection creation / deletion
//---------------------------------------------------------------------------------------------------------------------

// @ 0x100070e58. Asks the player before building a link. The original only builds the localized dialog text
// (cSLString 0x6a231eaa/0xaa8e151b + 0x0a8e152d + 0x0a8e15f9) and returns the confirmation.
bool cSC4NeighborsSimulator::RequestCreateConnection(std::uint32_t type, int x, int z, bool /*fromOther*/) {
    if (HasConnection(type, x, z)) return false;
    INeighborsHost* h = Host();
    if (!h) return false;
    cRZString cityName;
    cISC4RegionalCity* city = FindCityOnOtherSideOfConnection(x, z);
    if (city && h->CityEstablished(city)) h->CityName(city, cityName);
    // cSLString 0x6a231eaa / {0xaa8e151b title, 0x0a8e152d format, 0x0a8e15f9 OK}; the format is Sprintf'd with the name.
    return h->ConfirmationDialog(0xaa8e151b, 0x0a8e152d, 0x0a8e15f9, cityName);
}

// @ 0x100071026. Creates the connection record, tells the city on the other side and, unless the type has no
// occupant, creates the connection prop on the city-edge cell with its edge flags and properties.
void cSC4NeighborsSimulator::CreateConnection(std::uint32_t type, int x, int z, bool fromOther) {
    if (HasConnection(type, x, z)) return;
    INeighborsHost* h = Host();
    if (!h) return;

    auto* connection = new cSC4NeighborConnection();
    connection->AddRef();   // the original constructor leaves the count at 1
    connections_.push_back(cRZAutoRefCount<cISC4NeighborConnection>(connection));
    connection->Release();

    int otherX = 0, otherZ = 0;
    ConvertConnectionCellToNeighboringRegionCell(x, z, otherX, otherZ, nullptr);
    connection->Initialize(type, x, z, otherX, otherZ);

    cISC4RegionalCity* other = OtherCity(connection);
    if (other) h->CityChangeSymmetricConnection(other, connection, true);

    if (type != kConnectionTypeNoOccupant) {
        std::uint32_t flags = 0;
        int cellX = 0, cellZ = 0;
        if (x < 0) {
            flags = fromOther ? 1u : 3u;
            cellX = x + 1;
            cellZ = z;
        } else if (z < 0) {
            flags = static_cast<std::uint32_t>(fromOther) * 2u;
            cellX = x;
            cellZ = z + 1;
        } else if (h->CityCellCountX() <= x) {
            flags = static_cast<std::uint32_t>(fromOther) * 2u + 1u;
            cellX = x - 1;
            cellZ = z;
        } else {
            flags = fromOther ? 0u : 2u;
            cellX = x;
            cellZ = z - 1;
        }
        float positionX = 0.0f, positionZ = 0.0f;
        h->CityCellCenterToPosition(cellX, cellZ, positionX, positionZ);
        float positionY = h->TerrainAltitudeAtPosition(positionX, positionZ);
        if (type < 0xc && ((kRaisedConnectionTypeMask >> (type & 0x1f)) & 1u) != 0) positionY += 15.5f;
        const float position[3] = {positionX, positionY, positionZ};
        void* prop = nullptr;
        if (h->CreateProp(kConnectionOccupantPropType, &prop) && prop) {
            h->SetPropPosition(prop, position);
            h->SetPropEdgeFlags(prop, flags);
            h->AddPropConnectionProperties(prop, type);
            h->FinishProp(prop);
            h->ReleaseProp(prop);
        }
    }

    h->PostMessage(kNeighborsMessageConnectionCreated, type, static_cast<std::uint32_t>(x),
                   static_cast<std::uint32_t>(z), 0, nullptr);
}

// @ 0x100071514.
void cSC4NeighborsSimulator::DeleteConnection(std::uint32_t type, int x, int z) {
    cISC4NeighborConnection* connection = FindConnection(type, x, z);
    if (connection) DeleteConnection(connection, true);
}

// @ 0x100071546. Removes the symmetric link, deletes the connection prop, posts the message and optionally
// drops the record from the list.
void cSC4NeighborsSimulator::DeleteConnection(cISC4NeighborConnection* connection, bool removeFromList) {
    if (!connection) return;
    INeighborsHost* h = Host();
    if (!h) return;
    if (cISC4RegionalCity* other = OtherCity(connection)) h->CityChangeSymmetricConnection(other, connection, false);

    int x = 0, z = 0;
    connection->GetConnectionCell(x, z);
    const int cellsX = h->CityCellCountX();
    if (x < 0) {
        x += 1;
    } else if (z < 0) {
        z += 1;
    } else if (cellsX <= x) {
        x -= 1;
    } else {
        z -= 1;
    }
    std::vector<cISC4Occupant*> occupants;
    h->OccupantsInRect(x, z, x, z, occupants);
    for (cISC4Occupant* occupant : occupants) {
        if (!occupant) continue;
        bool isConnectionOccupant = false;
        cSCPropertyHelp::GetPropertyValue(h->OccupantProperties(occupant), kNeighborsPropertyConnectionFlag,
                                          isConnectionOccupant);
        if (isConnectionOccupant) h->DeleteProp(occupant);
    }
    h->ReleaseOccupantList(occupants);

    h->PostMessage(kNeighborsMessageConnectionDeleted, connection->GetType(), static_cast<std::uint32_t>(x),
                   static_cast<std::uint32_t>(z), 0, nullptr);

    if (removeFromList) {
        connections_.remove_if([connection](const cRZAutoRefCount<cISC4NeighborConnection>& ref) {
            return ref.get() == connection;
        });
    }
}

// @ 0x100071842. The occupant's network type selects the connection type; the connection cells at the city
// border next to the occupant are removed.
void cSC4NeighborsSimulator::DeleteOccupantConnection(cISC4Occupant* occupant) {
    if (!occupant) return;
    INeighborsHost* h = Host();
    if (!h) return;
    const std::uint32_t occupantType = h->OccupantType(occupant);
    int connectionType = -1;
    if (occupantType == kOccupantTypePowerPole) {
        connectionType = 4;
    } else if (occupantType == kOccupantTypeNetworkOccupant) {
        // Priority order of the original's cascade: 1, 9, 8, 7, 2, 0xc, 6, 0, 0xb, 4.
        static constexpr struct {
            std::uint32_t networkType;
            int connectionType;
        } kMapping[] = {{1, 1}, {9, 0xb}, {8, 7}, {7, 6}, {2, 3}, {0xc, 0xc}, {6, 8}, {0, 2}, {0xb, 10}, {4, 5}};
        for (const auto& entry : kMapping) {
            if (h->OccupantHasNetworkType(occupant, entry.networkType)) {
                connectionType = entry.connectionType;
                break;
            }
        }
        if (connectionType < 0) return;
    } else {
        return;
    }

    float positionX = 0.0f, positionZ = 0.0f;
    h->OccupantPosition(occupant, positionX, positionZ);
    int cellX = 0, cellZ = 0;
    h->CityPositionToCell(positionX, positionZ, cellX, cellZ);
    const int cellsX = h->CityCellCountX();
    const int cellsZ = h->CityCellCountZ();
    if (cellX == 0) {
        DeleteConnection(static_cast<std::uint32_t>(connectionType), -1, cellZ);
    } else if (cellX == cellsX - 1) {
        DeleteConnection(static_cast<std::uint32_t>(connectionType), cellX + 1, cellZ);
    }
    if (cellZ == 0) {
        DeleteConnection(static_cast<std::uint32_t>(connectionType), cellX, -1);
    } else if (cellZ == cellsZ - 1) {
        DeleteConnection(static_cast<std::uint32_t>(connectionType), cellX, cellZ + 1);
    }
}

// @ 0x100071b24.
void cSC4NeighborsSimulator::DeleteAllConnectionsOfType(std::uint32_t type) {
    for (auto& ref : connections_) {
        if (ref.get() && ref->GetType() == type) DeleteConnection(ref.get(), false);
    }
    connections_.remove_if([type](const cRZAutoRefCount<cISC4NeighborConnection>& ref) {
        return ref.get() && ref->GetType() == type;
    });
}

//---------------------------------------------------------------------------------------------------------------------
// deal queries
//---------------------------------------------------------------------------------------------------------------------

// @ 0x100072ade.
cISC4NeighborConnection* cSC4NeighborsSimulator::FindConnectionSuitableForDeal(std::uint32_t dealType,
                                                                              const cISC4RegionalCity* city) {
    ConnectionList list;
    bool checkConnection = true;
    bool requireActive = true;
    switch (dealType) {
        case kDealTypePowerBuy:
            checkConnection = false;
            [[fallthrough]];
        case kDealTypePowerSell:
            GetConnectionsOfTypeToCity(4, city, list);
            break;
        case kDealTypeWaterBuy:
            checkConnection = false;
            [[fallthrough]];
        case kDealTypeWaterSell:
            GetConnectionsOfTypeToCity(5, city, list);
            break;
        case kDealTypeGarbageBuy:
            checkConnection = false;
            [[fallthrough]];
        case kDealTypeGarbageSell:
            GetConnectionsOfTypeToCity(3, city, list);
            if (list.empty()) {
                GetConnectionsOfTypeToCity(0xc, city, list);
                if (list.empty()) {
                    GetConnectionsOfTypeToCity(8, city, list);
                    if (list.empty()) {
                        GetConnectionsOfTypeToCity(2, city, list);
                        if (list.empty()) {
                            GetConnectionsOfTypeToCity(10, city, list);
                            requireActive = false;
                        }
                    }
                }
            }
            break;
        default:
            return nullptr;
    }
    if (list.empty()) return nullptr;
    if (!requireActive) return list.front().get();
    for (const auto& ref : list) {
        cISC4NeighborConnection* connection = ref.get();
        if (!connection) continue;
        if (checkConnection) {
            if (connection->GetIsActive()) return connection;
        } else {
            cISC4NeighborConnection* other = FindConnectionOnOtherSide(connection);
            if (other && other->GetIsActive()) return connection;
        }
    }
    return nullptr;
}

// @ 0x100072d0e.
bool cSC4NeighborsSimulator::ProposeDeal(std::uint32_t dealType, const cISC4RegionalCity* city,
                                        cISC4NeighborDeal** out) {
    if (!out) return false;
    *out = nullptr;
    INeighborsHost* h = Host();
    if (!h) return false;
    auto* deal = new cSC4NeighborDeal();
    deal->AddRef();   // the original constructor leaves the count at 1
    cISC4NeighborConnection* connection = FindConnectionSuitableForDeal(dealType, city);
    if (!connection) {
        deal->Release();
        return false;
    }
    int x = 0, z = 0;
    connection->GetRegionCellOnOtherSideOfConnection(x, z);
    deal->SetRegionCellOnOtherSideOfConnection(x, z);
    cISC4RegionalCity* current = h->CurrentRegionalCity();
    if (!current) {
        deal->Release();
        return false;
    }
    h->CityPosition(current, x, z);
    deal->SetRegionCellOnCitySideOfConnection(x, z);
    deal->SetType(dealType);
    deal->SetAmount(0);
    *out = deal;      // hand the caller our reference
    deal->AddRef();
    deal->Release();
    return true;
}

// @ 0x100072e2e. A proposition for a new deal of `dealType` for `amount` units, with a random term between
// the min/max years from the exemplar. The four dates start at the current sim date.
bool cSC4NeighborsSimulator::create_deal_proposition(std::uint32_t dealType, int amount,
                                                    const cISC4RegionalCity* city, DealList& out, bool silent) {
    INeighborsHost* h = Host();
    if (!h) return false;
    cISC4NeighborDeal* deal = nullptr;
    if (!ProposeDeal(dealType, city, &deal) || !deal) return false;
    if (silent) {
        deal->SetFlags(kDealFlagSilent, false);
        deals_.push_back(cRZAutoRefCount<cISC4NeighborDeal>(deal));
    }
    deal->SetAmount(amount);

    float minYears = 1.0f, maxYears = 5.0f;
    const cISCPropertyHolder* properties = GetTuningExemplar();
    if (properties) {
        cSCPropertyHelp::GetPropertyValue(properties, kNeighborsPropertyMinDealYears, minYears);
        cSCPropertyHelp::GetPropertyValue(properties, kNeighborsPropertyDealYears, maxYears);
    }
    if (maxYears < minYears) maxYears = minYears;

    // sGetRandom(): the original's function-local generator, seeded with 0xffffffff.
    static cRZRandom sGetRandom(0xffffffffu);
    const double yearsValue =
        sGetRandom.RandomDoubleRangeUniform(static_cast<double>(minYears), static_cast<double>(maxYears) + 0.001);
    const float years = static_cast<float>(yearsValue);

    cGZDate now;
    h->SimDate(now);
    const int nowDay = static_cast<int>(now.DayNumber());
    const int termYears = static_cast<int>(years);   // the original's 1.5*2^23 truncation trick
    cGZDate expiration(static_cast<std::uint32_t>(termYears * 0x16d + nowDay));

    deal->SetCurrentDate(now);
    deal->SetStartDate(now);
    deal->SetExpirationDate(expiration);
    out.push_back(cRZAutoRefCount<cISC4NeighborDeal>(deal));
    deal->Release();
    return true;
}

// @ 0x100073090. Proposes one deal per adjacent city and commodity when the city can afford it.
bool cSC4NeighborsSimulator::ProposeDeals(DealList& out, bool fromUI) {
    if (!cityEstablished_) return true;
    INeighborsHost* h = Host();
    if (!h) return true;
    UpdateCommodityStats(h->City());
    out.clear();
    const cISC4RegionalCity* current = h->CurrentRegionalCity();
    if (!current) return true;
    const cISCPropertyHolder* properties = GetTuningExemplar();
    if (!properties) return true;

    struct Entry {
        std::uint32_t even;
        std::uint32_t odd;
        std::uint32_t commodity;
        float threshold;
    };
    Entry table[4] = {{kDealTypePowerBuy, kDealTypePowerSell, 1, 0.0f},
                      {kDealTypeWaterBuy, kDealTypeWaterSell, 2, 0.0f},
                      {kDealTypeGarbageBuy, kDealTypeGarbageSell, 3, 0.0f},
                      {0, 0, 0, 0.0f}};
    cSCPropertyHelp::GetPropertyValue(properties, kNeighborsPropertyPowerPrice, table[0].threshold);
    cSCPropertyHelp::GetPropertyValue(properties, kNeighborsPropertyWaterPrice, table[1].threshold);
    cSCPropertyHelp::GetPropertyValue(properties, kNeighborsPropertyGarbagePrice, table[2].threshold);
    float costFactor = 1.0f;
    cSCPropertyHelp::GetPropertyValue(properties, kNeighborsPropertyDealCostFactor, costFactor);
    costFactor = static_cast<float>(static_cast<double>(costFactor) + 1.0);
    float minBudget = 0.0f;
    cSCPropertyHelp::GetPropertyValue(properties, kNeighborsPropertyMinBudget, minBudget);
    float budgetCostFactor = 1.0f;
    cSCPropertyHelp::GetPropertyValue(properties, kNeighborsPropertyBudgetCostFactor, budgetCostFactor);

    std::list<cRZAutoRefCount<cISC4RegionalCity>> cities;
    GetAdjacentCities(cities, false);
    for (auto& cityRef : cities) {
        cISC4RegionalCity* city = cityRef.get();
        if (!city) continue;
        for (const Entry& entry : table) {
            if (!entry.even) break;
            // The sale side first, then the purchase side (the original's order).
            for (int side = 1; side >= 0; --side) {
                const std::uint32_t dealType = side ? entry.odd : entry.even;
                if (!FindConnectionSuitableForDeal(dealType, city)) continue;
                DealList existing;
                FindDeals(dealType, city, existing);
                if (!existing.empty()) continue;
                const double budget = h->CityGetBudget(side ? city : current);
                const double maxAmount = GetDealMaxAmount(dealType, city);
                const double threshold = static_cast<double>(entry.threshold);
                const double amount = maxAmount <= threshold ? maxAmount : threshold;
                if (threshold > amount) continue;
                const double cost = GetDealCost(dealType, amount, city);
                if (!(cost < budget)) continue;
                if (!(static_cast<double>(minBudget) < budget)) continue;
                if (!(cost * static_cast<double>(budgetCostFactor) < budget)) continue;
                create_deal_proposition(dealType, 0, city, out, fromUI);
            }
        }
    }
    if (!out.empty() && fromUI) {
        dealsDirty_ = true;
        update_deals_in_regional_cities(true, true);
    }
    return true;
}

// @ 0x100073540.
bool cSC4NeighborsSimulator::AddDeal(cISC4NeighborDeal* deal) {
    if (!deal) return false;
    deal->SetFlags(kDealFlagSilent, false);
    deals_.push_back(cRZAutoRefCount<cISC4NeighborDeal>(deal));
    update_deals_in_regional_cities(true, true);
    return true;
}

void cSC4NeighborsSimulator::FindDealsAll(DealList& out) {   // @ 0x1000735e4
    if (&out != &deals_) out.assign(deals_.begin(), deals_.end());
}

void cSC4NeighborsSimulator::FindDeals(std::uint32_t dealType, DealList& out) {   // @ 0x100073606
    out.clear();
    for (const auto& ref : deals_) {
        if (ref.get() && ref->GetType() == dealType) out.push_back(ref);
    }
}

void cSC4NeighborsSimulator::FindDeals(std::uint32_t dealType, const cISC4RegionalCity* city,
                                       DealList& out) {   // @ 0x1000736aa
    out.clear();
    for (const auto& ref : deals_) {
        if (!ref.get() || ref->GetType() != dealType) continue;
        if (OtherCity(ref.get()) == city) out.push_back(ref);
    }
}

// @ 0x100073760. Deals whose sell type is in {power, water, garbage} and that are not proposed/cancelled or in
// a failure state contribute their monthly cost.
std::int64_t cSC4NeighborsSimulator::GetTotalMonthlyNeighborDealIncome() {
    std::int64_t total = 0;
    for (const auto& ref : deals_) {
        cISC4NeighborDeal* deal = ref.get();
        if (!deal || deal->CheckFlagsOr(0x3d)) continue;
        const std::uint32_t type = deal->GetType();
        const std::uint32_t index = type + 0xb88c001fu;
        if (index < 5u && ((0x15u >> (index & 0x1f)) & 1u) != 0) total += deal->GetCost();
    }
    return total;
}

// @ 0x1000737e6.
std::int64_t cSC4NeighborsSimulator::GetTotalMonthlyNeighborDealExpense() {
    std::int64_t total = 0;
    for (const auto& ref : deals_) {
        cISC4NeighborDeal* deal = ref.get();
        if (!deal || deal->CheckFlagsOr(0x3d)) continue;
        const std::uint32_t type = deal->GetType();
        const std::uint32_t index = type + 0xb88c0020u;
        if (index < 5u && ((0x15u >> (index & 0x1f)) & 1u) != 0) total += deal->GetCost();
    }
    return total;
}

//---------------------------------------------------------------------------------------------------------------------
// deal pricing
//---------------------------------------------------------------------------------------------------------------------

// @ 0x100074040.
double cSC4NeighborsSimulator::GetDealCost(std::uint32_t dealType, double amount,
                                          const cISC4RegionalCity* city) const {
    INeighborsHost* h = Host();
    if (!h) return 0.0;
    const cISC4RegionalCity* current = h->CurrentRegionalCity();
    const cISCPropertyHolder* properties = GetTuningExemplar();
    if (!properties || !current) return 0.0;
    float scale = 1.0f;
    float price = 0.0f;
    cSCPropertyHelp::GetPropertyValue(properties, kNeighborsPropertyDealCostScale, scale);
    if (!(1e-08 <= amount)) return 0.0;

    bool sell = true;                 // uVar6: 1 for the odd (sell) types
    int commodity = 1;
    switch (dealType) {
        case kDealTypePowerBuy:
            sell = false;
            [[fallthrough]];
        case kDealTypePowerSell:
            cSCPropertyHelp::GetPropertyValue(properties, kNeighborsPropertyPowerPrice, price);
            commodity = 1;
            break;
        case kDealTypeWaterBuy:
            sell = false;
            [[fallthrough]];
        case kDealTypeWaterSell:
            cSCPropertyHelp::GetPropertyValue(properties, kNeighborsPropertyWaterPrice, price);
            commodity = 2;
            break;
        case kDealTypeGarbageBuy:
            sell = false;
            [[fallthrough]];
        case kDealTypeGarbageSell:
            cSCPropertyHelp::GetPropertyValue(properties, kNeighborsPropertyGarbagePrice, price);
            commodity = 3;
            break;
        default:
            // The original's default path keeps the commodity variable holding a city pointer, so it would index
            // out of range; unknown deal types are not produced by the game, so this port uses power.
            sell = true;
            commodity = 1;
            break;
    }
    const cISC4RegionalCity* demandSide = sell ? city : current;
    const cISC4RegionalCity* supplySide = sell ? current : city;

    DealList existingDeals;
    const_cast<cSC4NeighborsSimulator*>(this)->FindDeals(dealType, city, existingDeals);
    const double existing = existingDeals.empty() ? 0.0 : static_cast<double>(existingDeals.front()->GetAmount());

    const double demandedDemand = h->CityGetDemanded(demandSide, commodity);
    const double exportedDemand = h->CityGetExported(demandSide, commodity);
    const double demandedSupply = h->CityGetDemanded(supplySide, commodity);
    const double exportedSupply = h->CityGetExported(supplySide, commodity);
    const double producedDemand = h->CityGetProduced(demandSide, commodity);
    const double importedDemand = h->CityGetImported(demandSide, commodity);
    const double producedSupply = h->CityGetProduced(supplySide, commodity);
    const double importedSupply = h->CityGetImported(supplySide, commodity);

    const float negative = static_cast<float>(static_cast<double>(price / scale) + 1.0);   // local_74
    float costFactor = 1.0f;
    cSCPropertyHelp::GetPropertyValue(properties, kNeighborsPropertyDealCostFactor, costFactor);
    const double factorA = static_cast<double>(costFactor) + 1.0;
    float costFactor2 = 1.0f;
    cSCPropertyHelp::GetPropertyValue(properties, kNeighborsPropertyDealCostFactor2, costFactor2);
    const double factorB = static_cast<double>(costFactor2) + 1.0;
    const double unitCost = h->CityGetCostPerUnit(supplySide, commodity);

    double demand = static_cast<double>(negative);
    if (static_cast<double>(negative) <= demandedDemand + exportedDemand) demand = demandedDemand + exportedDemand;
    const double low = factorB * unitCost;
    const double high = unitCost * factorA;
    const float curveLow =
        curve_.GetOutputValue(static_cast<float>((1.0 - (demand - ((producedDemand + importedDemand) - existing))) /
                                                 amount));
    double supplyAvailable = 1e-08;
    if (1e-08 <= producedSupply + importedSupply) supplyAvailable = producedSupply + importedSupply;
    const float curveHigh = curve_.GetOutputValue(
        static_cast<float>(((supplyAvailable - amount) - ((demandedSupply + exportedSupply) - existing)) /
                           supplyAvailable));
    double value = (high - low) * (static_cast<double>(curveHigh) + static_cast<double>(curveLow)) * 0.5 + low;
    double bounded = low;
    if (low <= value) bounded = value;
    if (high < bounded) bounded = high;
    return bounded * amount;
}

// @ 0x100074434.
double cSC4NeighborsSimulator::GetDealMaxAmount(std::uint32_t dealType, const cISC4RegionalCity* city) const {
    if (!city) return 0.0;
    INeighborsHost* h = Host();
    if (!h) return 0.0;
    const cISC4RegionalCity* current = h->CurrentRegionalCity();
    const cISCPropertyHolder* properties = GetTuningExemplar();
    if (!properties || !current) return 0.0;
    float scale = 1.0f;
    float price = 0.0f;
    cSCPropertyHelp::GetPropertyValue(properties, kNeighborsPropertyDealCostScale, scale);

    bool sell = true;
    int commodity = 1;
    switch (dealType) {
        case kDealTypePowerBuy:
            sell = false;
            [[fallthrough]];
        case kDealTypePowerSell:
            cSCPropertyHelp::GetPropertyValue(properties, kNeighborsPropertyPowerPrice, price);
            break;
        case kDealTypeWaterBuy:
            sell = false;
            [[fallthrough]];
        case kDealTypeWaterSell:
            cSCPropertyHelp::GetPropertyValue(properties, kNeighborsPropertyWaterPrice, price);
            commodity = 2;
            break;
        case kDealTypeGarbageBuy:
            sell = false;
            [[fallthrough]];
        case kDealTypeGarbageSell:
            cSCPropertyHelp::GetPropertyValue(properties, kNeighborsPropertyGarbagePrice, price);
            commodity = 3;
            break;
        default:
            sell = true;
            break;
    }
    DealList existingDeals;
    const_cast<cSC4NeighborsSimulator*>(this)->FindDeals(dealType, city, existingDeals);
    const double existing = existingDeals.empty() ? 0.0 : static_cast<double>(existingDeals.front()->GetAmount());

    const cISC4RegionalCity* demandSide = sell ? city : current;
    const cISC4RegionalCity* supplySide = sell ? current : city;
    const double demandedDemand = h->CityGetDemanded(demandSide, commodity);
    const double exportedDemand = h->CityGetExported(demandSide, commodity);
    const float negative = static_cast<float>(static_cast<double>(price / scale) + 1.0);
    const double producedDemand = h->CityGetProduced(demandSide, commodity);
    const double importedDemand = h->CityGetImported(demandSide, commodity);
    const double producedSupply = h->CityGetProduced(supplySide, commodity);
    const double importedSupply = h->CityGetImported(supplySide, commodity);
    const double demandedSupply = h->CityGetDemanded(supplySide, commodity);
    const double exportedSupply = h->CityGetExported(supplySide, commodity);

    double demand = static_cast<double>(negative);
    if (static_cast<double>(negative) <= demandedDemand + exportedDemand) demand = demandedDemand + exportedDemand;
    const double fromDemand = static_cast<double>(scale) * demand - ((producedDemand + importedDemand) - existing);
    const double fromSupply = (producedSupply + importedSupply) - ((demandedSupply + exportedSupply) - existing);
    double result = fromSupply;
    if (fromDemand <= fromSupply) result = fromDemand;
    return result < 0.0 ? 0.0 : result;
}

//---------------------------------------------------------------------------------------------------------------------
// commodity statistics
//---------------------------------------------------------------------------------------------------------------------

// @ 0x10007386c. Copies the city's budget/income into the current region city and writes the three commodity
// statistics (power, water, garbage) both into the region city and into the history warehouse.
void cSC4NeighborsSimulator::UpdateCommodityStats(cISC4City* city) {
    INeighborsHost* h = Host();
    if (!h || !city || h->City() != city) return;
    cISC4RegionalCity* current = h->CurrentRegionalCity();
    if (!current) return;

    h->CitySetBudget(current, h->BudgetTotal());       // 0x198
    h->CitySetIncome(current, h->BudgetIncome());      // 0x1a8

    const cISCPropertyHolder* properties = GetTuningExemplar();
    if (!properties) return;
    float dealYears = 0.0f;
    cSCPropertyHelp::GetPropertyValue(properties, kNeighborsPropertyDealYears, dealYears);
    const float dealMonths = static_cast<float>(static_cast<double>(dealYears) * 12.0);
    // (The original also builds a "sim date - 91 days" temporary here; it is never read.)

    // Sum of the deal costs per commodity; the loop widens each cost through a float.
    double costSum[3] = {0.0, 0.0, 0.0};
    for (const auto& ref : deals_) {
        cISC4NeighborDeal* deal = ref.get();
        if (!deal) continue;
        const float cost = static_cast<float>(deal->GetCost());
        switch (deal->GetType()) {
            case kDealTypePowerBuy: costSum[0] += static_cast<double>(cost); break;
            case kDealTypeWaterBuy: costSum[1] += static_cast<double>(cost); break;
            case kDealTypeGarbageBuy: costSum[2] += static_cast<double>(cost); break;
            default: break;
        }
    }
    update_deals_in_regional_cities(true, true);

    // ---- power (commodity 1) ----
    {
        const int capacity = h->PowerCapacity();
        const int demand = h->PowerDemand();
        const double imported = h->CityGetImported(current, 1);
        const double exported = h->CityGetExported(current, 1);
        double lineAmount = 0.0;
        const bool hasLine = h->BudgetLineItemAmount(kNeighborsBudgetPower, lineAmount);
        double price = 1e-05;
        if (hasLine) {
            price = (costSum[0] + lineAmount) / (imported + static_cast<double>(capacity) + 1.0);
        }
        h->CitySetProduced(current, 1, static_cast<double>(capacity));
        h->CitySetDemanded(current, 1, static_cast<double>(demand));
        h->CitySetExported(current, 1, exported);
        h->CitySetImported(current, 1, imported);
        h->CitySetCostPerUnit(current, 1, price);
        h->RecordHistory(kNeighborsHistoryPowerImported, static_cast<int>(imported));
        h->RecordHistory(kNeighborsHistoryPowerExported, static_cast<int>(exported));
    }
    double powerSubLine = 0.0;
    h->BudgetSubLineItemAmount(kNeighborsBudgetPower, kNeighborsBudgetPowerSub, powerSubLine);

    // ---- water (commodity 2) ----
    {
        const int capacity = h->WaterCapacity();
        const int demand = h->WaterDemand();
        const double imported = h->CityGetImported(current, 2);
        const double exported = h->CityGetExported(current, 2);
        double lineAmount = 0.0;
        const bool hasLine = h->BudgetLineItemAmount(kNeighborsBudgetWater, lineAmount);
        double price = 1e-05;
        if (hasLine) {
            price = (costSum[1] + lineAmount) / (imported + static_cast<double>(capacity) + 1.0);
        }
        h->CitySetProduced(current, 2, static_cast<double>(capacity));
        h->CitySetDemanded(current, 2, static_cast<double>(demand));
        h->CitySetExported(current, 2, exported);
        h->CitySetImported(current, 2, imported);
        h->CitySetCostPerUnit(current, 2, price);
        h->RecordHistory(kNeighborsHistoryWaterImported, static_cast<int>(imported));
        h->RecordHistory(kNeighborsHistoryWaterExported, static_cast<int>(exported));
    }

    // ---- garbage (commodity 3) ----
    {
        int landfillStart = 0, landfillEnd = 0;
        h->LandfillRange(landfillStart, landfillEnd);
        const int pollution0 = h->PollutionValue(0);   // 0xf8
        const int pollution1 = h->PollutionValue(1);   // 0x100
        const int pollution2 = h->PollutionValue(2);   // 0x130
        const int pollution3 = h->PollutionValue(3);   // 0x110
        const int pollution4 = h->PollutionValue(4);   // 0x118
        const double imported = h->CityGetImported(current, 3);
        const double exported = h->CityGetExported(current, 3);
        const float ratio = static_cast<float>(landfillEnd - landfillStart) / dealMonths;
        float produced = static_cast<float>(pollution0) + ratio;
        produced = produced + static_cast<float>(pollution1);
        produced = produced + static_cast<float>(pollution2);
        produced = produced + static_cast<float>(pollution3);

        double price = 1e-05;
        double garbageLine = 0.0;
        if (h->BudgetLineItemAmount(kNeighborsBudgetGarbage, garbageLine)) {
            std::vector<float> curveValues;
            if (cSCPropertyHelp::GetPropertyValue(properties, kNeighborsPropertyGarbageCurve, curveValues)) {
                curve_.SetResponseCurveData(curveValues.data(), curveValues.size());
            }
            const float input = static_cast<float>((powerSubLine + costSum[2] + garbageLine) /
                                                   (imported + static_cast<double>(produced) + 1.0));
            price = static_cast<double>(curve_.GetOutputValue(input));
        }
        h->CitySetProduced(current, 3, static_cast<double>(produced));
        h->CitySetDemanded(current, 3, static_cast<double>(pollution4));
        h->CitySetExported(current, 3, exported);
        h->CitySetImported(current, 3, imported);
        h->CitySetCostPerUnit(current, 3, price);
        h->RecordHistory(kNeighborsHistoryGarbageExported, static_cast<int>(exported));
        h->RecordHistory(kNeighborsHistoryGarbageImported, static_cast<int>(imported));
        h->RecordHistory(kNeighborsHistoryGarbageDemanded, pollution4);
        h->RecordHistory(kNeighborsHistoryGarbageProduced, static_cast<int>(produced));
    }
}

}  // namespace sc4
