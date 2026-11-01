// Platform glue of the native Metal application (no recovered counterpart on macOS); derived from the early host.
// The game's own UI path: the recovered UI script service reads the window scripts of the game data (the same scripts
// the game uses) and builds the windows through GZCOM and the recovered control classes. This file supplies what those
// recovered classes reach through seams: class makers for GZCOM, the vocabulary / control / interpreter hosts and the
// image loader.
#pragma once
#include <functional>
#include <cstdint>
#include <vector>

#include "gz/Win.h"
#include "gz/cGZPersistResourceKey.h"

class cGZBuffer;
namespace native {
class WinMgr;

// PNG bytes -> a CPU buffer with one reference (the host's ImageIO code supplies it).
using PngDecoder = cGZBuffer* (*)(const std::vector<std::uint8_t>&);
void SetPngDecoder(PngDecoder decoder);

// A bitmap resource {0x856ddbac, group, instance} of the game data as a CPU buffer (ARGB8888), one counted reference; null when no
// package has it. The same decoder serves files (DecodeFunction).
// The bytes of one resource from the game's .dat packages (false when none has it).
bool LoadResourceBytes(std::uint32_t type, std::uint32_t group, std::uint32_t instance, std::vector<std::uint8_t>& bytes);
// A string resource (type 0x2026960b, LTEXT) as UTF-8; false when there is none.
bool LoadLocalizedString(std::uint32_t group, std::uint32_t instance, cIGZString& out);
cGZBuffer* LoadBitmapResource(std::uint32_t group, std::uint32_t instance);
PngDecoder DecodeFunction();

// Registers the makers, seams and the script service. Needs RunBoot(.., keepAlive = true) to have run (GZCOM, the
// resource manager). Returns false when a step fails (the reason is printed).
bool InitUiScripts(WinMgr* mgr);
// SC4CreateWindowFromScript: the window of script `key` (control id `id`) inside `parent`, one reference, or null.
cIGZWin* CreateWindowFromScript(const cGZPersistResourceKey& key, cIGZWin* parent, std::uint32_t id);
// The modal "Game Options" dialog (cSC4GamePreferencesWinProc); `preferences` is the blob its check boxes mirror.
void RunGameOptionsDialog(std::function<std::uint8_t*()> preferences);
// The modal "Audio Options" dialog (cSC4AudioPreferencesWinProc) over the same blob.
void RunAudioOptionsDialog(std::function<std::uint8_t*()> preferences);
// The modal "Graphic Options" dialog (cSC4VideoPreferencesWinProc) over the same blob.
void RunGraphicOptionsDialog(std::function<std::uint8_t*()> preferences);
// cSC4View3DWin::DoEstablishCityDialog: the city and mayor names and the difficulty, then the city is established. True on OK.
bool RunEstablishCityDialog(cIGZWin* view);
// The game's logical height for the next start (<documents>/display.conf; 0 = none chosen).
int LoadDisplayHeight();
// The window's size and the game's logical height (the main sets them); the resolution choices offered for them.
void SetDisplayGeometry(int windowWidth, int windowHeight, int gameHeight);
bool ResolutionChoice(int index, int& width, int& height);
int CurrentGameHeight();
void SaveDisplayHeight(int height);
}  // namespace native
