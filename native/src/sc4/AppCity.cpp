// cSC4App: the city life cycle — new / load / close / save requests, quit, the application record of a city file, the
// application's message handlers and the city message helper (0x100011a14..0x100011f14, 0x100044510..0x1000475cc).
//
// Everything the original reaches through unrecovered neighbours (dialogs, the region screens, the regional city records,
// the city file segment, the busy cursor) goes through cityHooks_. Quirks kept:
//  * DoLoadCityUI fetches the application directory and then overwrites it: the default load path is "SavedCity.sc4".
//  * RequestQuit / RequestGoToRegionView show a different dialog when saving is disabled, and only the "OK" answer
//    (0xea57da59) of that dialog goes on.
//  * Quit and the "go to region" request flush the graphics system twice before the city is closed.
//  * PartialCloseCity pauses the game (all but graphics) and never resumes it itself.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "gz/RZServices.h"
#include "gz/WinInterfaces.h"
#include "rz/cRZFile.h"
#include "rz/cRZIniFile.h"
#include "rz/cRZKeyboard.h"
#include "rz/cRZMessage2Standard.h"
#include "rz/cRZVersion.h"
#include "rz/cRZVariant.h"
#include <cstdio>
#include <cstdlib>

#include "sc4/App.h"
#include "sc4/AutoHelpers.h"
#include "sc4/DBSegment.h"
#include "sc4/ServiceLocator.h"

bool cSC4App::sMacQuitDialogIsShowing_ = false;

namespace {
constexpr std::uint32_t kUITextGroup = 0x6a231eaa;
constexpr std::uint32_t kKeyMenu = 0x12;                                // VK_MENU (Alt)
constexpr std::uint32_t kDialogNo = 0x8a921f65, kDialogYes = 0x8a921f5b, kDialogQuitRegionNo = 0x8a921f70, kDialogOk = 0xea57da59;
constexpr std::uint32_t kMessageQuit = 0x29a89b05;
constexpr std::uint32_t kCityRecordIid = 0x66ba4db5;

// cSLString(group, instance) through the hook
std::string Text(sc4::AppCityHooks& hooks, std::uint32_t instance) { return hooks.loadText ? hooks.loadText(kUITextGroup, instance) : std::string(); }

void Notify(sc4::AppCityHooks& hooks, std::uint32_t titleInstance, std::uint32_t textInstance, const std::string& suffix = std::string()) {
    if (hooks.notify) hooks.notify(Text(hooks, titleInstance), Text(hooks, textInstance) + suffix);
}

struct BusyScope {
    sc4::AppCityHooks& hooks;
    explicit BusyScope(sc4::AppCityHooks& h) : hooks(h) { if (hooks.busy) hooks.busy(true); }
    ~BusyScope() { if (hooks.busy) hooks.busy(false); }
};

void SendMessage(cIGZMessage2& message) { if (SL::spMS2) SL::spMS2->MessageSend(&message); }
}  // namespace

// ---- messages ---------------------------------------------------------------------------------------------------

void cSC4App::PostCityMessage(cISC4City* city, std::uint32_t type, std::uint32_t data2, std::uint32_t data3, bool post) {
    if (!SL::spMS2) return;
    cRZAutoRefCount<cRZMessage2Standard> message(new cRZMessage2Standard());
    message->SetType(type);
    message->SetVoid1(city);
    message->SetIGZUnknown(city);
    message->SetData2(data2);
    message->SetData3(data3);
    if (post) SL::spMS2->MessagePost(message.get(), false);
    else SL::spMS2->MessageSend(message.get());
}

// File change notices re-scan the plugin folders; the cheat-code message handles the pause-on-focus-loss cheat and the
// "next stage" cheat; unknown cheats are not handled.
bool cSC4App::DoMessage(cGZMessage& message) {
    if (message.type == 0x87163754) {
        const void* source = reinterpret_cast<const void*>(message.param1);
        if (source == &pluginWatcher_ || source == &dataWatcher_) {
            cRZString directory;
            static_cast<const FileChangeNotification*>(source)->GetWatchedDirectory(directory);
            if (AddCodePlugins(directory) != 0 && SL::spMS2) {
                cRZAutoRefCount<cRZMessage2Standard> reply(new cRZMessage2Standard());
                reply->SetType(0xa42396c6);
                SL::spMS2->MessagePost(reply.get(), false);
            }
        }
    } else if (message.type == 0x230e27ac) {
        if (message.param1 == 0x8b6d4f8d) {
            if (SL::spMS2) {
                cRZAutoRefCount<cRZMessage2Standard> reply(new cRZMessage2Standard());
                reply->SetType(0xba44eb51);
                reply->SetData1(0x3c);
                SL::spMS2->MessagePost(reply.get(), false);
            }
        } else if (message.param1 == 0x2ab5c431) {
            const cIGZString* text = reinterpret_cast<const cIGZString*>(message.param2);
            cRZCmdLine line(text->ToChar());
            bool enable = !pauseOnFocusLoss_;
            if (line.argc() > 1) {
                cRZString option(line[1].ToChar());
                option.MakeLower();
                if (option.IsEqualToChars("on", 2, true)) enable = true;
                else if (option.IsEqualToChars("off", 3, true)) enable = false;
            }
            if (enable != pauseOnFocusLoss_) EnableFullGamePauseOnAppFocusLoss(enable);
        } else if (message.param1 != 0x2b60e45b) {
            return false;
        }
    } else if (message.type == 0xc7aa61c6) {
        RequestQuit(false, false);
    }
    return true;
}

bool cSC4App::DoMessage(cIGZMessage2* message) {
    switch (message->GetType()) {
        case 0xea65224e: DoCityObliteration(); break;
        case 0xea8ae29a: appState_ = 2; break;
        case 0xcbb5bb45: appState_ = 1; break;
        case 0x8bb5bb46:
        case 0x26d31ec2: appState_ = 0; break;
        case 0xfa0c6390:
        case 0x7a104750: timer428_.Restart(); break;
        case 0xba44eb51: if (SL::spRM) SL::spRM->ForceGarbageCollection(); break;
        case 0x0b627ad3:
        case 0x49a780b2: LaunchExceptionReportUploader(); break;
        case 0x231bbf91: DoPreferencesChanged(false); break;
        case kMessageQuit: Quit(); break;
        default: break;
    }
    return true;
}

// ---- the application record --------------------------------------------------------------------------------------

// Record version 2: the city object, then (through the regional city holder) the regional city records.
bool cSC4App::Write(cIGZOStream& stream, cIGZPersistDBSegment* segment) const {
    stream.SetUint16(2);
    if (!sc4::SC4WriteGZSerializable(stream, city_)) return false;
    if (std::getenv("SC4_DEBUG")) std::printf("save: regional city %p\n", regionalCity_ ? static_cast<void*>(regionalCity_->get()) : nullptr);
    if (regionalCity_ && regionalCity_->get()) return cityHooks_.writeRegionalCity && cityHooks_.writeRegionalCity(segment, regionalCity_->get());
    return true;
}

bool cSC4App::Read(cIGZIStream& stream, cIGZPersistDBSegment* segment) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != 2) return false;
    void* city = nullptr;
    if (!sc4::SC4ReadGZSerializable(stream, kCityRecordIid, &city, false)) return false;
    city_ = static_cast<cISC4City*>(city);
    cRZAutoRefCount<cISC4RegionalCity> loaded;
    if (cityHooks_.readRegionalCity && cityHooks_.readRegionalCity(segment, loaded) && regionalCity_) {
        if (regionalCity_->get()) regionalCity_->get()->Shutdown();
        *regionalCity_ = loaded;
    }
    return true;
}

// ---- life cycle -----------------------------------------------------------------------------------------------

bool cSC4App::RequestNewCity(cRZAutoRefCount<cISC4RegionalCity>* holder) {
    if (city_) {
        RequestCloseCity(true);
        if (city_) return false;
    }
    regionalCity_ = holder;
    return NewCity();
}

bool cSC4App::DoLoadCityUI(cRZString& path) {
    RZ::ADir(path);
    path.AssignFromCString("SavedCity");
    path.AppendChars(".sc4", 4);
    return true;
}

bool cSC4App::RequestLoadCity() {
    if (city_) {
        RequestCloseCity(true);
        if (city_) return false;
    }
    cRZString path;
    DoLoadCityUI(path);
    const bool loaded = LoadCity(path, nullptr);
    if (!loaded) Notify(cityHooks_, 0xca56ce8c, 0x4a56cea6);
    return loaded;
}

bool cSC4App::RequestCloseCity(bool confirmFirst) {
    sc4::cSC4AutoGamePause pause(false, true, true, true);
    if (!city_) return false;
    if (confirmFirst) {
        const bool answeredYes = cityHooks_.confirm && cityHooks_.confirm(Text(cityHooks_, 0x2a56cda8), Text(cityHooks_, 0x2a56cd9b));
        if (answeredYes) {
            const bool saved = RequestSaveCity(true, cRZKeyboard::IsKeyDownNowStatic(kKeyMenu));
            return saved ? CloseCity() : false;
        }
    }
    return CloseCity();
}

bool cSC4App::RequestSaveCity(bool announce, bool alternate) {
    sc4::cSC4AutoGamePause pause(false, true, true, true);
    if (!city_) return false;
    if (city_->IsSaveDisabled()) {
        Notify(cityHooks_, 0xaa666afd, 0xca666b15);
        return false;
    }
    cRZString path;
    city_->GetCitySaveFilePath(path);
    if (path.Strlen() == 0) return false;
    const bool saved = SaveCity(path, alternate);
    city_->GetCitySaveFilePath(path);
    cRZString name;
    cRZFile::GetPathName(path, name, true);
    const std::string suffix = std::string(" (") + name.ToChar() + ")";
    if (!saved) Notify(cityHooks_, 0x2a56cda8, 0x4a56ce71, suffix);
    else if (announce) Notify(cityHooks_, 0x2a56cda8, 0x2a56ce36, suffix);
    return saved;
}

bool cSC4App::SaveCity(bool alternate) {
    if (!city_) return false;
    cRZString path;
    city_->GetCitySaveFilePath(path);
    if (path.Strlen() == 0) return false;
    return SaveCity(path, alternate);
}

// The dialogs of RequestQuit and RequestGoToRegionView. Returns true when the request goes on.
bool cSC4App::RequestQuit(bool askPlayer, bool saveFirst) {
    sc4::cSC4AutoGamePause pause(false, true, true, true);
    auto scripted = [&](std::uint32_t script, std::uint32_t window) { return cityHooks_.scriptedModal ? cityHooks_.scriptedModal(script, window) : 0; };
    if (!city_ || !city_->IsSaveDisabled()) {
        std::uint32_t answer;
        if (askPlayer) answer = static_cast<std::uint32_t>(scripted(0x0a55161d, 0xaa921f4f));
        else answer = saveFirst ? kDialogYes : kDialogNo;
        if (answer != kDialogNo) {
            if (answer != kDialogYes) return false;
            RunMessageServerPump(100, 500, 5000);
            RunMessageServer2Pump(100, 500, 5000);
            RequestSaveCity(false, cRZKeyboard::IsKeyDownNowStatic(kKeyMenu));
        }
    } else if (askPlayer) {
        if (static_cast<std::uint32_t>(scripted(0x4a89b3f2, 0x2a96ed21)) != kDialogOk) return false;
    }
    if (!SL::spMS2) return true;
    cRZAutoRefCount<cRZMessage2Standard> message(new cRZMessage2Standard());
    message->SetType(kMessageQuit);
    SL::spMS2->GeneralMessagePostToTarget(message.get(), static_cast<cIGZMessageTarget2*>(this));
    return true;
}

bool cSC4App::RequestQuitFromRegion(bool askPlayer) {
    if (askPlayer) {
        if (sMacQuitDialogIsShowing_) return false;
        sMacQuitDialogIsShowing_ = true;
        std::uint32_t answer;
        {
            sc4::cSC4AutoGamePause pause(false, true, true, true);
            answer = static_cast<std::uint32_t>(cityHooks_.scriptedModal ? cityHooks_.scriptedModal(0x4a551b4c, 0xaa921f4f) : 0);
        }
        sMacQuitDialogIsShowing_ = false;
        if (answer == kDialogQuitRegionNo) return false;
    }
    if (SL::spMS2) {
        cRZAutoRefCount<cRZMessage2Standard> message(new cRZMessage2Standard());
        message->SetType(kMessageQuit);
        SL::spMS2->GeneralMessagePostToTarget(message.get(), static_cast<cIGZMessageTarget2*>(this));
    }
    return true;
}

bool cSC4App::RequestGoToRegionView(bool askPlayer) {
    if (askPlayer) {
        sc4::cSC4AutoGamePause pause(false, true, true, true);
        auto scripted = [&](std::uint32_t script, std::uint32_t window) { return static_cast<std::uint32_t>(cityHooks_.scriptedModal ? cityHooks_.scriptedModal(script, window) : 0); };
        if (!city_ || !city_->IsSaveDisabled()) {
            const std::uint32_t answer = scripted(0x6a553aa4, 0xaa921f4f);
            if (answer != kDialogNo) {
                if (answer != kDialogYes) return false;
                RunMessageServerPump(100, 500, 5000);
                RunMessageServer2Pump(100, 500, 5000);
                RequestSaveCity(false, cRZKeyboard::IsKeyDownNowStatic(kKeyMenu));
            }
        } else if (scripted(0x4a89b3f2, 0x2a96ed21) != kDialogOk) {
            return false;
        }
    }
    if (cityHooks_.flushGraphics) { cityHooks_.flushGraphics(); cityHooks_.flushGraphics(); }
    CloseCity();
    if (!city_) {
        if (cityHooks_.destroyCurrentScreen) cityHooks_.destroyCurrentScreen();
        if (cityHooks_.createRegionScreen) cityHooks_.createRegionScreen();
    }
    return true;
}

bool cSC4App::Quit() {
    const bool alreadyQuitting = field7c_;
    if (!alreadyQuitting) {
        field7c_ = true;
        if (cityHooks_.flushGraphics) { cityHooks_.flushGraphics(); cityHooks_.flushGraphics(); }
        CloseCity();
        if (cityHooks_.destroyCurrentScreen) cityHooks_.destroyCurrentScreen();
        if (cityHooks_.quitFramework) cityHooks_.quitFramework();
    }
    return !alreadyQuitting;
}

bool cSC4App::NewCity() {
    if (city_) return false;
    BusyScope busy(cityHooks_);
    sc4::cSC4AutoFlushInputMessageQueue flush(1000);
    if (cityHooks_.destroyCurrentScreen) cityHooks_.destroyCurrentScreen();
    if (cityHooks_.createCityScreen) cityHooks_.createCityScreen();
    city_ = cityHooks_.createCity ? cityHooks_.createCity() : nullptr;
    if (!city_) return false;
    if (regionalCity_ && regionalCity_->get()) regionalCity_->get()->SetupCity(city_);
    PostCityMessage(city_, 0x26d31ec0, 1, 0, false);
    city_->Init();
    PostCityMessage(city_, 0x2b96b3ea, 0, 0, true);
    PostCityMessage(city_, 0x26d31ec1, 1, 0, true);
    PostCityMessage(city_, 0xab96b05f, 0, 0, true);
    PostCityMessage(city_, 0xea8ae29a, 0, 0, true);
    AddRef();
    RunMessageServerPump(100, 500, 5000);
    RunMessageServer2Pump(100, 500, 5000);
    Release();
    if (SL::spRM) SL::spRM->ForceGarbageCollection();
    return true;
}

// Opens the city file as a database segment, registers it with the resource manager, reads the application record (which
// creates the city) and brings the city up; the "city loading" / "city loaded" messages carry the segment and the path.
bool cSC4App::LoadCity(const cRZString& path, cRZAutoRefCount<cISC4RegionalCity>* holder) {
    if (city_) CloseCity();
    regionalCity_ = holder;
    BusyScope busy(cityHooks_);
    sc4::cSC4AutoFlushInputMessageQueue flush(1000);
    bool result = false;
    cIGZPersistDBSegment* segment = cityHooks_.createCitySegment ? cityHooks_.createCitySegment() : nullptr;
    if (!segment) return false;
    if (segment->Init()) {
        segment->SetPath(path);
        if (segment->Open(true, false)) {
            const std::uint32_t version = cityHooks_.readLoadSaveVersion ? cityHooks_.readLoadSaveVersion(segment) : 0;
            if (version >= 0x3e && version <= 0x42) {
                cRZAutoRefCount<cRZString> pathCopy(new cRZString(path.ToChar()));
                {
                    cRZAutoRefCount<cRZMessage2Standard> loading(new cRZMessage2Standard());
                    loading->SetType(0x26c63340);
                    loading->SetVoid1(segment);
                    loading->SetVoid2(pathCopy.get());
                    SendMessage(*loading);
                }
                citySegment_ = segment;
                segment->AddRef();
                if (SL::spRM) SL::spRM->RegisterDBSegmentBack(*segment);
                // 0x1000457c8: the segment's vslot 0x38 is cSC4DBSegment::LoadClassObjects(version) - the class table
                // and every serialized object of the file, created through the serializer's factory. Only then is the
                // application record read: the stream {0x49dd6f4c, 0x49dd6f4e, 1} must have record version 1, and the
                // file's save version is handed to it as user data (stream vslot 0x98) so the serializer reads object ids
                // 32-bit below save version 0x42. The hooks stand in for both steps in tests.
                const auto loadClassObjects = [&]() -> bool {
                    if (cityHooks_.loadClassObjects) return cityHooks_.loadClassObjects(segment, version);
                    sc4::cISC4DBSegment* isc4 = nullptr;
                    if (!segment->QueryInterface(sc4::kGZIID_cISC4DBSegment, reinterpret_cast<void**>(&isc4)) || !isc4) return false;
                    const bool loaded = isc4->LoadClassObjects(version);
                    isc4->Release();
                    return loaded;
                };
                const auto readAppRecord = [&]() -> bool {
                    auto reader = [&](cIGZIStream& stream) { return Read(stream, segment); };
                    if (cityHooks_.readAppRecord) return cityHooks_.readAppRecord(segment, version, reader);
                    sc4::cSC4AutoLoadStream record(segment, 0x49dd6f4c, 0x49dd6f4e, 1);
                    cRZAutoRefCount<cRZVariant> versionValue(new cRZVariant(version));
                    sc4::cISC4DBSegmentIStream* stream = record.GetStream();
                    if (!record.IsOpen() || !stream || record.GetVersion() != 1) return false;
                    stream->SetUserData(versionValue.get());
                    return reader(*stream);
                };
                const bool classesLoaded = loadClassObjects();
                if (std::getenv("SC4_DEBUG")) std::fprintf(stderr, "LoadCity: class objects %s\n", classesLoaded ? "loaded" : "FAILED");
                const bool appRecordRead = classesLoaded && readAppRecord();
                if (std::getenv("SC4_DEBUG") && classesLoaded) std::fprintf(stderr, "LoadCity: app record %s\n", appRecordRead ? "read" : "FAILED");
                if (classesLoaded && appRecordRead) {
                    // The original dereferences the caller's regional city unconditionally; tools that load a bare city file (the
                    // probe) pass none, so the serial number stays 0 then.
                    city_->SetCitySerialNumber(regionalCity_ && regionalCity_->get() ? regionalCity_->get()->GetCitySerialNumber() : 0);
                    city_->SetCitySaveFilePath(path);
                    city_->SetCityNameChanged(false);
                    if (cityHooks_.destroyCurrentScreen) cityHooks_.destroyCurrentScreen();
                    if (cityHooks_.createCityScreen) cityHooks_.createCityScreen();
                    cRZVariant versionValue(static_cast<std::uint32_t>(version));
                    {
                        cRZAutoRefCount<cRZMessage2Standard> loaded(new cRZMessage2Standard());
                        loaded->SetType(0x26c63341);
                        loaded->SetVoid1(segment);
                        loaded->SetVoid2(pathCopy.get());
                        loaded->SetVoid3(&versionValue);
                        SendMessage(*loaded);
                    }
                    {
                        cRZAutoRefCount<cRZMessage2Standard> finished(new cRZMessage2Standard());
                        finished->SetType(0x26c63342);
                        finished->SetVoid1(segment);
                        finished->SetVoid2(pathCopy.get());
                        SendMessage(*finished);
                    }
                    PostCityMessage(city_, 0x26d31ec0, 0, 0, false);
                    city_->Init();
                    PostCityMessage(city_, 0x2b96b3ea, 0, 0, true);
                    PostCityMessage(city_, 0x26d31ec1, 0, 0, true);
                    PostCityMessage(city_, 0xab96b05f, 0, 0, true);
                    PostCityMessage(city_, 0xea8ae29a, 0, 0, true);
                    result = true;
                }
                if (!result && city_) {
                    city_->Release();
                    city_ = nullptr;
                }
            }
        }
    }
    if (!result) UnregisterCityDBSegment();
    segment->Release();
    AddRef();
    RunMessageServerPump(100, 500, 5000);
    RunMessageServer2Pump(100, 500, 5000);
    Release();
    if (SL::spRM) SL::spRM->ForceGarbageCollection();
    return result;
}

void cSC4App::UnregisterCityDBSegment() {
    if (!citySegment_) return;
    if (SL::spRM) SL::spRM->UnregisterDBSegment(*citySegment_);
    citySegment_->Close();
    citySegment_->Shutdown();
    citySegment_->Release();
    citySegment_ = nullptr;
}

bool cSC4App::PartialCloseCity() {
    bool result = false;
    if (city_) {
        sc4::SC4GamePause(true, false, true, true, true);
        RunMessageServerPump(100, 500, 5000);
        RunMessageServer2Pump(100, 500, 5000);
        PostCityMessage(city_, 0x26d31ec2, 0, 0, false);
        city_->Shutdown();
        PostCityMessage(city_, 0x26d31ec3, 0, 0, true);
        city_->Release();
        city_ = nullptr;
        UnregisterCityDBSegment();
        result = true;
    }
    if (SL::spRM) SL::spRM->ForceGarbageCollection();
    return result;
}

// The region screen's refresh for the closed city (which city of the region is selected, which neighbours need a new
// thumbnail) is the host's regionCityClosed hook until the region classes are recovered.
bool cSC4App::CloseCity() {
    cISC4RegionalCity* regional = regionalCity_ ? regionalCity_->get() : nullptr;
    std::string savePath;
    if (regional) {
        cRZString path;
        regional->GetCitySaveFilePath(path);
        savePath = path.ToChar();
    }
    if (!PartialCloseCity()) return false;
    if (regional && cityHooks_.regionCityClosed) cityHooks_.regionCityClosed(regional, savePath);
    if (SL::spRM) SL::spRM->ForceGarbageCollection();
    return true;
}

// ---- the height-field dialog (window message filter) ---------------------------------------------------------------

// The app is the message filter of the "establish city" window (0x27df05be): its height-field button (0x30) opens the
// load-height-field dialog with the folder remembered under [Settings]/"Terrain Load/Save Location" in the app's .ini and
// copies the chosen path into the new city specification (a 0x200-byte buffer); the other command (0x20) closes the
// window with code 0xf. Every message of another window or sender is not handled.
bool cSC4App::DoMessage(cIGZWin* window, cGZMessage& message) {
    if (!window || window->GetID() != 0x27df05be || message.type != kGZMsgCommand || message.param1 != 0x287259f6) return false;
    cIGZWinMgr* manager = RZ::WM();
    if (message.param2 == 0x20) {
        if (manager) manager->EndDialog(0xf, window, 0x20);
        return true;
    }
    if (message.param2 != 0x30 || !cityHooks_.createLoadHeightFieldDialog) return true;

    const cRZString section("Settings"), key("Terrain Load/Save Location");
    cRZIniFile ini;
    cRZString iniPath;
    GetAppIniFilePath(iniPath);
    ini.SetPath(iniPath);
    cRZString directory;
    ini.ReadEntry(section, key, directory);

    sc4::ILoadHeightFieldDialog* dialog = cityHooks_.createLoadHeightFieldDialog();
    if (!dialog) return true;
    dialog->AddRef();
    dialog->SetFlag(0x10000, true);     // kWinPrivateBuffer
    dialog->SetFlag(0x100, true);       // kWinFlag100
    dialog->SetInitialDirectory(directory);
    dialog->Init();
    if (manager) {
        if (cIGZWin* main = manager->GetMainWindow()) dialog->GZWinMoveTo((main->GetW() - dialog->GetW()) / 2, (main->GetH() - dialog->GetH()) / 2);
    }
    cIGZWin* dialogWindow = dialog->AsWindow();
    if (dialogWindow && cityHooks_.restoreDialogLocation) cityHooks_.restoreDialogLocation(dialogWindow);

    bool picked = false;
    cRZString selected;
    if (manager && manager->RunPopup(dialogWindow) != 0) {
        cRZString current;
        dialog->GetInitialDirectory(current);
        ini.WriteEntry(section, key, current);
        picked = dialog->GetSelection(selected);
    }
    if (dialogWindow && cityHooks_.saveDialogLocation) cityHooks_.saveDialogLocation(dialogWindow);
    if (manager && dialogWindow) manager->DestroyWindow(dialogWindow);
    dialog->Release();
    if (picked) {
        std::string path = selected.ToChar();
        if (path.size() > 0x1ff) path.resize(0x1ff);
        std::strcpy(reinterpret_cast<char*>(newCitySpecification_), path.c_str());
    }
    return true;
}

// ---- saving --------------------------------------------------------------------------------------------------------

namespace {
constexpr std::uint32_t kSaveVersionStream[] = {0xb7d45e0, 0xb7d45e1, 0xb7d45e2};   // version record, stream version 3
constexpr std::uint32_t kAppRecordStream[] = {0x49dd6f4c, 0x49dd6f4e, 1};            // the application record, version 1
// the save progress messages: segment + temporary path + final path + (data4 = the alternate flag)
constexpr std::uint32_t kSaveStartedMessage = 0x26c63343, kSaveDataWrittenMessage = 0x26c63346,
                        kSaveAppRecordMessage = 0x26c63344, kSaveFinishedMessage = 0x26c63345;
}

// Writes the city file. The canonical path is rebuilt from the city name and serial number; the file is written to a
// temporary segment, then the old file is renamed aside, the temporary file takes its place and the backup is dropped.
// PARTIAL: the canonical path comes from SC4CreateSavePathForCity (0x1001e98a4, unrecovered) through a host hook, and the
// neighbour sweep that rewrites the connected regional cities goes through two more hooks (SL::spNeighborsSim vslot 0xa0
// and SC4WriteRegionalCity(path, city) @ 0x1001e9799).
bool cSC4App::SaveCity(const cRZString& path, bool alternate) {
    sc4::cSC4AutoGamePause pause(false, true, true, true);
    sc4::cSC4AutoFlushInputMessageQueue flush(1000);
    sc4::cSC4AutoDisableAppSwitching switchBlock;
    (void)switchBlock;
    BusyScope busy(cityHooks_);
    if (!city_) return false;

    cRZString cityName;
    city_->GetCityName(cityName);
    const std::uint32_t serial = city_->GetCitySerialNumber();
    cRZString savePath;
    if (cityHooks_.createSavePathForCity) cityHooks_.createSavePathForCity(cityName, serial, savePath);
    const bool pathDiffers = savePath.Strlen() == 0 || std::strcmp(savePath.ToChar(), path.ToChar()) != 0;

    cRZString previousSavePath;
    city_->GetCitySaveFilePath(previousSavePath);
    city_->SetCitySaveFilePath(savePath);

    bool success = false;
    cRZString tempPath;
    if (cRZFile::MakeTempPathName(tempPath, nullptr)) {
        cRZAutoRefCount<cRZString> tempCopy(new cRZString(tempPath.ToChar()));
        cRZAutoRefCount<cRZString> saveCopy(new cRZString(savePath.ToChar()));
        auto* rawSegment = new sc4::cSC4DBSegment();
        cIGZPersistDBSegment* segment = rawSegment->AsIGZPersistDBSegment();
        segment->AddRef();
        auto sendProgress = [&](std::uint32_t type) {
            if (!SL::spMS2) return;
            cRZAutoRefCount<cRZMessage2Standard> message(new cRZMessage2Standard());
            message->SetType(type);
            message->SetVoid1(segment);
            message->SetVoid2(tempCopy.get());
            message->SetVoid3(saveCopy.get());
            message->SetData4(alternate ? 1 : 0);
            SL::spMS2->MessageSend(message.get());
        };

        const bool debug = std::getenv("SC4_DEBUG") != nullptr;
        if (debug) std::printf("save: %s -> temp %s\n", savePath.ToChar(), tempPath.ToChar());
        if (segment->Init() && segment->SetPath(*tempCopy) && segment->Open(false, true)) {
            sendProgress(kSaveStartedMessage);
            sendProgress(kSaveDataWrittenMessage);
            if (cityHooks_.writeLoadSaveVersion) (void)cityHooks_.writeLoadSaveVersion(segment);
            {
                sc4::cSC4AutoSaveStream record(segment, kSaveVersionStream[0], kSaveVersionStream[1], kSaveVersionStream[2], 3);
                if (sc4::cISC4DBSegmentOStream* stream = record.GetStream()) {
                    // the four module version words (cRZVersion::GetModuleVersion always answers 0xffffffff)
                    cRZVersion version;
                    for (std::uint32_t module = 0; module < 4; ++module) stream->SetUint32(version.GetModuleVersion(module));
                }
            }
            {
                sc4::cSC4AutoSaveStream appRecord(segment, kAppRecordStream[0], kAppRecordStream[1], kAppRecordStream[2], 1);
                if (sc4::cISC4DBSegmentOStream* stream = appRecord.GetStream()) {
                    if (Write(*stream, segment)) {
                        sendProgress(kSaveAppRecordMessage);
                        sendProgress(kSaveFinishedMessage);
                        success = true;
                    }
                }
            }
            if (debug) std::printf("save: app record %s\n", success ? "written" : "FAILED");
            if (success) success = rawSegment->SaveClassObjects();
            if (debug) std::printf("save: class objects %s\n", success ? "written" : "FAILED");
            segment->Close();
            segment->Shutdown();
        } else {
            if (debug) std::printf("save: temp segment could not be opened\n");
            segment->Shutdown();
        }
        segment->Release();
    }

    UnregisterCityDBSegment();
    cRZString backupPath;
    if (success) {
        city_->SetCityNameChanged(false);
        if (cRZFile::FileExists(savePath)) {
            backupPath = savePath.ToChar();
            backupPath.AppendChars("_", 1);
            if (cRZFile::FileExists(backupPath)) success = cRZFile::Remove(backupPath);
            if (success) success = cRZFile::Rename(savePath, backupPath);
        }
    }
    if (success) {
        if (cRZFile::Rename(tempPath, savePath)) {
            if (backupPath.Strlen() != 0) cRZFile::Remove(backupPath);
        } else {
            cRZFile::Remove(tempPath);
            if (backupPath.Strlen() != 0) cRZFile::Rename(backupPath, savePath);
            success = false;
        }
        if (pathDiffers && cRZFile::FileExists(path)) cRZFile::Remove(path);
    }
    if (tempPath.Strlen() != 0) cRZFile::Remove(tempPath);
    if (std::getenv("SC4_DEBUG")) std::printf("save: %s %s\n", savePath.ToChar(), success ? "done" : "FAILED");

    city_->SetCitySaveFilePath(success ? savePath : previousSavePath);
    cRZString reopenPath;
    city_->GetCitySaveFilePath(reopenPath);
    RegisterCityDBSegment(reopenPath);

    if (success) {
        std::vector<cISC4RegionalCity*> neighbours;
        if (cityHooks_.connectedCities) cityHooks_.connectedCities(neighbours);
        for (cISC4RegionalCity* neighbour : neighbours) {
            if (!neighbour) continue;
            cRZString neighbourPath;
            neighbour->GetCitySaveFilePath(neighbourPath);
            if (neighbourPath.Strlen() != 0 && cityHooks_.writeRegionalCityFile) cityHooks_.writeRegionalCityFile(neighbourPath, neighbour);
        }
    }
    return success;
}

// The segment the city file is mounted as: <path> opened read-only, kept in +0x7a8 and registered at the back of the
// resource manager's segment list. Its reference is held by citySegment_.
void cSC4App::RegisterCityDBSegment(const cRZString& path) {
    auto* rawSegment = new sc4::cSC4DBSegment();
    cIGZPersistDBSegment* segment = rawSegment->AsIGZPersistDBSegment();
    segment->AddRef();
    segment->Init();
    segment->SetPath(path);
    if (!segment->Open(true, false)) {
        segment->Shutdown();
    } else {
        citySegment_ = segment;
        segment->AddRef();
        if (SL::spRM) SL::spRM->RegisterDBSegmentBack(*segment);
    }
    segment->Release();
}

// ---- obliterating a city -------------------------------------------------------------------------------------------

// Saves an empty ("obliterated") city over the city's file: the camera position is kept across the close, the city is
// partially closed, the old file is removed, a new empty city is created with a temporary segment parked in +0x670,
// saved to the old path and the segment is dropped. Returns false only when no temporary path could be made.
bool cSC4App::DoCityObliteration() {
    BusyScope busy(cityHooks_);
    bool result = false;
    cRZString tempPath;
    if (cRZFile::MakeTempPathName(tempPath, nullptr)) {
        result = true;
        SaveCityForObliteration(tempPath);
    }

    int zoom = 0, rotation = 0;
    if (SL::spViewUtilities && cityHooks_.viewZoomAndRotation) cityHooks_.viewZoomAndRotation(zoom, rotation);

    cRZString previousPath;
    if (city_) {
        city_->GetCitySaveFilePath(previousPath);
        city_->SetEstablished(false);
    }
    PartialCloseCity();
    if (previousPath.Strlen() != 0) cRZFile::Remove(previousPath);

    auto* rawSegment = new sc4::cSC4DBSegment();
    cIGZPersistDBSegment* segment = rawSegment->AsIGZPersistDBSegment();
    segment->AddRef();
    if (segment->Init()) {
        segment->SetPath(tempPath);
        if (segment->Open(true, false)) {
            obliterationSegment_ = segment;
            NewCity();
            if (city_) {
                city_->SetEstablished(false);
                city_->SetCitySaveFilePath(previousPath);
            }
            obliterationSegment_ = nullptr;
            segment->Close();
            if (city_) SaveCity(previousPath, false);
        }
    }
    if (SL::spView3D && cityHooks_.setViewZoomAndRotation) cityHooks_.setViewZoomAndRotation(zoom, rotation);
    segment->Shutdown();
    segment->Release();
    cRZFile::Remove(tempPath);
    return result;
}

// The city object writes its empty state into <path> (opened for writing); the game is paused and the input queue
// flushed around it. Always answers true.
bool cSC4App::SaveCityForObliteration(const cRZString& path) {
    sc4::cSC4AutoFlushInputMessageQueue flush(1000);
    sc4::cSC4AutoGamePause pause(false, true, true, true);
    auto* rawSegment = new sc4::cSC4DBSegment();
    cIGZPersistDBSegment* segment = rawSegment->AsIGZPersistDBSegment();
    segment->AddRef();
    if (segment->Init()) {
        segment->SetPath(path);
        if (segment->Open(false, true)) {
            if (city_) city_->SaveObliterated(segment);
            segment->Close();
        }
        segment->Shutdown();
    }
    segment->Release();
    return true;
}
