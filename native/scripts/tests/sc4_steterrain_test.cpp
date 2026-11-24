// cSTETerrain parts 2, 3 and 4: COM identity, the message posts, the display/accumulation state, the vertex
// editing helpers, the mark/rect/span helpers, the temporary water-level copies, the brush tuning
// queries, the lighting row walk, the edge-altitude subfiles and the stream Read/Write wrapper; then the
// first-boot path (Init/Shutdown/StaticInit/StaticShutdown/DoPostCityInit/DoMessage/DoAnimationTick), the
// redisplay + altitude range, the water-proximity/land-type surface helpers, the shoreline helpers and the
// effect-id property; finally the exemplar/texture pipeline (ReadTuningParameters/ReadTextureInfos/
// ReadTextureInfo/ReadTerrainIniFile/ReadMiscTexturesForTerrainTypes/DetermineTextureFamilyMembersForGridCells),
// DecideSurfaceCover and the two-triangle normal/cliff pass (CalculateNormalsAndAssignCliffTextures/MarkCliff);
// and finally part 5 -- the shore-cliff cluster (DetectShoreVertices/GetShoreStartingAtVert/TrackShore*/
// AnalyzeShoreAndCreateCliffs/CreateShoreCliff/CreateShoreCliffs), the marked-region helpers
// (MarkRegion/SmoothenMarkedRegion) and the erosion/flood kernels (DoErosion/DoWindErosion/DoWaterErosion/
// StartFlood/DoFloodFlowStep).
// The engine services (message server, lighting manager, view, city, surface water, simulator, command
// registry, flood fill, texture resources, terrain ini) are fakes here; the scripting payloads use the small
// parameter-set / property-holder fakes below so the test keeps the registered link set.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <array>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "gz/CommandServer.h"
#include "gz/cIGZMessage2.h"
#include "gz/cIGZPersistDBSegment.h"
#include "rz/cRZMessage2Standard.h"
#include "rz/cRZVariant.h"
#include "rz/cRZString.h"
#include "sc4/DBSegment.h"
#include "sc4/SCProperty.h"
#include "sc4/ISC4RegionalCity.h"
#include "sc4/Region.h"
#include "sc4/cSTETerrain.h"

using namespace sc4;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
static bool Near(float a, float b, float e = 1e-4f) { return std::fabs(a - b) <= e; }

namespace {

// ---------------------------------------------------------------------- fake message server
struct PostedMessage {
    std::uint32_t type = 0;
    std::uint64_t data[4] = {0, 0, 0, 0};
};

class FakeMessageServer final : public cIGZMessageServer2 {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override { return --refs_; }
    bool MessageSend(cIGZMessage2* message) override { Record(message); return true; }
    bool MessagePost(cIGZMessage2* message, bool) override { Record(message); return true; }
    bool AddNotification(cIGZMessageTarget2*, std::uint32_t) override { return true; }
    bool RemoveNotification(cIGZMessageTarget2*, std::uint32_t) override { return true; }
    bool GeneralMessagePostToTarget(cIGZMessage2* message, cIGZMessageTarget2* target) override {
        Record(message);
        lastTarget = target;
        return true;
    }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return true; }
    bool OnTick(std::uint32_t) override { return true; }
    std::uint32_t GetMessageQueueSize() override { return static_cast<std::uint32_t>(posts.size()); }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return refs_; }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }

    void Record(cIGZMessage2* message) {
        PostedMessage post;
        post.type = message->GetType();
        void* raw = nullptr;
        if (message->QueryInterface(kGZIID_cIGZMessage2Standard, &raw) && raw) {
            auto* standard = static_cast<cIGZMessage2Standard*>(raw);
            post.data[0] = standard->GetData1();
            post.data[1] = standard->GetData2();
            post.data[2] = standard->GetData3();
            post.data[3] = standard->GetData4();
            standard->Release();
        }
        posts.push_back(post);
    }

    std::vector<PostedMessage> posts;
    cIGZMessageTarget2* lastTarget = nullptr;
    std::uint32_t refs_ = 0;
};

// ------------------------------------------------------------------ fake engine services
class FakeService final : public cIGZUnknown {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    int refs = 0;
};

// The command registry: records the seven terrain script commands.
class FakeCommandServer final : public cIGZCommandServer {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 0; }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    void CreateCommandParameterSet(cIGZCommandParameterSet**) override {}
    bool RegisterCommand(std::uint32_t id, const char* name, const char* help) override {
        registered.emplace_back(id, name ? name : "", help ? help : "");
        return true;
    }
    bool RegisterCommands(const cGZCommandInfoEntry*, std::uint32_t) override { return true; }
    bool RegisterCommandParameterInfo(std::uint32_t, const char*) override { return true; }
    bool UnregisterCommand(std::uint32_t id) override { unregistered.push_back(id); return true; }
    bool UnregisterCommands(const cGZCommandInfoEntry*, std::uint32_t) override { return true; }
    bool GetCommandInfo(std::uint32_t, cIGZString&, cIGZString*) const override { return false; }
    std::uint32_t GetCommandID(const cIGZString&) const override { return 0; }
    std::uint32_t GetCommandIDFromCommandString(const char*, std::uint32_t) const override { return 0; }
    void EnumerateCommands(std::vector<std::uint32_t>&) const override {}
    bool RegisterCommandGenerator(cIGZCommandGenerator*) override { return true; }
    bool UnregisterCommandGenerator(cIGZCommandGenerator*) override { return true; }
    cIGZCommandDispatcher* GetCommandDispatcher() const override { return nullptr; }
    bool SetCommandDispatcher(cIGZCommandDispatcher*) override { return true; }
    std::uint32_t ExecuteCommand(std::uint32_t, cIGZCommandParameterSet*, cIGZCommandParameterSet*) override { return 0; }
    std::uint32_t ExecuteCommand(const char*, std::uint32_t, cIGZCommandParameterSet*) override { return 0; }
    std::uint32_t ExecuteCommandFormatted(const char*, cIGZCommandParameterSet*, ...) override { return 0; }
    bool ConvertStringToCommand(const char*, std::uint32_t, std::uint32_t&, cIGZCommandParameterSet*&) override { return false; }
    bool ConvertCommandToString(std::uint32_t, cIGZCommandParameterSet*, cIGZString&) override { return false; }
    bool ConvertCommandOutputToString(cIGZCommandParameterSet*, cIGZString&) override { return false; }

    struct Command {
        std::uint32_t id;
        std::string name, help;
    };
    std::vector<Command> registered;
    std::vector<std::uint32_t> unregistered;
};

class FakeView final : public ITerrainView {
public:
    cIGZSerializable* Serializable() override { return nullptr; }
    bool Init(cSTETerrain* terrain) override {
        ++inits;
        initTerrain = terrain;
        return true;
    }
    void Shutdown() override { ++shutdowns; }
    void MarkTextureSpanInfoDirty(std::int32_t firstRow, std::int32_t lastRow) override {
        dirtyRows.emplace_back(firstRow, lastRow);
    }
    void SetLightingUpdateRect(const TerrainRect* r) override {
        ++lightingRectUpdates;
        if (r) lastLightingRect = *r;
    }
    void ChangeTerrainType(std::int32_t type) override { changedTypes.push_back(type); }
    void SetUpTexturesForCurrentTerrainTypeAndZoom(std::int32_t zoom) override { textureZooms.push_back(zoom); }
    float MaxDisplayAltitude() const override { return maxAltitude; }
    float MinDisplayAltitude() const override { return minAltitude; }
    cIGZUnknown* OverlayManager(std::uint32_t which) override {
        overlayRequests.push_back(which);
        return which < 3 ? &overlays[which] : nullptr;
    }
    ITerrainWaveSystem* WaveSystem() override { return waves; }
    void HighlightCells(const std::vector<TerrainPointI>& highlighted, bool a, bool b) override {
        highlightedCells = highlighted;
        highlightArgs = {a, b};
    }
    void ClearCurrentSelections() override { ++selectionClears; }

    int inits = 0, shutdowns = 0, lightingRectUpdates = 0;
    TerrainRect lastLightingRect;
    cSTETerrain* initTerrain = nullptr;
    float minAltitude = 0.0f, maxAltitude = 50.0f;
    std::vector<std::pair<std::int32_t, std::int32_t>> dirtyRows;
    std::vector<std::int32_t> changedTypes, textureZooms, overlayRequests;
    std::vector<TerrainPointI> highlightedCells;
    std::pair<bool, bool> highlightArgs{false, false};
    int selectionClears = 0;
    ITerrainWaveSystem* waves = nullptr;
    FakeService overlays[3];
};

// The scripting payload of a command message: a sparse set of cRZVariant values (the real class lives in
// src/gz/CommandParameterSet.cpp, which is kept out of this test's link set).
class FakeParameterSet final : public cIGZCommandParameterSet {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 0; }
    std::uint32_t GetParameterCount() const override { return static_cast<std::uint32_t>(values.size()); }
    void SetParameterCount(std::uint32_t) override {}
    cIGZVariant& operator[](std::uint32_t id) override { return values[id]; }
    cIGZVariant* GetParameter(std::uint32_t id) override {
        auto it = values.find(id);
        return it == values.end() ? nullptr : &it->second;
    }
    void SetParameter(std::uint32_t id, const cIGZVariant& value) override { values[id].CopyFrom(value); }
    void AppendParameter(const cIGZVariant&) override {}
    void RemoveParameter(std::uint32_t id) override { values.erase(id); }
    void SetStatusParameterValue(std::int32_t value) override {
        status = value;
        values[kGZStatusParameterID].SetValSint32(value);
    }
    bool CompareParameterString(std::uint32_t, const char*, bool) override { return false; }
    bool ValidateParameter(std::uint32_t, std::uint32_t) override { return true; }
    std::uint32_t GetFirstParameterID() const override { return 0; }
    std::uint32_t GetNextParameterID(std::uint32_t) const override { return 0; }

    std::map<std::uint32_t, cRZVariant> values;
    std::int32_t status = 0;
};

// A property holder with one two-dword array property (GetEffectIDProperty's input).
class FakeProperty final : public sc4::cISCProperty {
public:
    explicit FakeProperty(const cRZVariant& value) { value_.CopyFrom(value); }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override { return --refs_; }
    std::uint32_t GetPropertyID() const override { return 0; }
    void SetPropertyID(std::uint32_t) override {}
    const cIGZVariant* GetPropertyValue() const override { return &value_; }
    cIGZVariant* GetPropertyValue() override { return &value_; }
    bool SetPropertyValue(const cIGZVariant& value) override { return value_.CopyFrom(value); }
    bool Write(cIGZOStream&) const override { return false; }
    bool Read(cIGZIStream&) override { return false; }

private:
    int refs_ = 0;
    cRZVariant value_;
};

class FakePropertyHolder final : public sc4::cISCPropertyHolder {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 0; }
    bool HasProperty(std::uint32_t id) const override { return properties.count(id) != 0; }
    bool GetPropertyList(cIGZUnknownList**) const override { return false; }
    sc4::cISCProperty* GetProperty(std::uint32_t id) const override {
        auto it = properties.find(id);
        return it == properties.end() ? nullptr : it->second;
    }
    bool GetProperty(std::uint32_t, std::uint32_t&) const override { return false; }
    bool GetProperty(std::uint32_t, cIGZString&) const override { return false; }
    bool GetProperty(std::uint32_t, std::uint32_t, void**) const override { return false; }
    bool GetProperty(std::uint32_t, void*, std::uint32_t&) const override { return false; }
    void AddProperty(sc4::cISCProperty*, bool) override {}
    void AddProperty(std::uint32_t, const cIGZVariant*, bool) override {}
    void AddProperty(std::uint32_t, std::uint32_t, bool) override {}
    void AddProperty(std::uint32_t, const cIGZString&, bool) override {}
    void AddProperty(std::uint32_t, cIGZUnknown*, bool) override {}
    void AddProperty(std::uint32_t, const void*, std::uint32_t, bool) override {}
    void CopyAddProperty(sc4::cISCProperty*, bool) override {}
    bool RemoveProperty(std::uint32_t, bool) override { return false; }
    void RemoveAllProperties(bool) override {}
    void EnumProperties(void (*)(sc4::cISCProperty*, void*), void*) const override {}
    void EnumProperties(bool (*)(sc4::cISCProperty*, void*), void (*)(sc4::cISCProperty*, void*), void*) const override {}
    void CompactProperties() override {}

    std::map<std::uint32_t, sc4::cISCProperty*> properties;
};

// A property holder builder for the exemplar / texture-info fixtures (owns its properties).
class PropertyBag {
public:
    explicit PropertyBag(FakePropertyHolder& holder) : holder_(holder) {}
    ~PropertyBag() {
        for (FakeProperty* property : properties_) delete property;
    }
    PropertyBag(const PropertyBag&) = delete;
    PropertyBag& operator=(const PropertyBag&) = delete;

    void Float(std::uint32_t id, float value) {
        cRZVariant variant;
        variant.SetValFloat32(value);
        Add(id, variant);
    }
    void Uint(std::uint32_t id, std::uint32_t value) {
        cRZVariant variant;
        variant.SetValUint32(value);
        Add(id, variant);
    }
    void Uint8(std::uint32_t id, std::uint8_t value) {
        cRZVariant variant;
        variant.SetValUint8(value);
        Add(id, variant);
    }
    void Floats(std::uint32_t id, std::initializer_list<float> values) {
        std::vector<float> data(values);
        cRZVariant variant;
        variant.RefFloat32(data.data(), static_cast<std::uint32_t>(data.size()));
        Add(id, variant);
    }
    void Uints(std::uint32_t id, std::initializer_list<std::uint32_t> values) {
        std::vector<std::uint32_t> data(values);
        cRZVariant variant;
        variant.RefUint32(data.data(), static_cast<std::uint32_t>(data.size()));
        Add(id, variant);
    }

private:
    void Add(std::uint32_t id, const cRZVariant& value) {
        auto* property = new FakeProperty(value);
        properties_.push_back(property);
        holder_.properties[id] = property;
    }
    FakePropertyHolder& holder_;
    std::vector<FakeProperty*> properties_;
};

// The ini reader the terrain reads: sections -> keys -> values, in insertion order (std::map sorts the keys,
// which is all the temperature-table enumeration needs).
class FakeIni final : public ITerrainIniFile {
public:
    bool ReadEntry(const cRZString& section, const cRZString& key, cRZString& value) override {
        auto sectionIt = sections.find(section.ToChar());
        if (sectionIt == sections.end()) return false;
        auto entry = sectionIt->second.find(key.ToChar());
        if (entry == sectionIt->second.end()) return false;
        value = cRZString(entry->second.c_str());
        return true;
    }
    bool EnumEntries(const cRZString& section, EnumCallback callback, void* context) override {
        auto sectionIt = sections.find(section.ToChar());
        if (sectionIt == sections.end()) return false;
        for (const auto& entry : sectionIt->second) {
            cRZString key(entry.first.c_str());
            cRZString value(entry.second.c_str());
            callback(key, value, context);
        }
        return true;
    }
    std::map<std::string, std::map<std::string, std::string>> sections;
};


// ------------------------------------------------------------------ the brush / wave / create fakes
class FakeWaveSystem final : public ITerrainWaveSystem {
public:
    bool GetMeanWaveOrientation(float& orientation) override {
        orientation = meanOrientation;
        return hasMean;
    }
    bool AddProducer(float x, float z, float orientation) override {
        producers.push_back({x, z, orientation});
        return addProducerSucceeds;
    }
    std::int32_t ProducerCount() const override { return static_cast<std::int32_t>(producers.size()); }
    std::int32_t MaxProducerCount() const override { return maxProducers; }
    float WaveSeaTravelDistance() const override { return seaTravelDistance; }
    float MaxWaveWidth() const override { return maxWaveWidth; }
    float MaxWaveTravelDistance() const override { return maxWaveTravelDistance; }
    float DistanceFromShoreToWaveStart() const override { return distanceFromShore; }
    float MaxOrientationDifference() const override { return maxOrientationDifference; }
    float InterWaveGap() const override { return interWaveGap; }

    struct Producer { float x, z, orientation; };
    std::vector<Producer> producers;
    float meanOrientation = 0.0f, seaTravelDistance = 200.0f, maxWaveWidth = 340.0f;
    float maxWaveTravelDistance = 900.0f, distanceFromShore = 320.0f;
    float maxOrientationDifference = 0.17f, interWaveGap = 64.0f;
    std::int32_t maxProducers = 100;
    bool hasMean = false, addProducerSucceeds = true;
};

class FakeHeightImage final : public ITerrainHeightImage {
public:
    std::uint32_t Width() const override { return width; }
    std::uint32_t Height() const override { return height; }
    std::uint8_t Gray(std::uint32_t x, std::uint32_t y) const override {
        if (x >= width || y >= height) return 0;
        return pixels[y * width + x];
    }
    void Release() override { ++releases; }
    std::vector<std::uint8_t> pixels;
    std::uint32_t width = 0, height = 0;
    int releases = 0;
};

class FakeTerrainEffect final : public ITerrainEffect {
public:
    explicit FakeTerrainEffect(std::uint32_t a, std::uint32_t b) : id0(a), id1(b) {}
    void SetPlacement(const TerrainEffectPlacement& p) override { placement = p; ++placements; }
    void Start() override { ++starts; }
    void Release() override { ++releases; }
    std::uint32_t id0 = 0, id1 = 0;
    TerrainEffectPlacement placement;
    int placements = 0, starts = 0, releases = 0;
};

// cISC4Region: only the four queries the edge reconcile makes are interesting.
class FakeRegion final : public cISC4Region {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 0; }
    cIGZString* GetName() override { return nullptr; }
    void SetName(const cIGZString&) override {}
    cIGZString* GetDirectoryName() override { return nullptr; }
    void SetDirectoryName(const cIGZString&) override {}
    void LoadConfig() override {}
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    void Delete() override {}
    cRZAutoRefCount<cISC4RegionalCity>* GetCity(int x, int z) override {
        lookups.emplace_back(x, z);
        auto it = cities.find({x, z});
        if (it == cities.end()) return nullptr;
        slot = cRZAutoRefCount<cISC4RegionalCity>(it->second);
        return &slot;
    }
    void InsertCity(cISC4RegionalCity*) override {}
    void RemoveCity(cRZAutoRefCount<cISC4RegionalCity>*&) override {}
    void DeleteCity(cRZAutoRefCount<cISC4RegionalCity>*&) override {}
    void ReloadCity(cRZAutoRefCount<cISC4RegionalCity>*&) override {}
    bool MoveCity(cISC4Region*, cISC4RegionalCity*, int, int) override { return false; }
    void GetAllCities(RegionCityList&) const override {}
    eBaseTerrainType GetBaseTerrainType() override { return kBaseTerrainType0; }
    void SetBaseTerrainType(eBaseTerrainType) override {}
    std::uint32_t GetBaseTerrainHeight() override { return baseTerrainHeight; }
    void GetWaterPrefs(std::uint8_t& minimum, std::uint8_t& maximum) override { minimum = maximum = 0; }
    bool ResetTutorialCity(int) override { return false; }
    void GetCityLocations(std::vector<cLocation>&) override {}
    void GetBoundingRect(sc4::SC4RectT<int>& out) override { out = bounds; }

    std::map<std::pair<int, int>, cISC4RegionalCity*> cities;
    std::vector<std::pair<int, int>> lookups;
    sc4::SC4RectT<int> bounds;
    std::uint32_t baseTerrainHeight = 8;
    cRZAutoRefCount<cISC4RegionalCity> slot;
};

class FakeRegionalCity final : public cISC4RegionalCity {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 0; }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    void SetupCity(cISC4City*) override {}
    std::uint32_t GetCitySerialNumber() const override { return 0; }
    void GetCitySaveFilePath(cIGZString& out) const override { out.AssignFromCString(savePath.c_str()); }
    std::string savePath;
};

// ---------------------------------------------------------------------------- fake terrain host
class FakeHost final : public ITerrainHost {
public:
    ~FakeHost() override { for (FakeTerrainEffect* effect : effectsCreated) delete effect; }   // the terrain only releases (counts), the host owns them
    struct LightRow {
        std::int32_t z, x, endX, normalStride, colourStride;
        float* normals;
        void* colours;
    };

    cIGZMessageServer2* Messages() override { return &messages; }
    // The model's `new cRZMessage2Standard`; the real class also implements every payload setter.
    cIGZMessage2Standard* NewMessage() override {
        auto* message = new cRZMessage2Standard();
        message->AddRef();
        return message;
    }
    bool WriteGZSerializable(cIGZOStream& stream, const cIGZUnknown* object) override {
        return sc4::SC4WriteGZSerializable(stream, object);
    }
    bool ReadGZSerializable(cIGZIStream& stream, std::uint32_t iid, void** out) override {
        if (hasSerializedView) {
            *out = serializedView;
            return true;
        }
        return sc4::SC4ReadGZSerializable(stream, iid, out, false);
    }
    double RandomDoubleRangeUniform(double lo, double hi) override {
        // a deterministic stand-in for the class's seeded sRand
        randomState = randomState * 1664525u + 1013904223u;
        const double unit = static_cast<double>(randomState >> 8) / 16777216.0;
        return lo + (hi - lo) * unit;
    }
    double RandomDoubleRangeGaussian(double mean, double sigma) override {
        // Box-Muller over the same generator, so a fixed seed gives a fixed cliff height
        randomState = randomState * 1664525u + 1013904223u;
        const double first = (static_cast<double>(randomState >> 8) + 0.5) / 16777216.0;
        randomState = randomState * 1664525u + 1013904223u;
        const double second = static_cast<double>(randomState >> 8) / 16777216.0;
        const double normal = std::sqrt(-2.0 * std::log(first)) * std::cos(6.283185307179586 * second);
        return mean + sigma * normal;
    }
    void ApplyTerrainLightingRow(std::int32_t z, std::int32_t x, std::int32_t endX, float* normals,
                                 std::int32_t normalStride, void* colours, std::int32_t colourStride) override {
        rows.push_back({z, x, endX, normalStride, colourStride, normals, colours});
    }

    // ---- the life-cycle seams ----
    cIGZUnknown* City() override { return &city; }
    std::uint32_t CityCellCountX() override { return cellCountX; }
    std::uint32_t CityCellCountZ() override { return cellCountZ; }
    std::int32_t CityTerrainType() override { return terrainType; }
    cIGZUnknown* CityLandSimulator() override { return &landSim; }
    cIGZUnknown* CityFloraSimulator() override { return &floraSim; }
    cIGZUnknown* CityEffectsManager() override { return &effects; }
    cIGZUnknown* Simulator() override { return &simulator; }
    cIGZUnknown* SurfaceWater() override { return &surfaceWater; }
    bool SurfaceWaterInit() override { ++surfaceWaterInits; return true; }
    void RegisterWithSimulator(cSTETerrain*) override { ++simulatorRegistrations; }
    void UnregisterFromSimulator(cSTETerrain*) override { ++simulatorUnregistrations; }
    void AddToAutomataManager(cSTETerrain*) override { ++automataAdds; }
    void RemoveFromAutomataManager(cSTETerrain*) override { ++automataRemoves; }
    cIGZCommandServer* CommandServer() override { return &commands; }
    bool AddTerrainNotification(cSTETerrain*, std::uint32_t type) override {
        notifications.push_back(type);
        return true;
    }
    bool RemoveTerrainNotification(cSTETerrain*, std::uint32_t type) override {
        removedNotifications.push_back(type);
        return true;
    }
    void UpdateLandSimulator(cIGZUnknown*) override { ++landUpdates; }
    void ResetLandSimulator(cIGZUnknown*) override { ++landResets; }
    bool WorldManagerBusy() override { return worldBusy; }
    bool PreferencesFlag4ec() override { return preferencesFlag; }
    std::uint32_t ViewZoomLevel() override { return viewZoom; }
    void VisibleEdges(std::int32_t& horizontal, std::int32_t& vertical) override {
        horizontal = visibleHorizontal;
        vertical = visibleVertical;
    }
    ITerrainView* CreateTerrainView() override { return view; }
    void RegisterStaticShutdown() override { ++staticShutdownAdds; }
    void UnregisterStaticShutdown() override { ++staticShutdownRemoves; }
    cIGZPersistResourceManager* AcquireResourceManager() override { ++resourceManagerAcquires; return nullptr; }
    void ReleaseResourceManager() override { ++resourceManagerReleases; }
    void FloodFillDistance(std::vector<std::vector<std::uint8_t>>& grid, std::int32_t source,
                           std::int32_t threshold) override {
        ++floodFills;
        lastFloodSource = source;
        lastFloodThreshold = threshold;
        lastFloodGrid = grid;
    }
    cISCPropertyHolder* GetPropertyHolder(const cGZPersistResourceKey& key) override {
        ++propertyHolderRequests;
        lastPropertyKey = key;
        auto it = propertyHolders.find(key.instance);
        if (it == propertyHolders.end()) return nullptr;
        it->second->AddRef();
        return it->second;
    }
    void ListTerrainTextureResources(std::vector<cGZPersistResourceKey>& out) override {
        ++textureResourceLists;
        out = textureResourceKeys;
    }
    ITerrainIniFile* OpenTerrainIni() override { return terrainIni; }
    void ChangeTerrainType(std::int32_t terrainType) override { changedTerrainTypes.push_back(terrainType); }
    void SetUpTexturesForCurrentTerrainTypeAndZoom(std::int32_t zoom) override { textureZooms.push_back(zoom); }
    std::uint32_t TerrainTextureSubTextureCount(std::int32_t, std::uint32_t) override { return textureSubTextureCount; }
    std::int32_t MoistureIndexAtX(float worldX) override {
        lastMoistureX = worldX;
        return moistureIndex;
    }


    // ---- the brush path ----
    std::uint32_t RandomUint32UniformBelow(std::uint32_t bound) override {
        randomState = randomState * 1664525u + 1013904223u;
        return bound ? (randomState >> 8) % bound : 0;
    }
    void* LoadBrushComponentResource(std::uint32_t instance) override {
        brushResourceLoads.push_back(instance);
        auto it = brushResources.find(instance);
        return it == brushResources.end() ? nullptr : it->second;
    }
    void* BrushComponentTextureInfo(void* resource) override { return textureInfoAvailable ? resource : nullptr; }
    bool BrushTextureInfoSize(void* textureInfo, std::uint32_t& width, std::uint32_t& height) override {
        if (!textureInfo) return false;
        width = textureWidth;
        height = textureHeight;
        return true;
    }
    const std::uint8_t* BrushTexturePixels(void* textureInfo) override {
        return textureInfo ? texturePixels.data() : nullptr;
    }
    bool SurfaceWaterRain(std::int32_t vertexIndex, TerrainBrushContext&, TerrainRect&, float amount) override {
        rainCalls.push_back({vertexIndex, amount});
        return surfaceWaterPerVertexRain;
    }
    void SurfaceWaterAddRain(std::int32_t vertexIndex, float amount) override {
        rainAdds.push_back({vertexIndex, amount});
    }
    void SurfaceWaterCreateSpring(std::int32_t vertexIndex) override { springs.push_back(vertexIndex); }
    void SurfaceWaterEndBrushScrub(std::uint32_t brushType, std::uint32_t arg) override {
        brushScrubEnds.push_back({brushType, arg});
    }
    void PostBrushSound(std::uint32_t type, std::uint32_t data1, std::uint32_t data2) override {
        soundPosts.push_back({type, data1, data2});
    }
    std::uint32_t FloraBrushCount() override { return static_cast<std::uint32_t>(floraIDs.size()); }
    void FloraBrushList(std::vector<std::uint32_t>& ids) override { ids = floraIDs; }
    bool FloraOccupantIsBrushable(std::uint32_t id) override { return brushableFlora.count(id) != 0; }
    void AddFloraAt(float x, float y, float z) override { floraPlaced.push_back({x, y, z}); }
    bool CityEstablished() override { return cityIsEstablished; }
    void AddFloraAura(float x, float z) override { floraAuras.push_back({x, z}); }
    bool AddOverlayDecal(cIGZUnknown* overlay, const cGZPersistResourceKey& key, const TerrainPoint& position,
                         float strength, float randomAmount) override {
        decals.push_back({overlay, key, position, strength, randomAmount});
        return true;
    }
    ITerrainEffect* CreateTerrainEffect(std::uint32_t id0, std::uint32_t id1) override {
        if (!createEffects) return nullptr;
        auto* effect = new FakeTerrainEffect(id0, id1);
        effectsCreated.push_back(effect);
        return effect;
    }
    void ListBrushResources(std::vector<cGZPersistResourceKey>& out) override { out = brushResourceKeys; }

    // ---- create / image import ----
    cRZString NewCityName() override { return cRZString(newCityName.c_str()); }
    ITerrainHeightImage* NewCityHeightImage() override { return newCityImage; }
    cIGZPersistDBSegment* NewCityObliteratedTerrainSegment() override { return newCityObliteratedSegment; }
    bool NewCityBaseAltitude(float& out) override {
        if (!hasRegion) return false;
        out = static_cast<float>(region.GetBaseTerrainHeight()) * 1.5f;
        return true;
    }
    ITerrainHeightImage* LoadHeightImage(const cRZString& path) override {
        loadedImages.push_back(path.ToChar() ? path.ToChar() : "");
        return namedImage;
    }

    // ---- edge reconcile ----
    cISC4RegionalCity* RegionalCity() override { return regionalCity; }
    cISC4Region* Region() override { return hasRegion ? &region : nullptr; }
    bool CityLoadInProgress() override { return cityLoadInProgress; }
    cIGZMessageTarget2* TerrainMessageTarget() override { return messageTarget; }
    void RegionalCityPosition(const cISC4RegionalCity&, std::int32_t& x, std::int32_t& z) override {
        x = regionalCityX;
        z = regionalCityZ;
    }
    void RegionalCitySize(const cISC4RegionalCity&, std::int32_t& width, std::int32_t& height) override {
        width = regionalCityWidth;
        height = regionalCityHeight;
    }
    cIGZPersistDBSegment* OpenSaveGame(const cIGZString& path, bool, bool) override {
        openedSaveGames.push_back(path.ToChar() ? path.ToChar() : "");
        return saveGameSegment;
    }
    void CloseSaveGame(cIGZPersistDBSegment* segment) override { ++saveGameCloses; (void)segment; }
    bool PopupDialogsEnabled() override { return popupDialogsEnabled; }
    bool ShowEdgeReconcileDialog(std::int32_t kind) override {
        dialogKinds.push_back(kind);
        return dialogsSucceed;
    }
    ITerrainView* AdoptLoadedTerrainView(void* loadedObject) override {
        adoptedViewObject = loadedObject;
        return view;
    }

    struct RainCall { std::int32_t index; float amount; };
    struct SoundPost { std::uint32_t type, data1, data2; };
    struct Decal { cIGZUnknown* overlay; cGZPersistResourceKey key; TerrainPoint position; float strength, randomAmount; };

    std::map<std::uint32_t, void*> brushResources;
    std::vector<std::uint32_t> brushResourceLoads;
    std::vector<cGZPersistResourceKey> brushResourceKeys;
    std::vector<std::uint8_t> texturePixels;
    std::uint32_t textureWidth = 1, textureHeight = 1;
    std::vector<RainCall> rainCalls, rainAdds;
    std::vector<std::int32_t> springs;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> brushScrubEnds;
    std::vector<SoundPost> soundPosts;
    std::vector<std::uint32_t> floraIDs;
    std::set<std::uint32_t> brushableFlora;
    std::vector<std::array<float, 3>> floraPlaced;
    std::vector<std::pair<float, float>> floraAuras;
    std::vector<Decal> decals;
    std::vector<FakeTerrainEffect*> effectsCreated;
    bool surfaceWaterPerVertexRain = true, cityIsEstablished = false, createEffects = true;
    bool textureInfoAvailable = true, hasSerializedView = false;
    void* serializedView = nullptr;

    std::string newCityName;
    ITerrainHeightImage* newCityImage = nullptr;
    cIGZPersistDBSegment* newCityObliteratedSegment = nullptr;
    ITerrainHeightImage* namedImage = nullptr;
    std::vector<std::string> loadedImages;

    FakeRegion region;
    FakeRegionalCity regionalCityStorage;
    cISC4RegionalCity* regionalCity = nullptr;
    bool hasRegion = false, cityLoadInProgress = false, popupDialogsEnabled = true, dialogsSucceed = true;
    std::int32_t regionalCityX = 0, regionalCityZ = 0, regionalCityWidth = 1, regionalCityHeight = 1;
    cIGZPersistDBSegment* saveGameSegment = nullptr;
    std::vector<std::string> openedSaveGames;
    int saveGameCloses = 0;
    std::vector<std::int32_t> dialogKinds;
    cIGZMessageTarget2* messageTarget = nullptr;
    void* adoptedViewObject = nullptr;

    FakeMessageServer messages;
    std::vector<LightRow> rows;
    std::uint32_t randomState = 12345;
    FakeService city, landSim, floraSim, effects, simulator, surfaceWater;
    FakeCommandServer commands;
    std::vector<std::uint32_t> notifications, removedNotifications;
    FakeView* view = nullptr;
    std::uint32_t cellCountX = 8, cellCountZ = 6, viewZoom = 2;
    std::int32_t terrainType = 2;
    std::int32_t visibleHorizontal = 0, visibleVertical = 1;
    int surfaceWaterInits = 0, simulatorRegistrations = 0, simulatorUnregistrations = 0;
    int automataAdds = 0, automataRemoves = 0, landUpdates = 0, landResets = 0;
    int staticShutdownAdds = 0, staticShutdownRemoves = 0;
    int resourceManagerAcquires = 0, resourceManagerReleases = 0;
    int floodFills = 0;
    std::int32_t lastFloodSource = 0, lastFloodThreshold = 0;
    std::vector<std::vector<std::uint8_t>> lastFloodGrid;
    std::map<std::uint32_t, sc4::cISCPropertyHolder*> propertyHolders;
    std::vector<cGZPersistResourceKey> textureResourceKeys;
    ITerrainIniFile* terrainIni = nullptr;
    std::vector<std::int32_t> changedTerrainTypes, textureZooms;
    std::uint32_t textureSubTextureCount = 1;
    std::int32_t moistureIndex = 3;
    float lastMoistureX = 0.0f;
    int propertyHolderRequests = 0, textureResourceLists = 0;
    cGZPersistResourceKey lastPropertyKey{};
    bool worldBusy = false, preferencesFlag = false;
};


class FakeMessageTarget final : public cIGZMessageTarget2 {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 0; }
    bool DoMessage(cIGZMessage2*) override { return true; }
};

// The city-load serializer creates cSTETerrain from its class id (0xe98f9525) through this factory,
// then hands out the cIGZSerializable view via QueryInterface(0xe4fda3d4).
class FakeSerializableFactory final : public ISerializableFactory {
public:
    cIGZSerializable* CreateSerializable(std::uint32_t classId) override {
        if (classId != 0xe98f9525) return nullptr;
        auto* terrain = new cSTETerrain();
        terrain->AddRef();
        return terrain;
    }
};

// The records the edge subfiles live in are opened on a real package file.
cGZDBSegmentPackedFile* Open(const std::string& path, bool write) {
    auto* segment = new cGZDBSegmentPackedFile();
    segment->AddRef();
    segment->Init();
    cRZString p(path.c_str());
    segment->SetPath(p);
    CHECK(segment->Open(true, write));
    return segment;
}

void Close(cGZDBSegmentPackedFile* segment) {
    segment->Close();
    segment->Shutdown();
    segment->Release();
}

}  // namespace

int main() {
    FakeHost host;
    SetTerrainHost(&host);
    CHECK(TerrainHost() == &host);

    // ------------------------------------------------------------------ COM identity + thunks
    {
        auto* object = new cSTETerrain();
        CHECK(object->GetGZCLSID() == 0xe98f9525 && object->GetGZCLSIDThunk() == 0xe98f9525);
        void* out = nullptr;
        CHECK(object->QueryInterface(1, &out) && out == object);
        CHECK(object->QueryInterface(0xab953253, &out) && out == object);
        CHECK(object->QueryInterface(0xc70bd37e, &out) && out == object);
        CHECK(object->QueryInterface(0xe4fda3d4, &out) && out == object);
        CHECK(object->QueryInterface(0x452294aa, &out) && out == object);
        CHECK(!object->QueryInterface(0x12345678, &out));
        // the five successful QueryInterface calls each added a reference
        CHECK(object->AddRef() == 6);
        CHECK(object->AddRefThunk_13fc() == 7 && object->AddRefThunk_140a() == 8);
        CHECK(object->Release() == 7 && object->Release() == 6 && object->Release() == 5);
        CHECK(object->ReleaseThunk_e0da() == 4);
        CHECK(object->Release() == 3 && object->Release() == 2 && object->Release() == 1);
        object->ReleaseThunk_e0fe();                        // last reference: destroys
    }

    // ------------------------------------------------------------------------- the grid fixture
    cSTETerrain t;
    TerrainTuning tuning;
    tuning.seaLevel = 20.0f;
    t.Init(8, 6, tuning);
    CHECK(t.CellCountX() == 8 && t.CellCountZ() == 6);
    CHECK(t.VertexCountX() == 9 && t.VertexCountZ() == 7 && t.VertexCount() == 63);
    for (std::int32_t z = 0; z < 7; ++z)
        for (std::int32_t x = 0; x < 9; ++x) t.Vertices()[t.GetVertIndex(x, z)].altitude = 10.0f * x + 2.0f * z;
    t.RecomputeWaterLevel({0, 0, 8, 6});

    // --------------------------------------------------------------------- display + messages
    CHECK(t.GetWaterSurfaceDisplayState());
    host.messages.posts.clear();
    t.SetWaterSurfaceDisplayState(true);                   // unchanged: nothing is posted
    CHECK(host.messages.posts.empty());
    t.SetWaterSurfaceDisplayState(false);
    CHECK(!t.GetWaterSurfaceDisplayState());
    CHECK(host.messages.posts.size() == 1 && host.messages.posts[0].type == 0x6ab97795 &&
          host.messages.posts[0].data[0] == 0);
    t.SetWaterSurfaceDisplayState(true);
    CHECK(host.messages.posts.size() == 2 && host.messages.posts[1].data[0] == 1);

    CHECK(!t.GetPlumbingInfoDisplayState());
    t.SetPlumbingInfoDisplayState(true);
    CHECK(t.GetPlumbingInfoDisplayState());
    t.SetPlumbingInfoDisplayState(false);

    host.messages.posts.clear();
    t.BeginLongTerrainOperation();
    t.EndLongTerrainOperation();
    CHECK(host.messages.posts.size() == 2 && host.messages.posts[0].type == 0xea3b0af1 &&
          host.messages.posts[1].type == 0x6a3b0afd);

    t.PostMessageA(0xc922eb91, 7, 8, 9, 10);
    CHECK(host.messages.posts.size() == 3 && host.messages.posts[2].type == 0xc922eb91);
    CHECK(host.messages.posts[2].data[0] == 7 && host.messages.posts[2].data[1] == 8 &&
          host.messages.posts[2].data[2] == 9 && host.messages.posts[2].data[3] == 10);

    FakeMessageTarget target;
    t.PostMessageToTarget(0x2a612643, &target);
    CHECK(host.messages.posts.size() == 4 && host.messages.posts[3].type == 0x2a612643);
    CHECK(host.messages.lastTarget == &target);
    // a missing server posts nothing
    SetTerrainHost(nullptr);
    t.BeginLongTerrainOperation();
    CHECK(host.messages.posts.size() == 4);
    SetTerrainHost(&host);

    // -------------------------------------------------------------------- accumulate flag
    CHECK(!t.IsAccumulatingTextureUpdates());
    t.StartAccumulatingTextureUpdates();
    CHECK(t.IsAccumulatingTextureUpdates());
    t.StartAccumulatingTextureUpdates();                   // idempotent
    CHECK(t.IsAccumulatingTextureUpdates());
    t.StopAccumulatingTextureUpdates();
    CHECK(!t.IsAccumulatingTextureUpdates());
    t.StopAccumulatingTextureUpdates();                    // idempotent

    // ---------------------------------------------------------------------------- view
    CHECK(t.GetView() == nullptr);
    FakeView view;
    t.SetView(&view);
    CHECK(t.GetView() == &view);
    t.SetView(nullptr);

    // ---------------------------------------------------------------------- limits and counts
    CHECK(cSTETerrain::MinAllowedHeight() == 5.0f && cSTETerrain::MaxAllowedHeight() == 200.0f);
    CHECK(cSTETerrain::GetDefaultTerrainRaiseLowerAmount() == 20.0f);
    CHECK(t.NumberOfWaterVertices() == 0);                 // +0x108, filled by the water update
    CHECK(t.NumberOfWaterVertices({0, 0, 2, 2}) == 6);     // alt < 20 m: 0,10,20? 20 is not below the sea
    CHECK(t.GetAltitudeCheckSum({0, 0, 8, 6}) == 2898.0f);
    CHECK(t.GetAltitudeCheckSum({0, 0, 9, 6}) == 0.0f);    // x1 out of bounds
    CHECK(t.GetAltitudeCheckSum({0, 4, 2, 3}) == 0.0f);    // z1 < z0
    CHECK(t.AreSeaVertices({0, 0, 1, 1}));
    CHECK(!t.AreSeaVertices({0, 0, 2, 2}));
    CHECK(!t.AreSeaVertices({0, 0, 9, 6}));                // bounds
    CHECK(t.AreSeaVertices({0, 4, 2, 3}));                 // empty range: "all sea"

    // ------------------------------------------------------------------------ vertex editing
    // LowerVertex clamps at the 5 m floor and rewrites the surface-water depth.
    t.Vertices()[t.GetVertIndex(4, 4)].altitude = 100.0f;
    t.WaterDepth()[t.GetVertIndex(4, 4)] = 0.0f;
    t.LowerVertex(static_cast<std::uint32_t>(t.GetVertIndex(4, 4)), 10.0f);
    CHECK(t.GetAltitudeAtVertex(4, 4) == 90.0f && t.GetWaterDepthAtVertex(4, 4) == -70.0f);   // negative (quirk)
    t.LowerVertex(static_cast<std::uint32_t>(t.GetVertIndex(4, 4)), 1000.0f);
    CHECK(t.GetAltitudeAtVertex(4, 4) == 5.0f && t.GetWaterDepthAtVertex(4, 4) == 15.0f);
    // RaiseVertex clamps at 200 m.
    t.RaiseVertex(static_cast<std::uint32_t>(t.GetVertIndex(4, 4)), 1000.0f);
    CHECK(t.GetAltitudeAtVertex(4, 4) == 200.0f && t.GetWaterDepthAtVertex(4, 4) == -180.0f);
    // A locked vertex ignores both.
    t.Vertices()[t.GetVertIndex(4, 4)].flags |= 0x4000;
    t.RaiseVertex(static_cast<std::uint32_t>(t.GetVertIndex(4, 4)), 5.0f);
    CHECK(t.GetAltitudeAtVertex(4, 4) == 200.0f);
    t.Vertices()[t.GetVertIndex(4, 4)].flags &= static_cast<std::uint16_t>(~0x4000);
    // SetAltitudeAtVertex picks the direction and the amount.
    t.SetAltitudeAtVertex(4, 4, 30.0f);
    CHECK(t.GetAltitudeAtVertex(4, 4) == 30.0f && t.GetWaterDepthAtVertex(4, 4) == -10.0f);
    t.SetAltitudeAtVertex(4, 4, 30.0f);                    // no change, no message
    CHECK(t.GetAltitudeAtVertex(4, 4) == 30.0f);

    // ------------------------------------------------------------------ flags and mark helpers
    t.Vertices()[t.GetVertIndex(1, 1)].flags = 0xffff;
    t.Vertices()[t.GetVertIndex(2, 2)].flags = 0xffff;
    t.ClearVertexPropertyFlags(static_cast<std::uint16_t>(0x0003));
    CHECK((t.Vertices()[t.GetVertIndex(1, 1)].flags & 0x0003) == 0);
    t.Vertices()[t.GetVertIndex(1, 1)].flags = 0xffff;
    t.ClearVertexPropertyFlags({1, 1, 1, 1}, static_cast<std::uint16_t>(0x0018));
    CHECK((t.Vertices()[t.GetVertIndex(1, 1)].flags & 0x0018) == 0);
    CHECK((t.Vertices()[t.GetVertIndex(2, 2)].flags & 0x0018) == 0x0018);

    // The cliff flip flag also clears the internal triangulation flip.
    {
        const std::size_t i = static_cast<std::size_t>(t.GetVertIndex(3, 1));
        t.Vertices()[i].flags = static_cast<std::uint16_t>(t.Vertices()[i].flags | 0x0800);
        t.FlipInternalTriangulation(3, 1, true);
        CHECK(t.IsTriangulationFlipped(3, 1));
        t.ClearVertexCliffFlipTriangulationFlags({3, 1, 3, 1});
        CHECK((t.Vertices()[i].flags & 0x0800) == 0);
        CHECK((t.Vertices()[i].flags & 0x1000) == 0);
        CHECK(!t.IsTriangulationFlipped(3, 1));
    }

    // Marks 1 and 3 mark "land above the sea level" (strictly above it).
    {
        const std::int32_t seaLevelIndex = t.GetVertIndex(2, 0);   // altitude exactly 20 m
        const std::int32_t landIndex = t.GetVertIndex(3, 0);       // 30 m
        CHECK(!t.IsLandAndNotMark1(seaLevelIndex) && t.IsLandAndNotMark1(landIndex));
        CHECK(t.IsLandAndNotMark3(landIndex));
        t.Vertices()[static_cast<std::size_t>(landIndex)].flags |= 0x0001;
        CHECK(t.IsVertexWithMark1(landIndex) && !t.IsLandAndNotMark1(landIndex));
        t.Vertices()[static_cast<std::size_t>(landIndex)].flags |= 0x0004;
        CHECK(t.IsVertexWithMark3(landIndex) && !t.IsLandAndNotMark3(landIndex));
        CHECK(!t.IsVertexWithMark1(seaLevelIndex) && !t.IsVertexWithMark3(seaLevelIndex));
        t.Vertices()[static_cast<std::size_t>(landIndex)].flags &= static_cast<std::uint16_t>(~0x0005);
    }

    // ------------------------------------------------------------------------- rect helpers
    {
        TerrainRect r{1, 1, 6, 4};
        t.PadRectangle(r, 2);
        CHECK(r.left == 0 && r.top == 0 && r.right == 8 && r.bottom == 6);
        TerrainRect q{1, 1, 2, 2};
        t.PadRectangle(q, 100);
        CHECK(q.left == 0 && q.top == 0 && q.right == 8 && q.bottom == 6);
        TerrainRect v{};
        t.CreateVertRectAroundVertex(v, 5, 3, 2);
        CHECK(v.left == 3 && v.top == 1 && v.right == 7 && v.bottom == 5);
        t.CreateVertRectAroundVertex(v, 0, 0, 9);          // clamped to the grid
        CHECK(v.left == 0 && v.top == 0 && v.right == 8 && v.bottom == 6);
    }

    // ------------------------------------------------------------- temporary water-level copy
    {
        TerrainRect r{0, 0, 3, 3};
        t.SaveWaterLevelsInTempArray(r);
        const float before = t.WaterDepth()[t.GetVertIndex(2, 2)];
        t.WaterDepth()[t.GetVertIndex(2, 2)] = 123.0f;
        t.RestoreWaterLevelsFromTempArray(r);
        CHECK(t.WaterDepth()[t.GetVertIndex(2, 2)] == before);
        t.WaterDepth()[t.GetVertIndex(7, 6)] = 55.0f;       // outside the rect: untouched
        t.RestoreWaterLevelsFromTempArray(r);
        CHECK(t.WaterDepth()[t.GetVertIndex(7, 6)] == 55.0f);
        t.WaterDepth()[t.GetVertIndex(7, 6)] = 0.0f;
    }

    // ------------------------------------------------------------------------ sediment channel
    {
        t.Sediment().assign(t.VertexCount(), 0.0f);
        t.Sediment()[static_cast<std::size_t>(t.GetVertIndex(1, 2))] = 7.5f;   // cell-centred rounding
        CHECK(t.GetSedimentValue(24.0f, 40.0f) == 7.5f);
        CHECK(t.GetSedimentValue(24.0f, 39.9f) == 7.5f);
        CHECK(t.GetSedimentValue(-1.0f, 0.0f) == 0.0f);    // out of bounds
        CHECK(t.GetSedimentValue(8.0f, 8.0f) == 0.0f);
    }

    // ---------------------------------------------------------------- lowest water neighbour
    {
        cSTETerrain small;
        small.Init(2, 2, tuning);
        small.Vertices()[4].altitude = 100.0f;             // centre
        for (int i = 0; i < 9; ++i)
            if (i != 4) small.Vertices()[static_cast<std::size_t>(i)].altitude = 100.0f;
        small.RecomputeWaterLevel({0, 0, 2, 2});
        CHECK(small.GetNeighborWithLowestWaterLevel(4) == -1);   // flat: no lower neighbour
        small.Vertices()[3].altitude = 90.0f;              // direction 0 (index - 1): 10 m lower
        CHECK(small.GetNeighborWithLowestWaterLevel(4) == 0);
        small.Vertices()[0].altitude = 80.0f;              // direction 1 (index - 4 = 1 - vertsX): 20 m lower
        CHECK(small.GetNeighborWithLowestWaterLevel(4) == 1);
        small.Vertices()[0].altitude = 89.0f;              // 11 < 10 * sqrt(2): the straight one wins
        CHECK(small.GetNeighborWithLowestWaterLevel(4) == 0);
    }

    // ---------------------------------------------------------------------- brush tuning
    {
        auto& brushes = cSTETerrain::BrushDescriptions();
        brushes.clear();
        cSTETerrain brush;
        brush.SetZoomLevel(2);
        CHECK(brush.BrushRadius(3) == 32.0f && brush.GetCostPerBrushApplication(3) == 32);
        TerrainBrushUIElements ui{};
        CHECK(!brush.GetUIElementsForBrush(3, ui));
        CHECK(brushes.empty());

        TerrainBrushDescription description;
        description.radiusPerZoom.assign(5, 0.0f);
        description.radiusPerZoom[2] = 48.0f;
        description.radiusPerZoom[3] = 64.0f;
        description.costPerZoom[2] = 12.9f;
        for (int i = 0; i < 10; ++i) description.ui.values[i] = static_cast<std::uint32_t>(100 + i);
        brushes[3] = description;
        CHECK(brush.BrushRadius(3) == 48.0f);
        CHECK(brush.GetCostPerBrushApplication(3) == 12);           // truncated to a whole number
        CHECK(brush.GetUIElementsForBrush(3, ui) && ui.values[0] == 100 && ui.values[9] == 109);
        CHECK(brush.BrushRadius(4) == 32.0f);                       // unknown brush: the defaults
        cSTETerrain::ReleaseBrushDescriptions();
        CHECK(brushes.empty());
        CHECK(!brush.GetUIElementsForBrush(3, ui));
    }
    {
        cSTETerrain brush;
        std::uint32_t exemplar = 0;
        CHECK(brush.GetBrushExemplarID(0, exemplar) && exemplar == 0xea312ef9u);
        CHECK(brush.GetBrushExemplarID(6, exemplar) && exemplar == 0x2a31304eu);
        CHECK(!brush.GetBrushExemplarID(7, exemplar) && exemplar == 0u);
        // eight phases in [0, 2*pi)
        brush.InitBrushParameters();
        for (int i = 0; i < 8; ++i) CHECK(brush.BrushPhase(i) >= 0.0f && brush.BrushPhase(i) < 6.2831856f);
    }

    // --------------------------------------------------------------------------- lighting
    {
        host.rows.clear();
        t.DoLighting({2, 1, 5, 3});
        CHECK(host.rows.size() == 3);
        for (std::size_t i = 0; i < host.rows.size(); ++i) {
            const auto& row = host.rows[i];
            const std::size_t index = static_cast<std::size_t>(t.GetVertIndex(2, 1 + static_cast<int>(i)));
            CHECK(row.z == 1 + static_cast<std::int32_t>(i) && row.x == 2);
            CHECK(row.endX == (1 - 2) + 5);
            CHECK(row.normalStride == 0x24 && row.colourStride == 0x24);
            CHECK(row.normals == &t.Vertices()[index].normal[0]);
            CHECK(row.colours == t.Vertices()[index].surface);
        }
    }

    // ---------------------------------------------------------------- surface texture layers
    {
        const std::size_t i = static_cast<std::size_t>(t.GetVertIndex(2, 2));
        for (int k = 0; k < 0x12; ++k) t.Vertices()[i].surface[k] = static_cast<std::uint8_t>(k + 1);
        t.ClearSurfaceTextures({2, 2, 2, 2});
        CHECK(t.Vertices()[i].surface[6] == 0 && t.Vertices()[i].surface[0xa] == 0 &&
              t.Vertices()[i].surface[0xe] == 0);
        CHECK(t.Vertices()[i].surface[8] == 9 && t.Vertices()[i].surface[0xc] == 13);   // weights kept
    }

    // ---------------------------------------------------------------- marked-region bounds
    {
        for (std::size_t i = 0; i < t.Vertices().size(); ++i) t.Vertices()[i].flags = 0;
        t.Vertices()[static_cast<std::size_t>(t.GetVertIndex(2, 1))].flags = 0x0004;
        t.Vertices()[static_cast<std::size_t>(t.GetVertIndex(5, 4))].flags = 0x0004;
        TerrainRect bounds{};
        t.ComputeBoundsOfMarkedRegion(bounds, 0x0004);
        CHECK(bounds.left == 2 && bounds.top == 1 && bounds.right == 5 && bounds.bottom == 4);
        t.ComputeBoundsOfMarkedRegion(bounds, 0x0100);     // nothing marked
        CHECK(bounds.left == 0x7fffffff && bounds.right == -0x7fffffff - 1);
    }

    // -------------------------------------------------------- rotated rectangle + cell spans
    {
        TerrainPoint quad[4];
        t.ComputeRectAroundPt(quad, 32.0f, 24.0f, 64.0f, 48.0f, 0.0f);
        CHECK(Near(quad[0].x, 0.0f) && Near(quad[0].z, 0.0f));
        CHECK(Near(quad[1].x, 0.0f) && Near(quad[1].z, 48.0f));
        CHECK(Near(quad[2].x, 64.0f) && Near(quad[2].z, 48.0f));
        CHECK(Near(quad[3].x, 64.0f) && Near(quad[3].z, 0.0f));
        t.ComputeRectAroundPt(quad, 0.0f, 0.0f, 32.0f, 32.0f, 1.5707964f);   // 90 degrees
        CHECK(Near(quad[0].x, 16.0f) && Near(quad[0].z, -16.0f));
        CHECK(Near(quad[2].x, -16.0f) && Near(quad[2].z, 16.0f));

        t.ComputeRectAroundPt(quad, 32.0f, 24.0f, 64.0f, 48.0f, 0.0f);
        int spanMin[260];
        int spanMax[260];
        int firstRow = 0, lastRow = 0;
        t.ComputeSpansForRectangle(quad, spanMin, spanMax, firstRow, lastRow);
        CHECK(firstRow == 0 && lastRow == 3);
        // Rows 0..2 hold the quad's two vertical edges; each span is padded one cell on the right.
        for (int row = 0; row <= 2; ++row) CHECK(spanMin[row] == 0 && spanMax[row] == 5);
        // (sic) row 3 is the far boundary: no edge is half-open across it, so the minimum stays at
        // INT_MAX and the empty maximum becomes cellsX.
        CHECK(spanMin[3] == 0x7fffffff && spanMax[3] == 8);
        // every covered vertex is sea (the fixture's plane is below 20 m only up to x = 1):
        CHECK(t.ComputeBooleanFnForRect(quad, &cSTETerrain::IsSeaVertex, 0.0f));
        CHECK(!t.ComputeBooleanFnForRect(quad, &cSTETerrain::IsSeaVertex, 1.0f));
        CHECK(t.ComputeBooleanFnForRect(quad, &cSTETerrain::IsLandVertex, 0.5f));
    }

    // ------------------------------------------------------------------ edge altitude subfiles
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "sc4_steterrain_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string path = (dir / "city.dat").string();
    {
        auto* segment = Open(path, true);
        // Save writes the four edge subfiles itself (0x10073f17a loops over them).
        CHECK(t.Save(segment));
        for (int edge = 0; edge < 4; ++edge) CHECK(t.SaveEdgeAltitudes(edge, segment));
        // SaveObliterated clears every flag except 0x18 (its mask argument is inverted by the
        // original: flags &= ~0xffe7 == flags & 0x18) before it saves.
        const std::size_t obliterated = static_cast<std::size_t>(t.GetVertIndex(1, 1));
        t.Vertices()[obliterated].flags = 0xffff;
        t.SaveObliterated(segment);
        CHECK(t.Vertices()[obliterated].flags == 0x0018);
        Close(segment);

        segment = Open(path, false);
        std::vector<float> edge;
        CHECK(t.LoadEdgeAltitudes(0, segment, edge) && edge.size() == 7);   // west column, x = 0
        CHECK(edge[0] == 0.0f && edge[1] == 2.0f && edge[6] == 12.0f);
        CHECK(t.LoadEdgeAltitudes(1, segment, edge) && edge.size() == 9);   // north row, z = 0
        CHECK(edge[0] == 0.0f && edge[8] == 80.0f);
        CHECK(t.LoadEdgeAltitudes(2, segment, edge) && edge.size() == 7);   // east column, x = 8
        CHECK(edge[0] == 80.0f && edge[6] == 92.0f);
        CHECK(t.LoadEdgeAltitudes(3, segment, edge) && edge.size() == 9);   // south row, z = 6
        CHECK(edge[0] == 12.0f && edge[8] == 92.0f);
        cSTETerrain loaded;
        loaded.LoadObliterated(segment);
        CHECK(loaded.LastLoadSucceeded() && loaded.VertexCountX() == 9 && loaded.VertexCountZ() == 7);
        Close(segment);
    }

    // ---------------------------------------------------------------------- stream Read/Write
    const std::string streamPath = (dir / "stream.dat").string();
    {
        auto* segment = new cSC4DBSegment();
        segment->AddRef();
        segment->Init();
        cRZString p(streamPath.c_str());
        segment->SetPath(p);
        CHECK(segment->Open(true, true));
        cISC4DBSegmentOStream* out = nullptr;
        CHECK(segment->OpenOStream({0xaaaabbbb, 0x11112222, 1}, &out, true) && out != nullptr);
        t.Write(*out);
        CHECK(segment->CloseOStream(out));
        out->Release();
        CHECK(segment->Close());
        segment->Release();                                 // the destructor calls Close() again

        auto* reopen = new cSC4DBSegment();
        reopen->AddRef();
        reopen->Init();
        cRZString p2(streamPath.c_str());
        reopen->SetPath(p2);
        CHECK(reopen->Open(true, false));
        cISC4DBSegmentIStream* in = nullptr;
        CHECK(reopen->OpenIStream({0xaaaabbbb, 0x11112222, 1}, &in) && in != nullptr);
        cSTETerrain back;
        if (in) {
            CHECK(back.Read(*in));
            CHECK(reopen->CloseIStream(in));
            in->Release();
        }
        CHECK(reopen->Close());
        reopen->Release();
        CHECK(back.LastLoadSucceeded());
        CHECK(back.VertexCountX() == 9 && back.VertexCountZ() == 7);
        CHECK(back.GetAltitudeAtVertex(8, 6) == t.GetAltitudeAtVertex(8, 6));
    }

    // ----------------------------------------------------- SC4ReadGZSerializable round trip
    {
        FakeSerializableFactory factory;
        SetSerializableFactory(&factory);
        const std::string rtPath = (dir / "stream_rt.dat").string();
        {
            auto* segment = new cSC4DBSegment();
            segment->AddRef();
            segment->Init();
            cRZString p(rtPath.c_str());
            segment->SetPath(p);
            CHECK(segment->Open(false, true));   // write mode (serializer mode 2) so SaveClassObjects runs
            cISC4DBSegmentOStream* out = nullptr;
            CHECK(segment->OpenOStream({0xaaaabbbb, 0x11112223, 1}, &out, true) && out != nullptr);
            t.AddRef();    // keep the stack fixture alive across the serializer's add/release pair
            CHECK(SC4WriteGZSerializable(*out, static_cast<cIGZUnknown*>(&t)));
            CHECK(segment->CloseOStream(out));
            out->Release();
            CHECK(segment->Close());
            segment->Release();
        }
        {
            auto* reopen = new cSC4DBSegment();
            reopen->AddRef();
            reopen->Init();
            cRZString p(rtPath.c_str());
            reopen->SetPath(p);
            CHECK(reopen->Open(true, false));
            CHECK(reopen->LoadClassObjects(0x48));
            cISC4DBSegmentIStream* in = nullptr;
            CHECK(reopen->OpenIStream({0xaaaabbbb, 0x11112223, 1}, &in) && in != nullptr);
            void* raw = nullptr;
            CHECK(SC4ReadGZSerializable(*in, 0xc70bd37e, &raw, false));
            CHECK(raw != nullptr);
            auto* rt = static_cast<cSTETerrain*>(static_cast<cIGZSerializable*>(raw));
            CHECK(rt && rt->GetGZCLSID() == 0xe98f9525);
            CHECK(rt->VertexCountX() == 9 && rt->VertexCountZ() == 7);
            CHECK(rt->GetAltitudeAtVertex(8, 6) == t.GetAltitudeAtVertex(8, 6));
            static_cast<cIGZSerializable*>(raw)->Release();
            CHECK(reopen->CloseIStream(in));
            in->Release();
            CHECK(reopen->Close());
            reopen->Release();
        }
        SetSerializableFactory(nullptr);
    }
    // =================================================================================== life cycle
    cSTETerrain::Statics().seaLevel = 100.0f;      // the runtime statics are otherwise the demo defaults
    cSTETerrain::Statics().maxNormalTerrainHeight.assign(8, 200.0f);   // the exemplar's per-terrain-type vector
    cSTETerrain::Statics().maxNormalTerrainHeight[3] = 175.0f;
    {
        // ---- StaticInit / StaticShutdown: the flag, the shutdown hook, the resource manager hold
        host.staticShutdownAdds = host.staticShutdownRemoves = 0;
        host.resourceManagerAcquires = host.resourceManagerReleases = 0;
        auto& brushes = cSTETerrain::BrushDescriptions();
        brushes.clear();
        brushes[7] = TerrainBrushDescription{};
        CHECK(cSTETerrain::StaticInit());
        CHECK(host.staticShutdownAdds == 1 && host.resourceManagerAcquires == 1);
        CHECK(cSTETerrain::StaticInit());                        // already inited: nothing happens again
        CHECK(host.staticShutdownAdds == 1 && host.resourceManagerAcquires == 1);
        bool called = false;
        CHECK(cSTETerrain::StaticShutdown(&called));
        CHECK(host.staticShutdownRemoves == 1 && host.resourceManagerReleases == 1);
        CHECK(brushes.empty());                                  // ReleaseBrushDescriptions
        CHECK(cSTETerrain::StaticShutdown(&called));             // idempotent
        CHECK(host.staticShutdownRemoves == 1);
    }
    {
        // ---- Init / Shutdown: the city sizes the grid, every service is taken and released again
        host.city.refs = host.simulator.refs = host.surfaceWater.refs = 0;
        host.landSim.refs = host.floraSim.refs = host.effects.refs = 0;
        host.notifications.clear();
        host.removedNotifications.clear();
        host.commands.registered.clear();
        host.commands.unregistered.clear();
        host.surfaceWaterInits = 0;
        host.simulatorRegistrations = host.simulatorUnregistrations = 0;
        host.automataAdds = host.automataRemoves = 0;
        host.cellCountX = 8;
        host.cellCountZ = 6;
        host.terrainType = 3;

        cSTETerrain city;
        CHECK(!city.IsInitialized());
        CHECK(city.Init());
        CHECK(city.IsInitialized());
        CHECK(city.CellCountX() == 8 && city.CellCountZ() == 6);
        CHECK(city.VertexCountX() == 9 && city.VertexCountZ() == 7 && city.VertexCount() == 63);
        CHECK(city.City() == &host.city && city.SurfaceWaterObject() == &host.surfaceWater);
        CHECK(city.CityTerrainTypeValue() == 3);
        CHECK(city.GetSeaLevel() == 100.0f);                 // copied from kSeaLevel in Init
        CHECK(city.MaxNormalTerrainHeight() == 175.0f);      // kMaxNormalTerrainHeight[3]
        CHECK(city.SurfaceCoverGrid().size() == 7 && city.SurfaceCoverGrid()[0].size() == 9);
        CHECK(host.city.refs == 1 && host.simulator.refs == 1 && host.surfaceWater.refs == 1);
        CHECK(host.landSim.refs == 1 && host.floraSim.refs == 1 && host.effects.refs == 1);
        CHECK(host.surfaceWaterInits == 1);                      // Create() itself is not recovered
        CHECK(host.simulatorRegistrations == 1 && host.automataAdds == 1);
        CHECK(host.notifications.size() == 10);
        CHECK(host.notifications[0] == 0x26d31ec1 && host.notifications[9] == 0xc4a6af6);
        CHECK(host.commands.registered.size() == 7);
        CHECK(host.commands.registered[0].id == 0xc4a6af0 &&
              host.commands.registered[0].name == "GetAltitude");
        CHECK(host.commands.registered[3].name == "SetCellAltitude");
        CHECK(host.commands.registered[6].id == 0xc4a6af6 &&
              host.commands.registered[6].name == "ApplyTerrainBrush");

        CHECK(city.Shutdown());
        CHECK(!city.IsInitialized());
        CHECK(host.city.refs == 0 && host.simulator.refs == 0 && host.surfaceWater.refs == 0);
        CHECK(host.landSim.refs == 0 && host.floraSim.refs == 0 && host.effects.refs == 0);
        CHECK(host.simulatorUnregistrations == 1 && host.automataRemoves == 1);
        CHECK(host.removedNotifications.size() == 10);
        // (sic) Shutdown removes 0xc4a6af3 (never added by Init) and leaves 0xc4a6af6 subscribed.
        CHECK(host.removedNotifications[7] == 0xc4a6af3);
        for (std::uint32_t removed : host.removedNotifications) CHECK(removed != 0xc4a6af6);
        CHECK(host.commands.unregistered.size() == 7);
        CHECK(city.Vertices().empty());                          // the vertex block is freed (+0x88)
        CHECK(city.Shutdown());                                  // idempotent
    }
    {
        // ---- DoPostCityInit: the view is created, initialised and its overlays are taken
        FakeView cityView;
        host.view = &cityView;
        for (FakeService& overlay : cityView.overlays) overlay.refs = 0;
        host.landResets = 0;
        host.viewZoom = 4;
        host.messages.posts.clear();

        cSTETerrain city;
        CHECK(city.Init());
        CHECK(!city.IsPostCityInitDone() && !city.DisplayEnabled());
        city.DoPostCityInit();
        CHECK(city.IsPostCityInitDone() && city.DisplayEnabled());
        CHECK(city.GetView() == &cityView);
        CHECK(cityView.inits == 1 && cityView.initTerrain == &city);
        CHECK(city.ZoomLevel() == 4);                            // spViewUtilities +0x14
        CHECK(host.landResets == 1);                             // the land simulator reset (+0x140 slot 0x28)
        CHECK(!cityView.dirtyRows.empty());                      // AssignSurfaceTexturesAndColors ran
        // rows 0 .. cellsZ-1 (the last cell row is cellsZ - 1, not cellsZ)
        CHECK(cityView.dirtyRows[0].first == 0 && cityView.dirtyRows[0].second == 5);
        CHECK(cityView.overlayRequests.size() == 3);
        CHECK(cityView.overlays[0].refs == 1 && cityView.overlays[1].refs == 1 && cityView.overlays[2].refs == 1);
        city.DoPostCityInit();                                   // idempotent
        CHECK(cityView.inits == 1 && cityView.overlayRequests.size() == 3);
        CHECK(city.Shutdown());
        CHECK(cityView.shutdowns == 1);
        CHECK(cityView.overlays[0].refs == 0 && cityView.overlays[1].refs == 0 && cityView.overlays[2].refs == 0);
        host.view = nullptr;

        // the city-up message runs DoPostCityInit too
        host.view = &cityView;
        cRZMessage2Standard message;
        message.AddRef();
        message.SetType(0x26d31ec1);
        cSTETerrain city2;
        CHECK(city2.Init());
        CHECK(city2.DoMessage(&message) && city2.IsPostCityInitDone());
        CHECK(cityView.inits == 2);
        CHECK(city2.Shutdown());
        host.view = nullptr;
    }
    {
        // ---- DoMessage: the scripting commands (0xc4a6af0..6)
        host.view = nullptr;
        cSTETerrain city;
        CHECK(city.Init());
        for (cSTETerrain::Vertex& vertex : city.Vertices()) vertex.altitude = 42.0f;
        city.RecomputeWaterLevel({0, 0, 8, 6});

        cRZMessage2Standard message;
        message.AddRef();                           // the DoMessage QueryInterface/Release pair
        FakeParameterSet params, result;
        message.SetVoid1(&params);
        message.SetVoid2(&result);

        // GetAltitude(32, 16) -- a flat 42 m grid
        params.SetParameter(0, cRZVariant(32.0f));
        params.SetParameter(1, cRZVariant(16.0f));
        message.SetType(0xc4a6af0);
        CHECK(city.DoMessage(&message));
        CHECK(Near(result[0].GetValFloat32(), 42.0f));
        // out of bounds sets status 2
        params.SetParameter(0, cRZVariant(-1.0f));
        CHECK(city.DoMessage(&message));
        CHECK(result.status == 2);

        // SetAltitude(32, 16, 7.5) -- (sic) the original uses the x coordinate as the row too
        params.SetParameter(0, cRZVariant(32.0f));
        params.SetParameter(1, cRZVariant(16.0f));
        params.SetParameter(2, cRZVariant(7.5f));
        message.SetType(0xc4a6af1);
        CHECK(city.DoMessage(&message));
        CHECK(city.GetAltitudeAtVertex(2, 2) == 7.5f);
        CHECK(city.GetAltitudeAtVertex(2, 1) == 42.0f);          // the z argument is ignored (quirk)

        // GetCellAltitude(2, 1) -> 42
        params.SetParameter(0, cRZVariant(static_cast<std::uint32_t>(2)));
        params.SetParameter(1, cRZVariant(static_cast<std::uint32_t>(1)));
        message.SetType(0xc4a6af2);
        CHECK(city.DoMessage(&message));
        CHECK(Near(result[0].GetValFloat32(), 42.0f));

        // SetCellAltitude(1, 1, 9) -- three parameters: one cell
        FakeParameterSet single;
        single.SetParameter(0, cRZVariant(static_cast<std::uint32_t>(1)));
        single.SetParameter(1, cRZVariant(static_cast<std::uint32_t>(1)));
        single.SetParameter(2, cRZVariant(9.0f));
        message.SetVoid1(&single);
        message.SetType(0xc4a6af3);
        CHECK(city.DoMessage(&message));
        CHECK(city.GetAltitudeAtVertex(1, 1) == 9.0f);

        // SetSeaLevel(30) then GetSeaLevel() -> 30
        result.SetStatusParameterValue(0);
        params.SetParameter(0, cRZVariant(30.0f));
        message.SetVoid1(&params);
        message.SetType(0xc4a6af5);
        CHECK(city.DoMessage(&message));
        CHECK(result.status == 0);
        CHECK(city.GetSeaLevel() == 30.0f);
        message.SetType(0xc4a6af4);
        CHECK(city.DoMessage(&message));
        CHECK(Near(result[0].GetValFloat32(), 30.0f));
        // an out-of-range sea level fails with status 2
        params.SetParameter(0, cRZVariant(100000.0f));
        message.SetType(0xc4a6af5);
        CHECK(city.DoMessage(&message));
        CHECK(result.status == 2);
        // ApplyTerrainBrush reports failure (the brush payload is not modelled yet)
        message.SetType(0xc4a6af6);
        CHECK(city.DoMessage(&message));
        CHECK(result.status == 2);

        // an unknown type is accepted and ignored
        message.SetType(0x12345678);
        CHECK(city.DoMessage(&message));
        CHECK(city.Shutdown());
    }
    {
        // ---- the zoom and erosion messages, the animation tick and the redisplay posts
        FakeView cityView;
        host.view = &cityView;
        host.messages.posts.clear();
        host.landUpdates = 0;
        host.preferencesFlag = false;
        cityView.dirtyRows.clear();

        cSTETerrain city;
        CHECK(city.Init());
        city.DoPostCityInit();
        cityView.dirtyRows.clear();
        host.landUpdates = 0;                                    // the post-city-init surface pass

        // zoom changed: data 2 points at the new zoom
        std::int32_t newZoom = 3;
        cRZMessage2Standard message;
        message.AddRef();
        message.SetType(0xa6b79602);
        message.SetVoid2(&newZoom);
        CHECK(city.DoMessage(&message));
        CHECK(city.ZoomLevel() == 3);
        CHECK(host.landUpdates == 1);                            // UpdateMoistureValues in the surface pass
        CHECK(!cityView.dirtyRows.empty());
        cityView.dirtyRows.clear();
        CHECK(city.DoMessage(&message));                         // same zoom: nothing happens
        CHECK(cityView.dirtyRows.empty() && host.landUpdates == 1);

        // erosion message (data 1 = style 1, wind only) redisplays the whole grid
        host.messages.posts.clear();
        message.SetType(0x470bd214);
        message.SetData1(1);
        CHECK(city.DoMessage(&message));
        CHECK(host.messages.posts.size() == 3);
        CHECK(host.messages.posts[0].type == 0x278e6003 && host.messages.posts[0].data[0] == 0x00000008 &&
              host.messages.posts[0].data[1] == 0x00000006);
        CHECK(host.messages.posts[1].type == 0x678e6e3a);
        CHECK(host.messages.posts[2].type == 0x870bc918);

        // animation tick: the second tick posts the city-changed message (preference off)
        host.messages.posts.clear();
        city.DoAnimationTick(100);
        CHECK(host.messages.posts.empty());
        city.DoAnimationTick(100);
        CHECK(host.messages.posts.size() == 1 && host.messages.posts[0].type == 0xca63d517);
        city.DoAnimationTick(100);                               // the arming happens only once
        CHECK(host.messages.posts.size() == 1);
        // with the preference set the second tick posts nothing (it re-posts 0x2a612643 to itself)
        cSTETerrain other;
        CHECK(other.Init());
        host.preferencesFlag = true;
        host.messages.posts.clear();
        other.DoAnimationTick(1);
        other.DoAnimationTick(1);
        CHECK(host.messages.posts.empty());
        host.preferencesFlag = false;
        CHECK(other.Shutdown());
        CHECK(city.Shutdown());
        host.view = nullptr;
    }
    {
        // ---- RedisplayTerrain(point pair): sort, widen by one cell, clamp to the grid
        FakeView cityView;
        host.view = &cityView;
        cSTETerrain city;
        CHECK(city.Init());
        city.DoPostCityInit();
        city.Vertices()[city.GetVertIndex(0, 0)].altitude = 10.0f;
        city.Vertices()[city.GetVertIndex(4, 3)].altitude = 30.0f;
        city.FindMinAndMaxHeights();
        host.messages.posts.clear();
        city.RedisplayTerrain(true, true, TerrainPointI{3, 2}, TerrainPointI{1, 4}, 5);
        CHECK(host.messages.posts.size() == 3);
        CHECK(host.messages.posts[0].type == 0x278e6003);
        CHECK(host.messages.posts[0].data[0] == ((1u << 16) | 4u));      // left 1, right 3 + 1
        CHECK(host.messages.posts[0].data[1] == ((2u << 16) | 5u));      // top 2, bottom 4 + 1
        CHECK(host.messages.posts[0].data[3] == 5);
        CHECK(host.messages.posts[1].type == 0x678e6e3a);
        CHECK(host.messages.posts[2].type == 0x870bc918);
        // the altitude range: the minimum in the high half and the maximum in the low one (sic)
        const std::uint32_t packed = static_cast<std::uint32_t>(host.messages.posts[2].data[2]);
        CHECK((packed >> 16) == 0u);                              // lo = 0
        CHECK((packed & 0xffffu) == 100u);                        // hi = the view's +0x94
        // a view top above kMaxTerrainHeight is clamped to it
        cityView.maxAltitude = 500.0f;
        host.messages.posts.clear();
        city.RedisplayTerrain(true, true, TerrainPointI{3, 2}, TerrainPointI{1, 4}, 0);
        CHECK(host.messages.posts.size() == 3);
        const std::uint32_t clamped = static_cast<std::uint32_t>(host.messages.posts[2].data[2]);
        CHECK((clamped & 0xffffu) == 200u);                       // kMaxTerrainHeight
        CHECK((clamped >> 16) == 0u);
        CHECK(city.Shutdown());
        host.view = nullptr;
    }
    {
        // ---- SetAltitude(rect), EnsureMinAllowedHeight and ChangeTerrainLevel
        FakeView cityView;
        host.view = &cityView;
        host.messages.posts.clear();
        cSTETerrain city;
        TerrainTuning tuning2;
        tuning2.seaLevel = 0.0f;
        city.Init(4, 4, tuning2);
        city.SetDisplayEnabled(true);
        city.SetView(&cityView);
        city.SetAltitude(TerrainRect{1, 1, 2, 2}, 12.0f, true, 3);
        for (int z = 1; z <= 2; ++z)
            for (int x = 1; x <= 2; ++x) CHECK(city.GetAltitudeAtVertex(x, z) == 12.0f);
        CHECK(city.GetAltitudeAtVertex(0, 0) == 0.0f);
        CHECK(host.messages.posts.size() == 3);                  // the redisplay of the rect
        CHECK(host.messages.posts[0].data[3] == 3);

        // EnsureMinAllowedHeight raises everything to the 5 m floor when the minimum is below it
        for (std::size_t i = 0; i < city.Vertices().size(); ++i)
            city.Vertices()[i].altitude = static_cast<float>(i % 4);
        city.FindMinAndMaxHeights();
        CHECK(city.MinHeight() == 0.0f);
        city.EnsureMinAllowedHeight();
        CHECK(city.MinHeight() == 5.0f);
        for (const cSTETerrain::Vertex& vertex : city.Vertices()) CHECK(vertex.altitude >= 5.0f);

        // ChangeTerrainLevel(raise, 20) raises the whole grid and redisplays it
        host.messages.posts.clear();
        const float before = city.GetAltitudeAtVertex(2, 2);
        city.ChangeTerrainLevel(true, 20.0f);
        CHECK(city.GetAltitudeAtVertex(2, 2) == before + 20.0f);
        CHECK(host.messages.posts.size() == 3);
        city.ChangeTerrainLevel(false, 1000.0f);                 // clamps at the 5 m floor
        CHECK(city.GetAltitudeAtVertex(2, 2) == 5.0f);
        CHECK(city.Shutdown());
    }
    {
        // ---- water proximity grid, the 10 s sweep and the land-type ratios
        cSTETerrain city;
        TerrainTuning tuning2;
        tuning2.seaLevel = 50.0f;
        city.Init(16, 16, tuning2);
        for (std::int32_t z = 0; z < 17; ++z)
            for (std::int32_t x = 0; x < 17; ++x)
                city.Vertices()[static_cast<std::size_t>(city.GetVertIndex(x, z))].altitude =
                    (z == 0) ? 10.0f : 60.0f;                    // the first row is under the sea
        city.RecomputeWaterLevel({0, 0, 16, 16});
        host.floodFills = 0;
        city.ComputeWaterProximityValues({0, 0, 16, 16});
        CHECK(host.floodFills == 1);                             // 16 x 16 > 99 cells
        CHECK(host.lastFloodSource == 0);                        // water exists
        CHECK(host.lastFloodThreshold == 5);                     // kMaxBeachWidth + 1
        CHECK(host.lastFloodGrid.size() == 17 && host.lastFloodGrid[0].size() == 17);
        CHECK(host.lastFloodGrid[0][0] == 0);                    // wet
        CHECK(host.lastFloodGrid[1][0] == 6);                    // dry: kMaxBeachWidth + 2
        CHECK(city.NumberOfWaterVertices() == 17);
        // a small rect only accumulates into the empty rect
        host.floodFills = 0;
        city.ComputeWaterProximityValues({1, 1, 2, 2});
        CHECK(host.floodFills == 0);
        // the 10 s animation sweep calls it again
        host.floodFills = 0;
        city.ComputeWaterProximityValues({1, 1, 2, 2});
        city.DoAnimationTick(10000000);
        CHECK(host.floodFills == 1);

        // land type ratios: three weights above 250 are scaled down
        cSTETerrain::Vertex& vertex = city.Vertices()[static_cast<std::size_t>(city.GetVertIndex(4, 4))];
        for (int slot = 0; slot < 3; ++slot) {
            const std::uint16_t key = static_cast<std::uint16_t>(100 + slot);
            const std::uint16_t weight = 200;
            std::memcpy(&vertex.surface[6 + slot * 4], &key, sizeof key);
            std::memcpy(&vertex.surface[8 + slot * 4], &weight, sizeof weight);
        }
        city.AdjustLandTypeRatios({4, 4, 4, 4});
        for (int slot = 0; slot < 3; ++slot) {
            std::uint16_t weight = 0;
            std::memcpy(&weight, &vertex.surface[8 + slot * 4], sizeof weight);
            CHECK(weight == 83);                                 // 200 * 250 / 600
        }

        // AddLandTypeAtVertex: empty slot, matching key (keeps the larger weight), full (drops the smallest)
        cSTETerrain::Vertex& fresh = city.Vertices()[static_cast<std::size_t>(city.GetVertIndex(5, 5))];
        for (int k = 0; k < 0x12; ++k) fresh.surface[k] = 0;
        city.AddLandTypeAtVertex(static_cast<std::uint32_t>(city.GetVertIndex(5, 5)), 7, 0.5f);
        city.AddLandTypeAtVertex(static_cast<std::uint32_t>(city.GetVertIndex(5, 5)), 8, 0.25f);
        city.AddLandTypeAtVertex(static_cast<std::uint32_t>(city.GetVertIndex(5, 5)), 9, 0.75f);
        city.AddLandTypeAtVertex(static_cast<std::uint32_t>(city.GetVertIndex(5, 5)), 7, 0.9f);
        std::uint16_t keys[3], weights[3];
        for (int slot = 0; slot < 3; ++slot) {
            std::memcpy(&keys[slot], &fresh.surface[6 + slot * 4], sizeof(std::uint16_t));
            std::memcpy(&weights[slot], &fresh.surface[8 + slot * 4], sizeof(std::uint16_t));
        }
        CHECK(keys[0] == 7 && weights[0] == static_cast<std::uint16_t>(0.9f * 255.0f + 1.0f));
        CHECK(keys[1] == 8 && weights[1] == static_cast<std::uint16_t>(0.25f * 255.0f + 1.0f));
        CHECK(keys[2] == 9 && weights[2] == static_cast<std::uint16_t>(0.75f * 255.0f + 1.0f));
        city.AddLandTypeAtVertex(static_cast<std::uint32_t>(city.GetVertIndex(5, 5)), 10, 1.0f);
        std::memcpy(&keys[1], &fresh.surface[6 + 4], sizeof(std::uint16_t));
        CHECK(keys[1] == 10);                                    // the 0.25 slot was dropped
        CHECK(city.Shutdown());
    }
    {
        // ---- the shore helpers: IsShoreVertex tests the four axis neighbours (west, north, east, south);
        //      GetNeighboringSeaVertex divides the row by the vertex count in Z (sic)
        cSTETerrain city;
        TerrainTuning tuning2;
        tuning2.seaLevel = 20.0f;
        city.Init(2, 2, tuning2);
        for (cSTETerrain::Vertex& vertex : city.Vertices()) vertex.altitude = 100.0f;
        // (1,0) is land; a sea neighbour to the south is seen (direction 6 is tested)
        city.Vertices()[static_cast<std::size_t>(city.GetVertIndex(1, 0))].altitude = 30.0f;
        city.Vertices()[static_cast<std::size_t>(city.GetVertIndex(1, 1))].altitude = 10.0f;
        CHECK(city.IsShoreVertex(city.GetVertIndex(1, 0)));
        // the same sea neighbour to the east (direction 4) is seen too
        city.Vertices()[static_cast<std::size_t>(city.GetVertIndex(1, 1))].altitude = 100.0f;
        city.Vertices()[static_cast<std::size_t>(city.GetVertIndex(2, 0))].altitude = 10.0f;
        CHECK(city.IsShoreVertex(city.GetVertIndex(1, 0)));
        // the vertex itself must be dry land (strictly above the sea)
        city.Vertices()[static_cast<std::size_t>(city.GetVertIndex(1, 0))].altitude = 20.0f;
        CHECK(!city.IsShoreVertex(city.GetVertIndex(1, 0)));

        // GetNeighboringSeaVertex on a 2 x 4 cell grid (3 x 5 vertices): index 7 really is (1, 2) but the
        // original derives the row as index / vertsZ = 7 / 5 = 1 (sic), so it scans the ring around (1, 1):
        // west = index 3, north = 1, east = 5 and south = 7 (the vertex itself).
        cSTETerrain tall;
        tall.Init(2, 4, tuning2);
        for (cSTETerrain::Vertex& vertex : tall.Vertices()) vertex.altitude = 100.0f;
        CHECK(tall.GetNeighboringSeaVertex(7) == -1);
        tall.Vertices()[static_cast<std::size_t>(tall.GetVertIndex(0, 1))].altitude = 5.0f;   // west
        CHECK(tall.GetNeighboringSeaVertex(7) == tall.GetVertIndex(0, 1));
        tall.Vertices()[static_cast<std::size_t>(tall.GetVertIndex(0, 1))].altitude = 100.0f;
        tall.Vertices()[static_cast<std::size_t>(tall.GetVertIndex(1, 0))].altitude = 5.0f;   // north
        CHECK(tall.GetNeighboringSeaVertex(7) == tall.GetVertIndex(1, 0));
        tall.Vertices()[static_cast<std::size_t>(tall.GetVertIndex(1, 0))].altitude = 100.0f;
        tall.Vertices()[static_cast<std::size_t>(tall.GetVertIndex(2, 1))].altitude = 5.0f;   // east
        CHECK(tall.GetNeighboringSeaVertex(7) == tall.GetVertIndex(2, 1));
        tall.Vertices()[static_cast<std::size_t>(tall.GetVertIndex(2, 1))].altitude = 100.0f;
        // the quirk: the south neighbour is (1, 7 / 5 + 1) = (1, 2) = index 7 itself
        tall.Vertices()[7].altitude = 5.0f;
        CHECK(tall.GetNeighboringSeaVertex(7) == 7);
        // and the north-west corner (0, 0) -- the port's old pick -- is *not* consulted any more
        tall.Vertices()[7].altitude = 100.0f;
        tall.Vertices()[static_cast<std::size_t>(tall.GetVertIndex(0, 0))].altitude = 5.0f;
        CHECK(tall.GetNeighboringSeaVertex(7) == -1);
    }
    {
        // ---- DetectShoreVertices / GetShoreStartingAtVert / TrackShore*: a straight shoreline along z = 6
        //      of a 12 x 12 cell grid (13 x 13 vertices); z >= 7 is sea
        cSTETerrain shore;
        TerrainTuning seaTuning;
        seaTuning.seaLevel = 20.0f;
        shore.Init(12, 12, seaTuning);
        const auto vertexAt = [&shore](std::uint32_t x, std::uint32_t z) {
            return static_cast<std::size_t>(shore.GetVertIndex(static_cast<std::int32_t>(x),
                                                               static_cast<std::int32_t>(z)));
        };
        for (cSTETerrain::Vertex& vertex : shore.Vertices()) vertex.altitude = 100.0f;
        for (std::uint32_t z = 7; z <= 12; ++z)
            for (std::uint32_t x = 0; x <= 12; ++x) shore.Vertices()[vertexAt(x, z)].altitude = 10.0f;
        // dirty every shore bit first: DetectShoreVertices clears the mask over the whole grid rect
        for (cSTETerrain::Vertex& vertex : shore.Vertices()) vertex.flags |= kVertexShore;
        shore.DetectShoreVertices();
        for (std::uint32_t x = 0; x <= 12; ++x) {
            CHECK((shore.Vertices()[vertexAt(x, 6)].flags & kVertexShore) != 0);   // the shoreline row
            CHECK((shore.Vertices()[vertexAt(x, 5)].flags & kVertexShore) == 0);   // inland
            CHECK((shore.Vertices()[vertexAt(x, 7)].flags & kVertexShore) == 0);   // the sea itself
        }
        // the west and east ends of the row are shore vertices too (a sea neighbour in one direction)
        CHECK((shore.Vertices()[vertexAt(0, 6)].flags & kVertexShore) != 0);

        std::deque<std::int32_t> run;
        run.push_back(-1);                                     // GetShoreStartingAtVert clears the deque
        shore.GetShoreStartingAtVert(shore.GetVertIndex(0, 6), run, kVertexMark1);
        CHECK(run.size() == 13);
        CHECK(run.back() == shore.GetVertIndex(0, 6));         // the start vertex stays last
        CHECK(run.front() == shore.GetVertIndex(12, 6));       // TrackShoreWithLandToTheLeft walks east
        for (std::uint32_t x = 0; x <= 12; ++x) {
            const cSTETerrain::Vertex& vertex = shore.Vertices()[vertexAt(x, 6)];
            CHECK((vertex.flags & kVertexMark1) != 0);         // every visited vertex is marked
            CHECK((shore.Vertices()[vertexAt(x, 5)].flags & kVertexMark1) == 0);
        }
        // the order is strictly west to east: (12,6), (11,6), ... (0,6)
        for (std::uint32_t i = 0; i <= 12; ++i)
            CHECK(run[static_cast<std::size_t>(i)] == shore.GetVertIndex(12 - static_cast<std::int32_t>(i), 6));

        // ---- AnalyzeShoreAndCreateCliffs / CreateShoreCliff: five inland candidates per vertex, a random
        //      cliff height above the sea, mark-3 on both ends and the 5 candidate heights
        TerrainStatics& tuningStatics = cSTETerrain::Statics();
        tuningStatics.minShoreCliffSize = 3;
        tuningStatics.maxShoreCliffSize = 20;                  // the whole 13-vertex run becomes one cliff
        tuningStatics.minShoreCliffCandidateHeightSum = 100.0f;   // 6 x 80 m of land is well above it
        tuningStatics.minShoreCliffHeight = 30.0f;
        tuningStatics.maxShoreCliffHeight = 40.0f;
        tuningStatics.windErosionFactor = 0.0f;
        CHECK(shore.AnalyzeShoreAndCreateCliffs(run));
        for (std::uint32_t x = 0; x <= 12; ++x) {
            const float ridge = shore.Vertices()[vertexAt(x, 6)].altitude;
            CHECK(ridge > 20.0f && ridge < 80.0f);             // sea + 30..40, jittered
            CHECK((shore.Vertices()[vertexAt(x, 6)].flags & kVertexMark3) != 0);
            CHECK((shore.Vertices()[vertexAt(x, 7)].flags & kVertexMark3) != 0);
            for (std::uint32_t z = 1; z <= 5; ++z)
                CHECK(shore.Vertices()[vertexAt(x, z)].altitude < 80.0f);   // the five candidates moved
            CHECK(Near(shore.Vertices()[vertexAt(x, 0)].altitude, 100.0f));  // the sixth step inland did not
        }

        // A run of exactly kMinShoreCliffSize is dropped (the original flushes only when count > minimum).
        cSTETerrain shortRun;
        shortRun.Init(12, 12, seaTuning);
        for (cSTETerrain::Vertex& vertex : shortRun.Vertices()) vertex.altitude = 100.0f;
        for (std::uint32_t z = 7; z <= 12; ++z)
            for (std::uint32_t x = 0; x <= 12; ++x)
                shortRun.Vertices()[static_cast<std::size_t>(shortRun.GetVertIndex(static_cast<std::int32_t>(x),
                                                                                   static_cast<std::int32_t>(z)))]
                    .altitude = 10.0f;
        std::deque<std::int32_t> exact;
        for (std::uint32_t x = 0; x <= 12; ++x)
            exact.push_back(shortRun.GetVertIndex(static_cast<std::int32_t>(x), 6));
        tuningStatics.minShoreCliffSize = 13;                  // == the run's length
        CHECK(!shortRun.AnalyzeShoreAndCreateCliffs(exact));
        CHECK(Near(shortRun.Vertices()[static_cast<std::size_t>(shortRun.GetVertIndex(6, 6))].altitude, 100.0f));
        tuningStatics.minShoreCliffSize = 12;                  // one shorter: a cliff is created
        CHECK(shortRun.AnalyzeShoreAndCreateCliffs(exact));
        CHECK(!Near(shortRun.Vertices()[static_cast<std::size_t>(shortRun.GetVertIndex(6, 6))].altitude, 100.0f));

        // ---- CreateShoreCliffs end to end: returns true, keeps the ridge and clears the scratch marks
        cSTETerrain cliffs;
        cliffs.Init(12, 12, seaTuning);
        for (cSTETerrain::Vertex& vertex : cliffs.Vertices()) vertex.altitude = 100.0f;
        for (std::uint32_t z = 7; z <= 12; ++z)
            for (std::uint32_t x = 0; x <= 12; ++x)
                cliffs.Vertices()[static_cast<std::size_t>(cliffs.GetVertIndex(static_cast<std::int32_t>(x),
                                                                              static_cast<std::int32_t>(z)))]
                    .altitude = 10.0f;
        tuningStatics.minShoreCliffSize = 3;
        tuningStatics.maxShoreCliffSize = 20;
        CHECK(cliffs.CreateShoreCliffs(false));
        int raised = 0;
        for (std::uint32_t x = 0; x <= 12; ++x) {
            const std::size_t index = static_cast<std::size_t>(cliffs.GetVertIndex(static_cast<std::int32_t>(x), 6));
            if (cliffs.Vertices()[index].altitude < 80.0f) ++raised;
            // the three scratch marks (1, 2 and 4) are cleared over the whole grid at the end
            for (std::uint32_t z = 0; z <= 12; ++z) {
                const std::size_t any = static_cast<std::size_t>(cliffs.GetVertIndex(static_cast<std::int32_t>(x),
                                                                                    static_cast<std::int32_t>(z)));
                CHECK((cliffs.Vertices()[any].flags & (kVertexMark1 | kVertexMark2 | kVertexMark3)) == 0);
            }
        }
        CHECK(raised == 13);
        // no shore bit survives? DetectShoreVertices ran and the shore bit is not one of the cleared marks
        CHECK((cliffs.Vertices()[static_cast<std::size_t>(cliffs.GetVertIndex(4, 6))].flags & kVertexShore) != 0);

        // ---- MarkRegion: an 8-cell-free halo of mark-2 vertices around the mark-3 vertices, land only
        cSTETerrain marked;
        marked.Init(8, 8, seaTuning);
        for (cSTETerrain::Vertex& vertex : marked.Vertices()) vertex.altitude = 100.0f;
        marked.Vertices()[static_cast<std::size_t>(marked.GetVertIndex(2, 2))].altitude = 10.0f;   // sea
        marked.Vertices()[static_cast<std::size_t>(marked.GetVertIndex(4, 4))].flags |= kVertexMark3;
        marked.MarkRegion(2, &cSTETerrain::IsVertexWithMark3, &cSTETerrain::IsLandAndNotMark3, kVertexMark2);
        for (std::int32_t z = 0; z <= 8; ++z) {
            for (std::int32_t x = 0; x <= 8; ++x) {
                const bool inside = x >= 2 && x <= 6 && z >= 2 && z <= 6;
                const bool isCliff = x == 4 && z == 4;
                const bool isSea = x == 2 && z == 2;
                const bool markedOn = (marked.Vertices()[static_cast<std::size_t>(marked.GetVertIndex(x, z))].flags &
                                       kVertexMark2) != 0;
                CHECK(markedOn == (inside && !isCliff && !isSea));
            }
        }

        // ---- SmoothenMarkedRegion: the talus thresholds, cross and diagonal
        tuningStatics.crossHeightDifferenceThreshold = 5.0f;
        tuningStatics.diagonalHeightDifferenceThreshold = 5.0f;
        tuningStatics.windErosionFactor = 0.5f;
        cSTETerrain smooth;
        smooth.Init(4, 4, seaTuning);
        for (cSTETerrain::Vertex& vertex : smooth.Vertices()) vertex.altitude = 95.0f;   // the pair's average
        smooth.Vertices()[static_cast<std::size_t>(smooth.GetVertIndex(0, 0))].altitude = 100.0f;
        smooth.Vertices()[static_cast<std::size_t>(smooth.GetVertIndex(1, 0))].altitude = 90.0f;
        smooth.Vertices()[static_cast<std::size_t>(smooth.GetVertIndex(0, 0))].flags |= kVertexMark2;
        smooth.Vertices()[static_cast<std::size_t>(smooth.GetVertIndex(1, 0))].flags |= kVertexMark2;
        smooth.SmoothenMarkedRegion(TerrainRect{0, 0, 4, 4}, 1, kVertexMark2);
        // 10 m apart against a 5 m threshold: both move by (10 - 5) * 0.5, and nothing else moves
        CHECK(Near(smooth.Vertices()[static_cast<std::size_t>(smooth.GetVertIndex(0, 0))].altitude, 97.5f));
        CHECK(Near(smooth.Vertices()[static_cast<std::size_t>(smooth.GetVertIndex(1, 0))].altitude, 92.5f));
        CHECK(Near(smooth.Vertices()[static_cast<std::size_t>(smooth.GetVertIndex(2, 0))].altitude, 95.0f));
        CHECK(Near(smooth.Vertices()[static_cast<std::size_t>(smooth.GetVertIndex(1, 1))].altitude, 95.0f));
        // windErosionFactor 0: the pass cannot move anything
        tuningStatics.windErosionFactor = 0.0f;
        smooth.SmoothenMarkedRegion(TerrainRect{0, 0, 4, 4}, 3, kVertexMark2);
        CHECK(Near(smooth.Vertices()[static_cast<std::size_t>(smooth.GetVertIndex(0, 0))].altitude, 97.5f));
        CHECK(Near(smooth.Vertices()[static_cast<std::size_t>(smooth.GetVertIndex(1, 0))].altitude, 92.5f));
        // a diagonal pair uses kDiagonalHtDiffThreshold, not the cross one
        tuningStatics.windErosionFactor = 0.5f;
        tuningStatics.crossHeightDifferenceThreshold = 5.0f;
        tuningStatics.diagonalHeightDifferenceThreshold = 2.0f;
        cSTETerrain diagonal;
        diagonal.Init(4, 4, seaTuning);
        for (cSTETerrain::Vertex& vertex : diagonal.Vertices()) vertex.altitude = 97.5f;
        diagonal.Vertices()[static_cast<std::size_t>(diagonal.GetVertIndex(0, 0))].altitude = 100.0f;
        diagonal.Vertices()[static_cast<std::size_t>(diagonal.GetVertIndex(1, 1))].altitude = 95.0f;
        diagonal.Vertices()[static_cast<std::size_t>(diagonal.GetVertIndex(0, 0))].flags |= kVertexMark2;
        diagonal.Vertices()[static_cast<std::size_t>(diagonal.GetVertIndex(1, 1))].flags |= kVertexMark2;
        diagonal.SmoothenMarkedRegion(TerrainRect{0, 0, 4, 4}, 1, kVertexMark2);
        // 5 m apart against the 2 m diagonal threshold: (5 - 2) * 0.5 = 1.5 m each
        CHECK(Near(diagonal.Vertices()[static_cast<std::size_t>(diagonal.GetVertIndex(0, 0))].altitude, 98.5f));
        CHECK(Near(diagonal.Vertices()[static_cast<std::size_t>(diagonal.GetVertIndex(1, 1))].altitude, 96.5f));
        CHECK(Near(diagonal.Vertices()[static_cast<std::size_t>(diagonal.GetVertIndex(2, 2))].altitude, 97.5f));
        // leave the shared statics as the later blocks expect them
        tuningStatics.crossHeightDifferenceThreshold = 16.0f;
        tuningStatics.diagonalHeightDifferenceThreshold = 22.624f;
        tuningStatics.windErosionFactor = 0.0f;
        tuningStatics.minShoreCliffSize = 0;
        tuningStatics.maxShoreCliffSize = 0;
        tuningStatics.minShoreCliffCandidateHeightSum = 0.0f;
        tuningStatics.minShoreCliffHeight = 0.0f;
        tuningStatics.maxShoreCliffHeight = 0.0f;
    }
    {
        // ---- DoWindErosion / DoWaterErosion / DoErosion: the erosion kernels
        TerrainStatics& statics = cSTETerrain::Statics();
        TerrainTuning erosionTuning;
        erosionTuning.seaLevel = 20.0f;
        statics.maxTerrainHeight = 1000.0f;
        statics.minTerrainHeight = 0.0f;
        statics.crossHeightDifferenceThreshold = 16.0f;
        statics.diagonalHeightDifferenceThreshold = 22.624f;
        statics.windErosionFactor = 1.0f;
        // talus: a 20 m spike against the 16 m cross threshold drops by 4 m and only its west neighbour
        // (the first direction that exceeds the threshold) rises, because the spike is then level with the
        // other three
        cSTETerrain wind;
        wind.Init(4, 4, erosionTuning);
        for (cSTETerrain::Vertex& vertex : wind.Vertices()) vertex.altitude = 100.0f;
        wind.Vertices()[static_cast<std::size_t>(wind.GetVertIndex(2, 2))].altitude = 120.0f;
        wind.DoWindErosion(1);
        CHECK(Near(wind.Vertices()[static_cast<std::size_t>(wind.GetVertIndex(2, 2))].altitude, 116.0f));
        CHECK(Near(wind.Vertices()[static_cast<std::size_t>(wind.GetVertIndex(1, 2))].altitude, 104.0f));
        CHECK(Near(wind.Vertices()[static_cast<std::size_t>(wind.GetVertIndex(2, 1))].altitude, 100.0f));
        CHECK(Near(wind.Vertices()[static_cast<std::size_t>(wind.GetVertIndex(3, 2))].altitude, 100.0f));
        CHECK(Near(wind.Vertices()[static_cast<std::size_t>(wind.GetVertIndex(2, 3))].altitude, 100.0f));
        statics.windErosionFactor = 0.0f;                  // factor 0: the pass cannot move anything
        wind.DoWindErosion(3);
        CHECK(Near(wind.Vertices()[static_cast<std::size_t>(wind.GetVertIndex(2, 2))].altitude, 116.0f));
        CHECK(Near(wind.Vertices()[static_cast<std::size_t>(wind.GetVertIndex(1, 2))].altitude, 104.0f));

        // water: a slope down towards +x. With no flow step only the initial fill and the final sea-level
        // reset run; one flow step erodes the border vertices (they lose all their water) and the interior.
        statics.windErosionFactor = 1.0f;
        statics.maxSedimentPerUnitWater = 5.0f;
        statics.rainFallFactor = 0.01f;
        statics.minHydraulicErosionRate = 0.02f;
        statics.maxHydraulicErosionRate = 0.06f;
        statics.hydraulicErosionRateRampBegin = 5.128f;
        statics.hydraulicErosionRateRampEnd = 38.14f;
        statics.hydraulicDepositRate = 0.2f;
        cSTETerrain still;
        still.Init(4, 4, erosionTuning);
        const auto slopeAt = [](std::uint32_t x) { return 40.0f - 5.0f * static_cast<float>(x); };
        for (std::uint32_t z = 0; z <= 4; ++z)
            for (std::uint32_t x = 0; x <= 4; ++x)
                still.Vertices()[static_cast<std::size_t>(still.GetVertIndex(static_cast<std::int32_t>(x),
                                                                            static_cast<std::int32_t>(z)))]
                    .altitude = slopeAt(x);
        for (std::size_t i = 0; i < still.VertexCount(); ++i) {
            still.Vertices()[i].normal[0] = 1.0f;
            still.Sediment()[i] = 3.0f;
        }
        still.DoWaterErosion(TerrainRect{0, 0, 4, 4}, 0);
        for (std::uint32_t z = 0; z <= 4; ++z)
            for (std::uint32_t x = 0; x <= 4; ++x) {
                const std::size_t i = static_cast<std::size_t>(still.GetVertIndex(static_cast<std::int32_t>(x),
                                                                                 static_cast<std::int32_t>(z)));
                CHECK(Near(still.WaterDepth()[i], 20.0f - slopeAt(x)));   // reset to sea - altitude
                CHECK(still.Sediment()[i] == 0.0f);
                CHECK(Near(still.Vertices()[i].normal[0], 0.0f) && Near(still.Vertices()[i].normal[1], 0.0f) &&
                      Near(still.Vertices()[i].normal[2], 0.0f));
                CHECK(Near(still.Vertices()[i].altitude, slopeAt(x)));    // no flow, no erosion
            }

        cSTETerrain flow;
        flow.Init(4, 4, erosionTuning);
        for (std::uint32_t z = 0; z <= 4; ++z)
            for (std::uint32_t x = 0; x <= 4; ++x)
                flow.Vertices()[static_cast<std::size_t>(flow.GetVertIndex(static_cast<std::int32_t>(x),
                                                                           static_cast<std::int32_t>(z)))]
                    .altitude = slopeAt(x);
        flow.DoWaterErosion(TerrainRect{0, 0, 4, 4}, 1);
        // (0,0) is the first vertex: rain gives it 0.4 m of water, it is a border vertex so all of it runs
        // off with no slope, and (2 * 5 * 0.4) * 0.02 m erodes
        CHECK(Near(flow.Vertices()[static_cast<std::size_t>(flow.GetVertIndex(0, 0))].altitude, 39.96f, 1e-3f));
        // (1,0) is on the top border as well: rain 0.35 m, erosion (2 * 5 * 0.35) * 0.02
        CHECK(Near(flow.Vertices()[static_cast<std::size_t>(flow.GetVertIndex(1, 0))].altitude, 34.965f, 1e-3f));
        // (1,1) is interior and flows east (5 m of drop, the 5.128 m ramp start is not reached -> 0.02)
        CHECK(Near(flow.Vertices()[static_cast<std::size_t>(flow.GetVertIndex(1, 1))].altitude, 34.965f, 1e-3f));
        // (2,1) already received (1,1)'s water: rain 0.30 + 0.35 m, so (2 * 5 * 0.65) * 0.02
        CHECK(Near(flow.Vertices()[static_cast<std::size_t>(flow.GetVertIndex(2, 1))].altitude, 29.935f, 1e-3f));
        // every vertex ends up with the sea-level water surface again
        for (std::size_t i = 0; i < flow.VertexCount(); ++i)
            CHECK(Near(flow.WaterDepth()[i], 20.0f - flow.Vertices()[i].altitude));
        // a locked vertex keeps its altitude (but still passes its water on)
        cSTETerrain locked;
        locked.Init(4, 4, erosionTuning);
        for (std::uint32_t z = 0; z <= 4; ++z)
            for (std::uint32_t x = 0; x <= 4; ++x)
                locked.Vertices()[static_cast<std::size_t>(locked.GetVertIndex(static_cast<std::int32_t>(x),
                                                                               static_cast<std::int32_t>(z)))]
                    .altitude = slopeAt(x);
        locked.Vertices()[static_cast<std::size_t>(locked.GetVertIndex(1, 1))].flags |= kVertexLocked;
        locked.DoWaterErosion(TerrainRect{0, 0, 4, 4}, 1);
        CHECK(Near(locked.Vertices()[static_cast<std::size_t>(locked.GetVertIndex(1, 1))].altitude, 35.0f));

        // DoErosion: style 1 is the wind pass with the per-click count, style 0 runs the water kernel and
        // then feeds the *smoothing* count to the wind pass (sic), other styles do nothing.
        statics.numWindErosionIterationsPerUserClick = 0;
        statics.numWaterErosionIterationsPerPlayerClick = 0;
        statics.numFlowStepsForGlobalWaterErosion = 0;
        statics.numSmoothingIterationsAfterWaterErosion = 1;
        statics.imageImportWaterErosionIterations = 0;
        statics.imageImportFlowSteps = 0;
        statics.imageImportSmoothingIterations = 0;
        cSTETerrain dispatch;
        dispatch.Init(4, 4, erosionTuning);
        for (cSTETerrain::Vertex& vertex : dispatch.Vertices()) vertex.altitude = 100.0f;
        dispatch.Vertices()[static_cast<std::size_t>(dispatch.GetVertIndex(2, 2))].altitude = 120.0f;
        dispatch.DoErosion(2, false);                      // unknown style: nothing happens
        CHECK(Near(dispatch.Vertices()[static_cast<std::size_t>(dispatch.GetVertIndex(2, 2))].altitude, 120.0f));
        dispatch.DoErosion(1, false);                      // wind only, but the click count is 0
        CHECK(Near(dispatch.Vertices()[static_cast<std::size_t>(dispatch.GetVertIndex(2, 2))].altitude, 120.0f));
        dispatch.DoErosion(0, false);                      // no water passes, one smoothing pass of wind
        CHECK(Near(dispatch.Vertices()[static_cast<std::size_t>(dispatch.GetVertIndex(2, 2))].altitude, 116.0f));
        // the "from image" counts come from cSC4WinRegionScreen: one water pass with no flow step resets the
        // water surface even though the normal water count is 0
        cSTETerrain image;
        image.Init(4, 4, erosionTuning);
        for (cSTETerrain::Vertex& vertex : image.Vertices()) vertex.altitude = 40.0f;
        statics.imageImportWaterErosionIterations = 1;
        statics.imageImportSmoothingIterations = 0;
        image.WaterDepth().assign(image.VertexCount(), 0.0f);
        image.DoErosion(0, true);
        CHECK(Near(image.WaterDepth()[0], 20.0f - 40.0f));   // the sea-level reset of the import path
        statics.imageImportWaterErosionIterations = 0;
        // leave the shared statics in their binary defaults for the later blocks
        statics.maxTerrainHeight = 200.0f;
        statics.minTerrainHeight = 5.0f;
        statics.crossHeightDifferenceThreshold = 16.0f;
        statics.diagonalHeightDifferenceThreshold = 22.624f;
        statics.windErosionFactor = 0.0f;
        statics.numSmoothingIterationsAfterWaterErosion = 1;
    }
    {
        // ---- StartFlood / DoFloodFlowStep: the 20..40 cell block is flooded with 500 m of water, the first
        //      flow step hands half of it to the dry ring, the rectangle grows and after 500 steps the saved
        //      depths are restored
        cSTETerrain flood;
        TerrainTuning floodTuning;
        floodTuning.seaLevel = 20.0f;
        flood.Init(48, 48, floodTuning);
        for (cSTETerrain::Vertex& vertex : flood.Vertices()) vertex.altitude = 30.0f;
        flood.WaterDepth().assign(flood.VertexCount(), 0.0f);
        host.messages.posts.clear();
        flood.StartFlood();
        CHECK(flood.IsFloodActive());
        const std::size_t floodCentre =
            static_cast<std::size_t>(flood.GetVertIndex(30, 30));
        // the first step drains the block from its rim inwards (the pass updates in place), so the centre
        // only lost a little of its 500 m: 250 halved once per column of the block
        CHECK(flood.WaterDepth()[floodCentre] < 500.0f && 490.0f < flood.WaterDepth()[floodCentre]);
        // the 400 vertices of the block got 500 m each and the first step only moves water between vertices
        double total = 0.0;
        for (float depth : flood.WaterDepth()) total += depth;
        CHECK(Near(static_cast<float>(total), 400.0f * 500.0f, 1.0f));
        // the flow step posts the (already padded) rectangle and redisplays it
        CHECK(!host.messages.posts.empty());
        CHECK(host.messages.posts.back().type == 0x678e6e3a);
        CHECK(host.messages.posts.back().data[0] == 19u * 0x10000u + 41u);   // 20..40 grown by one
        CHECK(host.messages.posts.back().data[1] == 19u * 0x10000u + 41u);
        // the ring just outside the block received water from it
        CHECK(0.0f < flood.WaterDepth()[static_cast<std::size_t>(flood.GetVertIndex(30, 40))]);
        // two more steps keep the flood running
        flood.DoFloodFlowStep();
        flood.DoFloodFlowStep();
        CHECK(flood.IsFloodActive());
        // 500 steps end the flood and restore the depths saved by StartFlood (all zero)
        for (int step = 0; step < 500 && flood.IsFloodActive(); ++step) flood.DoFloodFlowStep();
        CHECK(!flood.IsFloodActive());
        for (float depth : flood.WaterDepth()) CHECK(depth == 0.0f);
        // a tick with the flood running takes a step (and the flood-active flag gates it)
        flood.StartFlood();
        CHECK(flood.IsFloodActive());
        flood.DoAnimationTick(16);
        CHECK(flood.IsFloodActive());
    }
    {
        // ---- GetEffectIDProperty: the two-dword property, anything else fails
        cSTETerrain city;
        const std::uint32_t effect[2] = {0x12345678u, 0x9abcdef0u};
        cRZVariant effectValue;
        effectValue.RefUint32(effect, 2);
        FakeProperty effectProperty(effectValue);
        const std::uint32_t triple[3] = {1u, 2u, 3u};
        cRZVariant tripleValue;
        tripleValue.RefUint32(triple, 3);
        FakeProperty tripleProperty(tripleValue);
        FakePropertyHolder holder;
        holder.properties[0x42u] = &effectProperty;
        holder.properties[0x44u] = &tripleProperty;
        std::uint32_t out[2] = {0, 0};
        CHECK(cSTETerrain::GetEffectIDProperty(&holder, 0x42u, out));
        CHECK(out[0] == 0x12345678u && out[1] == 0x9abcdef0u);
        CHECK(!cSTETerrain::GetEffectIDProperty(&holder, 0x43u, out));       // missing property
        CHECK(!cSTETerrain::GetEffectIDProperty(&holder, 0x44u, out));       // not two dwords
        CHECK(!cSTETerrain::GetEffectIDProperty(nullptr, 0x42u, out));       // no holder
    }
    {
        // ---- StopAccumulatingTextureUpdates re-textures and redisplays the accumulated rectangle
        FakeView cityView;
        host.view = &cityView;
        host.messages.posts.clear();
        host.landUpdates = 0;
        cSTETerrain city;
        CHECK(city.Init());
        city.DoPostCityInit();
        cityView.dirtyRows.clear();
        host.messages.posts.clear();
        city.StartAccumulatingTextureUpdates();
        CHECK(city.IsAccumulatingTextureUpdates());
        city.RedisplayTerrain(true, true, TerrainRect{1, 1, 2, 2}, 0);   // accumulates only
        CHECK(host.messages.posts.size() == 3);                            // the redisplay posts still run
        cityView.dirtyRows.clear();
        host.messages.posts.clear();
        city.StopAccumulatingTextureUpdates();
        CHECK(!city.IsAccumulatingTextureUpdates());
        CHECK(!cityView.dirtyRows.empty());                                // AssignSurfaceTexturesAndColors
        CHECK(host.messages.posts.size() == 1);                            // only the altitude-range post
        // an empty accumulation rectangle does nothing
        city.StartAccumulatingTextureUpdates();
        cityView.dirtyRows.clear();
        city.StopAccumulatingTextureUpdates();
        CHECK(cityView.dirtyRows.empty());
        CHECK(city.Shutdown());
        host.view = nullptr;
    }

    {
        // ---- ReadTuningParameters: the terrain-tuning exemplar's property holder
        FakePropertyHolder exemplar;
        {
            PropertyBag bag(exemplar);
            bag.Uints(0x89a312c2, {8, 16, 32, 64, 128});
            bag.Floats(0x48cd7b20, {150.0f, 160.0f, 170.0f});
            bag.Float(0x48cd7b26, 300.0f);
            bag.Float(0x48cd7b27, 4.0f);
            bag.Float(0x48cd7b21, 2.0f);                    // below the minimum -> clamped to 4
            bag.Float(0x48cd7b2a, 1.0f);                    // below the minimum -> clamped to 4
            bag.Floats(0x48cd7b22, {0.25f, 0.5f});
            bag.Floats(0x48cd7b23, {0.75f, 1.0f});
            bag.Floats(0x48cd7b24, {1.5f, 2.0f});
            bag.Floats(0x48cd7b25, {2.5f, 3.0f});
            bag.Float(0x48cd7b28, 2.5f);
            bag.Float(0x48cd7b30, 0.5f);
            bag.Float(0x48cd7b31, 3.0f);
            bag.Uint(0x48cd7b40, 7);
            bag.Uint(0x48cd7b41, 8);
            bag.Uint(0x48cd7b42, 9);
            bag.Floats(0x48cd7b43, {1.0f, 2.0f, 3.0f});
            bag.Uint(0x48cd7b50, 25);
            bag.Float(0x48cd7b51, 0.125f);
            bag.Float(0x48cd7b52, 0.25f);
            bag.Float(0x48cd7b53, 0.375f);
            bag.Uint(0x48cd7b54, 5);
            bag.Float(0x69ac5640, 12.0f);
            bag.Float(0x69ac5641, 40.0f);
            bag.Uint(0x48cd7b60, 3);                        // minShoreCliffSize: read as an unsigned int
            bag.Uint(0x48cd7b64, 9);                        // maxShoreCliffSize
            bag.Float(0x48cd7b65, 0.25f);
            bag.Float(0x48cd7b61, 4.0f);                    // x6 after the read -> 24
            bag.Float(0x48cd7b62, 2.0f);
            bag.Float(0x48cd7b63, 12.0f);
            bag.Float(0x48cd7b70, -9.0f);
            bag.Float(0x48cd7b71, 180.0f);
            bag.Float(0x48cd7b72, 15.0f);
            bag.Float(0x48cd7b73, 18.0f);
            bag.Uint8(0xa9c283d0, 6);                       // maxBeachWidth
            bag.Float(0xa9c283d1, 7.0f);                    // maxBeachAltitude
            bag.Uint8(0x48cd7b80, 1);
            bag.Uint8(0x48cd7b81, 1);
            bag.Floats(0x48cd7ba0, {10.0f, 20.0f, 30.0f, 40.0f, 50.0f});
            bag.Float(0x89861864, 100.0f);                  // + kVolcanicAshUpdateInterval = / 20
            bag.Uints(0x48cd7bf1, {1, 2, 3});
            bag.Uint(0x48cd7bf3, 4);
            bag.Uint(0x48cd7c01, 6);
            bag.Float(0x48cd7c02, 0.75f);
            host.propertyHolders[1] = &exemplar;
            cSTETerrain::ReadTuningParameters();
            host.propertyHolders.erase(1);
        }
        CHECK(host.propertyHolderRequests >= 1);
        CHECK(host.lastPropertyKey.type == 0x6534284a && host.lastPropertyKey.group == 0x88cd66e9 &&
              host.lastPropertyKey.instance == 1);
        TerrainStatics& statics = cSTETerrain::Statics();
        CHECK(statics.maxTerrainHeight == 300.0f && statics.minTerrainHeight == 4.0f);
        CHECK(statics.seaLevel == 4.0f);                    // 2 clamped up to the minimum
        CHECK(statics.defaultTerrainHeight == 4.0f);        // 1 clamped up to the minimum
        CHECK(statics.maxNormalTerrainHeight.size() == 3 && statics.maxNormalTerrainHeight[2] == 170.0f);
        CHECK(statics.textureRandomizationGridSize.size() == 5 &&
              statics.textureRandomizationGridSize[4] == 128);
        CHECK(statics.imageImportScaleFactor == 2.5f && statics.temperatureRangeFactor == 0.5f);
        CHECK(statics.waterProximityRangeFactor == 3.0f);
        CHECK(statics.numInitialErosionPasses == 7 && statics.numHydraulicIterationsPerInitialErosionPass == 8 &&
              statics.numWindIterationsPerInitialErosionPass == 9);
        CHECK(statics.minCellsInUsefulFlatArea == 25 && statics.autoFlatAreaEdgeSmootheningRadius == 5);
        CHECK(statics.minWaterDepthForDeepSeaBedTexture == 12.0f &&
              statics.maxWaterDepthForDeepSeaBedTexture == 40.0f);
        CHECK(statics.minShoreCliffSize == 3u && statics.maxShoreCliffSize == 9u);
        CHECK(statics.minShoreCliffCandidateHeightSum == 24.0f);   // read as 4, multiplied by 6
        CHECK(statics.maxNormalYForCliff == 0.25f);
        CHECK(statics.minSeaLevel == -9.0f && statics.maxSeaLevel == 180.0f);
        CHECK(statics.maxBeachWidth == 6 && statics.maxBeachAltitude == 7.0f);
        CHECK(statics.autoFlattenUponCreation && statics.shoreCliffsUponCreation);
        CHECK(statics.typicalBrushAngleIncrement.size() == 5 &&
              Near(statics.typicalBrushAngleIncrement[0], 10.0f * 0.017453292f, 1e-5f));
        CHECK(statics.volcanicAshLifeTime == 100.0f && statics.volcanicAshUpdateInterval == 5.0f);
        CHECK(statics.numFlowStepsPerWaterErosionBrushApplication.size() == 3);
        CHECK(statics.numSmoothingIterationsAfterWaterErosion == 4);
        CHECK(statics.edgeReconciliationSmoothingIterations == 6 &&
              statics.edgeReconciliationHeightDifferenceThreshold == 0.75f);
    }
    {
        // ---- ReadTextureInfos / ReadTextureInfo: the per-texture cover curves
        FakePropertyHolder texture;
        {
            PropertyBag bag(texture);
            bag.Float(0x9c2aef0, 0.75f);                                   // the cover weight
            for (std::uint32_t layer = 0; layer < 5; ++layer) {
                // three points: (0,1), (16,0.5), (32,0) -> max input 32, spread ceil(32/16) = 2
                bag.Floats(0x9c2aef3 + layer, {0.0f, 1.0f, 16.0f, 0.5f, 32.0f, 0.0f});
            }
            host.propertyHolders[7] = &texture;
            host.textureResourceKeys = {cGZPersistResourceKey{0x6534284a, 0x89c2a517, 7}};
            cSTETerrain::ReadTextureInfos();
            host.textureResourceKeys.clear();
            host.propertyHolders.erase(7);
        }
        CHECK(host.textureResourceLists >= 1);   // StaticInit's read plus this one
        auto found = cSTETerrain::SurfaceCoverTextures().find(7);
        CHECK(found != cSTETerrain::SurfaceCoverTextures().end());
        if (found != cSTETerrain::SurfaceCoverTextures().end()) {
            CHECK(found->second.weight == 0.75f);
            CHECK(found->second.maxSpread[0] == 2);
            CHECK(found->second.curves[0].size() == 3);
            CHECK(found->second.curveMaxInput[0] == 32.0f);
            CHECK(Near(found->second.curves[4][1].z, 0.5f));
        }
        CHECK(cSTETerrain::Statics().maxSurfaceTextureSpread[0] == 2);
        // a key without a resource is a no-op
        host.textureResourceKeys = {cGZPersistResourceKey{0x6534284a, 0x89c2a517, 9}};
        cSTETerrain::ReadTextureInfos();
        host.textureResourceKeys.clear();
        CHECK(cSTETerrain::SurfaceCoverTextures().count(9) == 1);   // the map entry exists, the properties did not
    }
    {
        // ---- ReadTerrainIniFile: the texture map table and the wind-erosion / hydrology tunables
        auto* ini = new FakeIni();      // ReadTerrainIniFile deletes the ini the host opened
        auto& table = ini->sections["TropicalTextureMapTable"];
        table["HighCliff"] = "0x21";
        table["LowCliff"] = "0x22";
        table["Beach"] = "0x23";
        table["24"] = "10,17,18";                     // temperature 24: moisture 0..2 (comma separated, the first token is column 0)
        table["31"] = "0x40,0x41";
        ini->sections["MiscTerrainTunables"]["RaiseLowerAmount"] = "12.5";
        ini->sections["MiscTerrainTunables"]["EnableSimulation"] = "0";
        ini->sections["WindErosion"]["NumIterationsPerUserClick"] = "7";
        ini->sections["WindErosion"]["NumIterationsPerSimMonth"] = "2";
        ini->sections["WindErosion"]["TalusAngle"] = "45";
        ini->sections["WindErosion"]["ErosionFactor"] = "0.125";
        ini->sections["Hydrology"]["NumWaterErosionIterationsPerPlayerClick"] = "3";
        ini->sections["Hydrology"]["NumFlowStepsForGlobalWaterErosion"] = "11";
        ini->sections["Hydrology"]["SurfaceWaterThreshold"] = "9.5";
        ini->sections["Hydrology"]["RainFallFactor"] = "0.02";
        ini->sections["Hydrology"]["WaterSourceCapacity"] = "6.5";
        ini->sections["Hydrology"]["WaterSinkCapacity"] = "7.5";
        ini->sections["Hydrology"]["MaxSedimentPerUnitWater"] = "8";
        ini->sections["Hydrology"]["MinErosionRate"] = "0.03";
        ini->sections["Hydrology"]["MaxErosionRate"] = "0.09";
        ini->sections["Hydrology"]["HydrErosionRateRampBegin"] = "30";
        ini->sections["Hydrology"]["HydrErosionRateRampEnd"] = "60";
        ini->sections["Hydrology"]["DepositRate"] = "0.4";
        ini->sections["Hydrology"]["SedimentSpreadRateUnderSea"] = "2.0";   // clamped to 1
        ini->sections["Hydrology"]["DepositRateUnderSea"] = "0.04";
        ini->sections["Hydrology"]["SedimentInfluenceOnWaterColor"] = "0.2";
        ini->sections["Hydrology"]["MaxSedimentAlpha"] = "3.0";             // clamped to 1
        ini->sections["Hydrology"]["SedimentColorRed"] = "1";
        ini->sections["Hydrology"]["SedimentColorGreen"] = "2";
        ini->sections["Hydrology"]["SedimentColorBlue"] = "3";
        host.terrainIni = ini;
        cSTETerrain::ReadTerrainIniFile();
        host.terrainIni = nullptr;
        TerrainStatics& s = cSTETerrain::Statics();
        CHECK(s.vertexHeightRaiseLowerAmount == 12.5f);
        CHECK(!s.simulationEnabled);
        CHECK(s.numWindErosionIterationsPerUserClick == 7 && s.numWindErosionIterationsPerSimMonth == 2);
        CHECK(Near(s.talusAngle, 45.0f * 0.017453292f, 1e-5f));
        CHECK(Near(s.crossHeightDifferenceThreshold, std::tan(45.0f * 0.017453292f) * 16.0f, 1e-4f));
        CHECK(Near(s.diagonalHeightDifferenceThreshold, s.crossHeightDifferenceThreshold * 1.4142135f, 1e-3f));
        CHECK(s.windErosionFactor == 0.125f);
        CHECK(s.numWaterErosionIterationsPerPlayerClick == 3 && s.numFlowStepsForGlobalWaterErosion == 11);
        CHECK(s.surfaceWaterThreshold == 9.5f && s.rainFallFactor == 0.02f);
        CHECK(s.waterSourceCapacity == 6.5f && s.waterSinkCapacity == 7.5f);
        CHECK(s.sedimentSpreadRateUnderSea == 1.0f);        // clamped
        CHECK(s.maxSedimentAlpha == 1.0f);                  // clamped
        CHECK(s.sedimentColorRed == 1 && s.sedimentColorGreen == 2 && s.sedimentColorBlue == 3);
        CHECK(Near(s.hydraulicErosionRateRampBegin, std::tan(30.0f * 0.017453292f) * 16.0f, 1e-3f));
        const std::vector<std::uint32_t>& used = cSTETerrain::TexturesUsedInTerrainType(0);
        // the fixed ids, the cliff/beach ids, and the ini's temperature tokens 10, 17, 18, 0x40, 0x41
        // (an ini row with fewer than 15 tokens still adds id 0: the original splits 15 times and converts the empty tail to 0)
        const std::vector<std::uint32_t> expected = {0, 10, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x21, 0x22, 0x23,
                                                      0x40, 0x41, 0xe0};
        CHECK(used == expected);                             // ascending and without duplicates
        CHECK(cSTETerrain::TexturesUsedInTerrainType(3).empty());
    }
    {
        // ---- DecideSurfaceCover: the map selects a texture, the beach band and the cliff mark override it
        TerrainTuning surfaceTuning;
        surfaceTuning.seaLevel = 4.0f;      // the reader's kSeaLevel, so the altitude -> temperature row is 24
        host.cellCountX = 4;
        host.cellCountZ = 4;
        host.terrainType = 0;
        cSTETerrain surface;
        CHECK(surface.Init());              // the full path: the land simulator is needed for the moisture column
        for (cSTETerrain::Vertex& vertex : surface.Vertices()) vertex.altitude = 40.0f;
        // One vertex in the beach band (<= sea level + kMaxBeachAltitude = 11) and one flagged as a cliff.
        const std::int32_t beachIndex = surface.GetVertIndex(0, 0);
        surface.Vertices()[static_cast<std::size_t>(beachIndex)].altitude = 10.0f;
        const std::int32_t cliffIndex = surface.GetVertIndex(4, 4);
        surface.Vertices()[static_cast<std::size_t>(cliffIndex)].flags |= kVertexCliffMark;
        // The texture-info entry for the map's id (17), with a curve that is 1 at distance 0.
        TerrainSurfaceCoverInfo cover;
        cover.weight = 1.0f;
        cover.curves[0] = {{0.0f, 1.0f}, {16.0f, 0.0f}};
        cover.curveMaxInput[0] = 16.0f;
        cover.maxSpread[0] = 1;
        cSTETerrain::SurfaceCoverTextures()[17] = cover;
        host.textureSubTextureCount = 1;
        host.moistureIndex = 1;
        surface.DecideSurfaceCover(TerrainRect{0, 0, 4, 4});
        auto keysOf = [](const cSTETerrain::Vertex& vertex) {
            std::vector<std::uint16_t> keys;
            for (int slot = 0; slot < 3; ++slot) {
                std::uint16_t key = 0;
                std::memcpy(&key, &vertex.surface[6 + slot * 4], sizeof(key));
                if (key != 0) keys.push_back(key);
            }
            return keys;
        };
        auto hasKey = [&](const cSTETerrain::Vertex& vertex, std::uint16_t key) {
            const std::vector<std::uint16_t> keys = keysOf(vertex);
            return std::find(keys.begin(), keys.end(), key) != keys.end();
        };
        // altitude 40: temperature 31 - round(((40-4)/((300-11)*0.5))*30 - 0.5) = 24, moisture 1 -> id 17
        CHECK(hasKey(surface.Vertices()[static_cast<std::size_t>(surface.GetVertIndex(1, 1))], 17));
        CHECK(hasKey(surface.Vertices()[static_cast<std::size_t>(beachIndex)], 0x23));
        CHECK(hasKey(surface.Vertices()[static_cast<std::size_t>(cliffIndex)], 0x22));
        CHECK(host.lastMoistureX > 0.0f && std::fmod(host.lastMoistureX, 16.0f) == 0.0f);   // x * 16 m
        CHECK(!host.changedTerrainTypes.empty() && host.changedTerrainTypes.back() == 0);
        // a water vertex gets the shallow sea-bed id (0x14), which has no info: nothing is added
        surface.WaterDepth()[static_cast<std::size_t>(surface.GetVertIndex(3, 3))] = 5.0f;
        cSTETerrain::SurfaceCoverTextures().erase(17);
        surface.DecideSurfaceCover(TerrainRect{3, 3, 3, 3});
        CHECK(!hasKey(surface.Vertices()[static_cast<std::size_t>(surface.GetVertIndex(3, 3))], 0x14));
        CHECK(hasKey(surface.Vertices()[static_cast<std::size_t>(surface.GetVertIndex(3, 3))], 17));
    }
    {
        // ---- CalculateNormalsAndAssignCliffTextures: flat normals, then a step becomes a cliff
        TerrainTuning normalTuning;
        normalTuning.seaLevel = 20.0f;
        cSTETerrain flat;
        flat.Init(3, 3, normalTuning);
        for (cSTETerrain::Vertex& vertex : flat.Vertices()) vertex.altitude = 10.0f;
        flat.CalculateNormalsAndAssignCliffTextures(TerrainRect{0, 0, 3, 3});
        for (std::uint32_t z = 0; z <= 2; ++z) {          // the last vertex row is not part of the pass
            for (std::uint32_t x = 0; x <= 3; ++x) {
                const cSTETerrain::Vertex& vertex =
                    flat.Vertices()[static_cast<std::size_t>(flat.GetVertIndex(static_cast<std::int32_t>(x),
                                                                               static_cast<std::int32_t>(z)))];
                CHECK(Near(vertex.normal[0], 0.0f) && Near(vertex.normal[1], 1.0f) && Near(vertex.normal[2], 0.0f));
                CHECK((vertex.flags & (kVertexCliffMark | kVertexCliffFlip)) == 0);
            }
        }
        cSTETerrain step;
        step.Init(3, 3, normalTuning);
        for (cSTETerrain::Vertex& vertex : step.Vertices()) vertex.altitude = 10.0f;
        // a single raised vertex makes the cell to its north-west have both triangles steep (the flip case)
        step.Vertices()[static_cast<std::size_t>(step.GetVertIndex(2, 1))].altitude = 90.0f;
        step.CalculateNormalsAndAssignCliffTextures(TerrainRect{0, 0, 3, 3});
        int cliffMarks = 0, flips = 0;
        for (const cSTETerrain::Vertex& vertex : step.Vertices()) {
            if ((vertex.flags & kVertexCliffMark) != 0) ++cliffMarks;
            if ((vertex.flags & kVertexCliffFlip) != 0) ++flips;
        }
        CHECK(cliffMarks > 0);
        CHECK(flips > 0);
        // the flat part of the step keeps a unit y normal
        const cSTETerrain::Vertex& west = step.Vertices()[static_cast<std::size_t>(step.GetVertIndex(0, 1))];
        CHECK(Near(west.normal[1], 1.0f, 1e-3f));
    }

    // ================================================================= phase 0: the two brush layouts
    {
        // cISTETerrain::tBrushParameters
        static_assert(sizeof(TerrainBrushParameters) == 0x38);
        static_assert(offsetof(TerrainBrushParameters, currentX) == 0x08);
        static_assert(offsetof(TerrainBrushParameters, zoom) == 0x10);
        static_assert(offsetof(TerrainBrushParameters, valid) == 0x14);
        static_assert(offsetof(TerrainBrushParameters, strengthScale) == 0x18);
        static_assert(offsetof(TerrainBrushParameters, radiusScale) == 0x1c);
        static_assert(offsetof(TerrainBrushParameters, redisplayFlags) == 0x20);
        static_assert(offsetof(TerrainBrushParameters, rect) == 0x24);
        static_assert(offsetof(TerrainBrushParameters, levelAltitude) == 0x34);
        // cSTETerrain::tBrushContext
        static_assert(sizeof(TerrainBrushContext) == 0x38);
        static_assert(offsetof(TerrainBrushContext, x) == 0x00);
        static_assert(offsetof(TerrainBrushContext, previousX) == 0x08);
        static_assert(offsetof(TerrainBrushContext, zoom) == 0x10);
        static_assert(offsetof(TerrainBrushContext, textureValue) == 0x14);
        static_assert(offsetof(TerrainBrushContext, strength) == 0x18);
        static_assert(offsetof(TerrainBrushContext, levelAltitude) == 0x1c);
        static_assert(offsetof(TerrainBrushContext, component) == 0x20);
        static_assert(offsetof(TerrainBrushContext, parameters) == 0x28);
        static_assert(offsetof(TerrainBrushContext, description) == 0x30);
        // cSTETerrain::tBrushComponent (0x78) and tBrushDescription (0x90, fields end at 0x8c)
        static_assert(sizeof(TerrainBrushComponent) == 0x78);
        static_assert(offsetof(TerrainBrushComponent, exemplarID) == 0x00);
        static_assert(offsetof(TerrainBrushComponent, type) == 0x04);
        static_assert(offsetof(TerrainBrushComponent, resource) == 0x08);
        static_assert(offsetof(TerrainBrushComponent, useRotationAngle) == 0x10);
        static_assert(offsetof(TerrainBrushComponent, radiusPerZoom) == 0x18);
        static_assert(offsetof(TerrainBrushComponent, strengthPerZoom) == 0x30);
        static_assert(offsetof(TerrainBrushComponent, textureValue) == 0x48);
        static_assert(offsetof(TerrainBrushComponent, maxDecals) == 0x4c);
        static_assert(offsetof(TerrainBrushComponent, decalInstance) == 0x50);
        static_assert(offsetof(TerrainBrushComponent, randomAmount) == 0x54);
        static_assert(offsetof(TerrainBrushComponent, effectID) == 0x58);
        static_assert(offsetof(TerrainBrushComponent, extraIDs) == 0x60);
        static_assert(sizeof(TerrainBrushDescription) == 0x90);
        static_assert(offsetof(TerrainBrushDescription, brushID) == 0x00);
        static_assert(offsetof(TerrainBrushDescription, rotateToDrag) == 0x04);
        static_assert(offsetof(TerrainBrushDescription, angleIncrement) == 0x08);
        static_assert(offsetof(TerrainBrushDescription, radiusPerZoom) == 0x10);
        static_assert(offsetof(TerrainBrushDescription, components) == 0x28);
        static_assert(offsetof(TerrainBrushDescription, costPerZoom) == 0x40);
        static_assert(offsetof(TerrainBrushDescription, ui) == 0x54);
        static_assert(offsetof(TerrainBrushDescription, exemplarID) == 0x7c);
        static_assert(offsetof(TerrainBrushDescription, soundID) == 0x80);
        static_assert(offsetof(TerrainBrushDescription, rotationPhase) == 0x84);
        static_assert(offsetof(TerrainBrushDescription, currentAngle) == 0x88);
    }

    // ==================================================================== phase 1: the brush path
    {
        cSTETerrain brush;
        TerrainTuning brushTuning;
        brushTuning.seaLevel = 20.0f;
        brush.Init(8, 8, brushTuning);
        for (cSTETerrain::Vertex& vertex : brush.Vertices()) vertex.altitude = 10.0f;
        brush.RecomputeWaterLevel({0, 0, 8, 8});
        CHECK(brush.WasCreatedFromImage() == false && brush.CreateDone() == false);

        auto makeDescription = [](std::int32_t type, std::uint8_t textureValue, std::uint32_t exemplar) {
            TerrainBrushDescription description;
            description.brushID = 3;
            description.radiusPerZoom.assign(5, 32.0f);
            TerrainBrushComponent component;
            component.type = type;
            component.exemplarID = exemplar;
            component.textureValue = textureValue;
            component.radiusPerZoom.assign(8, 32.0f);
            component.strengthPerZoom.assign(8, 1.0f);
            component.randomAmount = 0.0f;
            description.components.push_back(component);
            return description;
        };
        auto& brushes = cSTETerrain::BrushDescriptions();
        brushes.clear();
        // a 4x4 texture whose every texel is 10
        host.texturePixels.assign(16, 10);
        host.textureWidth = host.textureHeight = 4;
        host.brushResources[0x1234] = reinterpret_cast<void*>(0x1);
        TerrainBrushParameters parameters;
        parameters.previousX = parameters.currentX = 4 * 16.0f + 8.0f;   // cell (4, 4)
        parameters.previousZ = parameters.currentZ = 4 * 16.0f + 8.0f;
        parameters.zoom = 0;
        parameters.strengthScale = 1.0f;
        parameters.radiusScale = 1.0f;
        parameters.redisplayFlags = 2;

        // unknown brush: refused, and nothing is touched
        CHECK(!brush.ApplyBrush(99, parameters) && IsEmptyTerrainRect(parameters.rect));

        // RaiseLower through ApplyBrush: texture 10 above the component's 2 raises the ground. The caller's flag (+0x14)
        // decides whether the stroke redisplays what it touched; the terrain-changed post carries the caller's flags.
        brushes[3] = makeDescription(0, 2, 0x1234);
        brush.SetDisplayEnabled(true);
        host.messages.posts.clear();
        parameters.valid = false;
        CHECK(brush.ApplyBrush(3, parameters));
        CHECK(!IsEmptyTerrainRect(parameters.rect) && 10.0f < brush.GetAltitudeAtVertex(4, 4) && host.messages.posts.empty());
        {
            const auto raisedOnce = brush.Vertices();                  // the checks below read the ground after one stroke
            parameters.valid = true;
            CHECK(brush.ApplyBrush(3, parameters));
            bool announced = false;
            for (const PostedMessage& post : host.messages.posts) announced = announced || (post.type == 0x278e6003 && post.data[3] == 2);
            CHECK(announced);
            brush.Vertices() = raisedOnce;
        }
        brush.SetDisplayEnabled(false);
        CHECK(TerrainBrushParameters().valid && TerrainBrushParameters().redisplayFlags == 3 && TerrainBrushParameters().levelAltitude == 100.0f);
        CHECK(parameters.rect.left <= parameters.rect.right && parameters.rect.top <= parameters.rect.bottom);
        const std::int32_t centre = brush.GetVertIndex(4, 4);
        CHECK(Near(brush.GetAltitude(centre), 18.0f, 0.01f));
        CHECK(host.brushResourceLoads.size() == 1 && host.brushResourceLoads[0] == 0x1234);

        // Lower when the sampled value is below the component's
        for (cSTETerrain::Vertex& vertex : brush.Vertices()) vertex.altitude = 100.0f;
        host.texturePixels.assign(16, 0);
        brush.ApplyBrush(3, parameters);
        CHECK(Near(brush.GetAltitude(centre), 98.0f, 0.01f));   // lowered by |0 - 2| = 2

        // RaiseLower returns false when the value matches
        host.texturePixels.assign(16, 2);
        TerrainBrushContext context;
        context.textureValue = 2;
        context.component = &brushes[3].components[0];
        CHECK(!brush.BrushFnRaiseLower(centre, context));
        context.textureValue = 5;
        CHECK(brush.BrushFnRaiseLower(centre, context));

        // the texture-less variant runs the same callback over the square
        host.textureInfoAvailable = false;
        for (cSTETerrain::Vertex& vertex : brush.Vertices()) vertex.altitude = 10.0f;
        host.texturePixels.assign(16, 10);
        brush.ApplyBrush(3, parameters);
        // (the original leaves the context's texture byte uninitialised on this path; the port keeps it 0,
        // so the raise/lower goes the other way and the vertex drops by the component's 2)
        CHECK(Near(brush.GetAltitude(centre), 8.0f, 0.01f));
        host.textureInfoAvailable = true;

        // Level (type 5): textureValue * strength / 256 per touch, snapped when close
        {
            cSTETerrain level;
            level.Init(4, 4, brushTuning);
            level.Vertices()[static_cast<std::size_t>(level.GetVertIndex(1, 1))].altitude = 12.0f;
            TerrainBrushDescription description = makeDescription(5, 0, 0x1234);
            TerrainBrushComponent& component = description.components[0];
            TerrainBrushContext ctx;
            ctx.textureValue = 10;
            ctx.strength = 1.0f;
            ctx.levelAltitude = 10.0f;
            ctx.component = &component;
            CHECK(level.BrushFnLevel(level.GetVertIndex(1, 1), ctx));
            CHECK(Near(level.GetAltitude(level.GetVertIndex(1, 1)), 12.0f - 10.0f / 256.0f, 1e-5f));
            level.Vertices()[static_cast<std::size_t>(level.GetVertIndex(1, 1))].altitude = 10.0f + 10.0f / 512.0f;
            CHECK(level.BrushFnLevel(level.GetVertIndex(1, 1), ctx));
            CHECK(Near(level.GetAltitude(level.GetVertIndex(1, 1)), 10.0f));
            ctx.textureValue = 0;
            CHECK(!level.BrushFnLevel(level.GetVertIndex(1, 1), ctx));
        }
        // Volcano ash (type 7): the 0x400 mark
        {
            TerrainBrushComponent component = makeDescription(7, 0, 0).components[0];
            TerrainBrushContext ctx;
            ctx.component = &component;
            ctx.textureValue = 0;
            CHECK(!brush.BrushFnVolcanoAsh(1, ctx));
            ctx.textureValue = 4;
            CHECK(brush.BrushFnVolcanoAsh(1, ctx));
            CHECK((brush.Vertices()[1].flags & 0x400) != 0);
            brush.Vertices()[1].flags &= static_cast<std::uint16_t>(~0x400u);
        }
        // Lake bed (type 8): land is raised toward the level, water is not
        {
            cSTETerrain lake;
            lake.Init(4, 4, brushTuning);
            for (cSTETerrain::Vertex& vertex : lake.Vertices()) vertex.altitude = 25.0f;   // land
            lake.RecomputeWaterLevel({0, 0, 4, 4});
            TerrainBrushComponent component = makeDescription(8, 0, 0).components[0];
            TerrainBrushContext ctx;
            ctx.component = &component;
            ctx.textureValue = 10;
            ctx.strength = 1.0f;
            ctx.levelAltitude = 30.0f;
            ctx.zoom = 0;
            CHECK(lake.BrushFnLakeBed(lake.GetVertIndex(1, 1), ctx));       // land: raise by 10
            CHECK(Near(lake.GetAltitude(lake.GetVertIndex(1, 1)), 35.0f, 0.01f));
            lake.Vertices()[static_cast<std::size_t>(lake.GetVertIndex(2, 2))].altitude = 0.0f;
            lake.WaterDepth()[static_cast<std::size_t>(lake.GetVertIndex(2, 2))] = 5.0f;
            CHECK(!lake.BrushFnLakeBed(lake.GetVertIndex(2, 2), ctx));      // under water: nothing
            CHECK(Near(lake.GetAltitude(lake.GetVertIndex(2, 2)), 0.0f, 1e-6f));
        }
        // Smoothen (type 2): a step relaxes toward the lower neighbour
        {
            cSTETerrain smooth;
            smooth.Init(4, 4, brushTuning);
            for (cSTETerrain::Vertex& vertex : smooth.Vertices()) vertex.altitude = 10.0f;
            smooth.Vertices()[static_cast<std::size_t>(smooth.GetVertIndex(1, 1))].altitude = 60.0f;
            TerrainBrushComponent component = makeDescription(2, 0, 0).components[0];
            TerrainBrushContext ctx;
            ctx.component = &component;
            ctx.textureValue = 10;
            ctx.strength = 1.0f;
            const float before = smooth.GetAltitude(smooth.GetVertIndex(1, 1));
            CHECK(smooth.BrushFnSmoothen(smooth.GetVertIndex(1, 1), ctx));
            CHECK(smooth.GetAltitude(smooth.GetVertIndex(1, 1)) < before);
            ctx.textureValue = 0;
            CHECK(!smooth.BrushFnSmoothen(smooth.GetVertIndex(1, 1), ctx));
        }
        // Paint (type 3) and PlaceEffect (type 4) go through the third overlay / the effects manager
        {
            cSTETerrain paint;
            paint.Init(4, 4, brushTuning);
            FakeView paintView;
            paint.SetView(&paintView);
            paint.DoPostCityInit();
            TerrainBrushDescription description = makeDescription(3, 1, 0x1234);
            description.components[0].maxDecals = 1;
            description.components[0].decalInstance = 0x77;
            TerrainBrushContext ctx;
            ctx.component = &description.components[0];
            ctx.textureValue = 5;
            ctx.strength = 1.0f;
            const int before = paint.BrushPhase(0) == paint.BrushPhase(0) ? static_cast<int>(host.decals.size()) : 0;
            paint.BrushFnPaint(paint.GetVertIndex(2, 2), ctx);
            CHECK(static_cast<int>(host.decals.size()) == before + 1);
            CHECK(host.decals.back().key.instance == 0x77 && host.decals.back().key.type == 0x7ab50e44 &&
                  host.decals.back().key.group == 0x891b0e1a);
            paint.BrushFnPaint(paint.GetVertIndex(2, 2), ctx);   // count 1 <= maxDecals 1: one more
            CHECK(static_cast<int>(host.decals.size()) == before + 2);
            paint.BrushFnPaint(paint.GetVertIndex(2, 2), ctx);   // count 2 > maxDecals: full
            CHECK(static_cast<int>(host.decals.size()) == before + 2);

            TerrainBrushDescription effectDescription = makeDescription(4, 0, 0x1234);
            effectDescription.components[0].effectID[0] = 0x11;
            effectDescription.components[0].effectID[1] = 0x22;
            effectDescription.components[0].textureValue = 3;
            TerrainBrushContext effectContext;
            effectContext.component = &effectDescription.components[0];
            effectContext.textureValue = 9;
            CHECK(!paint.BrushFnPlaceEffect(paint.GetVertIndex(1, 1), effectContext));
            CHECK(!host.effectsCreated.empty());
            FakeTerrainEffect* effect = host.effectsCreated.back();
            CHECK(effect->id0 == 0x11 && effect->id1 == 0x22 && effect->starts == 1 && effect->releases == 1);
            CHECK(effect->placement.x >= 16.0f && effect->placement.x <= 32.0f);
            CHECK(effect->placement.y == std::max(paint.GetAltitude(effect->placement.x, effect->placement.z),
                                                  paint.GetSeaLevel()));
        }
        // Rain (type 9): both the per-vertex and the static mode
        {
            TerrainBrushComponent component = makeDescription(9, 0, 0).components[0];
            TerrainBrushContext ctx;
            ctx.component = &component;
            ctx.textureValue = 7;
            ctx.strength = 20.0f;
            host.rainAdds.clear();
            host.surfaceWaterPerVertexRain = true;
            brush.BrushFnRain(3, ctx);
            CHECK(host.rainAdds.size() == 1 && host.rainAdds[0].index == 3 && Near(host.rainAdds[0].amount, 2.0f));
            ctx.textureValue = component.textureValue;
            brush.BrushFnRain(4, ctx);
            CHECK(host.rainAdds.size() == 1);
        }
        // Flora (type 1): InitFloraBrushInfo filters the ids, the callback plants on land
        {
            host.floraIDs = {5, 6, 7};
            host.brushableFlora = {6, 7};
            brush.InitFloraBrushInfo();
            CHECK(brush.FloraBrushCount() == 2);
            CHECK(brush.FloraBrushIDs().size() == 2 && brush.FloraBrushIDs()[0] == 6 &&
                  brush.FloraBrushIDs()[1] == 7);
            TerrainBrushComponent component = makeDescription(1, 0, 0).components[0];
            TerrainBrushContext ctx;
            ctx.component = &component;
            ctx.textureValue = 10;
            host.floraPlaced.clear();
            host.floraAuras.clear();
            host.cityIsEstablished = true;
            // under water nothing is planted, and the "cannot plant here" sound is posted
            brush.WaterDepth().assign(brush.VertexCount(), 5.0f);
            host.soundPosts.clear();
            const auto randomBefore = host.randomState;
            for (std::int32_t i = 0; i < 200; ++i) brush.BrushFnFlora(brush.GetVertIndex(2 + i % 4, 2 + i % 3), ctx);
            CHECK(host.floraPlaced.empty() && !host.soundPosts.empty());
            host.randomState = randomBefore;
            brush.WaterDepth().assign(brush.VertexCount(), 0.0f);   // dry land: the "can plant" test is the water's depth
            bool planted = false;
            for (std::int32_t i = 0; i < 200 && !planted; ++i) {
                brush.BrushFnFlora(brush.GetVertIndex(2 + i % 4, 2 + i % 3), ctx);
                planted = !host.floraPlaced.empty();
            }
            CHECK(planted);
            if (!host.floraPlaced.empty()) {
                CHECK(host.floraPlaced[0][0] >= 32.0f && host.floraPlaced[0][0] <= 64.0f);
                CHECK(host.floraAuras.size() == host.floraPlaced.size());
            }
        }
        // GetTextureValue: the centre of the texture at the brush centre
        {
            cSTETerrain texture;
            texture.Init(4, 4, brushTuning);
            host.texturePixels.assign(64, 0);
            host.texturePixels[4 * 8 + 4] = 0x5a;
            host.textureWidth = host.textureHeight = 8;
            TerrainBrushDescription description = makeDescription(0, 0, 0x1234);
            description.components[0].radiusPerZoom.assign(8, 128.0f);   // scale = 16*8/128 = 1
            description.components[0].resource = reinterpret_cast<void*>(0x1);
            TerrainBrushParameters params;
            params.currentX = 2 * 16.0f;
            params.currentZ = 2 * 16.0f;
            params.radiusScale = 1.0f;
            params.zoom = 0;
            TerrainBrushContext ctx;
            ctx.component = &description.components[0];
            ctx.parameters = &params;
            ctx.description = &description;
            ctx.zoom = 0;
            ctx.x = params.currentX;
            ctx.z = params.currentZ;
            CHECK(texture.GetTextureValue(2, 2, ctx) && ctx.textureValue == 0x5a);
            CHECK(!texture.GetTextureValue(40, 40, ctx));
        }
        // ApplyWaterErosionBrush / DoPostBrushApplicationOperation
        {
            cSTETerrain erosion;
            erosion.Init(8, 8, brushTuning);
            for (cSTETerrain::Vertex& vertex : erosion.Vertices()) vertex.altitude = 10.0f;
            erosion.RecomputeWaterLevel({0, 0, 8, 8});
            TerrainBrushContext ctx;
            ctx.x = 4 * 16.0f;
            ctx.z = 4 * 16.0f;
            ctx.strength = 1.0f;
            ctx.zoom = 0;
            TerrainRect rect{};
            erosion.ApplyWaterErosionBrush(ctx, 4, rect);
            CHECK(rect.left <= 4 && rect.right >= 4 && rect.top <= 4 && rect.bottom >= 4);
            TerrainRect padded{4, 4, 4, 4};
            erosion.DoPostBrushApplicationOperation(2, ctx, padded);
            CHECK(padded.left == 3 && padded.right == 5 && padded.top == 3 && padded.bottom == 5);
            erosion.DoPostBrushApplicationOperation(7, ctx, rect);
            CHECK(!erosion.VolcanicAshRegions().empty());
        }
        // BeginBrushScrub / EndBrushScrub
        {
            host.soundPosts.clear();
            host.brushScrubEnds.clear();
            TerrainBrushDescription description = makeDescription(0, 0, 0x1234);
            description.exemplarID = 0xaa;
            description.soundID = 0xbb;
            brushes[3] = description;
            host.messages.posts.clear();
            brush.BeginBrushScrub(3);
            CHECK(host.soundPosts.size() == 1 && host.soundPosts[0].type == 0xea451903 &&
                  host.soundPosts[0].data1 == 0xaa && host.soundPosts[0].data2 == 3);
            CHECK(!host.messages.posts.empty() && host.messages.posts.back().type == 0xea3b0af1);
            host.soundPosts.clear();
            brush.EndBrushScrub(3, 9);
            CHECK(host.soundPosts.size() == 2);
            CHECK(host.soundPosts[0].type == 0x6a451908 && host.soundPosts[1].type == 0xea451903);
            CHECK(host.brushScrubEnds.size() == 1 && host.brushScrubEnds[0].second == 9);
            CHECK(host.messages.posts.back().type == 0x6a3b0afd);
        }
        brushes.clear();
    }

    // ================================================================= phase 2: the brush readers
    {
        cSTETerrain::ReleaseBrushDescriptions();
        host.brushResourceKeys = {{0x6534284a, 0x6a01fc2a, 77}};
        FakePropertyHolder holder;
        PropertyBag bag(holder);
        bag.Float(0x8a020102, 45.0f);                     // the angle increment in degrees
        bag.Floats(0x8a020101, {1.0f, 2.0f, 3.0f, 4.0f, 5.0f});
        bag.Uint(0x8a020103, 0xabc);
        bag.Uint(0x8a020104, 0xdef);
        // one component: type 0, exemplar 0x1234, radius/strength arrays, texture value 3
        bag.Uint(0x8a020205, 0);
        bag.Floats(0x8a020202, {8.0f, 16.0f, 24.0f, 32.0f, 40.0f});
        bag.Floats(0x8a020203, {1.0f, 1.0f, 1.0f, 1.0f, 1.0f});
        bag.Uint8(0x8a020201, 0);
        bag.Uint(0x8a020200, 0x1234);
        bag.Uint8(0x8a020204, 3);
        // the UI properties
        bag.Uints(0xaa2ed800, {0x11, 0x22});
        bag.Uint(0xaa2ed804, 0x33);
        host.propertyHolders[77] = &holder;
        cSTETerrain::ReadBrushDescriptions();
        auto& brushes = cSTETerrain::BrushDescriptions();
        CHECK(brushes.size() == 1);
        const TerrainBrushDescription& read = brushes[77];
        CHECK(read.brushID == 77);
        CHECK(read.rotateToDrag && Near(read.angleIncrement, 45.0f * 0.017453292f, 1e-5f));
        CHECK(read.costPerZoom[0] == 1.0f && read.costPerZoom[4] == 5.0f);
        CHECK(read.exemplarID == 0xabc && read.soundID == 0xdef);
        CHECK(read.radiusPerZoom.size() == 5 && read.radiusPerZoom[0] == 8.0f && read.radiusPerZoom[4] == 40.0f);
        CHECK(read.components.size() == 1);
        const TerrainBrushComponent& component = read.components[0];
        CHECK(component.type == 0 && component.exemplarID == 0x1234 && component.textureValue == 3);
        CHECK(component.radiusPerZoom.size() == 5 && component.radiusPerZoom[1] == 16.0f);
        CHECK(component.strengthPerZoom.size() == 5 && component.strengthPerZoom[0] == 1.0f);
        CHECK(read.ui.values[0] == 0x11 && read.ui.values[1] == 0x22 && read.ui.values[8] == 0x33);
        CHECK(read.rotationPhase >= 0.0f && read.rotationPhase < 6.2831855f);
        CHECK(read.currentAngle == 0.0f);
        // a second read is a no-op, a missing holder is skipped and a bad type drops the description
        cSTETerrain::ReadBrushDescription({0x6534284a, 0x6a01fc2a, 77}, nullptr);
        CHECK(brushes.size() == 1);
        cSTETerrain::ReadBrushDescription({0x6534284a, 0x6a01fc2a, 78}, nullptr);   // no holder
        CHECK(brushes.size() == 1);
        FakePropertyHolder badHolder;
        PropertyBag badBag(badHolder);
        badBag.Uint(0x8a020205, 12);                       // type > 11
        host.propertyHolders[79] = &badHolder;
        cSTETerrain::ReadBrushDescription({0x6534284a, 0x6a01fc2a, 79}, nullptr);
        CHECK(brushes.count(79) == 0);
        // without the angle property the reader keeps the fallback (rotate with the binary's 0.5 degree)
        FakePropertyHolder minimalHolder;
        PropertyBag minimalBag(minimalHolder);
        minimalBag.Uint(0x8a020205, 6);                    // type 6 needs no exemplar
        minimalBag.Floats(0x8a020202, {1.0f, 2.0f, 3.0f, 4.0f, 5.0f});
        minimalBag.Floats(0x8a020203, {1.0f, 1.0f, 1.0f, 1.0f, 1.0f});
        host.propertyHolders[80] = &minimalHolder;
        cSTETerrain::ReadBrushDescription({0x6534284a, 0x6a01fc2a, 80}, nullptr);
        CHECK(brushes.count(80) == 1 && brushes[80].rotateToDrag);
        CHECK(Near(brushes[80].angleIncrement, 0.008726646f, 1e-6f));
        CHECK(brushes[80].components.size() == 1 && brushes[80].components[0].randomAmount == 6.2831855f);
        host.propertyHolders.erase(79);
        host.propertyHolders.erase(80);
        cSTETerrain::ReleaseBrushDescriptions();
    }

    // ============================================================== phase 3: volcanic ash
    {
        cSTETerrain ash;
        TerrainTuning ashTuning;
        ashTuning.seaLevel = 20.0f;
        ash.Init(8, 8, ashTuning);
        for (cSTETerrain::Vertex& vertex : ash.Vertices()) vertex.altitude = 10.0f;
        TerrainStatics& statics = cSTETerrain::Statics();
        statics.volcanicAshLifeTime = 100.0f;
        statics.volcanicAshUpdateInterval = 20.0f;
        TerrainSurfaceCoverInfo info;
        info.weight = 1.0f;
        info.maxSpread[0] = 1;
        info.curves[0] = {{0.0f, 1.0f}, {64.0f, 1.0f}};
        cSTETerrain::SurfaceCoverTextures()[0xe0] = info;

        TerrainRect first{2, 2, 3, 3};
        ash.AddVolcanicAshRegion(first);
        CHECK(ash.VolcanicAshRegions().size() == 1);
        TerrainRect overlapping{3, 3, 4, 4};
        ash.AddVolcanicAshRegion(overlapping);              // overlaps at (3,3): merged
        CHECK(ash.VolcanicAshRegions().size() == 1);
        CHECK(ash.VolcanicAshRegions().front().rect.left == 2 && ash.VolcanicAshRegions().front().rect.right == 4);
        TerrainRect apart{7, 7, 8, 8};
        ash.AddVolcanicAshRegion(apart);
        CHECK(ash.VolcanicAshRegions().size() == 2);

        // Spray paints texture 0xe0 around the ash-marked vertices
        ash.Vertices()[static_cast<std::size_t>(ash.GetVertIndex(2, 2))].flags |= 0x400;
        const std::uint16_t before = ash.Vertices()[static_cast<std::size_t>(ash.GetVertIndex(3, 2))].surface[6] |
                                     (ash.Vertices()[static_cast<std::size_t>(ash.GetVertIndex(3, 2))].surface[7] << 8);
        (void)before;
        ash.SprayVolcanicAsh(first);
        bool painted = false;
        for (std::uint32_t z = 1; z <= 3 && !painted; ++z)
            for (std::uint32_t x = 1; x <= 3 && !painted; ++x) {
                const auto& vertex = ash.Vertices()[static_cast<std::size_t>(ash.GetVertIndex(
                    static_cast<std::int32_t>(x), static_cast<std::int32_t>(z)))];
                for (int layer = 0; layer < 3; ++layer) {
                    const std::uint16_t key = static_cast<std::uint16_t>(vertex.surface[6 + layer * 4]) |
                                              (vertex.surface[7 + layer * 4] << 8);
                    if (key == 0xe0) painted = true;
                }
            }
        CHECK(painted);

        // Tick: the update interval resprays, the lifetime expires and clears the marks
        ash.TickVolcanicAshRegions(10);
        CHECK(ash.VolcanicAshRegions().size() == 2);
        ash.TickVolcanicAshRegions(15);                     // 25 ms: past the 20 ms interval
        CHECK(ash.VolcanicAshRegions().size() == 2);
        ash.Vertices()[static_cast<std::size_t>(ash.GetVertIndex(2, 2))].flags |= 0x400;
        ash.TickVolcanicAshRegions(80);                     // 105 ms > 100 ms lifetime
        CHECK(ash.VolcanicAshRegions().empty());
        CHECK((ash.Vertices()[static_cast<std::size_t>(ash.GetVertIndex(2, 2))].flags & 0x400) == 0);
        cSTETerrain::SurfaceCoverTextures().erase(0xe0);
    }

    // ================================================================= phase 4: the waves
    {
        cSTETerrain waves;
        TerrainTuning waveTuning;
        waveTuning.seaLevel = 20.0f;
        waves.Init(64, 64, waveTuning);
        for (cSTETerrain::Vertex& vertex : waves.Vertices()) vertex.altitude = 10.0f;   // sea
        waves.RecomputeWaterLevel({0, 0, 64, 64});
        FakeWaveSystem waveSystem;
        waveSystem.seaTravelDistance = 100.0f;
        waveSystem.maxWaveWidth = 340.0f;
        waveSystem.maxWaveTravelDistance = 200.0f;
        waveSystem.distanceFromShore = 0.0f;
        waveSystem.maxOrientationDifference = 0.17f;
        FakeView waveView;
        waveView.waves = &waveSystem;
        waves.SetView(&waveView);
        // no wave system yet
        CHECK(!waves.IsGoodWaveProducerLocation(400.0f, 400.0f, 0.0f));
        waves.DoPostCityInit();
        CHECK(waves.WaveSystem() == &waveSystem);
        // all sea: the rectangle is fine but the shifted rectangle has no land -> false
        CHECK(!waves.IsGoodWaveProducerLocation(400.0f, 400.0f, 0.0f));
        // a land band across the shifted rectangle (x 600..704) makes the land test pass
        for (std::int32_t z = 0; z <= 64; ++z)
            for (std::int32_t x = 600 / 16; x <= 704 / 16; ++x)
                waves.Vertices()[static_cast<std::size_t>(waves.GetVertIndex(x, z))].altitude = 30.0f;
        waves.RecomputeWaterLevel({0, 0, 64, 64});
        CHECK(waves.IsGoodWaveProducerLocation(400.0f, 400.0f, 0.0f));
        // a land block inside the first rectangle breaks the 80% sea test
        for (std::int32_t z = 320 / 16; z <= 480 / 16; ++z)
            for (std::int32_t x = 400 / 16; x <= 500 / 16; ++x)
                waves.Vertices()[static_cast<std::size_t>(waves.GetVertIndex(x, z))].altitude = 30.0f;
        waves.RecomputeWaterLevel({0, 0, 64, 64});
        CHECK(!waves.IsGoodWaveProducerLocation(400.0f, 400.0f, 0.0f));

        // TryToCreateWaveProducerForShore: a straight run along row 10 (the original's orientation math)
        std::deque<std::int32_t> shore;
        for (std::int32_t x = 4; x <= 12; ++x) shore.push_back(waves.GetVertIndex(x, 10));
        waveSystem.hasMean = false;
        waveSystem.distanceFromShore = 0.0f;
        float outX = 0, outZ = 0, orientation = 0;
        const bool created = waves.TryToCreateWaveProducerForShore(shore, 0, 8, outX, outZ, orientation);
        // the run's middle is vertex (8, 10); the placement is the middle minus distance*cos/sin
        CHECK(Near(outX, 8 * 16.0f, 1e-2f) && Near(outZ, 10 * 16.0f, 1e-2f));
        CHECK(orientation > 0.0f && orientation < 6.2831855f);
        CHECK(created == waves.IsGoodWaveProducerLocation(outX, outZ, orientation));
        // a mean orientation far from the run's rejects it
        waveSystem.hasMean = true;
        waveSystem.meanOrientation = orientation + 1.5f;
        waveSystem.maxOrientationDifference = 0.17f;
        float rejectedX = 0, rejectedZ = 0, rejectedOrientation = 0;
        CHECK(!waves.TryToCreateWaveProducerForShore(shore, 0, 8, rejectedX, rejectedZ, rejectedOrientation));
        waveSystem.hasMean = false;

        // UpdateWaveProducers: a single land row produces shore vertices, places producers, clears mark 1
        for (std::int32_t z = 0; z <= 64; ++z)
            for (std::int32_t x = 0; x <= 64; ++x)
                waves.Vertices()[static_cast<std::size_t>(waves.GetVertIndex(x, z))].altitude =
                    z == 8 ? 30.0f : 10.0f;
        waves.RecomputeWaterLevel({0, 0, 64, 64});
        waveSystem.producers.clear();
        waveSystem.maxProducers = 2;
        waveSystem.seaTravelDistance = 8.0f;
        waveSystem.maxWaveWidth = 340.0f;       // the real constant keeps the walk steps positive
        waveSystem.maxWaveTravelDistance = 8.0f;
        waveSystem.interWaveGap = 64.0f;
        waves.UpdateWaveProducers();
        CHECK(waveSystem.producers.size() <= 2);
        for (const cSTETerrain::Vertex& vertex : waves.Vertices()) CHECK((vertex.flags & 1) == 0);
        for (std::int32_t x = 0; x <= 64; ++x)
            CHECK((waves.Vertices()[static_cast<std::size_t>(waves.GetVertIndex(x, 8))].flags & 0x10) != 0);
        (void)outX;
        (void)outZ;
    }

    // ====================================================== phase 5: create / image import
    {
        TerrainTuning createTuning;
        createTuning.seaLevel = 20.0f;
        // default flat terrain at the region base height
        {
            host.hasRegion = true;
            host.region.bounds = {0, 0, 1, 1};
            cSTETerrain created;
            created.Init(4, 4, createTuning);
            host.newCityName.clear();
            host.newCityImage = nullptr;
            host.newCityObliteratedSegment = nullptr;
            CHECK(created.Create());
            CHECK(created.CreateDone());
            for (const cSTETerrain::Vertex& vertex : created.Vertices()) CHECK(Near(vertex.altitude, 12.0f));
            CHECK(Near(created.MinHeight(), 12.0f) && Near(created.MaxHeight(), 12.0f));
            CHECK(Near(created.GetSeaLevel(), 20.0f));
        }
        // a height image through the spec
        {
            cSTETerrain created;
            created.Init(4, 4, createTuning);
            cSTETerrain::Statics().minTerrainHeight = 5.0f;
            cSTETerrain::Statics().imageImportScaleFactor = 1.5f;
            FakeHeightImage image;
            image.width = image.height = 5;
            image.pixels.resize(25);
            for (std::uint32_t y = 0; y < 5; ++y)
                for (std::uint32_t x = 0; x < 5; ++x) image.pixels[y * 5 + x] = static_cast<std::uint8_t>(x * 16 + y);
            {
                cSTETerrain direct;
                direct.Init(4, 4, createTuning);
                FakeHeightImage directImage;
                directImage.width = directImage.height = 5;
                directImage.pixels = image.pixels;
                CHECK(direct.CreateFromGrayScaleImage(&directImage));
                CHECK(direct.WasCreatedFromImage());
                CHECK(directImage.releases == 1);
                CHECK(Near(direct.Vertices()[static_cast<std::size_t>(direct.GetVertIndex(0, 0))].altitude, 0.0f));
                CHECK(Near(direct.Vertices()[static_cast<std::size_t>(direct.GetVertIndex(4, 0))].altitude, 96.0f));
                CHECK(Near(direct.Vertices()[static_cast<std::size_t>(direct.GetVertIndex(4, 4))].altitude, 102.0f));
            }
            host.newCityImage = &image;
            host.loadedImages.clear();
            CHECK(created.Create());
            CHECK(created.WasCreatedFromImage());
            CHECK(image.releases == 1);
            CHECK(Near(created.Vertices()[static_cast<std::size_t>(created.GetVertIndex(0, 0))].altitude, 5.0f));
            // vertex (4,0) samples image (4, 0) -> 4 * 16 * 1.5 = 96
            CHECK(Near(created.Vertices()[static_cast<std::size_t>(created.GetVertIndex(4, 0))].altitude, 96.0f));
            host.newCityImage = nullptr;
        }
        // a named image goes through the GZCOM loader
        {
            cSTETerrain created;
            created.Init(4, 4, createTuning);
            FakeHeightImage image;
            image.width = 1;
            image.height = 1;
            image.pixels = {10};
            host.newCityName = "terrain.png";
            host.namedImage = &image;
            CHECK(created.Create());
            CHECK(host.loadedImages.size() == 1 && host.loadedImages[0] == "terrain.png");
            CHECK(image.releases == 1);
            host.newCityName.clear();
            host.namedImage = nullptr;
        }
        // an obliterated segment is loaded as-is
        {
            cSTETerrain created;
            created.Init(4, 4, createTuning);
            cGZDBSegmentPackedFile* segment = Open((dir / "obliterated.sc4").string(), true);
            host.newCityObliteratedSegment = segment;
            CHECK(created.Create());
            CHECK(created.CreateDone());
            host.newCityObliteratedSegment = nullptr;
            Close(segment);
        }
        host.hasRegion = false;
    }

    // ------------------------------------------------- the flat-area / terrace pass (phase 5)
    {
        cSTETerrain flatAreas;
        TerrainTuning flatTuning;
        flatTuning.seaLevel = 20.0f;
        flatAreas.Init(8, 8, flatTuning);
        // a 30 m plateau west of x = 4 and a 40 m plateau east of it
        for (std::uint32_t z = 0; z < 9; ++z)
            for (std::uint32_t x = 0; x < 9; ++x)
                flatAreas.Vertices()[static_cast<std::size_t>(flatAreas.GetVertIndex(
                    static_cast<std::int32_t>(x), static_cast<std::int32_t>(z)))].altitude =
                    x < 4 ? 30.0f : 40.0f;
        flatAreas.RecomputeWaterLevel({0, 0, 8, 8});
        // LevelAreaStartingAt with a 5 m budget only takes the 30 m side
        CHECK(flatAreas.LevelAreaStartingAt(flatAreas.GetVertIndex(1, 1), 5.0f, 1));
        for (std::uint32_t z = 0; z < 9; ++z) {
            for (std::uint32_t x = 0; x < 4; ++x)
                CHECK(Near(flatAreas.Vertices()[static_cast<std::size_t>(flatAreas.GetVertIndex(
                    static_cast<std::int32_t>(x), static_cast<std::int32_t>(z)))].altitude, 30.0f));
            for (std::uint32_t x = 4; x < 9; ++x)
                CHECK(Near(flatAreas.Vertices()[static_cast<std::size_t>(flatAreas.GetVertIndex(
                    static_cast<std::int32_t>(x), static_cast<std::int32_t>(z)))].altitude, 40.0f));
        }
        flatAreas.ClearVertexPropertyFlags(1);
        // a minimum area that is not reached rejects the region and clears its marks
        const std::int32_t previousMinimum = cSTETerrain::Statics().minCellsInUsefulFlatArea;
        cSTETerrain::Statics().minCellsInUsefulFlatArea = 1000;
        CHECK(!flatAreas.LevelAreaStartingAt(flatAreas.GetVertIndex(1, 1), 5.0f, 1));
        for (const cSTETerrain::Vertex& vertex : flatAreas.Vertices()) CHECK((vertex.flags & 1) == 0);
        cSTETerrain::Statics().minCellsInUsefulFlatArea = previousMinimum;
        // CreateFlatAreas levels the whole in-band terrace and clears the scratch marks
        cSTETerrain::Statics().minTerraceAltitudeFactor = 0.1f;
        cSTETerrain::Statics().maxTerraceAltitudeFactor = 0.5f;
        cSTETerrain::Statics().maxHeightChangeToFlattenFactor = 0.1f;
        cSTETerrain::Statics().maxTerrainHeight = 200.0f;
        cSTETerrain::Statics().numSmoothingPassesAfterAutoFlattening = 1;
        cSTETerrain::Statics().autoFlatAreaEdgeSmootheningRadius = 1;
        flatAreas.CreateFlatAreas(false);
        float lowest = 1e9f, highest = -1e9f;
        for (const cSTETerrain::Vertex& vertex : flatAreas.Vertices()) {
            lowest = std::min(lowest, vertex.altitude);
            highest = std::max(highest, vertex.altitude);
            CHECK((vertex.flags & 3) == 0);
        }
        CHECK(Near(lowest, 30.0f) && Near(highest, 30.0f));   // the 10 m step is inside the 20 m budget
    }

    // ===================================================== phase 6: the edge reconcile
    {
        cSTETerrain edges;
        TerrainTuning edgeTuning;
        edgeTuning.seaLevel = 20.0f;
        edges.Init(64, 64, edgeTuning);
        for (cSTETerrain::Vertex& vertex : edges.Vertices()) vertex.altitude = 10.0f;
        edges.RecomputeWaterLevel({0, 0, 64, 64});
        TerrainStatics& statics = cSTETerrain::Statics();
        statics.edgeReconciliationSmoothingIterations = 1;
        statics.edgeReconciliationHeightDifferenceThreshold = 0.0f;
        statics.edgeReconciliationSmoothingRadiusCurve = {{0.0f, 1.0f}, {100.0f, 3.0f}};
        host.hasRegion = true;
        host.region.bounds = {0, 0, 1, 1};
        host.region.baseTerrainHeight = 20;                 // * 1.5 = 30
        host.regionalCity = &host.regionalCityStorage;
        host.regionalCityX = 0;
        host.regionalCityZ = 0;
        host.regionalCityWidth = 1;
        host.regionalCityHeight = 1;
        host.popupDialogsEnabled = true;
        host.dialogsSucceed = true;
        host.dialogKinds.clear();
        host.messages.posts.clear();
        edges.ReconcileEdgesWithNeighboringCities(false, true, true);
        CHECK(!host.dialogKinds.empty());
        CHECK(host.dialogKinds.back() == 2);                 // the damage dialog
        // the east and south edges were pulled to the default height (30) and the interior smoothed a bit
        bool edgeRaised = false;
        for (std::uint32_t z = 0; z < 65 && !edgeRaised; ++z)
            if (Near(edges.GetAltitudeAtVertex(64, static_cast<std::int32_t>(z)), 30.0f)) edgeRaised = true;
        CHECK(edgeRaised);
        CHECK(!host.messages.posts.empty() && host.messages.posts.back().type == 0xca63d517);
        // the pure edge helper: two side-by-side 1x1 cities share the vertical line at x = 64
        TerrainPointI aEdge, bEdge;
        CHECK(cSTETerrain::GetEdgeSharedWithNeighboringCity({0, 0, 64, 64}, {64, 0, 128, 64}, aEdge, bEdge));
        CHECK(aEdge.x == 64 && bEdge.x == 64 && aEdge.z == 0 && bEdge.z == 64);
        // the diagonal neighbours still share the vertical line x = 64, but their span is the single corner
        CHECK(cSTETerrain::GetEdgeSharedWithNeighboringCity({0, 0, 64, 64}, {64, 64, 128, 128}, aEdge, bEdge));
        CHECK(aEdge.z == 64 && bEdge.z == 64);
        CHECK(cSTETerrain::GetEdgeSharedWithNeighboringCity({0, 0, 64, 64}, {0, 64, 64, 128}, aEdge, bEdge));
        CHECK(aEdge.z == 64 && bEdge.z == 64 && aEdge.x == 0 && bEdge.x == 64);
        // ComputeMaxHeightsForEdgeVertices keeps the maximum of the two altitudes
        std::vector<float> heights = {5.0f, 50.0f, 7.0f, 9.0f};
        std::map<int, float> merged;
        edges.ComputeMaxHeightsForEdgeVertices({0, 0, 4, 4}, {0, 0, 4, 4}, {0, 0}, {3, 0}, heights, merged);
        CHECK(merged.size() == 4);                          // vertices (0,0)..(3,0)
        CHECK(merged[0] == 5.0f && merged[1] == 50.0f && merged[2] == 7.0f && merged[3] == 9.0f);
        host.hasRegion = false;
        host.regionalCity = nullptr;
    }

    // ======================================= the ApplyTerrainBrush command and the reconcile message
    {
        cSTETerrain scripted;
        TerrainTuning scriptTuning;
        scriptTuning.seaLevel = 20.0f;
        scripted.Init(8, 8, scriptTuning);
        for (cSTETerrain::Vertex& vertex : scripted.Vertices()) vertex.altitude = 10.0f;
        scripted.RecomputeWaterLevel({0, 0, 8, 8});
        auto& brushes = cSTETerrain::BrushDescriptions();
        brushes.clear();
        TerrainBrushDescription description;
        description.brushID = 3;
        description.radiusPerZoom.assign(5, 16.0f);
        TerrainBrushComponent component;
        component.type = 0;
        component.exemplarID = 0x1234;
        component.textureValue = 0;
        component.radiusPerZoom.assign(8, 16.0f);
        component.strengthPerZoom.assign(8, 1.0f);
        description.components.push_back(component);
        brushes[3] = description;
        host.brushResources[0x1234] = reinterpret_cast<void*>(0x1);
        host.texturePixels.assign(16, 10);
        host.textureWidth = host.textureHeight = 4;
        cRZMessage2Standard message;
        message.AddRef();                           // the DoMessage QueryInterface/Release pair
        message.SetType(0xc4a6af6);
        FakeParameterSet params;
        cRZVariant value;
        value.SetValUint32(3);
        params.SetParameter(0, value);
        for (std::uint32_t i = 0; i < 4; ++i) {
            cRZVariant point;
            point.SetValFloat32(i < 2 ? 4 * 16.0f + 8.0f : 5 * 16.0f + 8.0f);
            params.SetParameter(i + 1, point);
        }
        FakeParameterSet result;
        message.SetVoid1(&params);
        message.SetVoid2(&result);
        CHECK(scripted.DoMessage(&message));
        CHECK(result.status == 0);
        CHECK(scripted.GetAltitude(scripted.GetVertIndex(5, 5)) > 10.0f);
        // an out-of-bounds point reports status 2
        cRZMessage2Standard bad;
        bad.AddRef();
        bad.SetType(0xc4a6af6);
        FakeParameterSet badParams;
        badParams.SetParameter(0, value);
        cRZVariant outside;
        outside.SetValFloat32(-100.0f);
        for (std::uint32_t i = 1; i <= 4; ++i) badParams.SetParameter(i, outside);
        FakeParameterSet badResult;
        bad.SetVoid1(&badParams);
        bad.SetVoid2(&badResult);
        CHECK(scripted.DoMessage(&bad));
        CHECK(badResult.status == 2);
        brushes.clear();

        // the reconcile message: a busy world manager re-posts, an idle one reconciles
        host.hasRegion = true;
        host.region.bounds = {0, 0, 1, 1};
        host.regionalCity = &host.regionalCityStorage;
        host.regionalCityX = 0;
        host.regionalCityZ = 0;
        host.regionalCityWidth = 1;
        host.regionalCityHeight = 1;
        FakeMessageTarget target;
        host.messageTarget = &target;
        host.cityLoadInProgress = true;
        host.messages.posts.clear();
        cRZMessage2Standard reconcile;
        reconcile.AddRef();
        reconcile.SetType(0x2a612643);
        CHECK(scripted.DoMessage(&reconcile));
        CHECK(host.messages.posts.size() == 1 && host.messages.posts[0].type == 0x2a612643);
        CHECK(host.messages.lastTarget == &target);
        host.cityLoadInProgress = false;
        host.dialogsSucceed = true;
        host.dialogKinds.clear();
        host.messages.posts.clear();
        CHECK(scripted.DoMessage(&reconcile));
        CHECK(!host.dialogKinds.empty());
        CHECK(host.messages.posts.size() == 1 && host.messages.posts[0].type == 0xca63d517);
        host.messageTarget = nullptr;
        host.hasRegion = false;
        host.regionalCity = nullptr;
    }

    // ============================================================ phase 8: Read adopts the view
    {
        const std::string viewPath = (dir / "readview.dat").string();
        {
            auto* segment = new cSC4DBSegment();
            segment->AddRef();
            segment->Init();
            cRZString p(viewPath.c_str());
            segment->SetPath(p);
            CHECK(segment->Open(true, true));
            cISC4DBSegmentOStream* out = nullptr;
            CHECK(segment->OpenOStream({0xaaaabbbb, 0x33334444, 1}, &out, true) && out != nullptr);
            if (out) t.Write(*out);
            CHECK(segment->CloseOStream(out));
            out->Release();
            CHECK(segment->Close());
            segment->Release();
        }
        FakeView adopted;
        host.view = &adopted;
        host.hasSerializedView = true;
        int marker = 0;
        host.serializedView = &marker;
        auto* reopen = new cSC4DBSegment();
        reopen->AddRef();
        reopen->Init();
        cRZString p2(viewPath.c_str());
        reopen->SetPath(p2);
        CHECK(reopen->Open(true, false));
        cISC4DBSegmentIStream* in = nullptr;
        CHECK(reopen->OpenIStream({0xaaaabbbb, 0x33334444, 1}, &in) && in != nullptr);
        cSTETerrain loaded;
        if (in) {
            CHECK(loaded.Read(*in));
            CHECK(reopen->CloseIStream(in));
            in->Release();
        }
        CHECK(reopen->Close());
        reopen->Release();
        CHECK(host.adoptedViewObject == &marker);
        CHECK(loaded.GetView() == &adopted);            // the loaded view subobject replaced the old one
        host.hasSerializedView = false;
        host.serializedView = nullptr;
        host.view = nullptr;
    }

    std::filesystem::remove_all(dir);
    SetTerrainHost(nullptr);

    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::puts("sc4_steterrain ok");
    return 0;
}
