#include "sc4/AppPlatform.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <utility>

#include "sc4/ServiceLocator.h"

namespace {
std::function<bool(std::string&)> g_documentsFolder = [](std::string& out) {
    const char* home = std::getenv("HOME");
    if (!home || !*home) return false;
    out = std::string(home) + "/Documents";
    return true;
};
}  // namespace

void SetDocumentsFolderProvider(std::function<bool(std::string&)> provider) { g_documentsFolder = std::move(provider); }

void SC4SetupFPU() {}
void SC4DisableOSMessageBoxAlerts() {}

bool SC4GetInstalledLanguageAndCountryIDs(unsigned int& language, unsigned int& country) {
    language = 0;
    country = 0;
    return false;
}

std::string SC4FolderNameOnDisk(const std::string& base, const std::string& name) {
    std::error_code error;
    if (base.empty() || std::filesystem::is_directory(base + name, error)) return name;
    const auto lower = [](std::string text) {
        for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    };
    const std::string want = lower(name);
    for (const auto& entry : std::filesystem::directory_iterator(base, error)) {
        if (entry.is_directory(error) && lower(entry.path().filename().string()) == want) return entry.path().filename().string();
    }
    return name;
}

int SC4SetCPUUsageCount(int) { return 0; }

void SC4GetUserDataDirectory(cIGZString& out, bool createIfMissing) {
    SL::DataDirectory(out);
    std::string documents;
    if (g_documentsFolder && g_documentsFolder(documents)) {
        out.AssignFromChars(documents.c_str(), static_cast<std::uint32_t>(documents.size()));
        out.AppendChars("/", 1);
    }
    if (out.Strlen() == 0) return;
    out.AppendChars("SimCity 4", 9);
    out.AppendChars("/", 1);
    if (!createIfMissing) return;
    std::error_code error;
    if (std::filesystem::is_directory(out.ToChar(), error)) return;
    if (!std::filesystem::create_directory(out.ToChar(), error)) SL::DataDirectory(out);
}
