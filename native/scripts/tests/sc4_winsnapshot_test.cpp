// The snapshot taker and album (snapshots.md section 6): the album folder and album.ini (SN-C1), the information file's
// XOR (SN-C2), an empty description removing X.txt (SN-C3), Space wrapping the sizes and the digits setting the quality
// (SN-C4), the capture message handled on its fourth delivery (SN-C5); plus the frame's sizes, the information lines
// and their caption, and the album-name rules.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "fake_winmgr.h"
#include "rz/cRZFile.h"
#include "sc4/WinSnapshot.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {
std::string S(const cIGZString& s) { return std::string(s.Data(), s.Strlen()); }

struct Host final : sc4::ISnapshotHost {
    int captures = 0;
    cIGZWin* MainWindow() override { return nullptr; }
    bool CreateWindowFromScript(const cGZPersistResourceKey&, cIGZWin*, std::uint32_t, std::uint32_t, void**) override { return false; }
    cIGZBuffer* CaptureView(cIGZWin*, int, int, int, int) override { ++captures; return nullptr; }
    void CityInfo(sc4::SnapshotCityInfo& out) override {
        out = sc4::SnapshotCityInfo{};
        out.hasCity = true;
        out.city = cRZString("Big Town");
        out.number = 2;
        out.mayor = cRZString("Ann");
        out.hasSimulator = true;
        out.simDay = 730;
        out.dateText = cRZString("1/1/02");
        out.fileDate = cRZString("Jan. 1, 02");
        out.hasPopulation = true;
        out.population = 1234;
        out.hasFunds = true;
        out.funds = 56789;
        out.hasView = true;
        out.zoom = 3;
        out.rotation = 1;
        out.centerX = 64;
        out.centerZ = 70;
    }
    void UserName(cRZString& out) override { out = cRZString("mayor"); }
    std::string albums;
    void AlbumsDirectory(cRZString& out) override { out = cRZString(albums.c_str()); }
    void LoadString(std::uint32_t, std::uint32_t instance, cRZString& out) override {
        switch (instance) {
            case 0xea946efe: out = cRZString("City: %s"); break;
            case 0xea946efd: out = cRZString("Mayor: %s"); break;
            case 0xea946efc: out = cRZString("Date: %s"); break;
            case 0xea946efb: out = cRZString("Population: %s"); break;
            case 0xea946efa: out = cRZString("Funds: %s"); break;
            default: out = cRZString("text"); break;
        }
    }
    void MoneyString(std::int64_t amount, cRZString& out) override {
        char text[32];
        std::snprintf(text, sizeof text, "$%lld", static_cast<long long>(amount));
        out = cRZString(text);
    }
    void SimDateText(int simDay, cRZString& out) override {
        char text[32];
        std::snprintf(text, sizeof text, "day %d", simDay);
        out = cRZString(text);
    }
    cIGZBuffer* LoadImage(std::uint32_t, std::uint32_t) override { return nullptr; }
    std::uint32_t Color(std::uint32_t) override { return 0; }
    gz::IWinFont* Font() override { return nullptr; }
    void PlayEvent(std::uint32_t) override {}
    void PauseGame(bool) override {}
    void RestoreWindowLocation(cIGZWin*) override {}
    void SaveWindowLocation(cIGZWin*) override {}
    void Notify(const cRZString&, const cRZString&) override {}
    cGZGimexFactory* ImageFactory() override { return nullptr; }
};

bool Exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}
}  // namespace

int main() {
    const char* tmp = std::getenv("TMPDIR");
    const std::string root = std::string(tmp && *tmp ? tmp : "/tmp") + "/winsnapshot_test_" + std::to_string(getpid()) + "/";
    mkdir(root.c_str(), 0700);
    Host host;
    host.albums = root;
    sc4::SetSnapshotHost(&host);

    // album names: file-safe city names, the folder's "--" splits the name off (at least 3 bytes before the end)
    {
        cRZString name;
        CHECK(sc4::cSC4WinSnapshotAlbum::ConvertAlbumDirectoryNameToAlbumName(cRZString("Big Town--2/"), name) && S(name) == "Big Town");
        CHECK(!sc4::cSC4WinSnapshotAlbum::ConvertAlbumDirectoryNameToAlbumName(cRZString("ab--"), name));
        CHECK(!sc4::cSC4WinSnapshotAlbum::ConvertAlbumDirectoryNameToAlbumName(cRZString("plain/"), name));
        sc4::cSC4WinSnapshotAlbum::CityNameToAlbumName(cRZString(std::string(60, 'x').c_str()), name);
        CHECK(name.Strlen() == 0x30);   // cut to 48 bytes
    }

    // SN-C2: every byte XOR 0x7b, both ways
    {
        cRZString out, back;
        sc4::cSC4WinSnapshotAlbum::ObfuscateData(cRZString("SanityCheck\n"), out);
        CHECK(static_cast<unsigned char>(out.Data()[0]) == 0x28);
        sc4::cSC4WinSnapshotAlbum::ObfuscateData(out, back);
        CHECK(S(back) == "SanityCheck\n");
        cRZString path;
        sc4::cSC4WinSnapshotAlbum::GetPathForInfoFile(cRZString("/a/b.c/X.png"), path);
        CHECK(S(path) == "/a/b.c/X.info");
        sc4::cSC4WinSnapshotAlbum::GetPathForDescriptionFile(cRZString("/a/b.c/X.png"), path);
        CHECK(S(path) == "/a/b.c/X.txt");
    }

    // SN-C1: the city "Big Town", number 2 -> the folder "Big Town--2/" with an empty album.ini
    auto* album = new sc4::cSC4WinSnapshotAlbum();
    album->AddRef();
    album->SetRoot(cRZString(root.c_str()));
    CHECK(!album->AlbumExists(cRZString("Big Town"), 2));
    CHECK(album->CreateAlbum(cRZString("Big Town"), 2));
    CHECK(album->AlbumExists(cRZString("Big Town"), 2));
    CHECK(Exists(root + "Big Town--2/album.ini"));
    CHECK(album->Albums().size() == 1 && S(album->Albums()[0]) == "Big Town--2/");

    // SN-C3: a description is written beside the picture; an empty one removes X.txt
    const std::string picture = root + "Big Town--2/X.png";
    {
        std::FILE* f = std::fopen(picture.c_str(), "wb");
        std::fputs("png", f);
        std::fclose(f);
        CHECK(sc4::cSC4WinSnapshotAlbum::SaveDescriptionOfFile(cRZString(picture.c_str()), cRZString("our park")));
        CHECK(Exists(root + "Big Town--2/X.txt"));
        cRZString text;
        CHECK(album->GetDescriptionOfFile(cRZString(picture.c_str()), text) && S(text) == "our park");
        CHECK(sc4::cSC4WinSnapshotAlbum::SaveDescriptionOfFile(cRZString(picture.c_str()), cRZString("")));
        CHECK(!Exists(root + "Big Town--2/X.txt"));
        CHECK(!album->GetDescriptionOfFile(cRZString(picture.c_str()), text));   // the placeholder text instead
    }

    // the information lines of the current city, then the caption the album shows for them
    {
        cRZString info;
        CHECK(sc4::cSC4WinSnapshotTaker::GetGraphicInfoForCurrentCity(info));
        CHECK(S(info) == "SanityCheck\nmayor\nBig Town\nAnn\n730\n1/1/02\n1234\n56789\n\n3,1,64,70\n");
        CHECK(sc4::cSC4WinSnapshotAlbum::SaveInformationOfStandardFile(cRZString(picture.c_str()), info));
        cRZString caption;
        CHECK(album->GetInformationOfStandardFile(cRZString(picture.c_str()), caption));
        CHECK(S(caption) == "City: Big Town\nMayor: Ann\nDate: day 730\nPopulation: 1234\nFunds: $56789\n\nX.png\n");
        // a file without the marker shows only its name
        sc4::cSC4WinSnapshotAlbum::SaveInformationOfStandardFile(cRZString(picture.c_str()), cRZString("Other\n"));
        CHECK(album->GetInformationOfStandardFile(cRZString(picture.c_str()), caption) && S(caption) == "\nX.png\n");
    }

    // adding a picture from elsewhere copies it in under a new name with its information; the album lists it
    {
        const std::string outside = root + "outside.png";
        std::FILE* f = std::fopen(outside.c_str(), "wb");
        std::fputs("png", f);
        std::fclose(f);
        CHECK(album->AddNewGraphic(cRZString(outside.c_str()), cRZString("SanityCheck\n"), cRZString("hello"), cRZString("Big Town"), 2, true));
        CHECK(!Exists(outside));   // moved
        CHECK(album->Graphics().size() == 2);
        const std::string added = S(album->CurrentFile());
        CHECK(added.rfind("Big Town-Jan. 1, 02", 0) == 0 && added.size() > 4 && added.substr(added.size() - 4) == ".png");
        CHECK(Exists(root + "Big Town--2/" + added.substr(0, added.size() - 4) + ".info"));
        CHECK(Exists(root + "Big Town--2/" + added.substr(0, added.size() - 4) + ".txt"));
        // a file already in the album keeps its name (no copy)
        const std::size_t before = album->Graphics().size();
        CHECK(album->AddNewGraphic(cRZString((root + "Big Town--2/X.png").c_str()), cRZString(""), cRZString(""), cRZString("Big Town"), 2, false));
        CHECK(album->Graphics().size() == before && S(album->CurrentFile()) == "X.png");
    }
    album->Release();

    // the frame: sizes up to the parent's width, then the parent's size; Space wraps (SN-C4), the digits set the quality
    // The frame's album makes no window here (the host creates none), so its Albums folder (read by the window's
    // setup, as in the original) stays empty and its paths are relative: run that part inside the test folder.
    char cwd[4096];
    const bool haveCwd = getcwd(cwd, sizeof cwd) != nullptr;
    CHECK(chdir(root.c_str()) == 0);
    FakeMgr mgr;
    FakeCursors cursors;
    mgr.cursors = &cursors;
    cGZWin parent(&mgr);
    parent.SetArea(0, 0, 1024, 768);
    mgr.mainWin = &parent;
    mgr.cursorX = 500;
    mgr.cursorY = 400;
    CHECK(sc4::cSC4WinSnapshotTaker::CreateWinSnapshotTaker(&parent, 0));
    auto* taker = dynamic_cast<sc4::cSC4WinSnapshotTaker*>(parent.GetChildWindowFromID(sc4::kSnapshotTakerId));
    CHECK(taker != nullptr);
    if (taker) {
        CHECK(taker->Sizes().size() == 6);   // 160, 320, 480, 640, 800 wide, then the parent's 1024 x 768
        CHECK(taker->Sizes().back().right == 1024 && taker->Sizes().back().bottom == 768);
        CHECK(taker->SizeIndex() == 5 && taker->GetW() == 1024 && taker->GetL() == 0 && taker->GetT() == 0);
        CHECK(taker->Quality() == 80);
        taker->GZOnKeyDown(0x20, 0);   // Space on the last size: the first
        CHECK(taker->SizeIndex() == 0 && taker->GetW() == 160 && taker->GetH() == 120);
        taker->GZOnKeyDown('5', 0);
        CHECK(taker->Quality() == 60);
        taker->GZOnKeyDown('0', 0);
        CHECK(taker->Quality() == 10);
        taker->GZOnKeyDown('9', 0);
        CHECK(taker->Quality() == 100);
        // the frame follows the mouse, kept inside the parent
        taker->GZOnMouseMove(5000, 5000, 0);
        CHECK(taker->GetR() == 1024 && taker->GetB() == 768);
        // Enter: the frame hides and posts the capture message (close afterwards: 1; with Shift: 0)
        mgr.posted.clear();
        taker->GZOnKeyDown(0x0d, 0);
        CHECK(!taker->IsVisible() && mgr.posted.size() == 1 && mgr.posted[0].type == sc4::kSnapshotTakeMessage && mgr.posted[0].param1 == 1);
        // SN-C5: posted with counter 0, handled on the fourth delivery
        cGZMessage take = mgr.posted[0];
        take.param1 = 0;   // keep the frame
        for (int delivery = 1; delivery <= 3; ++delivery) {
            mgr.posted.clear();
            taker->DoMessage(take);
            CHECK(host.captures == 0 && mgr.posted.size() == 1 && mgr.posted[0].param2 == static_cast<std::uint64_t>(delivery));
            take = mgr.posted[0];
        }
        mgr.posted.clear();
        taker->DoMessage(take);
        CHECK(host.captures == 1 && mgr.posted.empty() && taker->IsVisible());   // kept: shown again
        // Esc closes it
        taker->GZOnKeyDown(0x1b, 0);
        CHECK(parent.GetChildWindowFromID(sc4::kSnapshotTakerId) == nullptr);
    }

    if (haveCwd && chdir(cwd) != 0) std::printf("note: could not return to %s\n", cwd);
    sc4::SetSnapshotHost(nullptr);
    std::string clean = "rm -rf '" + root + "'";
    if (std::system(clean.c_str()) != 0) std::printf("note: could not remove %s\n", root.c_str());
    if (failures) std::printf("%d failure(s)\n", failures);
    else std::printf("sc4_winsnapshot: all checks passed\n");
    return failures ? 1 : 0;
}
