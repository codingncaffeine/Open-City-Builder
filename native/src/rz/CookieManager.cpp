// cRZCookieManager — see CookieManager.h for the layout, the vtable order and the quirks kept on purpose.
//
// Every function below is a hand-written translation of the decompilation at the address in its header comment.
// The string helpers the original calls are trivial and are inlined here under their original names:
//   RZStringToCharString(cRZString const&, std::string&) @ 0x1000163f2 — std::string::operator=(other.str())
//   CharStringToRZString(std::string const&, cRZString&) @ 0x100016380 — out.str() = in
#include "rz/CookieManager.h"

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>

#include "gz/MiscFunctions.h"   // stricmp @ 0x1006421d8
#include "platform/Unwired.h"
#include "rz/URL.h"
#include "rz/cRZFile.h"
#include "rz/cRZIniFile.h"
#include "sc4/MiscTypes.h"      // sc4::HTTPMessage::GetSeparatedStringList @ 0x100634d40

namespace {

// Globals of the original (read from the binary).
constexpr const char* kDefaultCookieDataIniFileName = "CookieData.ini";          // 0x100b12de0
constexpr const char* kDefaultCookieDataIniFileSection = "CookieData";           // 0x100b12de8
constexpr const char* kCookieEntryKeyFormat = "CookieData%05u";                  // 0x1009f9ce5
constexpr const char* kBinaryFlagSection = "Admin";
constexpr const char* kBinaryFlagKey = "Binary";
constexpr const char* kBinaryFlagValue = "1";
constexpr const char* kDomainTextSuffix = ";  domain=";                          // 0x1009f9cd2
constexpr const char* kPathTextSuffix = "; path=";                               // 0x1009f9cdd
constexpr std::uint32_t kSessionCookieExpiry = 2;                                // "+0x94 == 2" never expires
constexpr std::uint32_t kDiscardCookieExpiry = 1;                                // ParseCookieText's Discard
constexpr std::int32_t kDefaultExpiryDelta = 0x15180;                            // one day
constexpr std::uint32_t kDefaultPort = 0x50;                                     // 80
constexpr std::size_t kMaxCookieTexts = 0x28;                                    // split limit of a cookie text
constexpr std::size_t kMaxPorts = 0x10;                                          // CookieData::numbers port list
constexpr std::size_t kMaxIniEntries = 10000;                                    // ReadCookieDataFile's entry guard
constexpr std::uint32_t kFoundCookieIndex = 0xffffffff;                          // GetEquivalentCookieDataIndex

// RZStringToCharString @ 0x1000163f2.
std::string ToCharString(const cRZString& text) { return text.str(); }
// CharStringToRZString @ 0x100016380.
void ToRZString(const std::string& text, cRZString& out) { out.str() = text; }

ICookieHTTPHost* g_cookieHTTPHost = nullptr;

// The URL check of AddCookie(CookieData const&, cRZString const*) @ 0x10062b406..0x10062ba6b, in address order:
// the cookie's path must be a prefix of the URL path (0x10062b4df/0x10062b5e0), then its domain must be a suffix of
// the URL server that starts no later than the server's first '.' (0x10062b628..0x10062ba0d), then the URL port must
// be in the cookie's port list (an empty list accepts any port, 0x10062ba29..0x10062ba56). A non-empty domain shorter
// than three characters, ending in '.' or without a second '.' after a leading '.' never matches.
bool CookieBelongsToURL(const CookieData& cookie, cRZURL& url) {
    const std::string urlPath = ToCharString(url.GetAbsolutePath());
    const std::string& cookiePath = cookie.field78;
    if (!cookiePath.empty() &&
        !(cookiePath.size() <= urlPath.size() && urlPath.compare(0, cookiePath.size(), cookiePath) == 0)) {
        return false;
    }
    const std::string server = ToCharString(url.GetServer());
    const std::string& domain = cookie.field60;
    if (domain.size() < 3) return false;
    if (domain.back() == '.') return false;
    if (domain.find('.', 0) >= domain.size()) return false;
    if (domain[0] == '.' && domain.find('.', 1) >= domain.size()) return false;
    if (domain.empty()) return false;
    const std::size_t at = server.find(domain);
    if (!(at < server.size() && at == server.size() - domain.size())) return false;
    if (server.find('.', 0) < at) return false;
    std::uint32_t port = url.GetPort();
    if (port == 0) port = kDefaultPort;
    const CookieFields& fields = FieldsOf(cookie);
    if (fields.ports[0] == 0) return true;
    for (std::size_t i = 0; i < kMaxPorts; ++i) {
        if (fields.ports[i] == 0) return false;
        if (static_cast<std::uint32_t>(fields.ports[i]) == port) return true;
    }
    return false;
}

}  // namespace

ICookieHTTPHost* CookieHTTPHost() { return g_cookieHTTPHost; }
void SetCookieHTTPHost(ICookieHTTPHost* host) { g_cookieHTTPHost = host; }

// 0x10062ae90 cRZCookieManager::cRZCookieManager()
cRZCookieManager::cRZCookieManager() {
    // The original zeroes +0x08/+0x0c/+0x10, stores 0x4000 in +0x14, constructs the lock at +0x18, clears the
    // vector's three pointers and builds "CookieData.ini" inside the cRZString at +0x40 (0x10062ae90..0x10062afc5).
    mCookieDataFilePath = cRZString(kDefaultCookieDataIniFileName);
}
// 0x10062afc6 (D2) / 0x10062b044 (D1) / 0x10062b04e (D0) cRZCookieManager::~cRZCookieManager()
cRZCookieManager::~cRZCookieManager() {
    // +0x10062afc6: Shutdown(), then the path string and the vector members are destroyed by their own destructors.
    Shutdown();
}

// 0x10062b07c cRZCookieManager::Init()
bool cRZCookieManager::Init() {
    if (!mInitialized) {
        if (mCookies.empty()) ReadCookieDataFile();   // vtable slot 0x98
        mInitialized = true;
    }
    return true;
}

// 0x10062b0ae cRZCookieManager::Shutdown()
bool cRZCookieManager::Shutdown() {
    if (mInitialized) {
        WriteCookieDataFile();   // vtable slot 0xa0
        mCookies.clear();
        mInitialized = false;
    }
    return true;
}

// 0x10062b10e cRZCookieManager::AddRef()
std::uint32_t cRZCookieManager::AddRef() {
    mLock.Lock();
    const std::uint32_t count = ++mRefCount;
    mLock.Unlock();
    return count;
}

// 0x10062b14c cRZCookieManager::Release()
std::uint32_t cRZCookieManager::Release() {
    mLock.Lock();
    if (mRefCount < 2) {   // also true for a never-addref'd object
        mLock.Unlock();
        delete this;
        return 0;
    }
    const std::uint32_t count = --mRefCount;
    mLock.Unlock();
    return count;
}

// 0x10062b19e cRZCookieManager::SetShouldEncryptCookieDataFile(bool)
void cRZCookieManager::SetShouldEncryptCookieDataFile(bool encrypt) { mShouldEncrypt = encrypt; }

// 0x10062b1a8 cRZCookieManager::GetCookieDataFilePath(cRZString&)
bool cRZCookieManager::GetCookieDataFilePath(cRZString& path) {
    mLock.Lock();
    path = mCookieDataFilePath;
    mLock.Unlock();
    return true;
}

// 0x10062b206 cRZCookieManager::SetCookieDataFilePath(cRZString const&)
bool cRZCookieManager::SetCookieDataFilePath(const cRZString& path) {
    mLock.Lock();
    if (mCookieDataFilePath.str() != path.str()) {
        mCookieDataFilePath = path;
        ReadCookieDataFile();   // vtable slot 0x98
    }
    mLock.Unlock();
    return true;
}

// 0x10062b300 cRZCookieManager::AddCookie(std::string const&, cRZString const*)
// No lock of its own: ParseCookieText and AddCookie(CookieData) take the lock (0x10062b300..0x10062b3b5).
bool cRZCookieManager::AddCookie(const std::string& text, const cRZString* url) {
    if (text.empty()) return false;
    CookieData parsed;   // zeroed record; only the text is copied (0x10062b32b..0x10062b35b)
    parsed.field0 = text;
    if (!ParseCookieText(parsed, url)) return false;          // vtable slot 0x78
    const bool added = AddCookie(parsed, url);                // vtable slot 0x50
    return added;
}

// 0x10062b3d6 cRZCookieManager::AddCookie(CookieData const&, cRZString const*)
bool cRZCookieManager::AddCookie(const CookieData& cookie, const cRZString* url) {
    mLock.Lock();
    if (cookie.attributes.empty()) {
        if (cookie.field0.empty()) {
            mLock.Unlock();
            return false;
        }
        CookieData parsed;                // zeroed record, only the text is copied (0x10062b55a..0x10062b58d)
        parsed.field0 = cookie.field0;
        ParseCookieText(parsed, url);     // vtable slot 0x78; the result is not tested here (0x10062b59f)
        const bool added = AddCookie(parsed, url);   // vtable slot 0x50
        mLock.Unlock();
        return added;
    }
    if (url != nullptr) {
        cRZURL parsedURL(*url);
        if (!CookieBelongsToURL(cookie, parsedURL)) {
            mLock.Unlock();
            return false;
        }
    }
    // Replace an equivalent cookie: the original shifts the tail down by hand and pops the last record.
    const std::uint32_t equivalent = GetEquivalentCookieDataIndex(cookie);
    if (equivalent < mCookies.size()) mCookies.erase(mCookies.begin() + equivalent);
    const std::uint32_t now = static_cast<std::uint32_t>(std::time(nullptr));
    const CookieFields& fields = FieldsOf(cookie);
    if (!(now < fields.expiry || fields.expiry == kSessionCookieExpiry)) {
        mLock.Unlock();
        return false;
    }
    if (mCookies.size() >= mMaxCookies) RemoveOldestCookies(4, nullptr);   // 0x10062b991
    mCookies.push_back(cookie);
    mLock.Unlock();
    return true;
}

// 0x10062bb00 cRZCookieManager::GetEquivalentCookieDataIndex(CookieData const&)
std::uint32_t cRZCookieManager::GetEquivalentCookieDataIndex(const CookieData& cookie) {
    const std::uint32_t count = static_cast<std::uint32_t>(mCookies.size());
    for (std::uint32_t i = 0; i < count; ++i) {
        const CookieData& candidate = mCookies[i];
        if (candidate.attributes.empty() || cookie.attributes.empty()) continue;
        if (candidate.field0 == cookie.field0 && candidate.field60 == cookie.field60 &&
            candidate.field78 == cookie.field78) {
            return i;
        }
    }
    return kFoundCookieIndex;
}

// 0x10062bd46 cRZCookieManager::RemoveOldestCookies(unsigned int, std::string const*)
//
// PARTIAL: the shipped binary never removes anything here. It sweeps the expired cookies, then scans for the smallest
// expiry (optionally only among cookies whose domain contains `domainFilter`, 0x10062be15..0x10062bf22) and then
// repeats that scan forever (0x10062bf2e..0x10062bf42: the total count it compares against is never decremented), so
// the whole removal body is missing from the build. The scan has no side effect, so the only observable result is the
// expired-cookie sweep; this port returns exactly that instead of spinning.
std::uint32_t cRZCookieManager::RemoveOldestCookies(std::uint32_t count, const std::string* domainFilter) {
    (void)count;
    (void)domainFilter;
    return static_cast<std::uint32_t>(RemoveExpiredCookies());
}

// 0x10062bf5a cRZCookieManager::RemoveAllCookies()
bool cRZCookieManager::RemoveAllCookies() {
    mLock.Lock();
    mCookies.clear();
    mLock.Unlock();
    return true;
}

// 0x10062bfb8 cRZCookieManager::GetCookieTextForURL(cRZString const&, std::string&)
bool cRZCookieManager::GetCookieTextForURL(const cRZString& url, std::string& text) {
    cRZURL parsedURL(url);
    return GetCookieTextForURL(parsedURL, text);   // vtable slot 0x68
}

// 0x10062c02a cRZCookieManager::GetCookieTextForURL(cRZURL&, std::string&)
bool cRZCookieManager::GetCookieTextForURL(cRZURL& url, std::string& text) {
    mLock.Lock();
    text.erase(0, std::string::npos);
    bool haveVersion = false;   // 0x10062c06c, cleared once for the whole call
    for (std::uint32_t cookieIndex = 0; cookieIndex < 1000; ++cookieIndex) {
        CookieData* cookie = nullptr;
        if (!GetCookieDataForURL(url, &cookie, cookieIndex)) break;
        for (std::size_t i = 0; i < cookie->attributes.size(); ++i) {
            const std::string& name = cookie->attributes[i].first;
            const std::string& value = cookie->attributes[i].second;
            if (stricmp(name.c_str(), "comment") == 0) continue;
            if (stricmp(name.c_str(), "commenturl") == 0) continue;
            if (stricmp(name.c_str(), "discard") == 0) continue;
            if (stricmp(name.c_str(), "expires") == 0) continue;
            if (stricmp(name.c_str(), "max-age") == 0) continue;
            if (stricmp(name.c_str(), "secure") == 0) continue;
            if (stricmp(name.c_str(), "version") == 0 && haveVersion) continue;
            if (cookieIndex != 0 || i != 0) text += "; ";
            if (stricmp(name.c_str(), "domain") == 0 || stricmp(name.c_str(), "path") == 0 ||
                stricmp(name.c_str(), "port") == 0) {
                text += '$';   // RFC 2109 attribute: "$domain=", "$path=", "$port="
            } else if (stricmp(name.c_str(), "version") == 0) {
                if (!text.empty()) {
                    // Build "$version=<v>; ", put it in front and shrink the text by two characters, which removes
                    // the "; " separator that was just appended for this attribute (0x10062c21f..0x10062c3e7).
                    std::string prefixed = "$";
                    prefixed += name;
                    prefixed += '=';
                    prefixed += value;
                    prefixed += "; ";
                    text.insert(0, prefixed);
                    text.resize(text.size() - 2);
                    continue;
                }
                haveVersion = true;
                text += '$';
            }
            text += name;
            text += '=';
            text += value;
        }
    }
    const bool nonEmpty = !text.empty();
    mLock.Unlock();
    return nonEmpty;
}

// 0x10062c476 cRZCookieManager::GetCookieDataForURL(cRZURL&, CookieData**, unsigned int)
bool cRZCookieManager::GetCookieDataForURL(cRZURL& url, CookieData** out, std::uint32_t index) {
    const std::uint32_t now = static_cast<std::uint32_t>(std::time(nullptr));
    const std::size_t count = mCookies.size();
    std::uint32_t matched = 0;   // 0x10062c476's local_90
    for (std::size_t i = 0; i < count; ++i) {
        CookieData& cookie = mCookies[i];
        const CookieFields& fields = FieldsOf(cookie);
        if (!(now < fields.expiry || fields.expiry == kSessionCookieExpiry)) continue;
        if (cookie.flags != 0 && url.GetURLType() != cRZURL::kTypeHTTPS) continue;   // 0x10062c4a4
        const std::string server = ToCharString(url.GetServer());
        const std::string& domain = cookie.field60;
        if (domain.empty()) continue;
        const std::size_t at = server.find(domain);
        if (!(at < server.size() && at == server.size() - domain.size())) continue;
        const std::string path = ToCharString(url.GetAbsolutePath());
        const std::string& cookiePath = cookie.field78;
        if (cookiePath.empty()) continue;
        if (!(cookiePath.size() <= path.size() && path.compare(0, cookiePath.size(), cookiePath) == 0)) continue;
        if (fields.ports[0] != 0) {
            std::uint32_t port = url.GetPort();
            if (port == 0) port = kDefaultPort;
            bool found = false;
            for (std::size_t k = 0; k < kMaxPorts; ++k) {
                if (fields.ports[k] == 0) break;
                if (static_cast<std::uint32_t>(fields.ports[k]) == port) {
                    found = true;
                    break;
                }
            }
            if (!found) continue;
        }
        if (matched == index) {
            *out = &cookie;
            return true;
        }
        ++matched;
    }
    return false;
}

// 0x10062c93a cRZCookieManager::EnumCookies(CookieData*, unsigned int&)
bool cRZCookieManager::EnumCookies(CookieData* out, std::uint32_t& count) {
    mLock.Lock();
    if (count > mCookies.size()) count = static_cast<std::uint32_t>(mCookies.size());
    for (std::uint32_t i = 0; i < count; ++i) out[i] = mCookies[i];
    mLock.Unlock();
    return true;
}

// 0x10062ca7c cRZCookieManager::ParseCookieText(CookieData&, cRZString const*)
//
// PARTIAL: everything except the "Expires" date conversion, which is cRZHTTPClient::TimeStringToSeconds
// (0x100635cb6, see ICookieHTTPHost). Without a host the conversion fails and the original's fallback
// (now + one day) is used, which is also the original's behaviour for an unparseable date.
bool cRZCookieManager::ParseCookieText(CookieData& cookie, const cRZString* url) {
    mLock.Lock();
    cRZURL parsedURL;
    const std::int32_t now = static_cast<std::int32_t>(std::time(nullptr));
    if (url != nullptr) parsedURL.SetFullURL(*url, true);
    // 0x10062cb1f..0x10062cbd5: drop the old attributes, comment/commentURL/domain/path, then reset the numbers to
    // version 0, expiry 2 (session cookie), an empty port list and no Secure flag. The original only stores
    // ports[0] = 0; the rest of the list stays stale but is unreachable behind the terminator.
    cookie.attributes.clear();
    cookie.field30.clear();
    cookie.field48.clear();
    cookie.field60.clear();
    cookie.field78.clear();
    FieldsOf(cookie) = CookieFields{};                       // version 0, expiry 0, empty port list
    FieldsOf(cookie).expiry = kSessionCookieExpiry;           // 0x10062cbbd stores the pair {0, 2}
    cookie.flags = 0;
    // 0x10062cbfc: split the raw text at ';' into at most 0x28 trimmed, quote-aware parts, then split each part at
    // its first '=' into the attribute name/value pair.
    std::string list[kMaxCookieTexts];
    const unsigned int parts = sc4::HTTPMessage::GetSeparatedStringList(cookie.field0, list, kMaxCookieTexts, true, ';', true);
    for (unsigned int i = 0; i < parts; ++i) {
        const std::string& entry = list[i];
        const std::size_t equals = entry.find('=');
        std::string name;
        std::string value;
        if (equals != std::string::npos) {
            name = entry.substr(0, equals);
            value = entry.substr(equals + 1);
        } else {
            name = entry;
        }
        cookie.attributes.emplace_back(std::move(name), std::move(value));
    }
    // 0x10062ce33..0x10062d24a: dispatch on the attribute name (case-insensitive, first match wins).
    for (std::size_t i = 0; i < cookie.attributes.size(); ++i) {
        const std::string& name = cookie.attributes[i].first;
        const std::string& value = cookie.attributes[i].second;
        CookieFields& fields = FieldsOf(cookie);
        if (stricmp(name.c_str(), "Comment") == 0) {
            cookie.field30 = value;
        } else if (stricmp(name.c_str(), "CommentURL") == 0) {
            cookie.field48 = value;
        } else if (stricmp(name.c_str(), "Domain") == 0) {
            cookie.field60 = value;
            if (!cookie.field60.empty() && cookie.field60[0] != '.') cookie.field60.insert(0, 1, '.');
        } else if (stricmp(name.c_str(), "Path") == 0) {
            cookie.field78 = value;
        } else if (stricmp(name.c_str(), "Secure") == 0) {
            cookie.flags = 1;
        } else if (stricmp(name.c_str(), "Version") == 0) {
            fields.version = std::atoi(value.c_str());
        } else if (stricmp(name.c_str(), "Discard") == 0) {
            fields.expiry = kDiscardCookieExpiry;
        } else if (stricmp(name.c_str(), "Expires") == 0) {
            cRZString timeText;
            ToRZString(value, timeText);
            std::int32_t seconds = 0;
            if (ICookieHTTPHost* host = CookieHTTPHost()) seconds = host->TimeStringToSeconds(timeText);
            else SC4_UNWIRED("cRZCookieManager::ParseCookieText: the Expires date (cRZHTTPClient::TimeStringToSeconds) is not converted");
            if (seconds == 0) seconds = now + kDefaultExpiryDelta;
            fields.expiry = static_cast<std::uint32_t>(seconds);
        } else if (stricmp(name.c_str(), "Max-Age") == 0) {
            fields.expiry = static_cast<std::uint32_t>(std::atoi(value.c_str()) + now);
        } else if (stricmp(name.c_str(), "Port") == 0) {
            std::string portList[kMaxPorts];
            const unsigned int ports = sc4::HTTPMessage::GetSeparatedStringList(value, portList, kMaxPorts, false, ',', true);
            if (ports == 0) {
                std::uint32_t port = kDefaultPort;
                if (url != nullptr) {
                    const std::uint32_t urlPort = parsedURL.GetPort();
                    if (urlPort != 0) port = urlPort;
                }
                fields.ports[0] = static_cast<std::int32_t>(port);
                fields.ports[1] = 0;
            } else {
                // Quirk: an n-value list keeps its first n-1 values and then stores the 0 terminator in the slot the
                // last value would have used, so "8080,80" becomes {8080, 0} and "8080" becomes {0} (0x10062cfc7).
                for (unsigned int k = 0; k + 1 < ports; ++k) fields.ports[k] = std::atoi(portList[k].c_str());
                fields.ports[ports - 1] = 0;
            }
        }
    }
    // 0x10062d24a..0x10062d377: no Domain attribute -> derive it from the URL server by stripping everything up to
    // the first '.' (so "www.maxis.com" yields ".maxis.com"), then record it in the raw text.
    if (cookie.field60.empty()) {
        if (url != nullptr) {
            cookie.field60 = ToCharString(parsedURL.GetServer());
            if (!cookie.field60.empty() && cookie.field60[0] != '.') {
                while (!cookie.field60.empty() && cookie.field60[0] != '.') cookie.field60.erase(0, 1);
            }
        }
        if (!cookie.field60.empty()) {
            cookie.field0 += kDomainTextSuffix;
            cookie.field0 += cookie.field60;
        }
    }
    // 0x10062d379..0x10062d418: no Path attribute -> the URL's absolute path, or "/" without a URL; recorded too.
    if (cookie.field78.empty()) {
        if (url == nullptr) cookie.field78 = "/";
        else cookie.field78 = ToCharString(parsedURL.GetAbsolutePath());
        if (!cookie.field78.empty()) {
            cookie.field0 += kPathTextSuffix;
            cookie.field0 += cookie.field78;
        }
    }
    mLock.Unlock();
    return true;
}

// 0x10062d592 cRZCookieManager::GetCookieDataForURL(cRZString const&, CookieData&, unsigned int)
bool cRZCookieManager::GetCookieDataForURL(const cRZString& url, CookieData& out, std::uint32_t index) {
    cRZURL parsedURL(url);
    return GetCookieDataForURL(parsedURL, out, index);   // vtable slot 0x88
}

// 0x10062d610 cRZCookieManager::GetCookieDataForURL(cRZURL&, CookieData&, unsigned int)
bool cRZCookieManager::GetCookieDataForURL(cRZURL& url, CookieData& out, std::uint32_t index) {
    mLock.Lock();
    CookieData* found = nullptr;
    if (!GetCookieDataForURL(url, &found, index)) {
        mLock.Unlock();
        return false;
    }
    out = *found;
    mLock.Unlock();
    return true;
}

// 0x10062d70a cRZCookieManager::RemoveExpiredCookies()
std::int32_t cRZCookieManager::RemoveExpiredCookies() {
    const std::uint32_t now = static_cast<std::uint32_t>(std::time(nullptr));
    std::int32_t removed = 0;
    std::size_t i = 0;
    while (i < mCookies.size()) {
        const std::uint32_t expiry = FieldsOf(mCookies[i]).expiry;
        if (expiry < now && expiry != kSessionCookieExpiry) {
            mCookies.erase(mCookies.begin() + static_cast<std::ptrdiff_t>(i));
            ++removed;
        } else {
            ++i;
        }
    }
    return removed;
}

// 0x10062d896 cRZCookieManager::EncryptString(std::string const&, std::string&)
bool cRZCookieManager::EncryptString(const std::string& text, std::string& encrypted) {
    encrypted = text;   // the "encryption" of this build is a copy
    return true;
}

// 0x10062d8f0 cRZCookieManager::DecryptString(std::string const&, std::string&)
bool cRZCookieManager::DecryptString(const std::string& encrypted, std::string& text) {
    text = encrypted;
    return true;
}

// 0x10062d94a cRZCookieManager::ClearCookieDataFile()
bool cRZCookieManager::ClearCookieDataFile() {
    mLock.Lock();
    bool removed = true;   // no file is success (0x10062d94a)
    if (cRZFile::FileExists(mCookieDataFilePath)) removed = cRZFile::Remove(mCookieDataFilePath);
    mLock.Unlock();
    return removed;
}

// 0x10062d9b6 cRZCookieManager::ReadCookieDataFile()
bool cRZCookieManager::ReadCookieDataFile() {
    mLock.Lock();
    mCookies.clear();
    if (cRZFile::FileExists(mCookieDataFilePath)) {
        cRZIniFile ini(mCookieDataFilePath);
        cRZString section(kDefaultCookieDataIniFileSection);
        cRZString key;
        cRZString value;
        // Binary (encrypted) files carry an [Admin] Binary entry (0x10062db15..0x10062db75).
        const bool binary = ini.ReadEntry(cRZString(kBinaryFlagSection), cRZString(kBinaryFlagKey), value);
        std::string buffer(0x4000, '\0');
        std::string text;
        for (std::uint32_t i = 0; i < kMaxIniEntries; ++i) {
            key.Sprintf(kCookieEntryKeyFormat, i);
            if (!binary) {
                if (!ini.ReadEntry(section, key, value)) break;
                text = value.str();
            } else {
                const std::uint32_t length = ini.ReadBinaryEntry(section, key,
                                                                 reinterpret_cast<std::uint8_t*>(&buffer[0]),
                                                                 static_cast<std::uint32_t>(buffer.size()));
                if (length == 0) break;
                text.assign(buffer.data(), length);
                DecryptString(text, text);   // identity; the original passes the same string in and out
            }
            AddCookie(text, nullptr);   // vtable slot 0x48
        }
    }
    mLock.Unlock();
    return true;
}

// 0x10062ddac cRZCookieManager::WriteCookieDataFile()
bool cRZCookieManager::WriteCookieDataFile() {
    mLock.Lock();
    ClearCookieDataFile();   // vtable slot 0x90: the old file goes away before anything is written
    if (mCookies.empty()) {
        mLock.Unlock();
        return true;
    }
    cRZIniFile ini(mCookieDataFilePath);
    cRZString section(kDefaultCookieDataIniFileSection);
    cRZString key;
    const std::uint32_t now = static_cast<std::uint32_t>(std::time(nullptr));
    if (mShouldEncrypt) {
        ini.WriteEntry(cRZString(kBinaryFlagSection), cRZString(kBinaryFlagKey), cRZString(kBinaryFlagValue));
    }
    std::uint32_t nameIndex = 0;
    for (std::size_t i = 0; i < mCookies.size(); ++i) {
        // Session cookies (expiry 2) and Discard cookies (expiry 1) are never persisted (0x10062e00c).
        if (now < FieldsOf(mCookies[i]).expiry) {
            key.Sprintf(kCookieEntryKeyFormat, nameIndex++);
            if (!mShouldEncrypt) {
                cRZString text;
                ToRZString(mCookies[i].field0, text);
                ini.WriteEntry(section, key, text);
            } else {
                std::string encrypted;
                EncryptString(mCookies[i].field0, encrypted);
                ini.WriteBinaryEntry(section, key, reinterpret_cast<const std::uint8_t*>(encrypted.data()),
                                     static_cast<std::uint32_t>(encrypted.size()));
            }
        }
    }
    mLock.Unlock();
    return true;
}
