#include "html/HTMLDocument.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "rz/TextEncoding.h"
#include "rz/cRZFile.h"
#include "rz/RZConvert.h"

namespace html {

namespace {

struct TagEntry { const char* name; int token; };
struct EntityEntry { const char* name; unsigned code; };

// InitTagList: tag name -> token id (end tags carry a leading '/')
const TagEntry kTagEntries[] = {
    {"!DOCTYPE", kTokenDoctype},
    {"!DOCTYPE", kTagDoctype},
    {"HTML", kTagHtml},
    {"/HTML", kTagEndHtml},
    {"HEAD", kTagHead},
    {"/HEAD", kTagEndHead},
    {"BODY", kTagBody},
    {"/BODY", kTagEndBody},
    {"TITLE", kTagTitle},
    {"/TITLE", kTagEndTitle},
    {"BASE", kTagBase},
    {"STYLE", kTagStyle},
    {"/STYLE", kTagEndStyle},
    {"META", kTagMeta},
    {"LINK", kTagLink},
    {"ISINDEX", kTagIsindex},
    {"TT", kTagTt},
    {"/TT", kTagEndTt},
    {"I", kTagI},
    {"/I", kTagEndI},
    {"B", kTagB},
    {"/B", kTagEndB},
    {"STRIKE", kTagStrike},
    {"/STRIKE", kTagEndStrike},
    {"BIG", kTagBig},
    {"/BIG", kTagEndBig},
    {"SMALL", kTagSmall},
    {"/SMALL", kTagEndSmall},
    {"U", kTagU},
    {"/U", kTagEndU},
    {"FONT", kTagFont},
    {"/FONT", kTagEndFont},
    {"EM", kTagEm},
    {"/EM", kTagEndEm},
    {"STRONG", kTagStrong},
    {"/STRONG", kTagEndStrong},
    {"DFN", kTagDfn},
    {"/DFN", kTagEndDfn},
    {"CODE", kTagCode},
    {"/CODE", kTagEndCode},
    {"SAMP", kTagSamp},
    {"/SAMP", kTagEndSamp},
    {"KBD", kTagKbd},
    {"/KBD", kTagEndKbd},
    {"VAR", kTagVar},
    {"/VAR", kTagEndVar},
    {"CITE", kTagCite},
    {"/CITE", kTagEndCite},
    {"ABBR", kTagAbbr},
    {"/ABBR", kTagEndAbbr},
    {"ACRONYM", kTagAcronym},
    {"/ACRONYM", kTagEndAcronym},
    {"A", kTagA},
    {"/A", kTagEndA},
    {"IMG", kTagImg},
    {"OBJECT", kTagObject},
    {"/OBJECT", kTagEndObject},
    {"BR", kTagBr},
    {"SCRIPT", kTagScript},
    {"/SCRIPT", kTagEndScript},
    {"MAP", kTagMap},
    {"/MAP", kTagEndMap},
    {"Q", kTagQ},
    {"/Q", kTagEndQ},
    {"SUB", kTagSub},
    {"/SUB", kTagEndSub},
    {"SUP", kTagSup},
    {"/SUP", kTagEndSup},
    {"SPAN", kTagSpan},
    {"/SPAN", kTagEndSpan},
    {"BDO", kTagBdo},
    {"/BDO", kTagEndBdo},
    {"BLINK", kTagBlink},
    {"/BLINK", kTagEndBlink},
    {"INPUT", kTagInput},
    {"SELECT", kTagSelect},
    {"/SELECT", kTagEndSelect},
    {"OPTION", kTagOption},
    {"/OPTION", kTagEndOption},
    {"OPTGROUP", kTagOptgroup},
    {"/OPTGROUP", kTagEndOptgroup},
    {"TEXTAREA", kTagTextarea},
    {"/TEXTAREA", kTagEndTextarea},
    {"LABEL", kTagLabel},
    {"/LABEL", kTagEndLabel},
    {"BUTTON", kTagButton},
    {"/BUTTON", kTagEndButton},
    {"H1", kTagH1},
    {"/H1", kTagEndH1},
    {"H2", kTagH2},
    {"/H2", kTagEndH2},
    {"H3", kTagH3},
    {"/H3", kTagEndH3},
    {"H4", kTagH4},
    {"/H4", kTagEndH4},
    {"H5", kTagH5},
    {"/H5", kTagEndH5},
    {"H6", kTagH6},
    {"/H6", kTagEndH6},
    {"P", kTagP},
    {"/P", kTagEndP},
    {"PRE", kTagPre},
    {"/PRE", kTagEndPre},
    {"UL", kTagUl},
    {"/UL", kTagEndUl},
    {"OL", kTagOl},
    {"/OL", kTagEndOl},
    {"DL", kTagDl},
    {"/DL", kTagEndDl},
    {"DIV", kTagDiv},
    {"/DIV", kTagEndDiv},
    {"NOSCRIPT", kTagNoscript},
    {"/NOSCRIPT", kTagEndNoscript},
    {"BLOCKQUOTE", kTagBlockquote},
    {"/BLOCKQUOTE", kTagEndBlockquote},
    {"FORM", kTagForm},
    {"/FORM", kTagEndForm},
    {"HR", kTagHr},
    {"TABLE", kTagTable},
    {"/TABLE", kTagEndTable},
    {"FIELDSET", kTagFieldset},
    {"/FIELDSET", kTagEndFieldset},
    {"ADDRESS", kTagAddress},
    {"/ADDRESS", kTagEndAddress},
    {"CENTER", kTagCenter},
    {"/CENTER", kTagEndCenter},
    {"DIR", kTagDir},
    {"/DIR", kTagEndDir},
    {"MENU", kTagMenu},
    {"/MENU", kTagEndMenu},
    {"CAPTION", kTagCaption},
    {"/CAPTION", kTagEndCaption},
    {"COL", kTagCol},
    {"COLGROUP", kTagColgroup},
    {"/COLGROUP", kTagEndColgroup},
    {"THEAD", kTagThead},
    {"/THEAD", kTagEndThead},
    {"TBODY", kTagTbody},
    {"/TBODY", kTagEndTbody},
    {"TFOOT", kTagTfoot},
    {"/TFOOT", kTagEndTfoot},
    {"TD", kTagTd},
    {"/TD", kTagEndTd},
    {"TH", kTagTh},
    {"/TH", kTagEndTh},
    {"TR", kTagTr},
    {"/TR", kTagEndTr},
    {"LI", kTagLi},
    {"/LI", kTagEndLi},
    {"DT", kTagDt},
    {"/DT", kTagEndDt},
    {"DD", kTagDd},
    {"/DD", kTagEndDd},
    {"AREA", kTagArea},
    {"FRAMESET", kTagFrameset},
    {"/FRAMESET", kTagEndFrameset},
    {"FRAME", kTagFrame},
    {"NOFRAMES", kTagNoframes},
    {"/NOFRAMES", kTagEndNoframes},
    {"NOBR", kTagNobr},
    {"/NOBR", kTagEndNobr},
};

// GetStringForEscape: the Latin-1 character references (the decompile of the original stops after "icirc"; the
// remaining ISO 8859-1 names follow the standard list)
const EntityEntry kEntityTable[] = {
    {"quot", 34},
    {"amp", 38},
    {"lt", 60},
    {"gt", 62},
    {"nbsp", 160},
    {"iexcl", 161},
    {"cent", 162},
    {"pound", 163},
    {"curren", 164},
    {"yen", 165},
    {"brvbar", 166},
    {"sect", 167},
    {"uml", 168},
    {"copy", 169},
    {"ordf", 170},
    {"laquo", 171},
    {"not", 172},
    {"shy", 173},
    {"reg", 174},
    {"macr", 175},
    {"deg", 176},
    {"plusmn", 177},
    {"sup2", 178},
    {"sup3", 179},
    {"acute", 180},
    {"micro", 181},
    {"para", 182},
    {"middot", 183},
    {"cedil", 184},
    {"sup1", 185},
    {"ordm", 186},
    {"raquo", 187},
    {"frac14", 188},
    {"frac12", 189},
    {"frac34", 190},
    {"iquest", 191},
    {"Agrave", 192},
    {"Aacute", 193},
    {"Acirc", 194},
    {"Atilde", 195},
    {"Auml", 196},
    {"Aring", 197},
    {"AElig", 198},
    {"Ccedil", 199},
    {"Egrave", 200},
    {"Eacute", 201},
    {"Ecirc", 202},
    {"Euml", 203},
    {"Igrave", 204},
    {"Iacute", 205},
    {"Icirc", 206},
    {"Iuml", 207},
    {"ETH", 208},
    {"Ntilde", 209},
    {"Ograve", 210},
    {"Oacute", 211},
    {"Ocirc", 212},
    {"Otilde", 213},
    {"Ouml", 214},
    {"times", 215},
    {"Oslash", 216},
    {"Ugrave", 217},
    {"Uacute", 218},
    {"Ucirc", 219},
    {"Uuml", 220},
    {"Yacute", 221},
    {"THORN", 222},
    {"szlig", 223},
    {"agrave", 224},
    {"aacute", 225},
    {"acirc", 226},
    {"atilde", 227},
    {"auml", 228},
    {"aring", 229},
    {"aelig", 230},
    {"ccedil", 231},
    {"egrave", 232},
    {"eacute", 233},
    {"ecirc", 234},
    {"euml", 235},
    {"igrave", 236},
    {"iacute", 237},
    {"icirc", 238},
    {"iuml", 239},
    {"eth", 240},
    {"ntilde", 241},
    {"ograve", 242},
    {"oacute", 243},
    {"ocirc", 244},
    {"otilde", 245},
    {"ouml", 246},
    {"divide", 247},
    {"oslash", 248},
    {"ugrave", 249},
    {"uacute", 250},
    {"ucirc", 251},
    {"uuml", 252},
    {"yacute", 253},
    {"thorn", 254},
    {"yuml", 255},
};

bool IsHtmlSpace(const std::string& text, std::size_t index) {
    if (index >= text.size()) return false;
    const unsigned char c = static_cast<unsigned char>(text[index]);
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

std::size_t HtmlAdvance(const std::string& text, std::size_t index) {
    if (index >= text.size()) return index + 1;
    return index + 1 + static_cast<std::size_t>(UTF8GetFollowByteCount(static_cast<unsigned char>(text[index])));
}

std::string Upper(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}
std::string Lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}
std::string Trimmed(std::string s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(0, 1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    return s;
}

constexpr std::uint32_t kDefaultFontSizes[7] = {8, 10, 12, 14, 18, 24, 36};
// H1..H6: font kind, pixel size and style flags (kHeaderFontType / kHeaderFontSize / kHeaderFontFlags)
constexpr std::uint32_t kHeaderFontType[6] = {9, 9, 9, 9, 9, 9};
constexpr std::uint32_t kHeaderFontSize[6] = {40, 32, 28, 24, 20, 16};
constexpr std::uint32_t kHeaderFontFlags[6] = {1, 1, 1, 1, 1, 1};

// font style bits kept in tFontAttributes::style
constexpr std::uint32_t kStyleBold = 1, kStyleItalic = 2, kStyleUnderline = 4, kStyleStrike = 8;

}  // namespace

// ---- grammar tables --------------------------------------------------------------------------------------------------

int LookupTag(const std::string& upperName) {
    static const std::unordered_map<std::string, int> table = [] {
        std::unordered_map<std::string, int> m;
        for (const TagEntry& e : kTagEntries) m.emplace(e.name, e.token);
        return m;
    }();
    const auto it = table.find(upperName);
    return it == table.end() ? 0 : it->second;
}

const tFirstSets& FirstSets() {
    static const tFirstSets sets = [] {
        tFirstSets s;
        s.fontStyle = {kTagTt, kTagI, kTagB, kTagStrike, kTagBig, kTagSmall, kTagFont, kTagU};
        s.phrase = {kTagEm, kTagStrong, kTagDfn, kTagCode, kTagSamp, kTagKbd, kTagVar, kTagCite, kTagAbbr, kTagAcronym};
        s.special = {kTagA, kTagImg, kTagObject, kTagBr, kTagScript, kTagMap, kTagQ, kTagSub, kTagSup, kTagSpan, kTagBdo, kTagBlink};
        s.formCtrl = {kTagInput, kTagSelect, kTagTextarea, kTagLabel, kTagButton};
        s.heading = {kTagH1, kTagH2, kTagH3, kTagH4, kTagH5, kTagH6};
        s.list = {kTagUl, kTagOl};
        s.block = {kTagP, kTagPre, kTagDl, kTagDiv, kTagNoscript, kTagBlockquote, kTagForm, kTagHr, kTagTable, kTagFieldset,
                   kTagAddress, kTagCenter, kTagDir, kTagMenu};
        // INLINE is every inline start tag plus character data
        s.inlineSet = {kTokenText};
        for (const auto* part : {&s.fontStyle, &s.phrase, &s.special, &s.formCtrl}) s.inlineSet.insert(part->begin(), part->end());
        s.block.insert(s.heading.begin(), s.heading.end());
        s.block.insert(s.list.begin(), s.list.end());
        s.flow = s.inlineSet;
        s.flow.insert(s.block.begin(), s.block.end());
        return s;
    }();
    return sets;
}

// ---- lifetime --------------------------------------------------------------------------------------------------------

cHTMLDocument::cHTMLDocument(const HtmlServices& services) : services_(services) {
    if (services_.graphics) {
        defaultColor_ = services_.graphics->MakeColor(0, 0, 0);
        visitedLinkColor_ = services_.graphics->MakeColor(100, 0x20, 0xa4);
        unvisitedLinkColor_ = services_.graphics->MakeColor(0, 0, 0xff);
        activeLinkColor_ = services_.graphics->MakeColor(0xc0, 0, 0xff);
    }
    SetFontSizeTable(kDefaultFontSizes, 7);
}

cHTMLDocument::~cHTMLDocument() {
    if (initialized_) Shutdown();
}

std::uint32_t cHTMLDocument::Release() {
    if (refCount_ <= 1) {
        delete this;
        return 0;
    }
    return --refCount_;
}

bool cHTMLDocument::Init(IHtmlWindow* window, IHtmlWebSystem* web, const cRZString& baseUrl) {
    if (initialized_) return true;
    initialized_ = true;
    window_ = window;
    web_ = web;
    SetBaseURL(baseUrl);
    font_.kind = 1;
    font_.size = fontSizes_[3];
    font_.color = defaultColor_;
    font_.style = 0;
    font_.preformatted = false;
    font_.isLink = false;
    return true;
}

bool cHTMLDocument::Shutdown() {
    if (!initialized_) return true;
    initialized_ = false;
    delete body_;
    body_ = nullptr;
    baseUrl_.reset();
    for (cHTMLMETA* meta : metas_) delete meta;
    metas_.clear();
    for (cHTMLLink* link : links_) delete link;
    links_.clear();
    for (auto& item : images_) {
        if (item.second.bitmap == nullptr) {
            if (web_) web_->CancelJob(item.second.jobId);
        } else {
            item.second.bitmap->Release();
        }
    }
    images_.clear();
    web_ = nullptr;
    window_ = nullptr;
    return true;
}

// ---- small accessors --------------------------------------------------------------------------------------------------

int cHTMLDocument::GetFontSizeTable(std::uint32_t* out, int count) const {
    const int copied = std::min(count, 7);
    for (int i = 0; i < copied; ++i) out[i] = fontSizes_[i];
    for (int i = copied; i < count; ++i) out[i] = 0;
    return copied;
}

void cHTMLDocument::SetFontSizeTable(const std::uint32_t* sizes, int count) {
    const int copied = std::min(count, 7);
    for (int i = 0; i < copied; ++i) fontSizes_[i] = sizes[i];
    variableStyle_ = fontSizes_[3];
    fixedStyle_ = fontSizes_[3];
}

void cHTMLDocument::GetDefaultVariableWidthFontInfo(std::string& faceName, std::uint32_t& style) {
    faceName = variableFace_;
    style = variableStyle_;
}

void cHTMLDocument::GetDefaultFixedWidthFontInfo(std::string& faceName, std::uint32_t& style) {
    faceName = fixedFace_;
    style = fixedStyle_;
}

bool cHTMLDocument::GetBaseURL(cRZString& out) const {
    if (!baseUrl_) {
        out.str().clear();
        return false;
    }
    out = *baseUrl_;
    return true;
}

bool cHTMLDocument::SetBaseURL(const cRZString& url) {
    if (!baseUrl_) baseUrl_ = std::make_unique<cRZString>(url);
    else *baseUrl_ = url;
    return true;
}

cHTMLAnchor* cHTMLDocument::FindAnchor(const cIGZString& name) {
    for (cHTMLAnchor* anchor : anchors_)
        if (anchor->name()->IsEqualToStringRef(name, true)) return anchor;
    return nullptr;
}

void cHTMLDocument::InvalidateVisual() {
    if (window_) window_->Invalidate();
}

// The reference table is filled lazily in the original; quot amp lt gt first, then 160..255.
std::string cHTMLDocument::GetStringForEscape(const cRZString& entityName) {
    static const std::unordered_map<std::string, unsigned> table = [] {
        std::unordered_map<std::string, unsigned> m;
        for (const EntityEntry& e : kEntityTable) m.emplace(e.name, e.code);
        return m;
    }();
    const auto it = table.find(entityName.str());
    if (it == table.end()) return "?";
    std::string out;
    const unsigned code = it->second;
    if (code < 0x80) {
        out.push_back(static_cast<char>(code));
    } else {                      // ISO 8859-1 -> UTF-8
        out.push_back(static_cast<char>(0xc0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    }
    return out;
}

// ---- tokenizer ----------------------------------------------------------------------------------------------------------

// Collects character data up to the next tag, skipping comments; true when anything was collected.
bool cHTMLDocument::ParseCDATA() {
    cdata_.str().clear();
    const std::string& text = documentText_.str();
    std::size_t p = position_;
    for (;;) {
        if (text.size() <= p) break;
        const std::size_t lt = text.find('<', p);
        if (lt >= text.size()) {
            cdata_.str().append(text, p, std::string::npos);
            position_ = static_cast<std::uint32_t>(text.size());
            break;
        }
        if (position_ == lt) {
            if (text.size() - lt < 4) break;
            if (text[lt + 1] != '!' || text[lt + 2] != '-' || text[lt + 3] != '-') break;
            position_ = static_cast<std::uint32_t>(lt + 4);
            if (position_ < text.size()) {
                const std::size_t end = text.find("-->", position_);
                position_ = static_cast<std::uint32_t>(end < text.size() ? end + 3 : text.size());
            }
            p = position_;
        } else if (position_ < lt) {
            cdata_.str().append(text, position_, lt - position_);
            position_ = static_cast<std::uint32_t>(lt);
            p = lt;
        }
    }
    return !cdata_.str().empty();
}

void cHTMLDocument::StripWhiteSpace(std::string& text) {
    std::size_t start = 0;
    while (start < text.size() && IsHtmlSpace(text, start)) start = HtmlAdvance(text, start);
    if (start != 0) text.erase(0, std::min(start, text.size()));
    while (!text.empty() && IsHtmlSpace(text, text.size() - 1)) text.pop_back();
}

// "TAG attr=..." -> upper-cased tag name and the attribute text
void cHTMLDocument::SplitTag(std::string& tag, std::string& rest) {
    rest.clear();
    for (std::size_t i = 0; i < tag.size(); i = HtmlAdvance(tag, i)) {
        if (IsHtmlSpace(tag, i)) {
            rest = tag.substr(HtmlAdvance(tag, i));
            tag.resize(i);
            break;
        }
    }
    StripWhiteSpace(tag);
    tag = Upper(tag);
    StripWhiteSpace(rest);
}

void cHTMLDocument::GetToken(bool skipText) {
    if (unget_) {
        unget_ = false;
        if (!skipText) return;
        if (token_ != kTokenText) return;
    }
    token_ = kTokenUnknown;
    const std::string& text = documentText_.str();
    for (;;) {
        if (!ParseCDATA()) {
            if (text.size() <= position_) {
                token_ = kTokenEof;
                return;
            }
        } else if (!skipText) {
            token_ = kTokenText;
            return;
        }
        if (text.size() - std::min<std::size_t>(position_, text.size()) > 3 && text[position_ + 1] == '!' &&
            text[position_ + 2] == '-' && text[position_ + 3] == '-') {
            position_ += 4;
            const std::size_t end = text.find("-->", position_);
            position_ = static_cast<std::uint32_t>(end < text.size() ? end + 3 : text.size());
            continue;
        }
        if (position_ < text.size() && text[position_] == '<') break;
    }
    ++position_;
    if (text.size() <= position_) return;
    std::size_t i = position_;
    while (text[i] != '>') {
        i = HtmlAdvance(text, i);
        if (text.size() <= i) return;
    }
    std::string tag = text.substr(position_, i - position_);
    position_ = static_cast<std::uint32_t>(i + 1);
    tagName_ = tag;
    SplitTag(tagName_, attrText_);
    const int id = LookupTag(tagName_);
    if (id != 0) token_ = id;
    ParseAttributeList();
}

bool cHTMLDocument::SkipUntilToken(int token) {
    while (position_ < documentText_.str().size()) {
        GetToken(false);
        if (token_ == token) return true;
    }
    return false;
}

void cHTMLDocument::SkipDocumentWhitespace() {
    while (IsHtmlSpace(documentText_.str(), position_))
        position_ = static_cast<std::uint32_t>(HtmlAdvance(documentText_.str(), position_));
}

// name[=value] pairs; values may be "double" or 'single' quoted or bare. Names are upper-cased, the first
// occurrence of a name wins. Quirks kept from the original: a quoted value is scanned from the character after
// the opening quote *inclusively* (an empty "" runs on to the next quote), and the first character of a name is
// taken whatever it is.
void cHTMLDocument::ParseAttributeList() {
    if (token_ == kTokenDoctype) return;
    attributes_.clear();
    StripWhiteSpace(attrText_);
    const std::string& s = attrText_;
    const std::size_t n = s.size();
    std::size_t i = 0;
    while (i < n) {
        if (IsHtmlSpace(s, i)) {
            i = HtmlAdvance(s, i);
            continue;
        }
        const std::size_t nameStart = i;
        std::size_t p = HtmlAdvance(s, i);
        while (p < n) {
            const char c = s[p];
            if (!(((c & 0xdf) >= 'A' && (c & 0xdf) <= 'Z') || c == '-' || c == '/')) break;
            p = HtmlAdvance(s, p);
        }
        const std::size_t nameEnd = p;
        while (p < n && IsHtmlSpace(s, p)) p = HtmlAdvance(s, p);
        std::string name = s.substr(nameStart, nameEnd - nameStart);
        std::string value;
        if (p < n && s[p] == '=') {
            const std::size_t start0 = p + 1;
            char quote = 0;
            if (start0 < n && (s[start0] == '"' || s[start0] == '\'')) quote = s[start0];
            std::size_t start, last, q;
            if (quote) {
                start = start0 + 1;
                q = start;
                do {
                    last = q;
                    q = HtmlAdvance(s, q);
                    if (n <= q) break;
                } while (s[q] != quote);
                value = last >= start ? s.substr(start, last - start + 1) : std::string();
                p = q + 1;
            } else {
                start = start0;
                q = start;
                do {
                    last = q;
                    q = HtmlAdvance(s, q);
                    if (n <= q) break;
                } while (!IsHtmlSpace(s, q));
                value = start < n ? s.substr(start, last - start + 1) : std::string();
                p = q;
            }
        }
        if (name.size() >= 2 && name.front() == '"' && name.back() == '"') name = name.substr(1, name.size() - 2);
        name = Upper(name);
        if (!name.empty()) attributes_.emplace(name, value);
        i = p;
    }
    attrText_.clear();
}

bool cHTMLDocument::FindAttribute(const char* upperName, cRZString& value, bool erase) {
    const auto it = attributes_.find(upperName);
    if (it == attributes_.end()) {
        value.str().clear();
        return false;
    }
    value.str() = it->second;
    if (erase) attributes_.erase(it);
    return true;
}

// ---- font and alignment state ---------------------------------------------------------------------------------------------

void cHTMLDocument::PushFont() { fontStack_.push_back(font_); }

void cHTMLDocument::PopFont() {
    if (fontStack_.empty()) return;
    font_ = fontStack_.back();
    fontStack_.pop_back();
}

int cHTMLDocument::MakeOutputStyle(cRZString& value) {
    value.MakeUpper();
    const std::string& s = value.str();
    if (s == "LEFT") return 0;
    if (s == "RIGHT") return 2;
    return s == "CENTER" ? 1 : 0;
}

int cHTMLDocument::MakeRelativeToTextAlignmentStyle(cRZString& value) {
    value.MakeUpper();
    const std::string& s = value.str();
    if (s == "LEFT") return 0;
    if (s == "RIGHT") return 1;
    if (s == "TOP") return 2;
    if (s == "BOTTOM") return 4;
    return s == "MIDDLE" ? 3 : 4;
}

void cHTMLDocument::InsertTextSegment(const cRZString& text, cHTMLContainerElement& parent) {
    parent.Insert(new cHTMLTextSegment(this, text, font_, outputStyle_));
}

// ---- document level -------------------------------------------------------------------------------------------------------

bool cHTMLDocument::ParseSGMLElements(const cRZString& text) {
    documentText_ = text;
    position_ = 0;
    ParseCDATA();
    GetToken(false);
    if (token_ == kTagHtml) {
        ParseHTMLElements();
        return true;
    }
    if (token_ == kTokenDoctype) return ParseSGMLDocumentType();
    cdata_.str().clear();
    position_ = 0;
    return ParseTextDocument();
}

// "<!DOCTYPE HTML PUBLIC ...>": only HTML documents are accepted. (The attribute map is not filled for a doctype
// token, so the version / language scan below never finds anything - kept as the original does it.)
bool cHTMLDocument::ParseSGMLDocumentType() {
    std::string head = Upper(attrText_.substr(0, 4));
    if (head != "HTML") return false;
    for (const auto& attribute : attributes_) {
        const std::string key = Upper(attribute.first);
        const std::size_t at = key.find("DTD HTML");
        if (at == std::string::npos) continue;
        std::size_t v = at + 1;
        while (v < key.size() && IsHtmlSpace(key, v)) ++v;
        std::size_t e = v;
        while (e < key.size() && ((key[e] >= '0' && key[e] <= '9') || key[e] == '.')) ++e;
        docTypeVersion_ = key.substr(v, e - v);
        if (key.size() >= 4 && key[key.size() - 4] == '/' && key[key.size() - 3] == '/') docTypeLanguage_ = key.substr(key.size() - 2, 2);
        break;
    }
    return ParseHTMLDocument();
}

bool cHTMLDocument::ParseHTMLDocument() {
    SkipDocumentWhitespace();
    ParseCDATA();
    GetToken(false);
    const int token = token_;
    if (token == kTagHtml) ParseHTMLElements();
    return token == kTagHtml;
}

bool cHTMLDocument::ParseHTMLElements() {
    body_ = new cHTMLBODY(unvisitedLinkColor_, visitedLinkColor_, activeLinkColor_);
    body_->graphics = services_.graphics;
    SkipDocumentWhitespace();
    if (token_ == kTagHtml) GetToken(false);
    ParseHEAD();
    SkipDocumentWhitespace();
    GetToken(false);
    ParseBODY();
    GetToken(true);
    if (token_ != kTagEndHtml) unget_ = true;
    return true;
}

// Anything that is not HTML becomes one preformatted text segment.
bool cHTMLDocument::ParseTextDocument() {
    delete body_;
    body_ = new cHTMLBODY(unvisitedLinkColor_, visitedLinkColor_, activeLinkColor_);
    body_->graphics = services_.graphics;
    PushFont();
    font_.preformatted = true;
    InsertTextSegment(documentText_, *body_);
    position_ = static_cast<std::uint32_t>(documentText_.str().size());
    PopFont();
    return true;
}

bool cHTMLDocument::ParseHEAD() {
    if (token_ == kTagHead) GetToken(true);
    for (;;) {
        switch (token_) {
            case kTagEndHead: return true;
            case kTagTitle: ParseTITLE(); break;
            case kTagBase: ParseBASE(); break;
            case kTagStyle: SkipUntilToken(kTagEndStyle); break;
            case kTagMeta: ParseMETA(); break;
            case kTagLink: ParseLINK(); break;
            case kTagIsindex: break;
            case kTagObject: SkipUntilToken(kTagEndObject); break;
            case kTagScript: SkipUntilToken(kTagEndScript); break;
            default:
                unget_ = true;       // the head is over: this token belongs to the body
                return true;
        }
        GetToken(true);
    }
}

bool cHTMLDocument::ParseTITLE() {
    GetToken(false);
    if (token_ == kTokenText) title_ = cdata_.str();
    GetToken(true);
    return token_ == kTagEndTitle;
}

bool cHTMLDocument::ParseBASE() {
    cRZString href;
    if (FindAttribute("HREF", href, true)) SetBaseURL(href);
    return true;
}

bool cHTMLDocument::ParseMETA() {
    auto* meta = new cHTMLMETA();
    cRZString value;
    if (FindAttribute("NAME", value, true)) meta->name = std::make_unique<cRZString>(value);
    if (FindAttribute("CONTENT", value, true)) meta->content = std::make_unique<cRZString>(value);
    if (FindAttribute("HTTP-EQUIV", value, true)) meta->httpEquiv = std::make_unique<cRZString>(value);
    if (!meta->content) {      // the original keeps a META only when it has content
        delete meta;
        return false;
    }
    metas_.push_back(meta);
    return true;
}

bool cHTMLDocument::ParseLINK() {
    cRZString href;
    if (!FindAttribute("HREF", href, true)) return true;
    auto* link = new cHTMLLink();
    link->href = std::make_unique<cRZString>(href);
    if (baseUrl_ && services_.resolveUrl)
        link->type = std::make_unique<cRZString>(services_.resolveUrl(href.str(), baseUrl_->str()).c_str());
    cRZString rev;
    link->flag = !FindAttribute("REV", rev, true);
    cRZString title;
    if (FindAttribute("TITLE", title, true)) link->rel = std::make_unique<cRZString>(title);
    links_.push_back(link);
    return true;
}

bool cHTMLDocument::ParseBODY() {
    cRZString value;
    std::string background;
    if (FindAttribute("BACKGROUND", value, true) && baseUrl_ && services_.resolveUrl)
        background = services_.resolveUrl(value.str(), baseUrl_->str());
    if (FindAttribute("BGCOLOR", value, true)) {
        Color color = 0xff000000;
        cHTMLShared::SetColor(value, color);
        body_->backgroundColor = color;
        body_->hasBackgroundColor = true;
    }
    if (FindAttribute("LINK", value, true)) body_->SetLINK(value);
    if (FindAttribute("ALINK", value, true)) body_->SetALINK(value);
    if (FindAttribute("VLINK", value, true)) body_->SetVLINK(value);
    if (FindAttribute("TEXT", value, true)) {
        Color color = 0xff000000;
        cHTMLShared::SetColor(value, color);
        font_.color = services_.graphics->MakeColor(static_cast<std::uint8_t>(color >> 16), static_cast<std::uint8_t>(color >> 8),
                                                    static_cast<std::uint8_t>(color));
        font_.isLink = false;
    }
    if (FindAttribute("BGPROPERTIES", value, true)) body_->fixedBackground = true;
    if (!background.empty()) RequestImage(body_, cRZString(background.c_str()));
    if (token_ == kTagBody) GetToken(false);
    bool ok = true;
    const tFirstSets& sets = FirstSets();
    do {
        if (InFirstSet(sets.block)) {
            ok = ParseBLOCK(*body_);
        } else if (token_ == kTagScript) {
            ok = SkipUntilToken(kTagEndScript);
        } else if (InFirstSet(sets.inlineSet)) {
            ok = ParseINLINE(*body_);
        } else {
            if (token_ == kTokenEof) {
                unget_ = true;
                return true;
            }
            ok = false;
        }
        GetToken(false);
        if (!ok) {
            if (token_ != kTagEndBody) unget_ = true;
            return false;
        }
    } while (true);
}

// ---- inline / block dispatch -------------------------------------------------------------------------------------------------

bool cHTMLDocument::ParseINLINE(cHTMLContainerElement& parent) {
    if (token_ == kTokenText) {
        InsertTextSegment(cdata_, parent);
        return true;
    }
    const tFirstSets& sets = FirstSets();
    if (InFirstSet(sets.fontStyle)) return ParseFONTSTYLE(parent);
    if (InFirstSet(sets.phrase)) return ParsePHRASE(parent);
    if (InFirstSet(sets.special)) return ParseSPECIAL(parent);
    if (InFirstSet(sets.formCtrl)) return ParseFORMCTRL(parent);
    return false;
}

bool cHTMLDocument::ParseBLOCK(cHTMLContainerElement& parent) {
    switch (token_) {
        case kTagP: ParseP(parent); break;
        case kTagPre: ParsePRE(parent); break;
        case kTagDl: return ParseDL(parent);
        case kTagDiv: ParseDIV(parent); break;
        case kTagNoscript: return SkipUntilToken(kTagEndNoscript);
        case kTagBlockquote: ParseBLOCKQUOTE(parent); break;
        case kTagForm: ParseFORM(parent); break;
        case kTagHr: ParseHR(parent); break;
        case kTagTable: return ParseTABLE(parent);
        case kTagFieldset: return SkipUntilToken(kTagEndFieldset);
        case kTagAddress: return SkipUntilToken(kTagEndAddress);
        case kTagCenter: ParseDIVActual(parent, kTagEndCenter, 1); break;
        case kTagDir: return SkipUntilToken(kTagEndDir);
        case kTagMenu: return SkipUntilToken(kTagEndMenu);
        default: {
            const tFirstSets& sets = FirstSets();
            if (InFirstSet(sets.heading)) return ParseHn(parent);
            if (InFirstSet(sets.list)) return ParseLIST(parent);
            return false;
        }
    }
    return true;
}

bool cHTMLDocument::ParseFLOW(cHTMLContainerElement& parent) {
    const tFirstSets& sets = FirstSets();
    if (InFirstSet(sets.inlineSet)) return ParseINLINE(parent);
    if (InFirstSet(sets.block)) return ParseBLOCK(parent);
    return false;
}

bool cHTMLDocument::ParseSPECIAL(cHTMLContainerElement& parent) {
    switch (token_) {
        case kTagA: return ParseA(parent);
        case kTagImg: return ParseIMG(parent);
        case kTagObject: return SkipUntilToken(kTagEndObject);
        case kTagBr: ParseBR(parent); return true;
        case kTagScript: return SkipUntilToken(kTagEndScript);
        case kTagMap: return ParseMAP(parent);
        case kTagQ: return SkipUntilToken(kTagEndQ);
        case kTagSub: return SkipUntilToken(kTagEndSub);
        case kTagSup: return SkipUntilToken(kTagEndSup);
        case kTagSpan: return ParseSPAN(parent);
        case kTagBdo: return SkipUntilToken(kTagEndBdo);
        case kTagBlink: return ParseBLINK(parent);
        default: return false;
    }
}

bool cHTMLDocument::ParseFORMCTRL(cHTMLContainerElement& parent) {
    switch (token_) {
        case kTagInput: ParseINPUT(parent); return true;
        case kTagSelect: return ParseSELECT(parent);
        case kTagTextarea: return SkipUntilToken(kTagEndTextarea);
        case kTagLabel: return SkipUntilToken(kTagEndLabel);
        case kTagButton: return SkipUntilToken(kTagEndButton);
        default: return false;
    }
}

bool cHTMLDocument::ParseINPUT(cHTMLContainerElement& parent) {
    parent.Insert(new cHTMLINPUT());
    return true;
}

// Font style tags (TT I B STRIKE BIG SMALL U FONT): set the style on a pushed font, parse the contents, pop.
bool cHTMLDocument::ParseFONTSTYLE(cHTMLContainerElement& parent) {
    PushFont();
    const int token = token_;
    switch (token) {
        case kTagTt:
            font_.faceNames.clear();
            font_.kind = 10;
            break;
        case kTagI: font_.style |= kStyleItalic; break;
        case kTagB: font_.style |= kStyleBold; break;
        case kTagStrike: font_.style |= kStyleStrike; break;
        case kTagBig: font_.size = fontSizes_[4]; break;
        case kTagSmall: font_.size = fontSizes_[2]; break;
        case kTagU: font_.style |= kStyleUnderline; break;
        case kTagFont: {
            cRZString value;
            if (FindAttribute("SIZE", value, true)) {
                int index;
                if (cHTMLShared::StringLeftEquals(value, '+')) {
                    index = std::min<int>(static_cast<int>(RZ::ConvertToUint(value.ToChar() + 1)) + 3, 6);
                } else if (cHTMLShared::StringLeftEquals(value, '-')) {
                    index = std::max<int>(3 - static_cast<int>(RZ::ConvertToUint(value.ToChar() + 1)), 0);
                } else {
                    index = std::min<int>(static_cast<int>(RZ::ConvertToUint(value.ToChar())), 6);
                }
                font_.size = fontSizes_[index];
            }
            if (FindAttribute("COLOR", value, true)) {
                Color color = 0xff000000;
                cHTMLShared::SetColor(value, color);
                font_.color = services_.graphics->MakeColor(static_cast<std::uint8_t>(color >> 16), static_cast<std::uint8_t>(color >> 8),
                                                            static_cast<std::uint8_t>(color));
                font_.isLink = false;
            }
            if (FindAttribute("FACE", value, true)) {
                font_.faceNames = value.str();
                font_.kind = 0;
            }
            break;
        }
        default: return false;       // (the original leaves the pushed font behind in this unreachable case)
    }
    const int endToken = token + 1;
    GetToken(false);
    const tFirstSets& sets = FirstSets();
    for (;;) {
        if (InFirstSet(sets.inlineSet)) ParseINLINE(parent);
        else if (InFirstSet(sets.block)) ParseBLOCK(parent);
        else break;
        GetToken(false);
    }
    bool ok = true;
    if (token_ != endToken) ok = SkipUntilToken(endToken);
    PopFont();
    return ok;
}

bool cHTMLDocument::ParsePHRASE(cHTMLContainerElement& parent) {
    PushFont();
    const int token = token_;
    bool ok = true;
    switch (token) {
        case kTagEm: case kTagDfn: case kTagCite: font_.style |= kStyleItalic; break;
        case kTagStrong: font_.style |= kStyleBold; break;
        case kTagCode: case kTagSamp: case kTagKbd: case kTagVar:
            font_.faceNames.clear();
            font_.kind = 10;
            break;
        case kTagAbbr: case kTagAcronym: break;
        default: ok = false; break;
    }
    GetToken(false);
    while (InFirstSet(FirstSets().inlineSet)) {
        ParseINLINE(parent);
        GetToken(false);
    }
    if (token_ != token + 1) SkipUntilToken(token + 1);
    PopFont();
    return ok;
}

// ---- blocks ---------------------------------------------------------------------------------------------------------------------

bool cHTMLDocument::ParseP(cHTMLContainerElement& parent) {
    auto* p = new cHTMLP();
    GetToken(false);
    while (InFirstSet(FirstSets().inlineSet)) {
        ParseINLINE(*p);
        GetToken(false);
    }
    if (token_ != kTagEndP) unget_ = true;
    parent.Insert(p);
    return true;
}

bool cHTMLDocument::ParsePRE(cHTMLContainerElement& parent) {
    PushFont();
    font_.kind = 2;
    font_.preformatted = true;
    GetToken(false);
    while (InFirstSet(FirstSets().inlineSet)) {
        ParseINLINE(parent);
        GetToken(false);
    }
    if (token_ != kTagEndPre) SkipUntilToken(kTagEndPre);
    PopFont();
    return true;
}

bool cHTMLDocument::ParseDL(cHTMLContainerElement& parent) {
    auto* dl = new cHTMLContainerElement(8);
    for (;;) {
        GetToken(true);
        if (token_ == kTagDd) { ParseDD(*dl); continue; }
        if (token_ == kTagDt) { ParseDT(*dl); continue; }
        if (!InFirstSet(FirstSets().block)) break;
        ParseBLOCK(*dl);
    }
    if (token_ == kTagEndDl || SkipUntilToken(kTagEndDl)) {
        if (!dl->children().empty()) {
            parent.Insert(dl);
            return true;
        }
        delete dl;
        return true;
    }
    delete dl;
    return false;
}

bool cHTMLDocument::ParseDD(cHTMLContainerElement& parent) {
    auto* dd = new cHTMLDD();
    GetToken(false);
    bool ok = true;
    for (;;) {
        if (!InFirstSet(FirstSets().inlineSet)) break;
        ok = ParseINLINE(*dd);
        GetToken(false);
        if (!ok) {
            ok = false;
            break;
        }
    }
    if (!ok) { /* the original reports the last parse result */ }
    if (token_ != kTagEndDd) unget_ = true;
    if (!ok && dd->children().empty()) { delete dd; return false; }
    parent.Insert(dd);
    return true;
}

bool cHTMLDocument::ParseDT(cHTMLContainerElement& parent) {
    auto* dt = new cHTMLDT();
    GetToken(false);
    bool ok = true;
    for (;;) {
        if (!InFirstSet(FirstSets().inlineSet)) break;
        ok = ParseINLINE(*dt);
        GetToken(false);
        if (!ok) break;
    }
    if (token_ != kTagEndDt) unget_ = true;
    if (!ok && dt->children().empty()) { delete dt; return false; }
    parent.Insert(dt);
    return true;
}

bool cHTMLDocument::ParseDIV(cHTMLContainerElement& parent) {
    int style = outputStyle_;
    cRZString value;
    if (FindAttribute("ALIGN", value, true)) style = MakeOutputStyle(value);
    ParseDIVActual(parent, kTagEndDiv, style);
    return true;
}

bool cHTMLDocument::ParseDIVActual(cHTMLContainerElement& parent, int endToken, int style) {
    auto* div = new cHTMLDIV();
    parent.Insert(div);
    outputStyleStack_.push_back(outputStyle_);
    outputStyle_ = style;
    GetToken(false);
    while (InFirstSet(FirstSets().flow)) {
        ParseFLOW(*div);
        GetToken(false);
    }
    if (token_ != endToken) SkipUntilToken(endToken);
    if (!outputStyleStack_.empty()) {
        outputStyle_ = outputStyleStack_.back();
        outputStyleStack_.pop_back();
    }
    return true;
}

bool cHTMLDocument::ParseBLOCKQUOTE(cHTMLContainerElement& parent) {
    auto* quote = new cHTMLBlockQuote();
    GetToken(false);
    const tFirstSets& sets = FirstSets();
    for (;;) {
        if (token_ == kTagScript) {
            SkipUntilToken(kTagEndScript);
        } else if (InFirstSet(sets.block)) {
            ParseBLOCK(*quote);
        } else if (InFirstSet(sets.inlineSet)) {
            ParseINLINE(*quote);
        } else {
            if (token_ != kTagEndBlockquote) SkipUntilToken(kTagEndBlockquote);
            if (quote->children().empty()) delete quote;
            else parent.Insert(quote);
            return true;
        }
        GetToken(false);
    }
}

bool cHTMLDocument::ParseFORM(cHTMLContainerElement& parent) {
    auto* form = new cHTMLFORM();
    const tFirstSets& sets = FirstSets();
    for (;;) {
        GetToken(false);
        if (token_ == kTagForm || token_ == kTagScript) {       // nested forms and scripts are skipped
            SkipUntilToken(token_ + 1);
            continue;
        }
        if (InFirstSet(sets.inlineSet)) { ParseINLINE(*form); continue; }
        if (InFirstSet(sets.block)) { ParseBLOCK(*form); continue; }
        break;
    }
    if (token_ != kTagEndForm) SkipUntilToken(kTagEndForm);
    if (form->children().empty()) delete form;
    else parent.Insert(form);
    return true;
}

bool cHTMLDocument::ParseHR(cHTMLContainerElement& parent) {
    auto* hr = new cHTMLHR();
    parent.Insert(hr);
    cRZString value;
    if (FindAttribute("ALIGN", value, true)) hr->align = MakeRelativeToTextAlignmentStyle(value);
    if (FindAttribute("SIZE", value, true)) hr->size = static_cast<std::int32_t>(RZ::ConvertToUint(value.ToChar()));
    if (FindAttribute("WIDTH", value, true)) {
        const std::size_t percent = value.str().find('%');
        if (percent < value.str().size()) {
            hr->widthIsPercent = true;
            value.str().erase(percent);
        }
        hr->width = static_cast<std::int32_t>(RZ::ConvertToUint(value.ToChar()));
    }
    return true;
}

bool cHTMLDocument::ParseBR(cHTMLContainerElement& parent) {
    parent.Insert(new cHTMLBR());
    return true;
}

bool cHTMLDocument::ParseHn(cHTMLContainerElement& parent) {
    const int token = token_;
    const unsigned offset = static_cast<unsigned>(token - kTagH1);
    if (offset >= 11 || ((0x555u >> offset) & 1) == 0) return false;     // not an <hN> start tag
    const unsigned level = offset / 2;
    auto* heading = new cHTMLHeading();
    PushFont();
    font_.kind = kHeaderFontType[level];
    font_.size = kHeaderFontSize[level];
    font_.style = kHeaderFontFlags[level];
    const int endToken = token + 1;
    const tFirstSets& sets = FirstSets();
    for (;;) {
        GetToken(false);
        if (InFirstSet(sets.inlineSet)) { ParseINLINE(*heading); continue; }
        if (InFirstSet(sets.block)) { ParseBLOCK(*heading); continue; }
        break;
    }
    if (token_ != endToken) SkipUntilToken(endToken);
    PopFont();
    if (heading->children().empty()) delete heading;
    else parent.Insert(heading);
    return true;
}

bool cHTMLDocument::ParseLIST(cHTMLContainerElement& parent) {
    if (token_ == kTagOl) return ParseOL(parent);
    if (token_ == kTagUl) return ParseUL(parent);
    return false;
}

bool cHTMLDocument::ParseUL(cHTMLContainerElement& parent) {
    auto* list = new cHTMLList(9, 0);
    ++ulDepth_;
    bool ok = true;
    const tFirstSets& sets = FirstSets();
    for (;;) {
        GetToken(false);
        if (token_ == kTagLi) { ParseLI(*list, -1); ok = true; continue; }
        if (InFirstSet(sets.block)) { ok = ParseBLOCK(*list); continue; }
        if (InFirstSet(sets.inlineSet)) { ok = ParseINLINE(*list); continue; }
        break;
    }
    if (token_ != kTagEndUl) SkipUntilToken(kTagEndUl);
    if (!ok || list->children().empty()) {
        delete list;
        --ulDepth_;
        return false;
    }
    parent.Insert(list);
    return true;
}

bool cHTMLDocument::ParseOL(cHTMLContainerElement& parent) {
    auto* list = new cHTMLList(10, 1);
    ++olDepth_;
    int number = 1;
    bool ok = true;
    const tFirstSets& sets = FirstSets();
    for (;;) {
        GetToken(false);
        if (token_ == kTagLi) { ParseLI(*list, number++); ok = true; continue; }
        if (InFirstSet(sets.block)) { ok = ParseBLOCK(*list); continue; }
        if (InFirstSet(sets.inlineSet)) { ok = ParseINLINE(*list); continue; }
        break;
    }
    if (token_ != kTagEndOl) SkipUntilToken(kTagEndOl);
    if (!ok || list->children().empty()) {
        delete list;
        --olDepth_;
        return false;
    }
    parent.Insert(list);
    return true;
}

// A list item: the bullet picture (number < 0) or "  n. " text, then the item's flow content on a hanging indent.
bool cHTMLDocument::ParseLI(cHTMLList& list, int number) {
    auto* item = new cHTMLListItem();
    list.Insert(item);
    if (number < 0) {
        if (cHTMLIMG* bullet = CreateBullet(kBulletDisc)) item->Insert(bullet);
    } else {
        char buffer[16];
        std::snprintf(buffer, sizeof buffer, "%3d. ", number);
        InsertTextSegment(cRZString(buffer), *item);
    }
    item->Insert(new cHTMLListItemHangingIndent());
    for (;;) {
        GetToken(false);
        if (!InFirstSet(FirstSets().flow)) break;
        ParseFLOW(*item);
    }
    if (token_ != kTagEndLi) unget_ = true;
    return true;
}

// ---- inline specials --------------------------------------------------------------------------------------------------------------

bool cHTMLDocument::ParseA(cHTMLContainerElement& parent) {
    auto* anchor = new cHTMLAnchor();
    cRZString value;
    bool hasHref = false;
    if (FindAttribute("HREF", value, true)) {
        anchor->SetHREF(value);
        hasHref = true;
        if (baseUrl_ && services_.resolveUrl)
            anchor->SetHREFAbsolute(cRZString(services_.resolveUrl(value.str(), baseUrl_->str()).c_str()));
    }
    if (FindAttribute("NAME", value, true)) {
        anchor->SetNAME(value);
        anchors_.push_back(anchor);
    }
    PushFont();
    font_.isLink = true;
    if (hasHref) {
        const bool visited = web_ && anchor->hrefAbsolute() && web_->IsVisited(anchor->hrefAbsolute()->str());
        font_.color = visited ? (body_ ? body_->visitedLinkColor : visitedLinkColor_)
                              : (body_ ? body_->linkColor : unvisitedLinkColor_);
        font_.style |= kStyleUnderline;
    }
    const tFirstSets& sets = FirstSets();
    for (;;) {
        GetToken(false);
        if (InFirstSet(sets.inlineSet)) { ParseINLINE(*anchor); continue; }
        if (InFirstSet(sets.block)) { ParseBLOCK(*anchor); continue; }
        break;
    }
    bool ok = true;
    if (token_ != kTagEndA) ok = SkipUntilToken(kTagEndA);
    PopFont();
    if (!ok) {
        delete anchor;
        return false;
    }
    parent.Insert(anchor);
    return true;
}

bool cHTMLDocument::ParseIMG(cHTMLContainerElement& parent) {
    cRZString value;
    if (!FindAttribute("SRC", value, true)) return false;
    auto* img = new cHTMLIMG();
    std::string url;
    if (baseUrl_ && services_.resolveUrl) url = services_.resolveUrl(value.str(), baseUrl_->str());
    if (FindAttribute("ALT", value, false)) img->source = std::make_unique<cRZString>(value);
    if (FindAttribute("WIDTH", value, true)) img->width = static_cast<std::int32_t>(RZ::ConvertToUint(value.ToChar()));
    if (FindAttribute("HEIGHT", value, true)) img->height = static_cast<std::int32_t>(RZ::ConvertToUint(value.ToChar()));
    if (FindAttribute("ALIGN", value, true)) img->align = MakeRelativeToTextAlignmentStyle(value);
    img->outputStyle = outputStyle_;
    parent.Insert(img);
    RequestImage(img, cRZString(url.c_str()));
    return true;
}

bool cHTMLDocument::ParseMAP(cHTMLContainerElement& parent) {
    auto* map = new cHTMLMAP();
    cRZString value;
    if (FindAttribute("NAME", value, true)) map->name = std::make_unique<cRZString>(value);
    GetToken(false);
    bool ok;
    if (token_ == kTagArea) {
        do {
            ParseAREA(*map);
            GetToken(false);
        } while (token_ == kTagArea);
        ok = true;
    } else if (InFirstSet(FirstSets().block)) {
        do {
            ok = ParseBLOCK(*map);
            GetToken(false);
        } while (InFirstSet(FirstSets().block));
    } else {
        ok = false;
    }
    if (token_ != kTagEndMap) SkipUntilToken(kTagEndMap);
    if (!ok) {
        delete map;
        return false;
    }
    parent.Insert(map);
    return true;
}

bool cHTMLDocument::ParseAREA(cHTMLContainerElement& parent) {
    auto* area = new cHTMLAREA();
    cRZString value;
    if (FindAttribute("SHAPE", value, true)) {
        value.MakeUpper();
        const std::string& s = value.str();
        if (s == "DEFAULT") area->shape = 1;
        else if (s == "RECT") area->shape = 2;
        else if (s == "CIRCLE") area->shape = 3;
        else if (s == "POLY") area->shape = 4;
    }
    parent.Insert(area);
    return true;
}

// <span style="color:...; text-decoration:...; font-family:...; font-size:..."> : the CSS subset the viewer knows.
bool cHTMLDocument::ParseSPAN(cHTMLContainerElement& parent) {
    auto* span = new cHTMLSPAN();
    PushFont();
    cRZString styleAttribute;
    if (FindAttribute("STYLE", styleAttribute, true)) {
        const std::string& css = styleAttribute.str();
        std::size_t colon = css.find(':');
        std::size_t start = 0;
        while (colon != std::string::npos) {
            const std::size_t semicolon = css.find(';', colon);
            const std::string name = Lower(Trimmed(css.substr(start, colon - start)));
            const std::string value = Lower(Trimmed(css.substr(colon + 1, semicolon == std::string::npos ? std::string::npos : semicolon - colon - 1)));
            if (name == "color") {
                unsigned long hex = 0;
                long r = 0, g = 0, b = 0;
                if (std::sscanf(value.c_str(), "#%lx", &hex) == 1) {
                    font_.color = static_cast<Color>(hex);
                } else if (std::sscanf(value.c_str(), "rgb(%ld,%ld,%ld)", &r, &g, &b) == 3) {
                    font_.color = services_.graphics->MakeColor(static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g), static_cast<std::uint8_t>(b));
                }
            } else if (name == "text-decoration") {
                if (value == "none") font_.style &= ~kStyleUnderline;
                else if (value == "underline") font_.style |= kStyleUnderline;
                else if (value == "line-through") font_.style |= kStyleStrike;
            } else if (name == "font-family") {
                font_.faceNames = value;
                font_.kind = 0;
            } else if (name == "font-size") {
                const std::size_t px = value.find("px");
                const std::size_t pt = value.find("pt");
                if ((px != std::string::npos && px != 0) || (pt != std::string::npos && pt != 0)) font_.size = static_cast<std::uint32_t>(std::atoi(value.c_str()));
                if (font_.size - 1 > 0xfe) font_.size = 0xe;
            }
            if (semicolon == std::string::npos || semicolon + 1 >= css.size()) break;
            start = semicolon + 1;
            colon = css.find(':', start);
        }
    }
    const tFirstSets& sets = FirstSets();
    for (;;) {
        GetToken(false);
        if (InFirstSet(sets.inlineSet)) { ParseINLINE(*span); continue; }
        if (InFirstSet(sets.block)) { ParseBLOCK(*span); continue; }
        break;
    }
    bool ok = true;
    if (token_ != kTagEndSpan) ok = SkipUntilToken(kTagEndSpan);
    PopFont();
    if (!ok) {
        delete span;
        return false;
    }
    parent.Insert(span);
    return true;
}

bool cHTMLDocument::ParseBLINK(cHTMLContainerElement& parent) {
    auto* blink = new cHTMLBLINK();
    const tFirstSets& sets = FirstSets();
    for (;;) {
        GetToken(false);
        if (InFirstSet(sets.inlineSet)) { ParseINLINE(*blink); continue; }
        if (InFirstSet(sets.block)) { ParseBLOCK(*blink); continue; }
        break;
    }
    blinkingPresent_ = true;
    if (token_ == kTagEndBlink || SkipUntilToken(kTagEndBlink)) {
        parent.Insert(blink);
        return true;
    }
    delete blink;
    return false;
}

bool cHTMLDocument::ParseSELECT(cHTMLContainerElement& parent) {
    auto* select = new cHTMLSELECT();
    for (;;) {
        GetToken(true);
        if (token_ == kTagOption) { ParseOPTION(*select); continue; }
        if (token_ == kTagOptgroup) { SkipUntilToken(kTagEndOptgroup); continue; }
        break;
    }
    if (token_ == kTagEndSelect || SkipUntilToken(kTagEndSelect)) {
        if (!select->children().empty()) {
            parent.Insert(select);
            return true;
        }
        delete select;
        return true;
    }
    delete select;
    return false;
}

bool cHTMLDocument::ParseOPTION(cHTMLContainerElement& parent) {
    auto* option = new cHTMLOPTION();
    GetToken(false);
    const bool hasText = token_ == kTokenText;
    if (!hasText) {
        delete option;
    } else {
        InsertTextSegment(cdata_, *option);
        parent.Insert(option);
    }
    if (token_ != kTagEndOption) unget_ = true;
    return hasText;
}

// ---- tables -----------------------------------------------------------------------------------------------------------------------------

bool cHTMLDocument::ParseTABLE(cHTMLContainerElement& parent) {
    auto* table = new cHTMLTABLE();
    cRZString value;
    if (FindAttribute("BORDER", value, true)) {
        std::uint32_t border = 0;
        cHTMLShared::SetPixels(value, border);
        table->border = static_cast<std::int32_t>(border);
    }
    if (FindAttribute("WIDTH", value, true)) table->SetWIDTH(value);
    if (FindAttribute("BGCOLOR", value, true)) table->SetBGCOLOR(value, services_.graphics);
    if (FindAttribute("CELLPADDING", value, true)) cHTMLShared::SetLength(value, table->heightAttr);
    if (FindAttribute("CELLSPACING", value, true)) cHTMLShared::SetLength(value, table->spacingAttr);
    GetToken(true);
    if (token_ == kTagCaption) {
        SkipUntilToken(kTagEndCaption);
        GetToken(true);
    }
    for (;;) {
        while (token_ == kTagCol) GetToken(true);
        if (token_ != kTagColgroup) break;
        ParseCOLGROUP(*table);
        GetToken(true);
    }
    if (token_ == kTagThead) {
        ParseTHEAD(*table);
        GetToken(true);
    }
    if (token_ == kTagTfoot) {
        ParseTFOOT(*table);
        GetToken(true);
    }
    while (token_ == kTagTbody || token_ == kTagTr) {
        ParseTBODY(*table);
        GetToken(true);
    }
    if (token_ != kTagEndTable && !SkipUntilToken(kTagEndTable)) {
        delete table;
        return false;
    }
    parent.Insert(table);
    return true;
}

bool cHTMLDocument::ParseCOLGROUP(cHTMLTABLE&) {
    do {
        GetToken(true);
    } while (token_ == kTagCol);
    unget_ = true;
    return true;
}

bool cHTMLDocument::ParseTHEAD(cHTMLTABLE& table) {
    for (;;) {
        GetToken(true);
        if (token_ != kTagTr) break;
        ParseTR(table);
    }
    if (token_ != kTagEndThead) unget_ = true;
    return true;
}

bool cHTMLDocument::ParseTFOOT(cHTMLTABLE& table) {
    for (;;) {
        GetToken(true);
        if (token_ != kTagTr) break;
        ParseTR(table);
    }
    if (token_ != kTagEndTfoot) unget_ = true;
    return true;
}

bool cHTMLDocument::ParseTBODY(cHTMLTABLE& table) {
    if (token_ == kTagTbody) GetToken(true);
    while (token_ == kTagTr) {
        ParseTR(table);
        GetToken(true);
    }
    if (token_ != kTagEndTbody) unget_ = true;
    return true;
}

bool cHTMLDocument::ParseTR(cHTMLTABLE& table) {
    auto* row = new cHTMLTR();
    if (table.hasBackground) {
        row->backgroundColor = table.backgroundColor;
        row->hasBackground = true;
    }
    cRZString value;
    if (FindAttribute("BGCOLOR", value, true)) row->SetBGCOLOR(value, services_.graphics);
    GetToken(true);
    for (;;) {
        if (token_ == kTagTd) {
            ParseTD(*row);
        } else if (token_ == kTagTh) {
            ParseTH(*row);
        } else {
            if (token_ != kTagEndTr) unget_ = true;
            if (row->cells.empty()) {
                delete row;
                return false;
            }
            table.Insert(row);
            table.AddRow(row);
            return true;
        }
        GetToken(true);
    }
}

bool cHTMLDocument::ParseTD(cHTMLTR& row) {
    auto* cell = new cHTMLTD();
    cell->graphics = services_.graphics;
    if (row.hasBackground) {
        cell->backgroundColor = row.backgroundColor;
        cell->hasBackground = true;
    }
    row.Insert(cell);
    row.cells.push_back(cell);
    cRZString value;
    if (FindAttribute("VALIGN", value, true)) {
        value.MakeUpper();
        const std::string& s = value.str();
        if (s == "TOP") cell->valign = 1;
        else if (s == "MIDDLE") cell->valign = 2;
        else if (s == "BOTTOM") cell->valign = 3;
        else if (s == "BASELINE") cell->valign = 4;
    }
    if (FindAttribute("WIDTH", value, true)) cHTMLShared::SetLength(value, cell->width);
    if (FindAttribute("BGCOLOR", value, true)) cell->SetBGCOLOR(value, services_.graphics);
    if (FindAttribute("NOWRAP", value, true)) cell->SetNOWRAP(value);
    int style = 0;
    if (FindAttribute("ALIGN", value, true)) style = MakeOutputStyle(value);
    outputStyleStack_.push_back(outputStyle_);
    outputStyle_ = style;
    GetToken(false);
    while (InFirstSet(FirstSets().flow)) {
        ParseFLOW(*cell);
        GetToken(false);
    }
    if (token_ != kTagEndTd) unget_ = true;
    if (!outputStyleStack_.empty()) {
        outputStyle_ = outputStyleStack_.back();
        outputStyleStack_.pop_back();
    }
    return true;
}

bool cHTMLDocument::ParseTH(cHTMLTR& row) {
    auto* header = new cHTMLTH();
    row.Insert(header);
    GetToken(false);
    while (InFirstSet(FirstSets().flow)) {
        ParseFLOW(*header);
        GetToken(false);
    }
    if (token_ != kTagEndTh) unget_ = true;
    return true;
}

// ---- images ------------------------------------------------------------------------------------------------------------------------------

bool cHTMLDocument::LoadFromImageFile(const std::string& path, IHtmlBitmap*& bitmap) {
    return services_.images && services_.images->LoadImageFile(path, bitmap);
}

// A bullet is a small picture (half the current font size) cached by (size, style, colour).
cHTMLIMG* cHTMLDocument::CreateBullet(tBulletStyle style) {
    int size = static_cast<int>(font_.size >> 1);
    if (size == 0) size = 1;
    IHtmlBitmap* bitmap = nullptr;
    for (const tBulletInfo& info : bullets_) {
        if (info.style == style && info.size == size && info.color == font_.color) {
            bitmap = info.bitmap;
            break;
        }
    }
    if (!bitmap) {
        if (!services_.createBullet) return nullptr;
        Color background = services_.graphics->MakeColor(0xc0, 0xc0, 0xc0);
        if (body_ && body_->hasBackgroundColor)
            background = services_.graphics->MakeColor(static_cast<std::uint8_t>(body_->backgroundColor >> 16),
                                                       static_cast<std::uint8_t>(body_->backgroundColor >> 8),
                                                       static_cast<std::uint8_t>(body_->backgroundColor));
        bitmap = services_.createBullet(size, background, font_.color, style == kBulletDisc);
        if (!bitmap) return nullptr;
        bullets_.push_back({size, style, font_.color, bitmap});
    }
    auto* img = new cHTMLIMG();
    img->outputStyle = outputStyle_;
    img->SetBitmap(bitmap);
    return img;
}

void cHTMLDocument::RequestImage(IHtmlBitmapReceiver* requester, const cRZString& url) {
    std::lock_guard<std::recursive_mutex> guard(lock_);
    auto it = images_.find(url.str());
    if (it == images_.end()) {
        images_[url.str()].pending.push_back(requester);
        if (web_) images_[url.str()].jobId = web_->StartJob(url.str(), this);
        return;
    }
    if (it->second.bitmap == nullptr) {
        it->second.pending.push_back(requester);
    } else if (requester->SetBitmap(it->second.bitmap)) {
        MarkPageLayoutStatus(false);
    }
}

void cHTMLDocument::ResourceHasBeenDownloaded(const tDownloadedJob& job) {
    std::lock_guard<std::recursive_mutex> guard(lock_);
    auto it = images_.find(job.url);
    if (it == images_.end()) return;
    std::string path = job.data;
    bool temporary = false;
    if (!job.isFile) {      // the data came in memory: park it in a temp file for the image decoder
        path = services_.makeTempPath ? services_.makeTempPath() : std::string();
        temporary = true;
        if (!path.empty()) {
            cRZFile file{cRZString(path.c_str())};
            if (file.Open(2, 1, 0)) {
                file.Write(job.data.data(), static_cast<std::uint32_t>(job.data.size()));
                file.Close();
            }
        }
    }
    LoadFromImageFile(path, it->second.bitmap);
    if (temporary && !path.empty()) {
        cRZFile file{cRZString(path.c_str())};
        file.Remove();
    }
    if (it->second.bitmap) {
        for (IHtmlBitmapReceiver* receiver : it->second.pending) receiver->SetBitmap(it->second.bitmap);
        it->second.pending.clear();
    }
}

void cHTMLDocument::Downloaded(bool succeeded, const tDownloadedJob& job) {
    if (!succeeded) return;
    DoMessage(kMessageDownloaded, &job);
    MarkPageLayoutStatus(false);
    InvalidateVisual();
}

bool cHTMLDocument::DoMessage(std::uint32_t type, const tDownloadedJob* job) {
    if (type != kMessageDownloaded) return false;
    if (job) ResourceHasBeenDownloaded(*job);
    return true;
}

}  // namespace html
