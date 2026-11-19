// Snapshots and the snapshot album (Mac i386 0x002b7ba2..0x002bea72; spec snapshots.md).
//
// cSC4WinSnapshotTaker (window 0x85202c0e, 0x110 bytes): a white frame over the 3D view, centered on the mouse at the
// largest of its sizes (160x120 .. 2048x1536 that fit the parent, then the parent's own size). Space cycles the sizes,
// the mouse drags it (kept inside the parent), Esc or a right click closes it, the digits 0..9 set the JPEG quality
// 10..100 (+0xf4, 80 at first). Enter or a left click hides the frame and posts it message 0x1a5d056d (close afterwards
// unless Shift / Ctrl / Alt is held), which re-posts itself until its counter is 3 so the picture is taken without the
// frame: the view's pixels inside the frame (the whole view when the frame has its size and sits within 5 pixels of the
// corner) with alpha forced opaque, saved as a new PNG in the current city's album, which then opens modally.
//
// cSC4WinSnapshotAlbum (0xa8 bytes, window procedure of UI script {0, 0x96a006b0, 0x4a8cc5ea} window 0x0a8cd3ee): the
// albums are the folders "<name>--<n>" of the user's Albums directory (name = the city's file-safe name cut to 48
// bytes, n = the city's number or 0x10000000 for a region) holding album.ini and the pictures; every picture X.png may
// have X.info (its information, every byte XOR 0x7b) and X.txt (the player's description). The window has a small
// (0x0a8cd3ef) and a large (0x0a8cd3ff) layout; the picture view moves between them. Buttons: previous 0x6a8cd21f,
// next 0xaa8cd25d, delete 0xaa8cd139, close 0xaa8cd14a / 0x42b7c353, enlarge 0x4a8cd356, shrink 0x4a8cd355 / 0x6a8cd222;
// the album list 0x8a8cc775, the information 0xaa8cc64e, the description 0x8a8cc773, the count text 0x4a8cd34e.
//
// cWinBMPViewGVExtra (0x184 bytes): the album's picture view; a double click (two left clicks within 500 ms) toggles
// "sized in" (fit to the view, the small rectangle +0x150) and "sized out" (its own size, scrolling, +0x160) and tells
// the notification target (message 0x9127484a, the view's id, sized in).
//
// Quirks kept: the info caption shows the city, mayor, date, population and funds lines, then a blank line and the
// file name; a description that never changed is not written back; deleting the last picture of an album deletes the
// album; the frame's fill color is white and its outline uses that same color.
#pragma once
#include <cstdint>
#include <list>
#include <string>
#include <vector>

#include "gz/Win.h"
#include "gz/WinProc.h"
#include "gz/cGZPersistResourceKey.h"
#include "rz/cRZString.h"
#include "sc4/WinBMPView.h"

class cIGZBuffer;
namespace gz { class cIGZWinListBox; class cIGZWinText; class cIGZWinTextEdit; }
namespace gz { class IWinFont; }

namespace sc4 {

constexpr std::uint32_t kSnapshotTakerId = 0x85202c0e;
constexpr std::uint32_t kSnapshotTakeMessage = 0x1a5d056d;
constexpr std::uint32_t kSnapshotAlbumWindowId = 0x0a8cd3ee, kSnapshotAlbumScript = 0x4a8cc5ea;
constexpr std::uint32_t kSnapshotAlbumSmall = 0x0a8cd3ef, kSnapshotAlbumLarge = 0x0a8cd3ff;
constexpr std::uint32_t kSnapshotAlbumSmallRect = 0x0a8cd400, kSnapshotAlbumLargeRect = 0x0a8cd401;
constexpr std::uint32_t kSnapshotButtonPrevious = 0x6a8cd21f, kSnapshotButtonNext = 0xaa8cd25d,
                        kSnapshotButtonDelete = 0xaa8cd139, kSnapshotButtonClose = 0xaa8cd14a,
                        kSnapshotButtonCloseAlt = 0x42b7c353, kSnapshotButtonEnlarge = 0x4a8cd356,
                        kSnapshotButtonShrink = 0x4a8cd355, kSnapshotButtonShrinkAlt = 0x6a8cd222;
constexpr std::uint32_t kSnapshotAlbumList = 0x8a8cc775, kSnapshotInfoText = 0xaa8cc64e,
                        kSnapshotDescription = 0x8a8cc773, kSnapshotCountText = 0x4a8cd34e;
constexpr std::uint32_t kSnapshotSizedInMessage = 0x9127484a;
constexpr std::uint32_t kSnapshotRegionAlbumNumber = 0x10000000;
constexpr std::uint8_t kSnapshotInfoKey = 0x7b;

// The current city as the album sees it.
struct SnapshotCityInfo {
    cRZString city;         // cISC4City slot 0x30 (or the region's name without a city)
    std::uint32_t number = kSnapshotRegionAlbumNumber;   // cISC4City slot 0x14
    bool hasCity = false;
    cRZString mayor;        // slot 0x40
    bool hasSimulator = false;
    int simDay = 0;         // the simulator date's day number (slot 0x68)
    cRZString dateText;     // the language manager's date (format 0)
    cRZString fileDate;     // the same date with format 4 (file names)
    bool hasPopulation = false;
    int population = 0;     // residential simulator slot 0x40
    bool hasFunds = false;
    std::int64_t funds = 0; // the budget's total funds (city slot 0xd0 -> slot 0x18)
    bool hasCheats = false;
    int cheats = 0;         // cheat code manager slot 0x34(0, &n): the number of registered cheats
    bool hasView = false;
    std::uint32_t zoom = 0, rotation = 0, centerX = 0, centerZ = 0;   // the view and its center cell (x16)
};

// What the snapshot windows reach outside the window toolkit.
class ISnapshotHost {
public:
    virtual ~ISnapshotHost() = default;
    virtual cIGZWin* MainWindow() = 0;
    // SC4CreateWindowFromScript(key, parent, id, iid, out): one counted reference in *out.
    virtual bool CreateWindowFromScript(const cGZPersistResourceKey& key, cIGZWin* parent, std::uint32_t id,
                                        std::uint32_t iid, void** out) = 0;
    // The GZ driver's color-buffer extension (QI 0xe69bfe2a) slot 3: the frame's pixels inside the rect in view
    // coordinates as a new buffer (one reference), or null.
    virtual cIGZBuffer* CaptureView(cIGZWin* view, int x, int y, int w, int h) = 0;
    virtual void CityInfo(SnapshotCityInfo& out) = 0;
    virtual void UserName(cRZString& out) = 0;                       // cRZVersion::GetUserName
    virtual void AlbumsDirectory(cRZString& out) = 0;                // cISC4App slot 0xf0, ends with the separator
    virtual void LoadString(std::uint32_t group, std::uint32_t instance, cRZString& out) = 0;   // SC4LoadString
    virtual void MoneyString(std::int64_t amount, cRZString& out) = 0;                         // SC4MakeMoneyString
    // The language manager's date (slot 0x28) for a day number of the simulation (cRZDate relative to the start).
    virtual void SimDateText(int simDay, cRZString& out) = 0;
    virtual cIGZBuffer* LoadImage(std::uint32_t group, std::uint32_t instance) = 0;   // cSLBuffer(0x856ddbac, ...): one reference
    virtual std::uint32_t Color(std::uint32_t id) = 0;              // the window color manager slot 0x7c
    virtual gz::IWinFont* Font() = 0;                                    // font 0xcad46524, else "Arial MT" 14
    virtual void PlayEvent(std::uint32_t id) = 0;                    // spSS slot 0x28(1, id, 0, 0, 0)
    virtual void PauseGame(bool on) = 0;                             // cSC4AutoGamePause(false, false, true, true)
    virtual void RestoreWindowLocation(cIGZWin* window) = 0;         // the window location saver slot 0x24(window, 1)
    virtual void SaveWindowLocation(cIGZWin* window) = 0;            // slot 0x14
    virtual void Notify(const cRZString& title, const cRZString& text) = 0;   // nSC4UI::GenericNotificationDialog
    virtual cGZGimexFactory* ImageFactory() = 0;                     // for WriteBMPToFile
};
ISnapshotHost* SnapshotHost();
void SetSnapshotHost(ISnapshotHost* host);

// ---- the picture view ----------------------------------------------------------------------------------------------
class cWinBMPViewGVExtra final : public cWinBMPView {
public:
    explicit cWinBMPViewGVExtra(cIGZWinMgr* mgr);                 // @ 0x002b86de
    bool Init() override;                                          // @ 0x002b7ae0
    bool GZOnMouseDownL(int x, int y, std::uint32_t flags) override;   // @ 0x002b7a5e
    bool SetSizeInOut(bool sizedIn);                               // @ 0x002b78c2
    void SetShouldFitImageAtSizeIn(bool fit);                      // @ 0x002b7b28
    void SetSizeInAndOutAreas(const cRZRect& in, const cRZRect& out) { sizeInArea_ = in; sizeOutArea_ = out; }
    bool SizedIn() const { return sizedIn_; }
    bool FitAtSizeIn() const { return fitAtSizeIn_; }
    // the clock of the double click (cRZTimer +0x170); a test may set it
    static std::uint64_t (*clock)();

private:
    bool fitAtSizeIn_ = false;                    // +0x14c
    bool sizedIn_ = true;                         // +0x14d
    cRZRect sizeInArea_{0, 0, 16, 16};            // +0x150
    cRZRect sizeOutArea_{0, 0, 32, 32};           // +0x160
    bool timerRunning_ = false;                   // +0x170
    std::uint64_t timerStart_ = 0;
};

class cSC4WinSnapshotAlbum;

// ---- the frame -----------------------------------------------------------------------------------------------------
class cSC4WinSnapshotTaker final : public cGZWin {
public:
    explicit cSC4WinSnapshotTaker(cIGZWinMgr* mgr);               // @ 0x002b8886
    ~cSC4WinSnapshotTaker() override;

    // Shows the frame in `parent` (an existing one is shown again), optionally at size index `size` - 1.   @ 0x002b8948
    static bool CreateWinSnapshotTaker(cIGZWin* parent, std::uint32_t size);
    // Makes the album window in `parent` (none when one is up), shown or hidden, at `file` or the city's album.  @ 0x002be596
    static cSC4WinSnapshotAlbum* ShowSnapshotAlbum(bool create, bool show, cIGZWin* parent, const cIGZString* file);
    // The ten information lines of the current city ("SanityCheck" first).   @ 0x002b925e
    static bool GetGraphicInfoForCurrentCity(cRZString& out);

    bool Init() override;                                          // @ 0x002b8d08
    bool Shutdown() override;                                      // @ 0x002b7e6a
    bool DoMessage(cGZMessage& message) override;                  // @ 0x002bea72
    bool GZPaint() override;                                       // @ 0x002b8010
    bool GZOnKeyDown(std::uint32_t key, std::uint32_t flags) override;     // @ 0x002b857a
    bool GZOnCharacter(char c) override;                          // @ 0x002b7f2c
    bool GZOnMouseDownL(int x, int y, std::uint32_t flags) override;       // @ 0x002b7f36
    bool GZOnMouseDownR(int x, int y, std::uint32_t flags) override;       // @ 0x002b7f5e
    bool GZOnMouseMove(int x, int y, std::uint32_t flags) override;        // @ 0x002b7f76

    bool GoToSizeIndex(std::uint32_t index);                       // @ 0x002b8444
    bool CycleSize();                                              // @ 0x002b8538
    void AdjustPosition();                                         // @ 0x002b7d52
    void RemoveSelf();                                             // @ 0x002b7ed4
    bool TakeSnapshot(bool close);                                 // @ 0x002be706

    std::uint32_t SizeIndex() const { return sizeIndex_; }
    std::uint32_t Quality() const { return quality_; }
    const std::vector<cRZRect>& Sizes() const { return sizes_; }   // width / height in right / bottom
    const std::string& HelpText() const { return help_; }

private:
    bool AddNewGraphicForCurrentCity(cSC4WinSnapshotAlbum* album, const cRZString& file, bool move);   // @ 0x002bd3cc

    bool initialized_ = false;          // +0xe4
    gz::IWinFont* font_ = nullptr;      // +0xe8
    std::uint32_t sizeIndex_ = 0;       // +0xec
    std::uint32_t quality_ = 80;        // +0xf4
    std::vector<cRZRect> sizes_;        // +0xf8 (cRZSize: width, height)
    std::string help_;                  // +0x108
};

// ---- the album -----------------------------------------------------------------------------------------------------
class cSC4WinSnapshotAlbum final : public gz::cIGZWinProc {
public:
    cSC4WinSnapshotAlbum();                                        // @ 0x002bb01e
    virtual ~cSC4WinSnapshotAlbum();

    bool QueryInterface(std::uint32_t iid, void** out) override;   // @ 0x002b7ba2
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;
    bool DoWinProcMessage(cIGZWin* window, cGZMessage& message) override;   // DoWinMsg @ 0x002be3a2

    bool CreateSnapshotAlbumWindow(cIGZWin*& out);                 // @ 0x002bdcf8
    void DoModalWin();                                             // @ 0x002bd5b0
    bool GoToAlbumForCurrentCity(bool create);                     // @ 0x002bd4fa
    bool GoToAlbum(const cRZString& city, std::uint32_t number, bool create);   // @ 0x002bce66
    bool GoToAlbum(std::uint32_t index);                           // @ 0x002bc922
    bool CreateAlbum(const cRZString& albumName, std::uint32_t number);         // @ 0x002bccb2
    bool AlbumExists(const cRZString& albumName, std::uint32_t number);         // @ 0x002b8c84
    bool DeleteAlbum(std::uint32_t index);                         // @ 0x002bbbee
    bool AddNewGraphic(const cRZString& file, const cRZString& info, const cRZString& description,
                       const cRZString& city, std::uint32_t number, bool move);   // @ 0x002bcfa2
    bool GetNewTempFileName(const cRZString& directory, cRZString& out);       // @ 0x002b990a
    bool GetNewTempFileNameForCurrentAlbum(cRZString& out);                   // @ 0x002b9bfa
    bool SetCurrentFile(const cRZString& file, bool refresh);      // @ 0x002bb48e
    bool GoToGraphic(const cRZString& file);                       // @ 0x002bb2fe
    bool GoToNextGraphic();                                        // @ 0x002bc5e6
    bool GoToPreviousGraphic();                                    // @ 0x002bc73c
    bool DeleteCurrentGraphic();                                   // @ 0x002bca0a
    bool RefreshGraphicList(bool* changed);                        // @ 0x002bc05a
    bool SetInitialFile();                                         // @ 0x002bc428
    bool SaveDescriptionOfFile();                                  // @ 0x002b9fba

    static void CityNameToAlbumName(const cRZString& city, cRZString& out);    // @ 0x002b7d18
    static bool ConvertAlbumDirectoryNameToAlbumName(const cRZString& directory, cRZString& out);   // @ 0x002b81da
    static void ObfuscateData(const cRZString& in, cRZString& out);           // @ 0x002b9d66 (also UnObfuscateData)
    static void GetPathForInfoFile(const cRZString& file, cRZString& out);     // @ 0x002ba20c
    static void GetPathForDescriptionFile(const cRZString& file, cRZString& out);   // @ 0x002b9df6
    static bool SaveDescriptionOfFile(const cRZString& file, const cRZString& text);   // @ 0x002b9eb0
    bool GetDescriptionOfFile(const cRZString& file, cRZString& out);         // @ 0x002ba0c8
    static bool SaveInformationOfStandardFile(const cRZString& file, const cRZString& info);   // @ 0x002ba2c6
    bool GetInformationOfStandardFile(const cRZString& file, cRZString& out);  // @ 0x002ba3e6

    // test access
    cIGZWin* Window() const { return window_; }
    const std::vector<cRZString>& Albums() const { return albums_; }
    const std::list<cRZString>& Graphics() const { return graphics_; }
    const cRZString& CurrentAlbum() const { return currentAlbum_; }
    const cRZString& CurrentFile() const { return currentFile_; }
    cWinBMPViewGVExtra* View() const { return view_; }
    void SetRoot(const cRZString& root) { root_ = root; }

private:
    bool on_init(cIGZWin* window);                                 // @ 0x002bd710
    bool on_shutdown();                                            // @ 0x002bdeee
    bool on_button_activated(cIGZWin* window, std::uint32_t id);   // @ 0x002be0d0
    void end_dialog(cIGZWin* window, std::uint32_t result);        // @ 0x002b7c12
    void set_image_count_text(std::uint32_t index, std::uint32_t count);   // @ 0x002b7ca2
    void set_image_count_text();                                   // @ 0x002b83d6
    void set_to_no_image_file(bool showImage);                     // @ 0x002b8248
    bool update_list_of_album_directories(bool& changed, bool needPictures);   // @ 0x002bb75e
    bool album_directory_has_album_entries(const cRZString& directory, bool needPictures);   // @ 0x002baf2e
    bool convert_album_directory_name_to_album_index(const cRZString& directory, std::uint32_t& index);   // @ 0x002b8c16
    void enable_button(std::uint32_t id, bool on);
    void MoveViewTo(std::uint32_t from, std::uint32_t to, bool sizedIn);   // on_button_activated's layout switch
    cRZString AlbumPath() const;                                   // root + current album + "/"

    std::uint32_t refCount_ = 0;
    bool initialized_ = false;                  // +8
    cIGZWin* window_ = nullptr;                 // +0xc (one reference)
    cIGZBuffer* noImage_ = nullptr;             // +0x10
    cIGZBuffer* background_ = nullptr;          // +0x14
    cIGZBuffer* sizeOutBackground_ = nullptr;   // +0x18
    cWinBMPViewGVExtra* view_ = nullptr;        // +0x20
    gz::cIGZWinListBox* list_ = nullptr;            // +0x24
    gz::cIGZWinTextEdit* info_ = nullptr;           // +0x28
    gz::cIGZWinText* count_ = nullptr;              // +0x2c
    gz::cIGZWinTextEdit* description_ = nullptr;    // +0x30
    cIGZUnknown* enlarge_ = nullptr;            // +0x38
    bool browseEnabled_ = false;                // +0x3c
    bool deleteEmptyAlbums_ = true;             // +0x3d
    cRZString root_;                            // +0x40 the Albums directory (ends with the separator)
    std::vector<cRZString> albums_;             // +0x4c album folders ("name--n/")
    cRZString currentAlbum_;                    // +0x58
    std::list<cRZString> graphics_;             // +0x64 the current album's picture file names
    cRZString currentFile_;                     // +0x6c
    cRZString shownFile_;                       // +0x78
    cRZString description_text_;                // +0x84 the description as loaded
    std::vector<cRZString> extensions_;         // +0x98
    void* script_ = nullptr;                    // +0xa4 the window's script object (5386d516 view)
};

}  // namespace sc4
