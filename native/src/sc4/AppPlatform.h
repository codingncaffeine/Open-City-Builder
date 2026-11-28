// Small application-level platform functions (0x100038d80..0x10003a555): the user data directory and the no-op hooks
// of the Windows build that the Mac port keeps as empty functions.
#pragma once
#include <functional>
#include <string>

#include "gz/cIGZString.h"

// @ 0x100038d80: FPU control word setup; nothing on this platform
void SC4SetupFPU();
// @ 0x100039486: suppress OS message boxes; nothing on this platform
void SC4DisableOSMessageBoxAlerts();
// @ 0x10003a547: no installer language information; both ids are zeroed and the answer is false
bool SC4GetInstalledLanguageAndCountryIDs(unsigned int& language, unsigned int& country);
// Not in the original (its file system ignores case): the spelling on disk of folder `name` under `base` ("base/" with a
// trailing slash), matched without regard to case; `name` itself when nothing matches.
std::string SC4FolderNameOnDisk(const std::string& base, const std::string& name);
// @ 0x10003a555: the CPU usage count is ignored
int SC4SetCPUUsageCount(int count);

// @ 0x10003972c: "<Documents>/SimCity 4/" (created when `createIfMissing`); the data directory (SL::DataDirectory) when
// the Documents folder cannot be found or created.
void SC4GetUserDataDirectory(cIGZString& out, bool createIfMissing);

// ASLCocoa_FindFolderPath('docs', ...): the folder the user data goes under (default: $HOME/Documents).
void SetDocumentsFolderProvider(std::function<bool(std::string&)> provider);
