// cSC4Region — tests the region object against the recovered behaviour: cell map hashing, the
// config-bitmap city locations, region.ini round trip, overflow city files, region creation,
// reload/delete and the tutorial-city reset quirk. All neighbours are a fake IRegionHost; the
// bitmap loader is a fake ISLBufferHost over a real cGZBuffer.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "gz/Buffer.h"
#include "sc4/Region.h"
#include "sc4/SLBuffer.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace fs = std::filesystem;

//---------------------------------------------------------------------------------------------------------------------
// fakes
//---------------------------------------------------------------------------------------------------------------------

struct FakeCity : cISC4RegionalCity {
    FakeCity(int px, int pz, int pw, int ph) : x(px), z(pz), width(pw), height(ph) {}
    virtual ~FakeCity() = default;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == 1 || iid == 0x6a037c24) { *out = this; AddRef(); return true; }
        return false;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        if (--refs == 0) { delete this; return 0; }
        return refs;
    }
    bool Init() override { return true; }
    bool Shutdown() override { ++shutdowns; return true; }
    void SetupCity(cISC4City*) override {}
    std::uint32_t GetCitySerialNumber() const override { return serial; }
    void GetCitySaveFilePath(cIGZString& out) const override { out.AssignFromIGZString(savePath); }

    int x, z, width, height;
    std::uint32_t serial = 0;
    std::uint32_t tutorialGUID = 0;
    cRZString savePath;
    cRZString name;
    int refs = 1;
    int shutdowns = 0;
};

struct FakeHost final : IRegionHost {
    cRZString regionDirectory;
    cRZString downloadDirectory;
    std::map<std::string, FakeCity*> byPath;
    std::vector<std::string> readPaths, writtenCities, recordWrites, renameCalls, tutorialCalls;
    cGZPersistResourceKey lastRecordKey{};
    std::uint32_t resourceCount = 0;      // consecutive city bitmaps starting at kRegionConfigBitmapFirstCity
    std::uint32_t randomValue = 0;
    std::uint32_t serialSeed = 0;
    int recordResult = 0;

    void GetRegionDirectory(cRZString& out) override { out = regionDirectory; }
    void GetRegionDownloadDirectory(cRZString& out) override { out = downloadDirectory; }

    bool ReadRegionalCity(const cIGZString& path, cISC4RegionalCity** out) override {
        readPaths.push_back(path.ToChar());
        auto it = byPath.find(path.ToChar());
        if (it == byPath.end()) return false;
        it->second->AddRef();
        *out = it->second;
        return true;
    }
    bool WriteRegionalCity(const cIGZString& path, cISC4RegionalCity* city) override {
        writtenCities.push_back(std::string(path.ToChar()) + "|" + static_cast<FakeCity*>(city)->name.ToChar());
        return true;
    }
    void CreateSavePathForCity(const cIGZString& directory, const cIGZString& cityName, std::uint32_t serial,
                               cRZString& out) override {
        char buffer[1024];
        std::snprintf(buffer, sizeof buffer, "%s%s-%08x.sc4", directory.ToChar(), cityName.ToChar(), serial);
        out = cRZString(buffer);
        renameCalls.push_back(std::string("download:") + buffer);
    }
    void CreateSavePathForCity(const cIGZString& cityName, std::uint32_t serial, cRZString& out) override {
        char buffer[1024];
        std::snprintf(buffer, sizeof buffer, "%s%s-%08x.sc4", downloadDirectory.ToChar(), cityName.ToChar(), serial);
        out = cRZString(buffer);
        renameCalls.push_back(std::string("city:") + buffer);
    }
    std::uint32_t CreateCitySerialNumber() override { return serialSeed; }
    int ReadBinaryRecordToFile(const cGZPersistResourceKey& key, const cIGZString& path) override {
        lastRecordKey = key;
        recordWrites.push_back(path.ToChar());
        std::ofstream(path.ToChar()).put('x');   // the extracted bitmap is a real file
        return recordResult;
    }
    bool HasResource(const cGZPersistResourceKey& key) override {
        return key.instance >= kRegionConfigBitmapFirstCity &&
               key.instance < kRegionConfigBitmapFirstCity + resourceCount;
    }
    std::uint32_t RandomUint32UniformBelow(std::uint32_t range) override {
        return range == 0 ? 0 : randomValue % range;
    }
    void SetTutorialActive(int guid, bool active) override {
        char buffer[64];
        std::snprintf(buffer, sizeof buffer, "%d:%d", guid, active ? 1 : 0);
        tutorialCalls.push_back(buffer);
    }

    FakeCity* City(const cISC4RegionalCity* city) { return static_cast<FakeCity*>(const_cast<cISC4RegionalCity*>(city)); }
    void GetCityPosition(const cISC4RegionalCity* city, int& x, int& z) override {
        x = City(city)->x;
        z = City(city)->z;
    }
    void GetCitySize(const cISC4RegionalCity* city, int& width, int& height) override {
        width = City(city)->width;
        height = City(city)->height;
    }
    void SetCityPosition(cISC4RegionalCity* city, int x, int z, bool) override {
        City(city)->x = x;
        City(city)->z = z;
    }
    std::uint32_t GetCitySerialNumber(const cISC4RegionalCity* city) override { return City(city)->serial; }
    void SetCitySerialNumber(cISC4RegionalCity* city, std::uint32_t serial) override { City(city)->serial = serial; }
    void GetCitySaveFilePath(const cISC4RegionalCity* city, cRZString& out) override { out = City(city)->savePath; }
    void SetCitySaveFilePath(cISC4RegionalCity* city, const cIGZString& path) override {
        City(city)->savePath.AssignFromIGZString(path);
    }
    void GetCityName(const cISC4RegionalCity* city, cRZString& out) override { out = City(city)->name; }
    std::uint32_t GetTutorialGUID(const cISC4RegionalCity* city) override { return City(city)->tutorialGUID; }
    void SetTutorialGUID(cISC4RegionalCity* city, std::uint32_t guid) override { City(city)->tutorialGUID = guid; }
};

struct FakeBufferHost final : ISLBufferHost {
    cIGZBuffer* buffer = nullptr;
    bool LoadBufferFromFile(const std::string&, std::int32_t, cIGZBuffer** out) override {
        if (!buffer) return false;
        buffer->AddRef();
        *out = buffer;
        return true;
    }
    bool GetBufferResource(const cGZPersistResourceKey&, std::int32_t, bool, cIGZBuffer**) override { return false; }
    bool HasResource(const cGZPersistResourceKey&) override { return false; }
    bool LowQualityTextures() override { return false; }
    cIGZBuffer* CreateBuffer() override { return nullptr; }
};

//---------------------------------------------------------------------------------------------------------------------
// helpers
//---------------------------------------------------------------------------------------------------------------------

static void Touch(const std::string& path) { std::ofstream(path).put('x'); }

static cGZBuffer* MakeBitmap(int width, int height) {
    auto* buffer = new cGZBuffer();
    buffer->AddRef();
    cGZBufferColorType type;
    type.type = kBufferColorXRGB8888;
    type.bitsPerPixel = 32;
    buffer->Init(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), type);
    buffer->Fill(0, nullptr);
    return buffer;
}

static void PaintSquare(cGZBuffer* buffer, int x, int z, int size, std::uint32_t color) {
    for (int i = 0; i < size; ++i)
        for (int j = 0; j < size; ++j)
            buffer->SetPixel(static_cast<std::uint32_t>(x + i), static_cast<std::uint32_t>(z + j), color);
}

static std::string ReadFile(const std::string& path) {
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

//---------------------------------------------------------------------------------------------------------------------

int main() {
    const fs::path root = fs::temp_directory_path() / "sc4_region_test";
    fs::remove_all(root);
    const fs::path regions = root / "Regions";
    const fs::path downloads = root / "Downloads";
    fs::create_directories(regions / "MyRegion");
    fs::create_directories(downloads);
    const std::string regionsDir = regions.string() + "/";
    const std::string downloadsDir = downloads.string() + "/";

    FakeHost host;
    host.regionDirectory = cRZString(regionsDir.c_str());
    host.downloadDirectory = cRZString(downloadsDir.c_str());
    SetRegionHost(&host);

    FakeBufferHost bufferHost;
    SetSLBufferHost(&bufferHost);

    // ---- construction, COM plumbing, config accessors ------------------------------------------------------------
    {
        auto* region = new cSC4Region();
        CHECK(region->GetName()->Strlen() == 0 && region->GetDirectoryName()->Strlen() == 0);
        CHECK(region->GetBaseTerrainType() == cISC4Region::kBaseTerrainType0);
        cSC4Region::sBaseTerrainTypeHeight[0] = 111;
        cSC4Region::sBaseTerrainTypeHeight[1] = 222;
        CHECK(region->GetBaseTerrainHeight() == 111);
        region->SetBaseTerrainType(cISC4Region::kBaseTerrainType1);
        CHECK(region->GetBaseTerrainType() == cISC4Region::kBaseTerrainType1 && region->GetBaseTerrainHeight() == 222);
        std::uint8_t minimum = 0, maximum = 0;
        region->GetWaterPrefs(minimum, maximum);
        CHECK(minimum == 0x3c && maximum == 100);

        void* out = reinterpret_cast<void*>(1);
        CHECK(!region->QueryInterface(0x12345678, &out) && out == reinterpret_cast<void*>(1));
        CHECK(region->QueryInterface(kGZIID_cSC4Region, &out) && out == region);
        CHECK(region->AddRef() == 2 && region->Release() == 1);
        CHECK(region->Release() == 0);   // shuts down and deletes (refcount 0 quirk)
    }

    // ---- the cell map, GetAllCities, RemoveCity and the config-bitmap locations ---------------------------------
    {
        auto* region = new cSC4Region();
        region->AddRef();
        auto* city = new FakeCity(0, 0, 2, 2);
        host.byPath.clear();
        region->InsertCity(city);
        CHECK(region->GetCity(0, 0) != nullptr && region->GetCity(0, 0)->get() == city);
        CHECK(region->GetCity(1, 1)->get() == city);
        CHECK(region->GetCity(2, 1) == nullptr && region->GetCity(-1, 0) == nullptr);
        CHECK(region->IsCitySpaceOccupied(city));
        auto* probe = new FakeCity(1, 1, 1, 1);
        CHECK(region->IsCitySpaceOccupied(probe));
        probe->x = 3;
        CHECK(!region->IsCitySpaceOccupied(probe));
        probe->Release();

        RegionCityList all;
        region->GetAllCities(all);
        CHECK(all.size() == 1 && all.front().get() == city);

        // hash: ((x & 0xf) << 4) | (z & 0xf)
        CHECK(cSC4Region::RegionalCellHashFunction{}(sc4::SC4PointT<int>{0x12, 0x34}) == 0x24);
        CHECK(cSC4Region::RegionalCellHashFunction{}(sc4::SC4PointT<int>{-1, -1}) == 0xff);

        sc4::SC4RectT<int> rect;
        region->GetBoundingRect(rect);
        CHECK(rect.x0 == 0 && rect.z0 == 0 && rect.x1 == 0 && rect.z1 == 0);   // no locations yet

        cGZBuffer* bitmap = MakeBitmap(8, 8);
        PaintSquare(bitmap, 0, 0, 2, 0x0000ff00);   // green: 2x2
        PaintSquare(bitmap, 3, 0, 1, 0x00ff0000);   // red: 1x1
        PaintSquare(bitmap, 4, 4, 4, 0x000000ff);   // blue: 4x4
        PaintSquare(bitmap, 0, 4, 1, 0x00ffff00);   // yellow: not a power of two, ignored
        region->ParseCityLocations(bitmap);
        std::vector<cISC4Region::cLocation> locations;
        region->GetCityLocations(locations);
        CHECK(locations.size() == 3);
        CHECK(locations[0].x == 0 && locations[0].z == 0 && locations[0].size == 1);
        CHECK(locations[1].x == 3 && locations[1].z == 0 && locations[1].size == 0);
        CHECK(locations[2].x == 4 && locations[2].z == 4 && locations[2].size == 2);
        region->GetBoundingRect(rect);
        CHECK(rect.x0 == 0 && rect.z0 == 0 && rect.x1 == 4 && rect.z1 == 4);

        // A single location is a degenerate rectangle: all zero.
        cGZBuffer* one = MakeBitmap(1, 1);
        PaintSquare(one, 0, 0, 1, 0x00ff0000);
        region->ParseCityLocations(one);
        region->GetCityLocations(locations);
        CHECK(locations.size() == 1);
        region->GetBoundingRect(rect);
        CHECK(rect.x0 == 0 && rect.z0 == 0 && rect.x1 == 0 && rect.z1 == 0);
        one->Release();

        cRZAutoRefCount<cISC4RegionalCity>* holder = region->GetCity(0, 0);
        region->RemoveCity(holder);
        CHECK(region->GetCity(0, 0) == nullptr && region->GetCity(1, 1) == nullptr);
        CHECK(city->shutdowns == 1);
        all.clear();
        region->GetAllCities(all);
        CHECK(all.empty());

        region->Release();
        city->Release();
        bitmap->Release();
    }

    // ---- Init: region.ini, config.bmp, city files, overflow city -------------------------------------------------
    FakeCity* placed = new FakeCity(0, 0, 2, 2);
    placed->serial = 101;
    FakeCity* overflow = new FakeCity(1, 0, 1, 1);
    overflow->serial = 102;
    overflow->name = cRZString("B City");
    {
        std::ofstream(regions / "MyRegion" / "region.ini")
            << "[Regional Settings]\nName=Loaded Region\nTerrain type=1\nWater Min=70\nWater Max=140\n";
        Touch((regions / "MyRegion" / "config.bmp").string());
        Touch((regions / "MyRegion" / "A.sc4").string());
        Touch((regions / "MyRegion" / "B.sc4").string());
        host.byPath[(regions / "MyRegion" / "A.sc4").string()] = placed;
        host.byPath[(regions / "MyRegion" / "B.sc4").string()] = overflow;

        cGZBuffer* bitmap = MakeBitmap(8, 8);
        PaintSquare(bitmap, 0, 0, 2, 0x0000ff00);
        bufferHost.buffer = bitmap;

        auto* region = new cSC4Region();
        region->AddRef();
        region->SetDirectoryName(cRZString("MyRegion"));
        CHECK(region->Init());
        CHECK(std::strcmp(region->GetName()->ToChar(), "Loaded Region") == 0);
        CHECK(region->GetBaseTerrainType() == cISC4Region::kBaseTerrainType1);
        std::uint8_t minimum = 0, maximum = 0;
        region->GetWaterPrefs(minimum, maximum);
        CHECK(minimum == 70 && maximum == 140);
        CHECK(region->GetCity(0, 0) != nullptr && region->GetCity(0, 0)->get() == placed);
        CHECK(region->GetCity(1, 0) != nullptr);   // cell of the placed city
        CHECK(overflow->shutdowns == 1);
        CHECK(!fs::exists(regions / "MyRegion" / "B.sc4"));
        CHECK(fs::exists(downloads / "B City-00000066.sc4"));
        CHECK(std::strcmp(placed->savePath.ToChar(), (regions / "MyRegion" / "A.sc4").c_str()) == 0);

        // Shutdown removes the placed city (its file stays), clears the list and rewrites region.ini.
        CHECK(region->Shutdown());
        CHECK(placed->shutdowns == 1);
        CHECK(region->GetCity(0, 0) == nullptr);
        CHECK(fs::exists(regions / "MyRegion" / "region.ini"));
        const std::string ini = ReadFile((regions / "MyRegion" / "region.ini").string());
        CHECK(ini.find("Loaded Region") != std::string::npos);
        CHECK(ini.find("Terrain type=1") != std::string::npos);
        CHECK(ini.find("Water Min=70") != std::string::npos);
        CHECK(ini.find("Water Max=140") != std::string::npos);

        // A second region reads the same configuration back.
        auto* reloaded = new cSC4Region();
        reloaded->AddRef();
        reloaded->SetDirectoryName(cRZString("MyRegion"));
        reloaded->LoadConfig();
        CHECK(std::strcmp(reloaded->GetName()->ToChar(), "Loaded Region") == 0);
        CHECK(reloaded->GetBaseTerrainType() == cISC4Region::kBaseTerrainType1);
        reloaded->GetWaterPrefs(minimum, maximum);
        CHECK(minimum == 70 && maximum == 140);
        reloaded->Release();

        // Delete runs Shutdown again (a no-op now), removes region.ini and config.bmp and then rmdir's the
        // directory. Quirk: the placed city's file is only deleted for cities still in the cell map, so after
        // Shutdown A.sc4 remains and the directory removal fails.
        region->Delete();
        CHECK(!fs::exists(regions / "MyRegion" / "region.ini"));
        CHECK(!fs::exists(regions / "MyRegion" / "config.bmp"));
        CHECK(fs::exists(regions / "MyRegion" / "A.sc4"));
        CHECK(fs::exists(regions / "MyRegion"));
        fs::remove(regions / "MyRegion" / "A.sc4");
        region->Delete();
        CHECK(!fs::exists(regions / "MyRegion"));

        region->Release();
        bitmap->Release();
        bufferHost.buffer = nullptr;
    }
    placed->Release();
    overflow->Release();

    // ---- CreateRegion: sanitised directory, " (n)" collision suffix, config bitmap extraction ----------------
    {
        host.resourceCount = 3;
        host.randomValue = 1;
        host.recordWrites.clear();
        auto* first = cSC4Region::CreateRegion(cRZString("New City"));
        CHECK(std::strcmp(first->GetName()->ToChar(), "New City") == 0);
        CHECK(std::strcmp(first->GetDirectoryName()->ToChar(), "New City") == 0);
        CHECK(fs::is_directory(regions / "New City"));
        CHECK(host.recordWrites.size() == 1);
        CHECK(host.recordWrites[0] == (regions / "New City" / "config.bmp").string());
        CHECK(host.lastRecordKey.type == kRegionConfigBitmapType && host.lastRecordKey.group == kRegionConfigBitmapGroup);
        CHECK(host.lastRecordKey.instance == kRegionConfigBitmapFirstCity + 1);

        auto* second = cSC4Region::CreateRegion(cRZString("New City"));
        CHECK(std::strcmp(second->GetName()->ToChar(), "New City") == 0);
        CHECK(std::strcmp(second->GetDirectoryName()->ToChar(), "New City (2)") == 0);
        CHECK(fs::is_directory(regions / "New City (2)"));

        // The sanitised name is what names the directory; the object keeps the requested name.
        auto* third = cSC4Region::CreateRegion(cRZString("a/b:c"));
        CHECK(std::strcmp(third->GetName()->ToChar(), "a/b:c") == 0);
        CHECK(std::strstr(third->GetDirectoryName()->ToChar(), "/") == nullptr);
        CHECK(fs::is_directory(regions / third->GetDirectoryName()->ToChar()));

        auto* starter = cSC4Region::CreateStarterRegion();
        CHECK(starter != nullptr && starter->GetDirectoryName()->Strlen() != 0);

        host.resourceCount = 0;
        first->Release();
        second->Release();
        third->Release();
        starter->Release();
    }

    // ---- LoadConfig bounds and the ignored bitmap area offset ---------------------------------------------------
    {
        fs::create_directories(regions / "Clamp");
        std::ofstream(regions / "Clamp" / "region.ini")
            << "[Regional Settings]\nName=Clamped\nTerrain type=5\nWater Min=300\nWater Max=255\n";
        auto* region = new cSC4Region();
        region->AddRef();
        region->SetDirectoryName(cRZString("Clamp"));
        region->LoadConfig();
        CHECK(std::strcmp(region->GetName()->ToChar(), "Clamped") == 0);
        CHECK(region->GetBaseTerrainType() == cISC4Region::kBaseTerrainType0);   // 5 rejected
        std::uint8_t minimum = 0, maximum = 0;
        region->GetWaterPrefs(minimum, maximum);
        CHECK(minimum == 0x3c && maximum == 100);   // 300 and 255 rejected (unsigned compare with 0xff)
        region->Release();

        // A bitmap area with a non-zero left/top: the width/height come from the area but the pixels are
        // read from 0,0 (quirk kept).
        struct OffsetBuffer : cGZBuffer {
            cRZRect area{2, 2, 10, 10};
            const cRZRect* GetBufferArea() const override { return &area; }
        };
        auto* buffer = new OffsetBuffer();
        buffer->AddRef();
        cGZBufferColorType type;
        type.type = kBufferColorXRGB8888;
        type.bitsPerPixel = 32;
        buffer->Init(8, 8, type);
        for (std::uint32_t y = 0; y < 8; ++y)
            for (std::uint32_t x = 0; x < 8; ++x) buffer->SetPixel(x, y, 0);
        buffer->SetPixel(0, 0, 0x00ff0000);
        auto* offsetRegion = new cSC4Region();
        offsetRegion->AddRef();
        offsetRegion->ParseCityLocations(buffer);
        std::vector<cISC4Region::cLocation> locations;
        offsetRegion->GetCityLocations(locations);
        CHECK(locations.size() == 1 && locations[0].x == 0 && locations[0].z == 0 && locations[0].size == 0);
        offsetRegion->Release();
        buffer->Release();
    }

    // ---- ReloadCity / DeleteCity --------------------------------------------------------------------------------
    {
        fs::create_directories(regions / "Rel");
        auto* region = new cSC4Region();
        region->AddRef();
        region->SetDirectoryName(cRZString("Rel"));
        auto* oldCity = new FakeCity(0, 0, 1, 1);
        oldCity->savePath = cRZString((regions / "Rel" / "X.sc4").string().c_str());
        Touch(oldCity->savePath.ToChar());
        auto* replacement = new FakeCity(0, 0, 1, 1);
        replacement->serial = 9;
        host.byPath[oldCity->savePath.ToChar()] = replacement;
        region->InsertCity(oldCity);
        cRZAutoRefCount<cISC4RegionalCity>* holder = region->GetCity(0, 0);
        region->ReloadCity(holder);
        CHECK(holder == region->GetCity(0, 0) && holder->get() == replacement);
        CHECK(oldCity->shutdowns == 1);
        CHECK(std::strcmp(replacement->savePath.ToChar(), oldCity->savePath.ToChar()) == 0);
        region->RemoveCity(holder);   // frees the holder the region allocated
        region->Release();
        oldCity->Release();
        replacement->Release();

        fs::create_directories(regions / "Del");
        region = new cSC4Region();
        region->AddRef();
        region->SetDirectoryName(cRZString("Del"));
        auto* doomed = new FakeCity(0, 0, 1, 1);
        doomed->savePath = cRZString((regions / "Del" / "D.sc4").string().c_str());
        Touch(doomed->savePath.ToChar());
        region->InsertCity(doomed);
        holder = region->GetCity(0, 0);
        region->DeleteCity(holder);
        CHECK(region->GetCity(0, 0) == nullptr);
        CHECK(doomed->shutdowns == 1);
        CHECK(!fs::exists(doomed->savePath.ToChar()));
        region->Release();
        doomed->Release();
    }

    // ---- ResetTutorialCity: the record-read result is inverted ------------------------------------------------
    {
        fs::create_directories(regions / "Tutorial");
        auto* region = new cSC4Region();
        region->AddRef();
        region->SetDirectoryName(cRZString("Tutorial"));
        auto* city = new FakeCity(2, 2, 1, 1);
        city->tutorialGUID = 7;
        city->savePath = cRZString((regions / "Tutorial" / "T.sc4").string().c_str());
        Touch(city->savePath.ToChar());
        host.byPath[city->savePath.ToChar()] = city;
        region->InsertCity(city);

        host.recordResult = 0;
        host.tutorialCalls.clear();
        CHECK(!region->ResetTutorialCity(7));   // quirk: false when the record WAS read
        CHECK(host.tutorialCalls.size() == 1 && host.tutorialCalls[0] == "7:1");
        CHECK(host.lastRecordKey.type == kRegionTutorialCityRecordType);
        CHECK(host.lastRecordKey.group == kRegionTutorialCityRecordGroup && host.lastRecordKey.instance == 7);
        CHECK(city->tutorialGUID == 7 && city->shutdowns == 1);   // reloaded
        CHECK(region->GetCity(2, 2) != nullptr && region->GetCity(2, 2)->get() == city);

        host.recordResult = -1;
        host.tutorialCalls.clear();
        CHECK(region->ResetTutorialCity(7));   // quirk: true when the record could NOT be read
        CHECK(host.tutorialCalls.empty());
        CHECK(!region->ResetTutorialCity(-1));
        CHECK(!region->ResetTutorialCity(1234));

        if (auto* h = region->GetCity(2, 2)) region->RemoveCity(h);   // frees the holder the region allocated
        region->Release();
        city->Release();
    }

    // ---- MoveCity ----------------------------------------------------------------------------------------------
    {
        fs::create_directories(regions / "MoveDst");
        auto* region = new cSC4Region();
        region->AddRef();
        region->SetDirectoryName(cRZString("MoveDst"));
        auto* occupant = new FakeCity(2, 2, 1, 1);
        occupant->savePath = cRZString((regions / "MoveDst" / "O.sc4").string().c_str());
        Touch(occupant->savePath.ToChar());
        region->InsertCity(occupant);

        // A second city in the destination whose serial equals the moving city's old serial: the old
        // serial collides, so the freshly generated 0xabc is tried next and is free.
        auto* keeper = new FakeCity(10, 10, 1, 1);
        keeper->serial = 500;
        region->InsertCity(keeper);

        auto* mover = new FakeCity(5, 5, 1, 1);
        mover->serial = 500;
        mover->name = cRZString("Mover");
        mover->savePath = cRZString((regions / "MoveDst" / "M.sc4").string().c_str());
        Touch(mover->savePath.ToChar());

        host.serialSeed = 0xabc;
        host.writtenCities.clear();
        host.readPaths.clear();
        const std::string expectedPath = downloadsDir + "Mover-00000abc.sc4";
        host.byPath[expectedPath] = mover;

        CHECK(region->MoveCity(nullptr, mover, 2, 2));
        CHECK(occupant->shutdowns == 1);
        CHECK(!fs::exists(occupant->savePath.ToChar()));
        CHECK(!fs::exists((regions / "MoveDst" / "M.sc4").string()));
        CHECK(fs::exists(expectedPath));
        CHECK(mover->serial == 0xabc && mover->x == 2 && mover->z == 2);
        CHECK(host.writtenCities.size() == 1 && host.writtenCities[0] == expectedPath + "|Mover");
        CHECK(mover->shutdowns == 1);   // shut down after the region record was written

        // Same city already at the target cell: immediate success, nothing else happens.
        auto* again = new FakeCity(2, 2, 1, 1);
        region->InsertCity(again);
        host.writtenCities.clear();
        CHECK(region->MoveCity(nullptr, again, 2, 2));
        CHECK(host.writtenCities.empty());

        region->Release();
        occupant->Release();
        keeper->Release();
        mover->Release();
        again->Release();
    }

    // ---- MoveCity without a serial collision: the moving city keeps its old serial -------------------------
    {
        fs::create_directories(regions / "Move2");
        auto* region = new cSC4Region();
        region->AddRef();
        region->SetDirectoryName(cRZString("Move2"));
        auto* keeper = new FakeCity(10, 10, 1, 1);
        keeper->serial = 999;
        region->InsertCity(keeper);
        auto* mover = new FakeCity(5, 5, 1, 1);
        mover->serial = 777;
        mover->name = cRZString("Mover2");
        mover->savePath = cRZString((regions / "Move2" / "N.sc4").string().c_str());
        Touch(mover->savePath.ToChar());
        host.serialSeed = 0x1234;
        const std::string expectedPath = downloadsDir + "Mover2-00000309.sc4";
        host.byPath[expectedPath] = mover;
        host.writtenCities.clear();
        CHECK(region->MoveCity(nullptr, mover, 1, 1));
        CHECK(mover->serial == 777 && mover->x == 1 && mover->z == 1);
        CHECK(fs::exists(expectedPath));
        CHECK(host.writtenCities.size() == 1);   // the generated 0x1234 was never needed
        region->Release();
        keeper->Release();
        mover->Release();
    }

    SetRegionHost(nullptr);
    SetSLBufferHost(nullptr);
    fs::remove_all(root);
    std::printf(failures ? "FAILED\n" : "sc4_region ok\n");
    return failures ? 1 : 0;
}
