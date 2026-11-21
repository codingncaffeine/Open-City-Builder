#include "sc4/AppResources.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <list>
#include <string>
#include <vector>

#include "gz/GZCOMDirector.h"
#include "gz/RZServices.h"
#include "gz/cGZCOM.h"
#include "gz/cGZDBSegmentPackedFile.h"
#include "gz/cGZPersistDBSegmentMultiPackedFiles.h"
#include "gz/FontSys.h"
#include "gz/ScriptTerp.h"
#include "gz/WinCtrlMgr.h"
#include "gz/WinFont.h"
#include "gz/cIGZFileSystem.h"
#include "gz/cIGZFrameWork.h"
#include "gz/cIGZPersistDBSegment.h"
#include "gz/cIGZPersistResourceManager.h"
#include "rz/LanguageManager.h"
#include "rz/cRZCmdLine.h"
#include "rz/RZStringUtils.h"
#include "rz/cRZDirectory.h"
#include "rz/cRZFile.h"
#include "rz/cRZIniFile.h"
#include "sc4/App.h"
#include "sc4/DBSegment.h"
#include "sc4/FontStyleSpec.h"
#include "sc4/ServiceLocator.h"

namespace {
constexpr std::uint32_t kPersistResourceManagerService = 0x056b906e, kPersistResourceManagerIid = 0x656b8efc;
constexpr std::uint32_t kSegmentIid = 0x656b8f0c;                      // cIGZPersistDBSegment
constexpr std::uint32_t kPackedFileClass = 0x086d4a89, kMultiPackedFilesClass = 0x090f8619;

cIGZPersistResourceManager* ResourceManager(cRZAutoRefCount<cIGZPersistResourceManager>& holder) {
    cIGZFrameWork* framework = RZGetFramework();
    if (framework) framework->GetSystemService(kPersistResourceManagerService, kPersistResourceManagerIid, holder.AsOutParam());
    return holder.get();
}

// The segment object: from COM, else built here (the resource director supplies the same two classes).
cRZAutoRefCount<cIGZPersistDBSegment> CreateSegment(bool multiFile) {
    cRZAutoRefCount<cIGZPersistDBSegment> segment;
    cIGZCOM* com = GZCOM();
    if (com && com->CreateInstance(multiFile ? kMultiPackedFilesClass : kPackedFileClass, kSegmentIid, segment.AsOutParam()) && segment) return segment;
    if (multiFile) return cRZAutoRefCount<cIGZPersistDBSegment>(static_cast<cIGZPersistDBSegment*>(new cGZPersistDBSegmentMultiPackedFiles()));
    return cRZAutoRefCount<cIGZPersistDBSegment>(static_cast<cIGZPersistDBSegment*>(new cGZDBSegmentPackedFile()));
}

// the files of a directory matching the *.dat glob (names only, sorted as the file system lists them)
std::vector<std::string> DatFilesIn(const std::string& directory) {
    std::vector<std::string> names;
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) return names;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        std::string name = entry.path().filename().string();
        if (entry.is_regular_file() && name.size() > 4 && name.compare(name.size() - 4, 4, ".dat") == 0) names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::string> AllFilesBelow(const std::string& directory) {
    std::vector<std::string> paths;
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) return paths;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory, error)) {
        if (entry.is_regular_file()) paths.push_back(entry.path().string());
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

std::string Extension(const std::string& path) {
    std::string::size_type dot = path.rfind('.');
    std::string::size_type slash = path.rfind('/');
    return dot == std::string::npos || (slash != std::string::npos && dot < slash) ? std::string() : path.substr(dot);
}
}  // namespace

void SetupPackedFileSegment(const cRZString& path, bool multiFile, bool onlyDatabases) {
    cRZAutoRefCount<cIGZPersistDBSegment> segment = CreateSegment(multiFile);
    if (!segment) return;
    if (onlyDatabases && cGZDBSegmentPackedFile::IsDatabaseFile(path, true) == -1) return;
    if (segment->Init()) {
        if (segment->SetPath(path) && segment->Open(true, false)) {
            cRZAutoRefCount<cIGZPersistResourceManager> manager;
            if (ResourceManager(manager) && manager->RegisterDBSegment(*segment)) return;
        }
        segment->Shutdown();
    }
}

// Registers, in order: the *.dat files of the data directory and of its language folder, the sku data, then every
// database below the plugin directory and the user plugin directory (the directories are created when missing) and
// the directories themselves as multi-file segments. At least three segments must exist afterwards.
bool cSC4App::SetupResources() {
    if (GetDebugFunctionalityEnabled()) {
        LoadRegistry();
        if (startupHooks_.registerSegmentCategoryCallback) startupHooks_.registerSegmentCategoryCallback();
    }

    cRZString directory;
    GetDataDirectory(directory, -1);
    for (const std::string& name : DatFilesIn(directory.ToChar())) SetupPackedFileSegment(cRZString((std::string(directory.ToChar()) + name).c_str()), false, false);
    GetDataDirectory(directory, 0);
    for (const std::string& name : DatFilesIn(directory.ToChar())) SetupPackedFileSegment(cRZString((std::string(directory.ToChar()) + name).c_str()), false, false);

    GetSkuSpecificDirectory(directory);
    SetupPackedFileSegment(directory, true, false);

    auto pluginDirectory = [&](auto getter) {
        (this->*getter)(directory);
        std::error_code error;
        if (!std::filesystem::is_directory(directory.ToChar(), error)) std::filesystem::create_directory(directory.ToChar(), error);
        for (const std::string& file : AllFilesBelow(directory.ToChar())) {
            if (Extension(file) != ".dat") SetupPackedFileSegment(cRZString(file.c_str()), false, true);
        }
        (this->*getter)(directory);
        SetupPackedFileSegment(directory, true, false);
    };
    pluginDirectory(&cSC4App::GetPluginDirectory);
    pluginDirectory(&cSC4App::GetUserPluginDirectory);

    cRZAutoRefCount<cIGZPersistResourceManager> manager;
    if (!ResourceManager(manager)) return false;
    // resource type associations resolved through COM (no factory object): class id, resource type
    static constexpr std::uint32_t kObjectFactories[][2] = {
        {0x0a63df8c, 0xca63e2a3}, {0x00436eb4, 0x2026960b}, {0xc8696797, 0x686aa4b0}, {0xa2ffb5d3, 0x856ddbac},
        {0x3ab50e2a, 0x7ab50e44}, {0x3ab50e2a, 0x7ab50e45}, {0x69b6f01c, 0x29a5d1ec}, {0x69b6f01c, 0x09adcd75},
        {0xfad0f0b6, 0x5ad0e817}, {0x453429b3, 0x6534284a}, {0x053429c8, 0x05342861}, {0xa83479ea, 0xa83479d3},
        {0x496678fe, 0x296678f7}, {0xea5118b5, 0xea5118b0}, {0x42e411c2, 0xa2e3d533},
    };
    for (const auto& pair : kObjectFactories) manager->RegisterObjectFactory(pair[0], pair[1], nullptr);
    return manager->GetSegmentCount() > 2;
}

// ---- the database-segment registry -------------------------------------------------------------------------------
//
// Sys/Resources.ini describes the loose segments the game mounts:
//
//   [DBSegments]
//   <segment id> = <class id> <path spec>        (the path spec may hold %DataDirectory% and %Language%)
//   [DBSegmentDirectories]
//   <segment id> = <class id> <path spec>
//
// LoadRegistry reads the two [DBSegments] switches and hands every entry of the two sections to its callback; the
// callbacks build the segment through COM and register it with the resource manager.

bool gbUsingPackedExemplars = true;
bool gbUsingPackedTextures = true;

namespace {
constexpr std::uint32_t kDBSegmentIid = 0x656b8f0c;                 // cIGZPersistDBSegment
constexpr std::uint32_t kDirectoryFilesIid = 0x856c8cf7;            // cIGZPersistDBSegmentDirectoryFiles
constexpr std::uint32_t kMultiPackedFilesIid = 0xe90f85f3;          // cGZPersistDBSegmentMultiPackedFiles
constexpr const char* kDataDirectoryToken = "%DataDirectory%";
constexpr const char* kLanguageToken = "%Language%";

// The language manager's folder for the current language ("English" and friends), empty when there is none.
std::string LanguageFolder() {
    cRZString folder;
    if (cIGZLanguageManager* languages = RZ::LM()) languages->GetLanguageDirectoryName(folder, 0);
    return folder.ToChar();
}

void ReplaceAll(std::string& text, const char* token, const std::string& with) {
    const std::size_t length = std::strlen(token);
    for (std::size_t at = text.find(token); at != std::string::npos; at = text.find(token, at + with.size()))
        text.replace(at, length, with);
}
}  // namespace

// The registry category callback of the segment creators (registered by SetupResources when debug functionality is on).
// The key is the segment id, the value "<class id> <path spec>"; the segment is created through COM, opened read-only
// and registered with the resource manager. PARTIAL: the original keeps two path candidates (the data directory and the
// data directory plus the language folder) and picks the one that exists as a file or as a directory depending on the
// segment kind; that two-candidate choice is not reproduced exactly (the language-folder retry below is an
// approximation).
void cSC4App::CreateDBSegmentCallback(const cIGZString& key, const cIGZString& value, void* context) {
    auto* app = static_cast<cSC4App*>(context);
    cRZString spec;
    spec.AssignFromIGZString(value);
    cRZString first = spec.SplitToken(" ");
    if (first.Strlen() == 0) return;
    const std::uint32_t segmentId = RZAtoUint32HexA(key.ToChar());
    const std::uint32_t classId = RZAtoUint32HexA(first.ToChar());
    SC4AdjustPathSeparatorsForPlatform(spec, spec);

    cIGZCOM* com = GZCOM();
    cRZAutoRefCount<cIGZPersistDBSegment> segment;
    if (!com || !com->CreateInstance(classId, kDBSegmentIid, segment.AsOutParam()) || !segment) return;
    if (!segment->Init()) return;
    segment->SetSegmentID(segmentId);

    // the kind decides whether the path has to exist as a directory or as a file
    bool directoryKind = false, multiFileKind = false;
    {
        cRZAutoRefCount<cIGZUnknown> probe;
        if (segment->QueryInterface(kDirectoryFilesIid, probe.AsOutParam())) directoryKind = true;
    }
    {
        cRZAutoRefCount<cIGZUnknown> probe;
        if (segment->QueryInterface(kMultiPackedFilesIid, probe.AsOutParam())) multiFileKind = true;
    }

    std::string path = spec.ToChar();
    if (app) ReplaceAll(path, kDataDirectoryToken, app->dataDirectory_.ToChar());
    ReplaceAll(path, kLanguageToken, LanguageFolder());

    auto exists = [&](const std::string& candidate) {
        return directoryKind ? cRZDirectory::DoesDirectoryExist(cRZString(candidate.c_str()))
                             : cRZFile::FileExists(cRZString(candidate.c_str()));
    };
    if (!exists(path) && app) {
        const std::string folder = LanguageFolder();
        const std::string specText = spec.ToChar();
        const std::size_t tokenAt = specText.find(kDataDirectoryToken);
        if (!folder.empty() && tokenAt != std::string::npos) {
            std::string withLanguage = app->dataDirectory_.ToChar() + folder + "/" +
                                       specText.substr(tokenAt + std::strlen(kDataDirectoryToken) + 1);
            if (exists(withLanguage)) path = withLanguage;
        }
    }

    if (!segment->SetPath(cRZString(path.c_str()))) { segment->Shutdown(); return; }
    if (!segment->Open(true, false)) {
        if (directoryKind || multiFileKind) {
            cRZDirectory listing(cRZString(path.c_str()), true, false, false);
            (void)listing.GetDirectoryEntryCount();   // the original only probes the listing here
        }
        segment->Shutdown();
        return;
    }
    if (SL::spRM) SL::spRM->RegisterDBSegment(*segment);
    if (segment->IsOpen()) segment->Close();
    segment->Shutdown();
}

// Reads <data directory>/Sys/Resources.ini (when it exists): the -packedexemplars / -packedtextures command-line switches
// override the two packed-file globals first, then the [DBSegments] and [DBSegmentDirectories] entries go to their
// callbacks. The registry category registrations themselves stay a host hook (registerSegmentCategoryCallback) until the
// cGZRegistry service is recovered.
bool cSC4App::LoadRegistry() {
    cIGZFileSystem* files = RZ::FS();
    cRZString path;
    if (files) {
        path.AssignFromCString(files->DataDirectory());
        path.AppendChars("Sys", 3);
        path.AppendChars("/", 1);
        path.AppendChars("Resources.ini", 13);
    }
    if (path.Strlen() == 0 || !cRZFile::FileExists(path)) return true;

    cRZIniFile ini(path);
    // the switches are only read when the ini exists (kept)
    cIGZFrameWork* framework = RZGetFramework();
    cIGZCmdLine* commandLine = framework ? framework->CommandLine() : nullptr;
    auto switchFlag = [&](const char* name, bool& value) {
        cRZString text;
        if (!commandLine || !commandLine->IsSwitchPresent(cRZString(name), text, true)) return;
        text.MakeLower();
        if (text.IsEqualToChars("off", 3, false)) value = false;
        else if (text.IsEqualToChars("on", 2, false)) value = true;
    };
    switchFlag("packedexemplars", gbUsingPackedExemplars);
    switchFlag("packedtextures", gbUsingPackedTextures);
    ini.EnumEntries(cRZString("DBSegments"), RegisterDBSegmentCallback, this);
    ini.EnumEntries(cRZString("DBSegmentDirectories"), RegisterDBSegmentDirectoryFilesCallback, this);
    return true;
}

// The [DBSegments] / [DBSegmentDirectories] entries: the original keeps a static list of the groups it is inside
// (BEGIN_GROUP / END_GROUP markers) and only creates the segment when the group flags allow it. PARTIAL: the entries are
// created by the cGZRegistry service (vslot 0x30 + 0x18, class cGZRegistry, unrecovered), so this keeps the bookkeeping
// and the group filtering and leaves the actual creation to the host (see SC4APP-REGISTRY).
void cSC4App::RegisterDBSegmentCallback(const cRZString& key, const cRZString& value, void* context) {
    static std::list<cRZString> groupList;
    static std::uint32_t groupflags = 0;
    static const char* const kGroupTags[] = {"PACKED_EXEMPLARS", "NON_PACKED_EXEMPLARS", "PACKED_TEXTURES", "NON_PACKED_TEXTURES"};
    if (key.IsEqualToChars("BEGIN_GROUP", 11, false)) {
        groupList.push_back(value);
        for (std::uint32_t i = 0; i < 4; ++i) {
            if (value.IsEqualToChars(kGroupTags[i], static_cast<std::uint32_t>(std::strlen(kGroupTags[i])), false)) {
                groupflags |= 1u << i;
                break;
            }
        }
        return;
    }
    if (key.IsEqualToChars("END_GROUP", 9, false)) {
        for (std::uint32_t i = 0; i < 4; ++i) {
            if (value.IsEqualToChars(kGroupTags[i], static_cast<std::uint32_t>(std::strlen(kGroupTags[i])), false)) {
                groupflags &= ~(1u << i);
                break;
            }
        }
        if (!groupList.empty()) groupList.pop_back();
        return;
    }
    // packedexemplars / packedtextures exclude the segment groups 0x1|0x2 / 0x4|0x8
    if ((groupflags & 1) != 0 && !gbUsingPackedExemplars) return;
    if ((groupflags & 2) != 0 && gbUsingPackedExemplars) return;
    if ((groupflags & 4) != 0 && !gbUsingPackedTextures) return;
    if ((groupflags & 8) != 0 && gbUsingPackedTextures) return;
    (void)key;
    (void)value;
    (void)context;   // the creation is the registry's (cGZRegistry, unrecovered): see SC4APP-REGISTRY
}

// [DBSegmentDirectories]: the original asks the registry to create the segment the entry describes (vslot 0x30) and then
// hands the created object back (vslot 0x18); both slots belong to cGZRegistry, which is not recovered.
void cSC4App::RegisterDBSegmentDirectoryFilesCallback(const cRZString& key, const cRZString& value, void* context) {
    (void)key;
    (void)value;
    (void)context;
}

// ---- fonts ---------------------------------------------------------------------------------------------------------

// [Font Aliases] of Sys/Resources.ini, one entry per alias. The original swaps the arguments: the ini value is the name
// asked for and the key the substitute (AddSubstitution(value, key)).
void cSC4App::SetFontAliasStatic(const cRZString& key, const cRZString& value, void* /*context*/) {
    if (cIGZUnknown* system = RZ::FO()) static_cast<cIGZFontSys*>(system)->AddSubstitution(value, key);
}

// [Font Styles] section callback: the context is the application.
void cSC4App::SetFontStyleStatic(const cRZString& key, const cRZString& value, void* context) {
    static_cast<cSC4App*>(context)->SetFontStyle(key, value);
}

namespace {
// The vocabulary the font styles are added to while SetupWinCtrlMgr reads [Font Styles] (_spFontStyleScriptVocabulary);
// the UI script interpreter holds it, so scripts name the styles ("font=GenBodyLight").
IScriptVocab* gFontStyleScriptVocabulary = nullptr;

constexpr std::uint32_t kFontNoBackground = 0x8000000;   // the background color "none"
}  // namespace

void cSC4App::SetFontStyleVocabulary(IScriptVocab* vocabulary) { gFontStyleScriptVocabulary = vocabulary; }

std::uint32_t cSC4App::GraphicSystemColor(std::uint8_t r, std::uint8_t g, std::uint8_t b) const {
    if (startupHooks_.graphicSystemColor) return startupHooks_.graphicSystemColor(r, g, b);
    return std::uint32_t(r) << 16 | std::uint32_t(g) << 8 | b;
}

// The parsed specification (sc4/FontStyleSpec.h): the faces and size acquire the font with the style flags; the other
// parameters are set on the acquired font.
cIGZFont* cSC4App::CreateFontStyleFromSpecification(const cRZString& specification, std::uint32_t& styleId) {
    auto* fonts = static_cast<cIGZFontSys*>(RZ::FO());
    if (!fonts) return nullptr;
    const sc4::FontStyleSpec spec = sc4::ParseFontStyleSpecification(
        specification, [this](std::uint8_t r, std::uint8_t g, std::uint8_t b) { return GraphicSystemColor(r, g, b); });
    styleId = spec.styleId;
    cIGZFont* font = fonts->FontAcquire(spec.faces, 0, spec.size, static_cast<int>(spec.styleFlags), true);
    if (!font || !RZ::WCM()) return font;   // the parameters are set only when the control manager exists
    if (spec.antialiasing != 0) font->SetAntialiasing(spec.antialiasing);
    if (spec.colorsGiven) font->SetFontColor(spec.textColor, spec.backgroundColor);
    else font->SetFontColor(GraphicSystemColor(0, 0, 0), kFontNoBackground);
    if (spec.gamma != 0.0f) font->SetAntialiasingGammaCorrection(spec.gamma);
    if (spec.sharpness != 0.0f) font->SetAntialiasingSharpness(spec.sharpness);
    if (spec.scaleX != 1.0f || spec.scaleY != 1.0f) font->SetScaling(spec.scaleX, spec.scaleY);
    if (spec.charSpacing != 0 || spec.lineSpacing != 0) font->SetSpacingAdjustment(spec.charSpacing, spec.lineSpacing);
    if (spec.advanceScaleX != 1.0f) font->SetAdvanceScaling(spec.advanceScaleX, 1.0f);
    return font;
}

// One [Font Styles] entry ("<name> = <specification>"): the described font becomes the control manager's system font of
// its style id, the name its font mapping, and the name a word of the font-style vocabulary.
void cSC4App::SetFontStyle(const cRZString& name, const cRZString& specification) {
    if (!RZ::FO()) return;
    std::uint32_t styleId = 0;
    cIGZFont* font = CreateFontStyleFromSpecification(specification, styleId);
    if (!font) return;
    if (gz::cIGZWinCtrlMgr* controls = RZ::WCM()) {
        gz::IWinFont* style = gz::MakeWinFont(font);
        controls->SetSystemFont(static_cast<int>(styleId), style);
        if (style) style->Release();
        controls->SetFontMapping(static_cast<int>(styleId), new cRZString(name));
        if (gFontStyleScriptVocabulary) gFontStyleScriptVocabulary->AddTranslation(name, styleId);
    }
    font->Release();
}

// The application's font set: every font of the plugin directory and of "<data directory>/Fonts/" is added to the font
// system. The original then filters the installed fonts down to a per-language name list and loads two system fonts
// (SC4GetSystemFontsDirectory @ 0x100039a97 answers false on this build, so that path is dead here).
// PARTIAL: the per-language name tables (five switch cases with ~20 inlined name pushes each, plus the name stripping
// done with RemoveFontByName) are not decoded yet.
void cSC4App::SetupFontSet() {
    cIGZUnknown* system = RZ::FO();
    if (!system) return;
    auto* fonts = static_cast<cIGZFontSys*>(system);
    cRZString directory;
    GetPluginDirectory(directory);
    fonts->AddAllFontsFromDirectory(directory);
    directory = dataDirectory_.ToChar();
    directory.AppendChars("Fonts", 5);
    directory.AppendChars("/", 1);
    fonts->AddAllFontsFromDirectory(directory);
}
