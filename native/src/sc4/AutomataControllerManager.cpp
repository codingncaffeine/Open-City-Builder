#include "sc4/AutomataControllerManager.h"

#include <bit>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "gz/StreamOps.h"
#include "rz/cRZCmdLine.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/AutomataStates.h"
#include "sc4/DBSegment.h"
#include "sc4/GeometryUtilities.h"
#include "sc4/SCPropertyHelp.h"

namespace sc4 {

namespace {
IControllerManagerHost* g_host = nullptr;
constexpr std::uint16_t kManagerStreamVersion = 1;
constexpr std::uint32_t kClockIid = 0x681bd53a, kOccupantIid = 0x26bb767e;
// kScrollGranularitySquared: camera movement (squared metres) that forces a view update, per zoom level
constexpr float kScrollGranularitySquared[8] = {283875.8125f, 77506.5546875f, 24087.0390625f, 7744.0f, 2787.83984375f, 0.0f, 0.0f, 0.0f};
const std::uint32_t kRegisteredMessages[] = {kCtrlMsgOccupantInserted, kCtrlMsgOccupantRemoved, kCtrlMsgPostCityInit, kCtrlMsgQueueReset,
                                             kCtrlMsgHourPassed, kCtrlMsgSourceDataChanged, kCtrlMsgCameraMoved, kCtrlMsgCameraChanged,
                                             kCtrlMsgEffectTriggers, kCtrlMsgScrollStart, kCtrlMsgScrollEnd, kCtrlMsgOccupantGroupAdded};
}  // namespace

IControllerManagerHost* ControllerManagerHost() { return g_host; }
void SetControllerManagerHost(IControllerManagerHost* host) { g_host = host; }

cSC4AutomataControllerManager::~cSC4AutomataControllerManager() = default;

bool cSC4AutomataControllerManager::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZIID_cISC4AutomataControllerManager) {
        *out = static_cast<cISC4AutomataControllerManager*>(this);
    } else if (iid == kGZIID_cIGZSerializable || iid == 1) {
        *out = static_cast<cIGZSerializable*>(this);   // (iid 1 answers the serializable view too)
    } else if (iid == kGZIID_cIGZMessageTarget2) {
        *out = static_cast<cIGZMessageTarget2*>(this);
    } else {
        return false;
    }
    AddRef();
    return true;
}

std::uint32_t cSC4AutomataControllerManager::Release() {
    if (refCount_ > 1) return --refCount_;
    FinalRelease();
    return 0;
}

void cSC4AutomataControllerManager::FinalRelease() {
    Shutdown();
    delete this;
}

// ---- lifecycle ----------------------------------------------------------------------------------------------------------------
bool cSC4AutomataControllerManager::Init() {
    if (initialized_) return true;
    IControllerManagerHost* host = g_host;
    grid_.Init();
    if (!clock_) {
        clock_ = host->AcquireClock();
        clock_->AddRef();
    }
    if (!script_) {
        script_ = host->AcquireScriptSystem();
        script_->AddRef();
    }
    if (cIGZMessageServer2* server = host->MessageServer()) {
        for (std::uint32_t message : kRegisteredMessages) server->AddNotification(static_cast<cIGZMessageTarget2*>(this), message);
    }
    host->AddTickNotification(static_cast<cIGZMessageTarget2*>(this), kControllerManagerChannel);
    if (host->CheatManagerAvailable() && host->DebugFunctionalityEnabled()) {
        host->RegisterCheat(kAutomataCheatControllers, "Controllers");
        host->AddCheatListener(static_cast<cIGZMessageTarget*>(this));
    }
    initialized_ = true;
    return true;
}

bool cSC4AutomataControllerManager::Shutdown() {
    if (!initialized_) return true;
    IControllerManagerHost* host = g_host;
    if (host->CheatManagerAvailable()) {
        host->UnregisterCheat(kAutomataCheatControllers);
        host->RemoveCheatListener(static_cast<cIGZMessageTarget*>(this));
    }
    host->RemoveTickNotification(static_cast<cIGZMessageTarget2*>(this), kControllerManagerChannel);
    if (cIGZMessageServer2* server = host->MessageServer()) {
        for (std::uint32_t message : kRegisteredMessages) server->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), message);
    }
    if (script_) {
        script_->Release();
        script_ = nullptr;
    }
    if (clock_) {
        clock_->Release();
        clock_ = nullptr;
    }
    ClearControllerQInfo();
    ClearControllers();
    initialized_ = false;
    return true;
}

void cSC4AutomataControllerManager::ClearControllerQInfo() {
    for (auto& node : queues_) {
        if (node.second.occupant) node.second.occupant->Release();
        node.second.info.Clear();
    }
    queues_.clear();
}

void cSC4AutomataControllerManager::ClearControllers() {
    grid_.Clear();
    for (auto& node : controllers_) {
        cISC4Occupant* occupant = node.second.occupant;
        for (cSC4AutomataController* controller : node.second.controllers) {
            controller->Shutdown();
            controller->Release();
        }
        node.second.controllers.clear();
        if (occupant) occupant->Release();
    }
    controllers_.clear();
}

void cSC4AutomataControllerManager::HandlePostCityInit() {
    if (!postCityInitPending_) return;
    for (auto& node : controllers_) {
        auto& list = node.second.controllers;
        for (auto it = list.begin(); it != list.end();) {
            cSC4AutomataController* controller = *it;
            if (!controller) {
                ++it;
                continue;
            }
            const cIGZString& name = controller->GetTemplateName();
            tControllerTemplate* tmpl = script_->FindControllerTemplate(name);
            if (!tmpl) {
                g_host->Report(std::string("Couldn't find automata controller template '") + name.ToChar() + "' in scripts.  Removing controller.\n");
                it = list.erase(it);
                controller->Shutdown();
                controller->Release();
            } else {
                controller->SetTemplate(tmpl);
                controller->Init();
                grid_.AddController(controller);
                ++it;
            }
        }
    }
}

// ---- time ---------------------------------------------------------------------------------------------------------------------
void cSC4AutomataControllerManager::SendControllerMessage(std::uint32_t type, cSC4AutomataController* controller) {
    cIGZMessageServer2* server = g_host->MessageServer();
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(type);
    message->SetVoid1(controller);
    server->MessageSend(message);
    message->Release();
}

void cSC4AutomataControllerManager::UpdateControllers() {
    for (auto& node : controllers_) {
        for (cSC4AutomataController* controller : node.second.controllers) {
            if (controller->CheckFlag(kCtrlFlagActive)) {
                grid_.MoveController(controller);
                controller->HandleViewChanged();
            }
        }
    }
}

void cSC4AutomataControllerManager::HandleViewChanged() {
    for (auto& node : controllers_) {
        for (cSC4AutomataController* controller : node.second.controllers) controller->HandleViewChanged();
    }
}

void cSC4AutomataControllerManager::DoAnimationTick(std::uint32_t data, float deltaMilliseconds) {
    IControllerManagerHost* host = g_host;
    const int hour = clock_->Hour();
    const int minute = clock_->Minute();
    const float t = static_cast<float>(minute) / 60.0f + static_cast<float>(hour);
    std::int16_t days = now_.days;
    if (t < now_.time) {
        ++days;
        now_.days = days;
    }
    now_.time = t;
    float lastTime = last_.time;
    for (; lastTime < 0.0f; lastTime += 24.0f) ++days;
    for (; lastTime >= 24.0f; lastTime -= 24.0f) --days;
    ControllerTime delta;
    delta.time = t - lastTime;
    delta.days = static_cast<std::int16_t>(days - last_.days);
    NormalizeControllerTime(delta);

    if (enabled_ && host->AutomataLayersRunning()) {
        UpdateControllers();
        for (auto& node : controllers_) {
            auto& list = node.second.controllers;
            for (auto it = list.begin(); it != list.end();) {
                cSC4AutomataController* controller = *it;
                if (controller->IsTickable()) controller->DoAnimationTick(deltaMilliseconds, data, delta);
                if (controller->IsExpired() && controller->ReadyToExpire()) {
                    it = list.erase(it);
                    SendControllerMessage(kMessageControllerRemoved, controller);
                    grid_.RemoveController(controller);
                    controller->Shutdown();
                    controller->Release();
                } else {
                    ++it;
                }
            }
        }
    }
    last_.days = now_.days;
    last_.time = now_.time;
}

ControllerTime cSC4AutomataControllerManager::ElapsedSinceLastTick(const ControllerTime& at) const {
    std::int16_t days = now_.days;
    float lastTime = last_.time;
    for (; lastTime < 0.0f; lastTime += 24.0f) ++days;
    for (; lastTime >= 24.0f; lastTime -= 24.0f) --days;
    ControllerTime delta;
    delta.time = at.time - lastTime;
    delta.days = static_cast<std::int16_t>(days - last_.days);
    return delta;
}

// ---- occupant bookkeeping ---------------------------------------------------------------------------------------------------
void cSC4AutomataControllerManager::DropControllerEntry(cISC4Occupant* occupant) {
    // the entry has no controllers left: its queue info and both counted occupant references go with it
    auto queue = queues_.find(KeyOf(occupant));
    if (queue != queues_.end()) {
        queue->second.info.Clear();
        if (occupant) occupant->Release();
        queues_.erase(queue);
    }
    controllers_.erase(KeyOf(occupant));
    if (occupant) occupant->Release();
}

void cSC4AutomataControllerManager::EraseController(std::list<cSC4AutomataController*>& list, std::list<cSC4AutomataController*>::iterator it) {
    cSC4AutomataController* controller = *it;
    list.erase(it);
    SendControllerMessage(kMessageControllerRemoved, controller);
    grid_.RemoveController(controller);
    controller->Shutdown();
    controller->Release();
}

tControllerQInfo* cSC4AutomataControllerManager::GetControllerQInfo(cISC4Occupant* occupant, bool create) {
    auto it = queues_.find(KeyOf(occupant));
    if (it != queues_.end()) return &it->second.info;
    if (!create) return nullptr;
    QueueEntry& entry = queues_[KeyOf(occupant)];
    entry.occupant = occupant;
    if (occupant) occupant->AddRef();
    entry.info.valid = false;
    entry.info.savedCount = 0;
    entry.info.InitQueuePoint(occupant);
    return &entry.info;
}

cSC4AutomataController* cSC4AutomataControllerManager::AddControllerFromTemplateName(cISC4Occupant* occupant, const cIGZString& name, const float* position) {
    tControllerTemplate* tmpl = script_->FindControllerTemplate(name);
    if (!tmpl) return nullptr;
    return AddControllerFromTemplate(occupant, tmpl, position);
}

cSC4AutomataController* cSC4AutomataControllerManager::AddControllerFromTemplate(cISC4Occupant* occupant, tControllerTemplate* tmpl, const float* position) {
    IControllerManagerHost* host = g_host;
    bool create = true;
    if (tmpl) {
        // never two controllers of one template on one lot (unless the template says so, flag 0x40)
        const void* lot = nullptr;
        if (occupant && (tmpl->flags & 0x40) == 0 && (lot = host->LotOf(occupant)) != nullptr) {
            for (auto& node : controllers_) {
                const void* other = node.second.occupant ? host->LotOf(node.second.occupant) : nullptr;
                if (other != lot) continue;
                for (cSC4AutomataController* controller : node.second.controllers) {
                    if (controller->GetTemplate() == tmpl) return nullptr;
                }
            }
        }
        // templates with a uniqueness radius refuse a second controller of the same template in reach
        if (tmpl->uniqueRadius >= 1.0f) {
            float where[3] = {0, 0, 0};
            if (!occupant) {
                if (position) { where[0] = position[0]; where[1] = position[1]; where[2] = position[2]; }
            } else {
                occupant->GetPosition(where);
            }
            TractRect bounds;
            grid_.GetGridBounds(where, tmpl->uniqueRadius, bounds);
            if (bounds.z1 >= bounds.z0) {
                bool found = false;
                for (unsigned z = bounds.z0; z <= bounds.z1; ++z) {
                    for (unsigned x = bounds.x0; x <= bounds.x1; ++x) {
                        const unsigned cx = std::min<unsigned>(x, static_cast<unsigned>(grid_.Width()) - 1);
                        const unsigned cz = std::min<unsigned>(z, static_cast<unsigned>(grid_.Height()) - 1);
                        for (cSC4AutomataController* controller : grid_.Tract(static_cast<int>(cx), static_cast<int>(cz))) {
                            if (controller && controller->GetTemplate() == tmpl) { found = true; break; }
                        }
                    }
                }
                if (found) return nullptr;
            }
        }
    }
    (void)create;
    cSC4AutomataController* created = host->CreateController(tmpl->classKey);   // (a null template crashes the original here)
    if (!created) {
        host->Report("Couldn't create automata controller of class ID 0x" + std::to_string(tmpl->classKey) + "!");
        return nullptr;
    }
    auto found = controllers_.find(KeyOf(occupant));
    if (found == controllers_.end()) {
        ControllerEntry& entry = controllers_[KeyOf(occupant)];
        entry.occupant = occupant;
        if (occupant) occupant->AddRef();
        found = controllers_.find(KeyOf(occupant));
    }
    found->second.controllers.push_back(created);
    if (!occupant) {
        if (position) created->SetPosition(position);
    } else {
        created->SetOccupant(occupant);
    }
    created->SetTemplate(tmpl);
    created->Init();
    grid_.AddController(created);
    return created;
}

bool cSC4AutomataControllerManager::AddControllersFromOccupantGroup(const cIGZString& group, cISC4Occupant* occupant, const float* position) {
    if (!script_) return false;
    IOccupantGroupTemplate* tmpl = script_->FindGroupTemplate(group);
    if (!tmpl) return false;
    for (tControllerTemplate* controller : tmpl->ControllerTemplates()) AddControllerFromTemplate(occupant, controller, position);
    return true;
}

bool cSC4AutomataControllerManager::AddControllersFromOccupantGroup(std::uint32_t group, cISC4Occupant* occupant, const float* position) {
    if (!script_) return false;
    IOccupantGroupTemplate* tmpl = script_->FindGroupTemplate(group);
    if (!tmpl) return false;
    for (tControllerTemplate* controller : tmpl->ControllerTemplates()) AddControllerFromTemplate(occupant, controller, position);
    return true;
}

bool cSC4AutomataControllerManager::AddControllers(cISC4Occupant* occupant) {
    if (!occupant) return true;
    std::vector<std::uint32_t> groups;
    if (cSCPropertyHelp::GetPropertyValue(occupant->AsPropertyHolder(), kPropertyControllerGroups, groups)) {
        for (std::uint32_t id : groups) {
            if (IOccupantGroupTemplate* tmpl = script_->FindGroupTemplate(id)) {
                for (tControllerTemplate* controller : tmpl->ControllerTemplates()) AddControllerFromTemplate(occupant, controller, nullptr);
            }
        }
    }
    for (IOccupantGroupTemplate* tmpl : script_->OccupantGroupTemplates()) {
        if (tmpl->Matches(occupant)) {
            for (tControllerTemplate* controller : tmpl->ControllerTemplates()) AddControllerFromTemplate(occupant, controller, nullptr);
        }
    }
    return true;
}

bool cSC4AutomataControllerManager::RemoveControllersFromOccupantGroup(const cIGZString& group, cISC4Occupant* occupant) {
    if (!script_) return false;
    IOccupantGroupTemplate* tmpl = script_->FindGroupTemplate(group);
    return tmpl ? RemoveControllersFromOccupantGroup(tmpl, occupant) : false;
}

bool cSC4AutomataControllerManager::RemoveControllersFromOccupantGroup(std::uint32_t group, cISC4Occupant* occupant) {
    if (!script_) return false;
    IOccupantGroupTemplate* tmpl = script_->FindGroupTemplate(group);
    return tmpl ? RemoveControllersFromOccupantGroup(tmpl, occupant) : false;
}

bool cSC4AutomataControllerManager::RemoveControllersFromOccupantGroup(IOccupantGroupTemplate* group, cISC4Occupant* occupant) {
    auto entry = controllers_.find(KeyOf(occupant));
    if (entry == controllers_.end()) return false;
    bool removed = false;
    auto& list = entry->second.controllers;
    for (auto it = list.begin(); it != list.end();) {
        bool remove = (group == nullptr);
        if (group) {
            const tControllerTemplate* own = (*it)->GetTemplate();
            const auto& templates = group->ControllerTemplates();
            remove = std::find(templates.begin(), templates.end(), own) != templates.end();
        }
        if (remove) {
            cSC4AutomataController* controller = *it;
            it = list.erase(it);
            SendControllerMessage(kMessageControllerRemoved, controller);
            grid_.RemoveController(controller);
            controller->Shutdown();
            controller->Release();
            removed = true;
        } else {
            ++it;
        }
    }
    if (list.empty()) DropControllerEntry(occupant);
    return removed;
}

bool cSC4AutomataControllerManager::RemoveControllers(cISC4Occupant* occupant) { return RemoveControllersFromOccupantGroup(static_cast<IOccupantGroupTemplate*>(nullptr), occupant); }

bool cSC4AutomataControllerManager::RemoveController(cISC4Occupant* occupant, const cIGZString& name) {
    auto entry = controllers_.find(KeyOf(occupant));
    if (entry == controllers_.end()) return false;
    auto& list = entry->second.controllers;
    for (auto it = list.begin(); it != list.end();) {
        cSC4AutomataController* controller = *it;
        const cIGZString& own = controller->GetTemplateName();
        if (own.Strlen() == name.Strlen() && std::memcmp(own.ToChar(), name.ToChar(), own.Strlen()) == 0) {
            it = list.erase(it);
            SendControllerMessage(kMessageControllerRemoved, controller);
            grid_.RemoveController(controller);
            controller->Shutdown();
            controller->Release();
        } else {
            ++it;
        }
    }
    if (list.empty()) DropControllerEntry(occupant);
    return false;   // quirk: always false
}

bool cSC4AutomataControllerManager::RemoveController(cSC4AutomataController* controller) {
    if (!controller) return false;
    bool removed = false;
    for (auto& node : controllers_) {
        auto& list = node.second.controllers;
        auto it = std::find(list.begin(), list.end(), controller);
        if (it != list.end()) {
            list.erase(it);
            SendControllerMessage(kMessageControllerRemoved, controller);
            grid_.RemoveController(controller);
            controller->Shutdown();
            controller->Release();
            removed = true;
        }
    }
    return removed;
}

bool cSC4AutomataControllerManager::MoveControllers(cISC4Occupant* occupant) {
    auto entry = controllers_.find(KeyOf(occupant));
    if (entry != controllers_.end()) {
        for (cSC4AutomataController* controller : entry->second.controllers) {
            grid_.MoveController(controller);
            controller->HandleViewChanged();
        }
    }
    return true;
}

bool cSC4AutomataControllerManager::MoveController(cSC4AutomataController* controller) {
    if (controller) {
        grid_.MoveController(controller);
        controller->HandleViewChanged();
    }
    return true;
}

// ---- queries ------------------------------------------------------------------------------------------------------------------
cSC4AutomataController* cSC4AutomataControllerManager::GetController(cISC4Occupant* occupant, const cIGZString& name) {
    tControllerTemplate* tmpl = script_->FindControllerTemplate(name);
    return tmpl ? GetController(occupant, static_cast<const tControllerTemplate*>(tmpl)) : nullptr;
}

cSC4AutomataController* cSC4AutomataControllerManager::GetController(cISC4Occupant* occupant, const tControllerTemplate* tmpl) {
    auto entry = controllers_.find(KeyOf(occupant));
    if (!tmpl || entry == controllers_.end()) return nullptr;
    for (cSC4AutomataController* controller : entry->second.controllers) {
        if (controller && controller->GetTemplate() == tmpl) return controller;
    }
    return nullptr;
}

cSC4AutomataController* cSC4AutomataControllerManager::GetController(const cS3DVector3& position, const cIGZString& name, float radius) {
    for (cSC4AutomataController* controller : grid_.GetTractByWorldPos(position.x, position.z)) {
        if (!controller) continue;
        const cIGZString& own = controller->GetTemplateName();
        if (own.Strlen() != name.Strlen() || std::memcmp(own.ToChar(), name.ToChar(), own.Strlen()) != 0) continue;
        float at[3] = {0, 0, 0};
        controller->GetPosition(at);
        const float dx = at[0] - position.x, dy = at[1] - position.y, dz = at[2] - position.z;
        if (std::sqrt(dx * dx + dy * dy + dz * dz) <= radius) return controller;
    }
    return nullptr;
}

bool cSC4AutomataControllerManager::GetRelevantControllersByWorldPos(float x, float z, std::uint32_t controllerType, std::list<cSC4AutomataController*>& out) {
    if (enabled_) {
        const float where[3] = {x, 0.0f, z};
        for (cSC4AutomataController* controller : grid_.GetTractByWorldPos(x, z)) {
            if (controller->IsActive() && controller->GetControllerType() == controllerType && controller->IsPointWithinRadius(where, nullptr)) {
                controller->AddRef();
                out.push_back(controller);
            }
        }
    }
    return !out.empty();
}

void cSC4AutomataControllerManager::ReleaseControllerList(std::list<cSC4AutomataController*>& list) {
    for (cSC4AutomataController* controller : list) controller->Release();
    list.clear();
}

cSC4AutomataController* cSC4AutomataControllerManager::GetCompositeAttractionEffect(cISC4Occupant* occupant, cS3DVector3& direction, float& distance, float& strength) {
    cSC4AutomataController* best = nullptr;
    if (!occupant || !enabled_) return nullptr;
    float where[3] = {0, 0, 0};
    occupant->GetPosition(where);
    direction = cS3DVector3{0.0f, 0.0f, 0.0f};
    float bestMagnitude = 0.0f;
    for (cSC4AutomataController* controller : grid_.GetTractByWorldPos(where[0], where[2])) {
        if (!controller->IsActive() || controller->IsFull() || controller->GetControllerType() != kControllerTypeAttractor) continue;
        float reach = 0.0f;
        if (!controller->IsPointWithinRadius(where, &reach)) continue;
        auto* attractor = static_cast<cSC4AutomataAttractor*>(controller);
        if (!attractor->TestAutomataFilter(occupant)) continue;
        const float pull = attractor->GetCurrentStrength(now_, reach);
        float centre[3] = {0, 0, 0};
        if (!controller->CheckFlag(kCtrlFlagLotFacing)) controller->GetPosition(centre); else controller->GetLotFacingPoint(centre);
        const float dx = centre[0] - where[0];
        const float dz = centre[2] - where[2];
        const float inverse = 1.0f / std::sqrt(dz * dz + dx * dx + 0.0f);
        direction.x += dx * inverse * pull;
        direction.y += inverse * 0.0f * pull;
        direction.z += dz * inverse * pull;
        const float magnitude = pull < 0.0f ? -pull : pull;
        if (bestMagnitude < magnitude) {
            strength = pull;
            distance = reach;
            best = controller;
            bestMagnitude = magnitude;
        }
    }
    return best;
}

float cSC4AutomataControllerManager::GetCompositeAttractionCommotion(INetworkOccupantView* networkOccupant) {
    float total = 0.0f;
    if (!networkOccupant || !enabled_) return total;
    float where[3] = {0, 0, 0};
    networkOccupant->GetWorldPosition(where);
    for (cSC4AutomataController* controller : grid_.GetTractByWorldPos(where[0], where[2])) {
        if (!controller->IsActive() || controller->GetControllerType() != kControllerTypeAttractor) continue;
        float reach = 0.0f;
        if (!controller->IsPointWithinRadius(where, &reach)) continue;
        const float pull = static_cast<cSC4AutomataAttractor*>(controller)->GetCurrentStrength(now_, reach);
        total += pull < 0.0f ? -pull : pull;
    }
    return total;
}

// ---- messages -----------------------------------------------------------------------------------------------------------------
bool cSC4AutomataControllerManager::DoMessage(cGZMessage& message) {
    if (message.type != kMessageCheatCode || message.param1 != kAutomataCheatControllers) return false;
    const char* text = reinterpret_cast<cIGZString*>(message.param2)->ToChar();
    cRZCmdLine line(text);
    if (line.argc() > 1) {
        const cRZString& argument = line[1];
        if (argument.Strlen() == 3 && std::strcmp(argument.ToChar(), "off") == 0) {
            enabled_ = false;
        } else if (line[1].Strlen() == 2 && std::strcmp(line[1].ToChar(), "on") == 0) {
            enabled_ = true;
        }
    }
    return true;
}

bool cSC4AutomataControllerManager::DoMessage(cIGZMessage2* base) {
    IControllerManagerHost* host = g_host;
    auto* message = static_cast<cIGZMessage2Standard*>(base);   // payload accessors live on the standard message
    switch (message->GetType()) {
        case kCtrlMsgTick:
            // data3 is the elapsed milliseconds as float bits (cSC4AnimationTickManager)
            DoAnimationTick(static_cast<std::uint32_t>(message->GetData2()), std::bit_cast<float>(static_cast<std::uint32_t>(message->GetData3())));
            return true;
        case kCtrlMsgQueueReset:
            if (message->GetIGZUnknown() == nullptr || message->GetData1() == 0) return true;
            for (auto& node : queues_) node.second.info.valid = false;
            return true;
        case kCtrlMsgOccupantInserted: {
            auto* occupant = static_cast<cISC4Occupant*>(message->GetVoid1());
            if (occupant->AsPropertyHolder()->HasProperty(kPropertyNoControllers)) return true;
            AddControllers(occupant);
            return true;
        }
        case kCtrlMsgOccupantRemoved: {
            auto* occupant = static_cast<cISC4Occupant*>(message->GetVoid1());
            void* probe = nullptr;
            if (occupant->QueryInterface(kOccupantIidQueueReset, &probe)) {
                for (auto& node : queues_) node.second.info.valid = false;
                static_cast<cIGZUnknown*>(probe)->Release();
            }
            if (occupant->AsPropertyHolder()->HasProperty(kPropertyNoControllers)) return true;
            RemoveControllers(occupant);
            return true;
        }
        case kCtrlMsgCameraMoved:
            break;   // (falls through to the view update)
        case kCtrlMsgCameraChanged: {
            if (scrolling_) return true;
            const float* view = host->ViewPosition();
            const float limit = kScrollGranularitySquared[std::clamp(host->ViewZoom(), 0, 7)];
            if (SC4GeometryUtilities::LengthOfLineSquared(viewPosition_, view) <= limit) return true;
            viewPosition_[0] = view[0];
            viewPosition_[1] = view[1];
            viewPosition_[2] = view[2];
            break;
        }
        case kCtrlMsgSourceDataChanged:
            for (auto& node : controllers_) for (cSC4AutomataController* controller : node.second.controllers) controller->NotifySourceDataChanged();
            return true;
        case kCtrlMsgEffectTriggers:
            for (auto& node : controllers_) for (cSC4AutomataController* controller : node.second.controllers) controller->CheckEffectTriggers();
            return true;
        case kCtrlMsgHourPassed:
            now_.time += 1.0f;
            for (auto& node : controllers_) {
                for (cSC4AutomataController* controller : node.second.controllers) {
                    const ControllerTime delta = ElapsedSinceLastTick(now_);
                    controller->CheckTimeOfDay(now_, delta);
                }
            }
            return true;
        case kCtrlMsgPostCityInit:
            HandlePostCityInit();
            return true;
        case kCtrlMsgAutomatonRemoved: {
            if (message->GetData2() != 3) return true;
            IControllerAutomaton* automaton = message->GetVoid1() ? AutomataControllerHost()->AutomatonLink(message->GetVoid1()) : nullptr;
            if (!automaton) return true;
            for (auto& node : controllers_) for (cSC4AutomataController* controller : node.second.controllers) controller->HandleRemoveAutomaton(automaton);
            return true;
        }
        case kCtrlMsgOccupantGroupAdded: {
            const std::uint32_t group = static_cast<std::uint32_t>(message->GetData4());
            auto* occupant = static_cast<cISC4Occupant*>(message->GetVoid1());
            if (occupant->AsPropertyHolder()->HasProperty(kPropertyNoControllers)) return true;
            IOccupantGroupTemplate* tmpl = script_->FindGroupTemplate(group);
            if (!tmpl) return true;
            if (static_cast<double>(tmpl->SpawnChance()) < host->RandomDouble()) return true;
            for (tControllerTemplate* controller : tmpl->ControllerTemplates()) AddControllerFromTemplate(occupant, controller, nullptr);
            return true;
        }
        case kCtrlMsgScrollStart:
            scrolling_ = true;
            return true;
        case kCtrlMsgScrollEnd:
            scrolling_ = false;
            break;
        default:
            return true;
    }
    HandleViewChanged();
    return true;
}

// ---- streams ------------------------------------------------------------------------------------------------------------------
bool cSC4AutomataControllerManager::Write(cIGZOStream& stream) const {
    stream.SetUint16(kManagerStreamVersion);
    SC4WriteGZSerializable(stream, clock_);
    stream.SetFloat32(now_.time);
    stream.SetSint16(now_.days);
    stream.SetFloat32(last_.time);
    stream.SetSint16(last_.days);
    std::uint32_t occupants = 0;
    for (const auto& node : controllers_) {
        for (cSC4AutomataController* controller : node.second.controllers) {
            if (!controller->CheckFlag(kControllerFlagTransient)) { ++occupants; break; }
        }
    }
    stream.SetUint32(occupants);
    for (const auto& node : controllers_) {
        std::uint32_t saved = 0;
        for (cSC4AutomataController* controller : node.second.controllers) saved += controller->CheckFlag(kControllerFlagTransient) ? 0 : 1;
        if (saved == 0) continue;
        SC4WriteGZSerializable(stream, node.second.occupant);
        stream.SetUint32(saved);
        for (cSC4AutomataController* controller : node.second.controllers) {
            if (!controller->CheckFlag(kControllerFlagTransient)) SC4WriteGZSerializable(stream, controller);
        }
    }
    stream.SetUint32(static_cast<std::uint32_t>(queues_.size()));
    for (const auto& node : queues_) {
        SC4WriteGZSerializable(stream, node.second.occupant);
        stream.SetUint32(static_cast<std::uint32_t>(node.second.info.queue.size()));
        for (IControllerAutomaton* automaton : node.second.info.queue) SC4WriteGZSerializable(stream, AutomataControllerHost()->AutomatonObject(automaton));
        stream << cS3DVector3{node.second.info.point[0], node.second.info.point[1], node.second.info.point[2]};
        stream << cS3DVector3{node.second.info.direction[0], node.second.info.direction[1], node.second.info.direction[2]};
        stream << node.second.info.valid;
    }
    return !stream.GetError();
}

bool cSC4AutomataControllerManager::Read(cIGZIStream& stream) {
    postCityInitPending_ = true;
    std::uint16_t version = 0;
    stream.GetUint16(version);
    void* read = nullptr;
    SC4ReadGZSerializable(stream, kClockIid, &read, false);
    clock_ = static_cast<IControllerClock*>(read);   // the previous clock reference is not released (kept)
    stream.GetFloat32(now_.time);
    stream.GetSint16(now_.days);
    stream.GetFloat32(last_.time);
    stream.GetSint16(last_.days);
    std::uint32_t count = 0;
    stream.GetUint32(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        void* occupantRaw = nullptr;
        SC4ReadGZSerializable(stream, kOccupantIid, &occupantRaw, false);
        auto* occupant = static_cast<cISC4Occupant*>(occupantRaw);
        const bool isNew = controllers_.find(KeyOf(occupant)) == controllers_.end();
        ControllerEntry& entry = controllers_[KeyOf(occupant)];
        if (isNew) entry.occupant = occupant;
        std::uint32_t controllerCount = 0;
        stream.GetUint32(controllerCount);
        for (std::uint32_t j = 0; j < controllerCount; ++j) {
            void* controller = nullptr;
            SC4ReadGZSerializable(stream, kGZIID_cISC4AutomataController, &controller, false);
            // Divergence: the original can create every saved controller class; the port's controller creators are not wired yet
            // (CTRL-1), so the read can answer null, and a null in the list crashed every later walk (view change, automaton removal).
            if (isNew && controller) entry.controllers.push_back(static_cast<cSC4AutomataController*>(controller));   // an existing entry drops them (kept)
        }
    }
    stream.GetUint32(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        void* occupantRaw = nullptr;
        SC4ReadGZSerializable(stream, kOccupantIid, &occupantRaw, false);
        auto* occupant = static_cast<cISC4Occupant*>(occupantRaw);
        const bool isNew = queues_.find(KeyOf(occupant)) == queues_.end();
        QueueEntry& entry = queues_[KeyOf(occupant)];
        if (isNew) entry.occupant = occupant;
        std::uint32_t queued = 0;
        stream.GetUint32(queued);
        if (isNew) entry.info.savedCount = static_cast<std::int32_t>(queued);
        for (std::uint32_t j = 0; j < queued; ++j) {
            void* automaton = nullptr;
            SC4ReadGZSerializable(stream, kAutomatonIid, &automaton, false);
            if (isNew && automaton) entry.info.queue.push_back(AutomataControllerHost()->AutomatonLink(automaton));
        }
        cS3DVector3 vector;
        stream >> vector;
        if (isNew) {
            entry.info.point[0] = vector.x; entry.info.point[1] = vector.y; entry.info.point[2] = vector.z;
            stream >> vector;
            entry.info.direction[0] = vector.x; entry.info.direction[1] = vector.y; entry.info.direction[2] = vector.z;
            bool valid = false;
            stream >> valid;
            entry.info.valid = valid;
        } else {
            stream >> vector;
            bool ignored = false;
            stream >> ignored;
        }
    }
    return !stream.GetError();
}

}  // namespace sc4
