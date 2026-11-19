#include "gz/WinKeyAcceleratorRes.h"

#include <cctype>
#include <cstdlib>
#include <vector>

namespace gz {
namespace {

std::string Trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> SplitAtSpaces(const std::string& s) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (at <= s.size()) {
        std::size_t end = s.find(' ', at);
        if (end == std::string::npos) end = s.size();
        std::string token = Trim(s.substr(at, end - at));
        if (!token.empty()) out.push_back(token);
        at = end + 1;
    }
    return out;
}

bool Contains(const std::string& s, const char* word) { return s.find(word) != std::string::npos; }
bool StartsWith(const std::string& s, const char* word) { return s.rfind(word, 0) == 0; }

}  // namespace

std::uint32_t cRZWinKeyAcceleratorRes::KeyFromName(const std::string& t) {
    if (t.empty()) return 0;
    if (t.size() == 1 && std::isalnum(static_cast<unsigned char>(t[0])))
        return static_cast<std::uint32_t>(std::toupper(static_cast<unsigned char>(t[0])));
    if (StartsWith(t, "0x")) return static_cast<std::uint32_t>(std::strtoul(t.c_str() + 2, nullptr, 16));
    bool digits = true;
    for (char c : t) digits = digits && std::isdigit(static_cast<unsigned char>(c));
    if (digits) return static_cast<std::uint32_t>(std::strtoul(t.c_str(), nullptr, 10));

    if (Contains(t, "numlock")) return 0x90;
    if (Contains(t, "capslock")) return 0x14;
    if (Contains(t, "scrolllock")) return 0x91;
    if (Contains(t, "back")) return 0x08;
    if (Contains(t, "tab")) return 0x09;
    if (Contains(t, "return") || Contains(t, "enter")) return 0x0d;
    if (Contains(t, "pause")) return 0x13;
    if (Contains(t, "break") || Contains(t, "cancel")) return 0x03;
    if (Contains(t, "esc")) return 0x1b;
    if (Contains(t, "space")) return 0x20;
    if (Contains(t, "pageup")) return 0x21;
    if (Contains(t, "pagedown")) return 0x22;
    if (Contains(t, "end")) return 0x23;
    if (Contains(t, "home")) return 0x24;
    if (Contains(t, "left")) return 0x25;
    if (Contains(t, "up")) return 0x26;
    if (Contains(t, "right")) return 0x27;
    if (Contains(t, "down")) return 0x28;
    if (Contains(t, "printscreen")) return 0x2c;
    if (Contains(t, "insert")) return 0x2d;
    if (Contains(t, "delete")) return 0x2e;
    if (Contains(t, "help")) return 0x2f;

    switch (t[0]) {
    case '`': case '~': return 0xc0;
    case '-': case '_': return 0xbd;
    case '=': case '+': return 0xbb;
    case '[': case '{': return 0xdb;
    case ']': case '}': return 0xdd;
    case '\\': case '|': return 0xdc;
    case ';': case ':': return 0xba;
    case '\'': case '"': return 0xde;
    case ',': case '<': return 0xbc;
    case '.': case '>': return 0xbe;
    case '/': case '?': return 0xbf;
    default: break;
    }
    if (StartsWith(t, "semicolon")) return 0xba;
    if (t[0] == 'f' && t.size() > 1 && std::isdigit(static_cast<unsigned char>(t[1])))
        return 0x6f + static_cast<std::uint32_t>(std::strtoul(t.c_str() + 1, nullptr, 10));
    const std::size_t pad = t.find("numpad");
    if (pad != std::string::npos && pad + 6 < t.size()) {
        const char c = t[pad + 6];
        if (std::isdigit(static_cast<unsigned char>(c))) return 0x60 + static_cast<std::uint32_t>(c - '0');
        switch (c) {
        case '*': return 0x6a;
        case '+': return 0x6b;
        case '-': return 0x6d;
        case '.': return 0x6e;
        case '/': return 0x6f;
        default: break;
        }
    }
    return 0;
}

bool cRZWinKeyAcceleratorRes::ParseLine(std::string line) {
    line = Trim(line);
    if (line.empty() || line[0] == ';' || StartsWith(line, "//")) return false;
    const std::size_t hex = line.find("0x");
    const std::size_t equals = line.find('=');
    if (hex == std::string::npos || equals == std::string::npos) return false;

    // A second '=' before the first "0x" makes the first one a key name (`control = = 0x...`).
    std::string keys, message;
    const std::size_t second = line.find('=', equals + 1);
    if (second != std::string::npos && second < hex) {
        keys = line.substr(0, second - 1);
        message = line.substr(second + 1);
    } else {
        keys = line.substr(0, equals);
        message = line.substr(equals + 1);
    }

    std::uint32_t vk = 0;
    bool up = false, shift = false, control = false, alt = false;
    for (const std::string& raw : SplitAtSpaces(keys)) {
        const std::string token = Lower(Trim(raw));
        if (vk != 0 && token == "up") {
            up = true;
            continue;
        }
        if (vk != 0 && token == "down") up = false;   // ...and goes on to become the Down key (quirk)
        if (token == "control") control = true;
        else if (token == "shift") shift = true;
        else if (token == "alt") alt = true;
        else vk = KeyFromName(token);
    }
    if (vk == 0) {   // a lone modifier is the key
        if (control) vk = 0x11, control = false;
        else if (shift) vk = 0x10, shift = false;
        else if (alt) vk = 0x12, alt = false;
        else return false;
    }

    const std::size_t quote = message.find('"');
    if (quote != std::string::npos) message.erase(quote);
    std::uint32_t values[4] = {0, 0, 0, 0};
    int count = 0;
    for (const std::string& token : SplitAtSpaces(message)) {
        if (count == 4) break;
        if (!StartsWith(token, "0x") && !StartsWith(token, "0X")) continue;
        values[count++] = static_cast<std::uint32_t>(std::strtoul(token.c_str() + 2, nullptr, 16));
    }

    const std::uint32_t code = vk | (up ? 0x10000000u : 0u) | (shift ? 0x10000u : 0u) | (control ? 0x20000u : 0u) |
                               (alt ? 0x40000u : 0u);
    if (bindings_.count(code)) return false;
    cGZMessage bound;
    bound.type = values[0];
    bound.param1 = values[1];
    bound.param2 = values[2];
    bound.param3 = values[3];
    bindings_.emplace(code, bound);
    return true;
}

bool cRZWinKeyAcceleratorRes::SerialRead(const char* text, std::size_t length) {
    std::string line;
    for (std::size_t i = 0; i <= length; ++i) {
        const char c = i < length ? text[i] : '\n';
        if (c == '\r' || c == '\n') {
            if (!line.empty()) ParseLine(line);
            line.clear();
        } else {
            line.push_back(c);
        }
    }
    return true;
}

void cRZWinKeyAcceleratorRes::ApplyTo(cIGZWinKeyAccelerator* accelerator) const {
    if (!accelerator) return;
    for (const auto& [code, message] : bindings_) accelerator->RegisterAccelerator(code, message);
}

}  // namespace gz
