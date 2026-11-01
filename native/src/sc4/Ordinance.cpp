#include "sc4/Ordinance.h"

#include <cstdio>

#include "gz/StreamOps.h"
#include "sc4/DBSegment.h"
#include "sc4/SCPropertyHelp.h"

namespace sc4 {

namespace {
IOrdinanceHost* g_host = nullptr;
std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
}  // namespace

IOrdinanceHost* OrdinanceHost() { return g_host; }
void SetOrdinanceHost(IOrdinanceHost* host) { g_host = host; }

// ----------------------------------------------------------------------------------- cSC4OrdinanceSimple

cSC4OrdinanceSimple::~cSC4OrdinanceSimple() {
    if (initialised_) {
        initialised_ = false;
        enabled_ = false;
    }
}

bool cSC4OrdinanceSimple::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == 1 || iid == kGZCLSID_cSC4OrdinanceSimple || iid == kGZIID_cISC4Ordinance) {
        *out = static_cast<cISC4Ordinance*>(this);
    } else if (iid == 0xe4fda3d4) {
        *out = static_cast<cIGZSerializable*>(this);
    } else {
        return properties_.QueryInterface(iid, out);              // the exemplar property holder answers the rest
    }
    AddRef();
    return true;
}

std::uint32_t cSC4OrdinanceSimple::Release() {
    if (refCount_ > 1) return --refCount_;
    delete this;
    return 0;
}

bool cSC4OrdinanceSimple::Init() {
    if (!initialised_) {
        initialised_ = true;
        enabled_ = true;
        FromData(key_);
    }
    return true;
}

bool cSC4OrdinanceSimple::Shutdown() {
    if (initialised_) {
        initialised_ = false;
        enabled_ = false;
    }
    return true;
}

// Loads the ordinance exemplar: id, localized name / description (falling back to the id in hex), availability
// year and chance, enactment / retraction / constant income, income factor, advisor and the income flag.
bool cSC4OrdinanceSimple::FromData(const cGZPersistResourceKey& key) {
    if (!g_host) return false;
    cISCResExemplar* exemplar = g_host->LoadExemplar(key);
    if (!exemplar) return false;
    SetKey(key);
    properties_.SetDefaultExemplar(exemplar);
    cISCPropertyHolder* holder = &properties_;
    std::uint32_t type = 0;
    cSCPropertyHelp::GetPropertyValue(holder, 0x10, type);
    cSCPropertyHelp::GetPropertyValue(holder, kOrdPropId, id_);
    cGZPersistResourceKey stringKey;
    char hex[16];
    std::snprintf(hex, sizeof hex, "0x%08x", id_);
    std::string name;
    if (cSCPropertyHelp::GetPropertyValue(holder, kOrdPropNameKey, stringKey) && stringKey.type != 0)
        name = g_host->LoadString(stringKey);
    if (name.empty()) {
        cRZString text;
        if (!cSCPropertyHelp::GetPropertyValue(holder, kOrdPropNameText, text)) text = cRZString(hex);
        name = text.ToChar();
    }
    name_ = cRZString(name.c_str());
    std::string description;
    if (cSCPropertyHelp::GetPropertyValue(holder, kOrdPropDescriptionKey, stringKey))
        description = g_host->LoadString(stringKey);
    if (description.empty()) {
        cRZString text;
        if (!cSCPropertyHelp::GetPropertyValue(holder, kOrdPropDescriptionText, text)) text = cRZString(hex);
        description = text.ToChar();
    }
    description_ = cRZString(description.c_str());
    cSCPropertyHelp::GetPropertyValue(holder, kOrdPropYearFirstAvailable, yearFirstAvailable_);
    SC4Percentage chance;
    if (cSCPropertyHelp::GetPropertyValue(holder, kOrdPropChanceAvailable, chance)) chanceAvailability_ = chance.value;
    cSCPropertyHelp::GetPropertyValue(holder, kOrdPropEnactmentIncome, enactmentIncome_);
    cSCPropertyHelp::GetPropertyValue(holder, kOrdPropRetractionIncome, retractionIncome_);
    cSCPropertyHelp::GetPropertyValue(holder, kOrdPropConstantIncome, constantIncome_);
    cSCPropertyHelp::GetPropertyValue(holder, kOrdPropIncomeFactor, incomeFactor_);
    cSCPropertyHelp::GetPropertyValue(holder, kOrdPropAdvisor, advisorId_);
    cSCPropertyHelp::GetPropertyValue(holder, kOrdPropIsIncome, incomeOrdinance_);
    exemplar->Release();
    return true;
}

bool cSC4OrdinanceSimple::SetAvailable(bool available) {
    ForceAvailable(available);
    adjustedIncome_ = 0;
    return true;
}

// Turning an ordinance on / off adds its enactment income / subtracts its retraction cost to the adjusted income.
bool cSC4OrdinanceSimple::SetOn(bool on) {
    if (on_ != on) {
        const float base = static_cast<float>(adjustedIncome_);
        const float adjusted = on ? base + std::fabs(static_cast<float>(enactmentIncome_))
                                  : base - std::fabs(static_cast<float>(retractionIncome_));
        adjustedIncome_ = static_cast<std::int64_t>(adjusted);
        on_ = on;
    }
    return true;
}

bool cSC4OrdinanceSimple::CheckConditions() {
    if (!enabled_ || !g_host) return false;
    return yearFirstAvailable_ <= static_cast<std::uint32_t>(g_host->CurrentYear());
}

std::int64_t cSC4OrdinanceSimple::GetCurrentMonthlyIncome() {
    const int base = g_host ? g_host->ResidentialIncomeBase() : 0;
    return static_cast<std::int64_t>(static_cast<float>(base) * incomeFactor_ + static_cast<float>(constantIncome_));
}

bool cSC4OrdinanceSimple::Simulate() {
    adjustedIncome_ = GetCurrentMonthlyIncome();
    return true;
}

bool cSC4OrdinanceSimple::Write(cIGZOStream& stream) const {
    stream.SetUint16(4);
    stream << initialised_;
    stream.SetUint32(id_);
    stream.SetGZStr(name_);
    stream.SetGZStr(description_);
    stream.SetUint32(yearFirstAvailable_);
    stream.SetFloat32(chanceAvailability_);
    stream.SetSint64(enactmentIncome_);
    stream.SetSint64(retractionIncome_);
    stream.SetSint64(constantIncome_);
    stream.SetFloat32(incomeFactor_);
    stream.SetUint32(advisorId_);
    stream << available_;
    stream << on_;
    stream << enabled_;
    stream.SetSint64(adjustedIncome_);
    stream << incomeOrdinance_;
    properties_.Write(stream);
    return !stream.GetError();
}

bool cSC4OrdinanceSimple::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    stream >> initialised_;
    stream.GetUint32(id_);
    stream.GetGZStr(name_);
    stream.GetGZStr(description_);
    stream.GetUint32(yearFirstAvailable_);
    float chance = 0.0f;
    stream.GetFloat32(chance);
    chanceAvailability_ = version < 4 ? chance * 0.01f : chance;
    stream.GetSint64(enactmentIncome_);
    stream.GetSint64(retractionIncome_);
    stream.GetSint64(constantIncome_);
    stream.GetFloat32(incomeFactor_);
    stream.GetUint32(advisorId_);
    stream >> available_;
    stream >> on_;
    stream >> enabled_;
    stream.GetSint64(adjustedIncome_);
    if (version < 2) incomeOrdinance_ = (id_ == static_cast<std::uint32_t>(-0x5f2f8ed7));
    else stream >> incomeOrdinance_;
    properties_.Read(stream);
    cGZPersistResourceKey stringKey;
    if (g_host && cSCPropertyHelp::GetPropertyValue(&properties_, kOrdPropNameKey, stringKey)) {
        const std::string text = g_host->LoadString(stringKey);
        if (!text.empty()) name_ = cRZString(text.c_str());
    }
    if (g_host && cSCPropertyHelp::GetPropertyValue(&properties_, kOrdPropDescriptionKey, stringKey)) {
        const std::string text = g_host->LoadString(stringKey);
        if (!text.empty()) description_ = cRZString(text.c_str());
    }
    if (cISCResExemplar* exemplar = properties_.GetDefaultExemplar()) exemplar->GetKey(key_);
    FromData(key_);
    return !stream.GetError();
}

// -------------------------------------------------------------------------------- cSC4OrdinanceSimulator

cSC4OrdinanceSimulator::~cSC4OrdinanceSimulator() = default;

bool cSC4OrdinanceSimulator::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == 1 || iid == kSimulatorTypeOrdinance || iid == kGZCLSID_cSC4OrdinanceSimulator) {
        *out = static_cast<cIGZSerializable*>(this);
    } else if (iid == 0xe4fda3d4 || iid == 0x452294aa) {
        *out = static_cast<cIGZSerializable*>(this);
    } else {
        return false;
    }
    AddRef();
    return true;
}

std::uint32_t cSC4OrdinanceSimulator::Release() {
    if (refCount_ < 2) {
        Shutdown();
        ReleaseAll();
        delete this;
        return 0;
    }
    return --refCount_;
}

void cSC4OrdinanceSimulator::ReleaseAll() {
    for (auto& entry : ordinances_)
        if (entry.second) entry.second->Release();
    ordinances_.clear();
}

bool cSC4OrdinanceSimulator::Init() {
    if (!initialised_) {
        initialised_ = true;
        if (!loaded_) {
            monthlyExpense_ = monthlyIncome_ = netAdjustment_ = 0;
        }
        loaded_ = false;
    }
    return true;
}

bool cSC4OrdinanceSimulator::Shutdown() {
    if (initialised_) initialised_ = false;
    return true;
}

bool cSC4OrdinanceSimulator::DoMessage(std::uint32_t messageType) {
    if (messageType == 0x66956810) SimulationBegin();
    else if (messageType == 0x66956816) DoMonthlySimulation();
    return true;
}

// Message 0x66956810 (simulation begin): availability is re-evaluated and the running totals rebuilt.
void cSC4OrdinanceSimulator::SimulationBegin() {
    monthlyIncome_ = monthlyExpense_ = 0;
    for (auto& entry : ordinances_) {
        cISC4Ordinance* o = entry.second;
        const bool available = o->IsAvailable();
        const bool conditions = o->CheckConditions();
        if (!available && conditions) SetOrdinanceAvailable(o->GetID(), true);
        else if (available && !conditions) SetOrdinanceAvailable(o->GetID(), false);
        if (o->IsOn()) {
            const std::int64_t income = o->GetMonthlyAdjustedIncome();
            if (income > 0) monthlyIncome_ += income;
            else monthlyExpense_ += -income;
        }
    }
}

// Monthly: pending enactment / retraction money is settled first, then every ordinance that is on contributes its
// adjusted income, and availability follows the ordinances' conditions.
void cSC4OrdinanceSimulator::DoMonthlySimulation() {
    monthlyIncome_ = monthlyExpense_ = 0;
    if (netAdjustment_ < 1) monthlyExpense_ = -netAdjustment_;
    else monthlyIncome_ = netAdjustment_;
    netAdjustment_ = 0;
    for (auto& entry : ordinances_) {
        cISC4Ordinance* o = entry.second;
        if (o->IsOn() && o->IsIncomeOrdinance()) {
            const std::int64_t income = o->GetMonthlyAdjustedIncome();
            if (income > 0) monthlyIncome_ += income;
            else monthlyExpense_ += -income;
        }
        const bool available = o->IsAvailable();
        const bool conditions = o->CheckConditions();
        if (!available && conditions) SetOrdinanceAvailable(o->GetID(), true);
        else if (available && !conditions) SetOrdinanceAvailable(o->GetID(), false);
    }
    if (g_host) g_host->PostMessage(kOrdinanceMessageMonthlyDone, 0, 0);
}

bool cSC4OrdinanceSimulator::AddOrdinance(cISC4Ordinance* ordinance) {
    if (!ordinance) return false;
    RemoveOrdinance(ordinance);
    const std::uint32_t id = ordinance->GetID();
    if (!ordinances_.count(id)) {
        ordinance->AddRef();
        ordinances_[id] = ordinance;
    }
    if (ordinance->IsAvailable()) {
        ordinance->SetAvailable(false);
        SetOrdinanceAvailable(id, true);
    }
    if (ordinance->IsOn()) {
        ordinance->SetOn(false);
        SetOrdinanceOn(id, true);
    }
    if (!ordinance->IsAvailable() && ordinance->CheckConditions()) SetOrdinanceAvailable(id, true);
    return true;
}

bool cSC4OrdinanceSimulator::RemoveOrdinance(cISC4Ordinance* ordinance) {
    if (!ordinance) return false;
    auto it = ordinances_.find(ordinance->GetID());
    if (it == ordinances_.end()) return false;
    cISC4Ordinance* o = it->second;
    if (o->IsOn()) SetOrdinanceOn(o->GetID(), false);
    if (o->IsAvailable()) SetOrdinanceAvailable(o->GetID(), false);
    ordinances_.erase(it);
    o->Release();
    return true;
}

void cSC4OrdinanceSimulator::CreateStandardOrdinance(const cGZPersistResourceKey& key) {
    auto* ordinance = new cSC4OrdinanceSimple();
    ordinance->AddRef();
    ordinance->SetKey(key);
    ordinance->Init();
    const std::uint32_t id = ordinance->GetID();
    if (!ordinances_.count(id)) {
        ordinance->AddRef();
        ordinances_[id] = ordinance;
    }
    ordinance->Release();
}

void cSC4OrdinanceSimulator::LoadAllStandardOrdinances() {
    if (g_host) g_host->ForEachOrdinanceKey([this](const cGZPersistResourceKey& key) { AddOrdinanceData(key); });
}

void cSC4OrdinanceSimulator::AddOrdinanceData(const cGZPersistResourceKey& key) {
    if (!g_host) return;
    cISCResExemplar* exemplar = g_host->LoadExemplar(key);
    if (!exemplar) return;
    std::uint32_t type = 0;
    if (cSCPropertyHelp::GetPropertyValue(exemplar->AsISCPropertyHolder(), 0x10, type) && type == kExemplarTypeOrdinance)
        CreateStandardOrdinance(key);
    exemplar->Release();
}

cISC4Ordinance* cSC4OrdinanceSimulator::GetOrdinanceByID(std::uint32_t id) const {
    auto it = ordinances_.find(id);
    return it == ordinances_.end() ? nullptr : it->second;
}

cISC4Ordinance* cSC4OrdinanceSimulator::GetOrdinanceByName(const std::string& name) const {
    const std::string wanted = Lower(name);
    for (const auto& entry : ordinances_)
        if (Lower(entry.second->GetName().ToChar()) == wanted) return entry.second;
    return nullptr;
}

std::uint32_t cSC4OrdinanceSimulator::GetOrdinanceIDByName(const std::string& name) const {
    cISC4Ordinance* o = GetOrdinanceByName(name);
    return o ? o->GetID() : 0;
}

bool cSC4OrdinanceSimulator::IsOrdinanceEnabled(std::uint32_t id) const {
    cISC4Ordinance* o = GetOrdinanceByID(id);
    return o && o->IsEnabled();
}

bool cSC4OrdinanceSimulator::SetOrdinanceEnabled(std::uint32_t id, bool enabled) {
    cISC4Ordinance* o = GetOrdinanceByID(id);
    if (o) o->SetEnabled(enabled);
    return o != nullptr;
}

bool cSC4OrdinanceSimulator::IsOrdinanceAvailable(std::uint32_t id) const {
    cISC4Ordinance* o = GetOrdinanceByID(id);
    return o && o->IsAvailable();
}

bool cSC4OrdinanceSimulator::IsOrdinanceOn(std::uint32_t id) const {
    cISC4Ordinance* o = GetOrdinanceByID(id);
    return o && o->IsOn();
}

bool cSC4OrdinanceSimulator::IsOrdinanceAvailableButOff(std::uint32_t id) const {
    cISC4Ordinance* o = GetOrdinanceByID(id);
    return o && o->IsAvailable() && !o->IsOn();
}

bool cSC4OrdinanceSimulator::SetOrdinanceAvailable(std::uint32_t id, bool available) {
    cISC4Ordinance* o = GetOrdinanceByID(id);
    if (!o) return false;
    if (o->IsAvailable() == available) return true;
    if (!available && o->IsOn()) SetOrdinanceOn(id, false);       // an unavailable ordinance cannot stay on
    o->SetAvailable(available);
    if (g_host) g_host->PostMessage(kOrdinanceMessageAvailabilityChanged, o->GetID(), o->IsAvailable() ? 1 : 0);
    if (available && o->IsOn()) {
        o->SetOn(false);
        SetOrdinanceOn(id, true);
    }
    return true;
}

bool cSC4OrdinanceSimulator::SetOrdinanceOn(std::uint32_t id, bool on) {
    cISC4Ordinance* o = GetOrdinanceByID(id);
    if (!o) return false;
    if (o->IsOn() != on) {
        o->SetOn(on);
        netAdjustment_ += on ? o->GetEnactmentIncome() : o->GetRetractionIncome();
        if (g_host) g_host->PostMessage(kOrdinanceMessageOnChanged, o->GetID(), o->IsOn() ? 1 : 0);
    }
    return true;
}

int cSC4OrdinanceSimulator::GetOrdinanceAvailableCount() const {
    int count = 0;
    for (const auto& entry : ordinances_) count += entry.second->IsAvailable() ? 1 : 0;
    return count;
}

int cSC4OrdinanceSimulator::GetOrdinanceOnCount() const {
    int count = 0;
    for (const auto& entry : ordinances_) count += entry.second->IsOn() ? 1 : 0;
    return count;
}

int cSC4OrdinanceSimulator::GetOrdinanceAvailableArray(std::uint32_t* out, std::uint32_t& count) const {
    int total = 0;
    std::uint32_t written = 0;
    for (const auto& entry : ordinances_) {
        if (!entry.second->IsAvailable()) continue;
        ++total;
        if (written < count) out[written++] = entry.second->GetID();
    }
    count = written;
    return total;
}

int cSC4OrdinanceSimulator::GetOrdinanceOnArray(std::uint32_t* out, std::uint32_t& count) const {
    int total = 0;
    std::uint32_t written = 0;
    for (const auto& entry : ordinances_) {
        if (!entry.second->IsOn()) continue;
        ++total;
        if (written < count) out[written++] = entry.second->GetID();
    }
    count = written;
    return total;
}

int cSC4OrdinanceSimulator::GetOrdinanceIDArray(std::uint32_t* out, std::uint32_t& count) const {
    int total = 0;
    std::uint32_t written = 0;
    for (const auto& entry : ordinances_) {
        if (written < count) out[written++] = entry.second->GetID();
        ++total;
    }
    count = written;
    return total;
}

// Sum (offsets) / product (factors) of the named property over every ordinance that is on; array properties are
// indexed, scalars apply to every index.
std::int32_t cSC4OrdinanceSimulator::GetCumulativeOffset(std::uint32_t property, std::uint32_t index) const {
    std::int32_t sum = 0;
    for (const auto& entry : ordinances_) {
        cISC4Ordinance* o = entry.second;
        if (!o->IsOn()) continue;
        cISCPropertyHolder* holder = o->GetMiscProperties();
        if (!holder || !holder->HasProperty(property)) continue;
        cIGZVariant* value = holder->GetProperty(property)->GetPropertyValue();
        std::int32_t part = 0;
        switch (value->GetType()) {
            case cIGZVariant::kTypeUint32:
            case cIGZVariant::kTypeSint32: part = value->GetValSint32(); break;
            case cIGZVariant::kTypeUint32 | cIGZVariant::kTypeArrayFlag:
                if (index < value->GetCount()) part = static_cast<std::int32_t>(value->RefUint32()[index]);
                break;
            case cIGZVariant::kTypeSint32 | cIGZVariant::kTypeArrayFlag:
                if (index < value->GetCount()) part = value->RefSint32()[index];
                break;
            default: break;
        }
        sum += part;
    }
    return sum;
}

float cSC4OrdinanceSimulator::GetCumulativeFactor(std::uint32_t property, std::uint32_t index) const {
    float product = 1.0f;
    for (const auto& entry : ordinances_) {
        cISC4Ordinance* o = entry.second;
        if (!o->IsOn()) continue;
        cISCPropertyHolder* holder = o->GetMiscProperties();
        if (!holder || !holder->HasProperty(property)) continue;
        cIGZVariant* value = holder->GetProperty(property)->GetPropertyValue();
        float part = 1.0f;
        if (value->GetType() == (cIGZVariant::kTypeFloat32 | cIGZVariant::kTypeArrayFlag)) {
            if (index < value->GetCount()) part = value->RefFloat32()[index];
        } else if (value->GetType() == cIGZVariant::kTypeFloat32) {
            part = value->GetValFloat32();
        }
        product *= part;
    }
    return product;
}

bool cSC4OrdinanceSimulator::Write(cIGZOStream& stream) const {
    stream.SetUint16(4);
    stream.SetSint64(monthlyExpense_);
    stream.SetSint64(monthlyIncome_);
    stream.SetUint32(static_cast<std::uint32_t>(ordinances_.size()));
    for (const auto& entry : ordinances_) {
        void* serializable = nullptr;
        if (entry.second->QueryInterface(0xe4fda3d4, &serializable) && serializable) {
            auto* object = static_cast<cIGZSerializable*>(serializable);
            SC4WriteGZSerializable(stream, object);
            object->Release();
        }
    }
    stream.SetSint64(netAdjustment_);
    return !stream.GetError();
}

bool cSC4OrdinanceSimulator::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != 4) return false;
    stream.GetSint64(monthlyExpense_);
    stream.GetSint64(monthlyIncome_);
    std::uint32_t count = 0;
    stream.GetUint32(count);
    ReleaseAll();
    for (std::uint32_t i = 0; i < count; ++i) {
        void* object = nullptr;
        if (!SC4ReadGZSerializable(stream, 0xe4fda3d4, &object, false) || !object) continue;
        auto* serializable = static_cast<cIGZSerializable*>(static_cast<cIGZUnknown*>(object));
        void* ordinance = nullptr;
        if (serializable->QueryInterface(kGZIID_cISC4Ordinance, &ordinance) && ordinance) {
            auto* o = static_cast<cISC4Ordinance*>(ordinance);          // reference kept by the map
            // keyed by the class id (serializable vslot 0x14), as the original: an ordinance whose record loads after this
            // one is not read yet, so its own id is still 0 here
            const std::uint32_t key = serializable->GetGZCLSID();
            auto existing = ordinances_.find(key);
            if (existing != ordinances_.end()) {
                existing->second->Release();
                existing->second = o;
            } else {
                ordinances_[key] = o;
            }
        }
        serializable->Release();
    }
    stream.GetSint64(netAdjustment_);
    loaded_ = true;
    return !stream.GetError();
}

}  // namespace sc4
