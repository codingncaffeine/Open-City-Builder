#include "app/NativeNetworkManager.h"

#include <cstdint>

#include "app/NativeDemolition.h"
#include "gz/cGZPersistResourceManager.h"
#include "sc4/ServiceLocator.h"
#include "sc4/NetworkManager.h"
#include "sc4/NetworkOccupant.h"
#include "sc4/NetworkTool.h"
#include "sc4/PathInfoFactory.h"

namespace native {
namespace {

// The network manager's view of a network piece (INetworkManagerOccupant): the port's pieces do not answer
// cISC4NetworkOccupant, so the manager gets this through SetNetworkOccupantViewProvider. One reference per view.
class ManagerOccupantView final : public sc4::INetworkManagerOccupant {
public:
    explicit ManagerOccupantView(sc4::cSC4NetworkOccupant* occupant) : occupant_(occupant) { occupant_->AddRef(); }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (--refs_ != 0) return refs_;
        occupant_->Release();
        delete this;
        return 0;
    }
    bool IsManagedGlobally() const override { return occupant_->IsManagedGlobally(); }
    bool HasNetworkFlag(std::uint32_t flag) override { return occupant_->HasNetworkFlag(flag); }
    std::uint32_t GetNetworkFlags() override { return occupant_->GetNetworkFlag(); }
    void SetLit(bool lit) override { occupant_->SetLit(lit); }
    bool SupportsLighting() override { return occupant_->CanBeLit(); }
    void UpdateViewRotation() override {}
    sc4::cISC4Occupant* AsOccupant() override { return occupant_; }
    sc4::cSC4NetworkOccupant* Network() const { return occupant_; }

private:
    sc4::cSC4NetworkOccupant* occupant_;
    std::uint32_t refs_ = 0;
};
sc4::INetworkManagerOccupant* NetworkOccupantView(sc4::cISC4Occupant* occupant) {
    auto* network = dynamic_cast<sc4::cSC4NetworkOccupant*>(occupant);
    if (!network) return nullptr;
    auto* view = new ManagerOccupantView(network);
    view->AddRef();
    return view;
}

// A road-class network tool for the network manager (Init made none: the city loads before these hosts exist). The
// tool itself is made on first use, when the view's network tool host is installed.
class ManagerRoadTool final : public sc4::INetworkManagerTool {
public:
    explicit ManagerRoadTool(std::uint32_t type) : type_(type) {}
    ~ManagerRoadTool() { if (tool_) tool_->Release(); }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (--refs_ != 0) return refs_;
        delete this;
        return 0;
    }
    bool Init() override {
        if (!tool_) {
            tool_ = new sc4::cSC4NetworkTool(type_);
            tool_->AddRef();
        }
        return tool_->Init();
    }
    bool Shutdown() override { return tool_ ? tool_->Shutdown() : true; }
    // The manager hands one reference per view; the tool borrows the pieces.
    void DestroyOccupantsAndResolve(std::vector<sc4::INetworkManagerOccupant*>& occupants) override {
        std::vector<sc4::cSC4NetworkOccupant*> pieces;
        for (sc4::INetworkManagerOccupant* view : occupants)
            if (auto* network = view ? dynamic_cast<sc4::cSC4NetworkOccupant*>(view->AsOccupant()) : nullptr) pieces.push_back(network);
        if (tool_) tool_->DestroyOccupantsAndResolve(pieces);
        for (sc4::INetworkManagerOccupant* view : occupants)
            if (view) view->Release();
    }

private:
    std::uint32_t type_;
    sc4::cSC4NetworkTool* tool_ = nullptr;
    std::uint32_t refs_ = 0;
};

// GetPathInfo's two services: the manager's (UK) path id mapping and the path files (type 0x296678f7) from the resource manager,
// whose factory reads the TEXT path format.
class PathHost final : public sc4::INetworkOccupantPathHost {
public:
    std::uint32_t MapPath(std::uint32_t pathId) override {
        sc4::cSC4NetworkManager* manager = CityNetworkManager();
        return manager ? manager->MapPath(pathId) : pathId;
    }
    sc4::cISC4PathInfo* LoadPathInfo(const cGZPersistResourceKey& key) override {
        void* out = nullptr;
        if (!SL::spRM || !SL::spRM->GetResource(key, 0xa9667793u, &out, 0, 0)) return nullptr;
        return static_cast<sc4::cISC4PathInfo*>(out);
    }
} g_pathHost;

void RegisterPathInfoFactory() {
    static bool registered = false;
    if (registered || !SL::spRM) return;
    registered = true;
    static sc4::cSC4PathInfoFactory factory;
    factory.AddRef();
    SL::spRM->RegisterObjectFactory(0x496678feu, sc4::kPathInfoResourceType, &factory);
    sc4::SetNetworkOccupantPathHost(&g_pathHost);
}

}  // namespace

void InstallNetworkManagerTools() {
    RegisterPathInfoFactory();
    // cSC4NetworkTool for types 0-3, 6, 8-12; the pipe, power line and subway tools are not recovered.
    sc4::SetNetworkOccupantViewProvider(&NetworkOccupantView);
    sc4::cSC4NetworkManager* manager = CityNetworkManager();
    if (!manager) return;
    for (std::uint32_t type : {0u, 1u, 2u, 3u, 6u, 8u, 9u, 10u, 11u, 12u}) {
        auto* tool = new ManagerRoadTool(type);
        tool->AddRef();
        manager->AdoptNetworkTool(static_cast<sc4::eNetworkType>(type), tool);
        tool->Release();
    }
}

void NetworkIntersectionPieces(sc4::cSC4NetworkOccupant* occupant, std::vector<sc4::cSC4NetworkOccupant*>& out) {
    sc4::cSC4NetworkManager* manager = CityNetworkManager();
    sc4::INetworkManagerOccupant* view = occupant ? NetworkOccupantView(occupant) : nullptr;
    if (!manager || !view) {
        if (view) view->Release();
        return;
    }
    sc4::ilist<sc4::cISC4Occupant*> pieces;
    manager->GetAllIntersectionPieces(view, pieces, nullptr, nullptr);
    view->Release();
    for (sc4::cISC4Occupant* piece : pieces) {
        if (auto* network = dynamic_cast<sc4::cSC4NetworkOccupant*>(piece)) out.push_back(network);   // keeps the list's reference
        else if (piece) piece->Release();
    }
}

}  // namespace native
