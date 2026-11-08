// cHTMLDocument — the HTML parser and owner of the element tree (98 functions, 0x10057d20c..0x1005b24a2,
// director-less COM object, interface ids 0xe0eca23a and 0x452294aa message target).
//
// A hand-written recursive-descent parser over a token stream: GetToken() cuts the text into character data
// (kTokenText) and tags (kTagXxx ids from the static tag table), ParseAttributeList() fills the attribute map of the
// current tag, and one Parse<Element> function per construct builds the elements. The grammar is the HTML 4 DTD in
// first-set form: INLINE = FONTSTYLE + PHRASE + SPECIAL + FORMCTRL (+ text), BLOCK = headings + lists + P PRE DL
// DIV NOSCRIPT BLOCKQUOTE FORM HR TABLE FIELDSET ADDRESS CENTER DIR MENU, FLOW = INLINE + BLOCK. Unknown tags have
// token 0 and end whatever loop is running; an end tag that is not the expected one is pushed back (unget).
//
// Font state (kind / size / colour / style / preformatted / link flag) is a tFontAttributes stack: PushFont before
// a style element, PopFont after. The output (text alignment) style has its own stack.
//
// Services the original reaches through the framework are passed in HtmlServices; images are requested through the
// web system and delivered to the elements (cHTMLIMG, the body background) when the download finishes.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "html/HTMLElements.h"

namespace html {

enum eHTMLToken : int {
    kTokenUnknown = 0, kTokenEof = 1, kTokenDoctype = 2, kTokenText = 3,
    kTagDoctype = 2,  // !DOCTYPE
    kTagHtml = 4,  // HTML
    kTagEndHtml = 5,  // /HTML
    kTagHead = 6,  // HEAD
    kTagEndHead = 7,  // /HEAD
    kTagBody = 8,  // BODY
    kTagEndBody = 9,  // /BODY
    kTagTitle = 10,  // TITLE
    kTagEndTitle = 11,  // /TITLE
    kTagBase = 12,  // BASE
    kTagStyle = 13,  // STYLE
    kTagEndStyle = 14,  // /STYLE
    kTagMeta = 15,  // META
    kTagLink = 16,  // LINK
    kTagIsindex = 17,  // ISINDEX
    kTagTt = 18,  // TT
    kTagEndTt = 19,  // /TT
    kTagI = 20,  // I
    kTagEndI = 21,  // /I
    kTagB = 22,  // B
    kTagEndB = 23,  // /B
    kTagStrike = 24,  // STRIKE
    kTagEndStrike = 25,  // /STRIKE
    kTagBig = 26,  // BIG
    kTagEndBig = 27,  // /BIG
    kTagSmall = 28,  // SMALL
    kTagEndSmall = 29,  // /SMALL
    kTagU = 30,  // U
    kTagEndU = 31,  // /U
    kTagFont = 32,  // FONT
    kTagEndFont = 33,  // /FONT
    kTagEm = 34,  // EM
    kTagEndEm = 35,  // /EM
    kTagStrong = 36,  // STRONG
    kTagEndStrong = 37,  // /STRONG
    kTagDfn = 38,  // DFN
    kTagEndDfn = 39,  // /DFN
    kTagCode = 40,  // CODE
    kTagEndCode = 41,  // /CODE
    kTagSamp = 42,  // SAMP
    kTagEndSamp = 43,  // /SAMP
    kTagKbd = 44,  // KBD
    kTagEndKbd = 45,  // /KBD
    kTagVar = 46,  // VAR
    kTagEndVar = 47,  // /VAR
    kTagCite = 48,  // CITE
    kTagEndCite = 49,  // /CITE
    kTagAbbr = 50,  // ABBR
    kTagEndAbbr = 51,  // /ABBR
    kTagAcronym = 52,  // ACRONYM
    kTagEndAcronym = 53,  // /ACRONYM
    kTagA = 54,  // A
    kTagEndA = 55,  // /A
    kTagImg = 56,  // IMG
    kTagObject = 57,  // OBJECT
    kTagEndObject = 58,  // /OBJECT
    kTagBr = 59,  // BR
    kTagScript = 60,  // SCRIPT
    kTagEndScript = 61,  // /SCRIPT
    kTagMap = 62,  // MAP
    kTagEndMap = 63,  // /MAP
    kTagQ = 64,  // Q
    kTagEndQ = 65,  // /Q
    kTagSub = 66,  // SUB
    kTagEndSub = 67,  // /SUB
    kTagSup = 68,  // SUP
    kTagEndSup = 69,  // /SUP
    kTagSpan = 70,  // SPAN
    kTagEndSpan = 71,  // /SPAN
    kTagBdo = 72,  // BDO
    kTagEndBdo = 73,  // /BDO
    kTagBlink = 74,  // BLINK
    kTagEndBlink = 75,  // /BLINK
    kTagInput = 76,  // INPUT
    kTagSelect = 77,  // SELECT
    kTagEndSelect = 78,  // /SELECT
    kTagOption = 79,  // OPTION
    kTagEndOption = 80,  // /OPTION
    kTagOptgroup = 81,  // OPTGROUP
    kTagEndOptgroup = 82,  // /OPTGROUP
    kTagTextarea = 83,  // TEXTAREA
    kTagEndTextarea = 84,  // /TEXTAREA
    kTagLabel = 85,  // LABEL
    kTagEndLabel = 86,  // /LABEL
    kTagButton = 87,  // BUTTON
    kTagEndButton = 88,  // /BUTTON
    kTagH1 = 89,  // H1
    kTagEndH1 = 90,  // /H1
    kTagH2 = 91,  // H2
    kTagEndH2 = 92,  // /H2
    kTagH3 = 93,  // H3
    kTagEndH3 = 94,  // /H3
    kTagH4 = 95,  // H4
    kTagEndH4 = 96,  // /H4
    kTagH5 = 97,  // H5
    kTagEndH5 = 98,  // /H5
    kTagH6 = 99,  // H6
    kTagEndH6 = 100,  // /H6
    kTagP = 101,  // P
    kTagEndP = 102,  // /P
    kTagPre = 103,  // PRE
    kTagEndPre = 104,  // /PRE
    kTagUl = 105,  // UL
    kTagEndUl = 106,  // /UL
    kTagOl = 107,  // OL
    kTagEndOl = 108,  // /OL
    kTagDl = 109,  // DL
    kTagEndDl = 110,  // /DL
    kTagDiv = 111,  // DIV
    kTagEndDiv = 112,  // /DIV
    kTagNoscript = 113,  // NOSCRIPT
    kTagEndNoscript = 114,  // /NOSCRIPT
    kTagBlockquote = 115,  // BLOCKQUOTE
    kTagEndBlockquote = 116,  // /BLOCKQUOTE
    kTagForm = 117,  // FORM
    kTagEndForm = 118,  // /FORM
    kTagHr = 119,  // HR
    kTagTable = 120,  // TABLE
    kTagEndTable = 121,  // /TABLE
    kTagFieldset = 122,  // FIELDSET
    kTagEndFieldset = 123,  // /FIELDSET
    kTagAddress = 124,  // ADDRESS
    kTagEndAddress = 125,  // /ADDRESS
    kTagCenter = 126,  // CENTER
    kTagEndCenter = 127,  // /CENTER
    kTagDir = 128,  // DIR
    kTagEndDir = 129,  // /DIR
    kTagMenu = 130,  // MENU
    kTagEndMenu = 131,  // /MENU
    kTagCaption = 132,  // CAPTION
    kTagEndCaption = 133,  // /CAPTION
    kTagCol = 134,  // COL
    kTagColgroup = 135,  // COLGROUP
    kTagEndColgroup = 136,  // /COLGROUP
    kTagThead = 137,  // THEAD
    kTagEndThead = 138,  // /THEAD
    kTagTbody = 139,  // TBODY
    kTagEndTbody = 140,  // /TBODY
    kTagTfoot = 141,  // TFOOT
    kTagEndTfoot = 142,  // /TFOOT
    kTagTd = 143,  // TD
    kTagEndTd = 144,  // /TD
    kTagTh = 145,  // TH
    kTagEndTh = 146,  // /TH
    kTagTr = 147,  // TR
    kTagEndTr = 148,  // /TR
    kTagLi = 149,  // LI
    kTagEndLi = 150,  // /LI
    kTagDt = 151,  // DT
    kTagEndDt = 152,  // /DT
    kTagDd = 153,  // DD
    kTagEndDd = 154,  // /DD
    kTagArea = 155,  // AREA
    kTagFrameset = 156,  // FRAMESET
    kTagEndFrameset = 157,  // /FRAMESET
    kTagFrame = 158,  // FRAME
    kTagNoframes = 159,  // NOFRAMES
    kTagEndNoframes = 160,  // /NOFRAMES
    kTagNobr = 161,  // NOBR
    kTagEndNobr = 162,  // /NOBR
};

// First sets of the grammar (BuildFirstSets), built once.
struct tFirstSets {
    std::set<int> fontStyle, phrase, special, formCtrl, inlineSet, heading, list, block, flow;
};
const tFirstSets& FirstSets();
// token id of an upper-case tag name ("/P" for end tags), 0 when unknown
int LookupTag(const std::string& upperName);

struct tDownloadedJob;

// Who a download reports back to. The original hands the WWW system a cIGZMessageTarget2 that receives
// kMessageDownloaded (data2 = succeeded, void3 = the job); the port's facade names that target with this interface.
class IHtmlDownloadTarget {
public:
    virtual ~IHtmlDownloadTarget() = default;
    virtual void Downloaded(bool succeeded, const tDownloadedJob& job) = 0;
};

class IHtmlWebSystem {
public:
    virtual ~IHtmlWebSystem() = default;
    // starts a download of `url`; 0 when it could not start
    virtual std::uint32_t StartJob(const std::string& url) = 0;           // vslot 0x80
    // the same, reporting the finished job to `target` (the original's message target argument) on a later message
    // pump; a cancelled job is never reported
    virtual std::uint32_t StartJob(const std::string& url, IHtmlDownloadTarget* target) { (void)target; return StartJob(url); }
    virtual void CancelJob(std::uint32_t jobId) = 0;                      // vslot 0xb0
    virtual bool IsVisited(const std::string& absoluteUrl) = 0;          // vslot 0xc0
};

class IHtmlImageDecoder {
public:
    virtual ~IHtmlImageDecoder() = default;
    virtual bool LoadImageFile(const std::string& path, IHtmlBitmap*& bitmap) = 0;   // class 0xa2ffb5d3, vslot 0x20
};

struct HtmlServices {
    IHtmlGraphics* graphics = nullptr;
    IHtmlFontSystem* fonts = nullptr;
    IHtmlImageDecoder* images = nullptr;
    // makes the bullet pictures; null falls back to no bullet
    std::function<IHtmlBitmap*(int size, Color background, Color foreground, bool disc)> createBullet;
    std::function<std::string(const std::string& href, const std::string& base)> resolveUrl;     // cRZURL::ChangeURL
    std::function<std::string()> makeTempPath;                                                    // cRZFile::MakeTempPathName
};

// A finished download as the message handler sees it.
struct tDownloadedJob {
    std::string url;
    bool isFile = false;          // true: `data` is the path of a file that already holds the resource
    std::string data;
};

class cHTMLDocument : public IHtmlDocumentServices, public IHtmlDownloadTarget {
public:
    enum tBulletStyle { kBulletDisc = 0, kBulletSquare = 1 };
    static constexpr std::uint32_t kInterfaceId = 0xe0eca23a;
    static constexpr std::uint32_t kMessageDownloaded = 0x75758483;

    explicit cHTMLDocument(const HtmlServices& services);
    ~cHTMLDocument() override;

    bool Init(IHtmlWindow* window, IHtmlWebSystem* web, const cRZString& baseUrl);
    bool Shutdown();
    std::uint32_t AddRef() { return ++refCount_; }
    std::uint32_t Release();

    // ---- document text and top-level parse (vtable slots 6..13)
    const cRZString& GetDocument() const { return documentText_; }
    void SetDocument(const cRZString& text) { ParseSGMLElements(text); }
    bool ParseSGMLElements(const cRZString& text);
    bool ParseSGMLDocumentType();
    bool ParseHTMLDocument();
    bool ParseTextDocument();
    bool ParseHTMLElements();

    // ---- state accessors
    cHTMLBODY* Body() const { return body_; }
    bool IsPageLayoutDone() const { return pageLayoutDone_; }
    void MarkPageLayoutStatus(bool done) { pageLayoutDone_ = done; }
    bool IsBlinkingPresent() const { return blinkingPresent_; }
    Color GetActiveLinkColor() const { return activeLinkColor_; }
    Color GetVisitedLinkColor() const { return visitedLinkColor_; }
    int GetFontSizeTable(std::uint32_t* out, int count) const;
    void SetFontSizeTable(const std::uint32_t* sizes, int count);
    bool GetBaseURL(cRZString& out) const;
    bool SetBaseURL(const cRZString& url);
    cHTMLAnchor* FindAnchor(const cIGZString& name);
    void InvalidateVisual();
    const std::string& title() const { return title_; }
    const std::vector<cHTMLMETA*>& metas() const { return metas_; }
    const std::vector<cHTMLLink*>& links() const { return links_; }
    const std::string& docTypeVersion() const { return docTypeVersion_; }
    const std::string& docTypeLanguage() const { return docTypeLanguage_; }

    // ---- images
    void RequestImage(IHtmlBitmapReceiver* requester, const cRZString& url);
    void ResourceHasBeenDownloaded(const tDownloadedJob& job);
    bool DoMessage(std::uint32_t type, const tDownloadedJob* job);
    // IHtmlDownloadTarget (port): the facade's delivery of kMessageDownloaded. A picture can change the layout, so the
    // page is laid out and painted again (the original's message pump repaints the window after the message).
    void Downloaded(bool succeeded, const tDownloadedJob& job) override;
    bool LoadFromImageFile(const std::string& path, IHtmlBitmap*& bitmap);
    cHTMLIMG* CreateBullet(tBulletStyle style);

    // ---- IHtmlDocumentServices
    IHtmlFontSystem* FontSystem() override { return services_.fonts; }
    std::string GetStringForEscape(const cRZString& entityName) override;
    void GetDefaultVariableWidthFontInfo(std::string& faceName, std::uint32_t& style) override;
    void GetDefaultFixedWidthFontInfo(std::string& faceName, std::uint32_t& style) override;

    // ---- tokenizer
    void GetToken(bool skipText);
    bool ParseCDATA();
    void SplitTag(std::string& tag, std::string& rest);
    void ParseAttributeList();
    bool FindAttribute(const char* upperName, cRZString& value, bool erase);
    bool SkipUntilToken(int token);
    void SkipDocumentWhitespace();
    static void StripWhiteSpace(std::string& text);
    int token() const { return token_; }
    void UngetToken() { unget_ = true; }

    // ---- element parsers (grammar productions)
    bool ParseA(cHTMLContainerElement& parent);
    bool ParseAREA(cHTMLContainerElement& parent);
    bool ParseBASE();
    bool ParseBLINK(cHTMLContainerElement& parent);
    bool ParseBLOCK(cHTMLContainerElement& parent);
    bool ParseBLOCKQUOTE(cHTMLContainerElement& parent);
    bool ParseBODY();
    bool ParseBR(cHTMLContainerElement& parent);
    bool ParseCOLGROUP(cHTMLTABLE& table);
    bool ParseDD(cHTMLContainerElement& parent);
    bool ParseDIV(cHTMLContainerElement& parent);
    bool ParseDIVActual(cHTMLContainerElement& parent, int endToken, int outputStyle);
    bool ParseDL(cHTMLContainerElement& parent);
    bool ParseDT(cHTMLContainerElement& parent);
    bool ParseFLOW(cHTMLContainerElement& parent);
    bool ParseFONTSTYLE(cHTMLContainerElement& parent);
    bool ParseFORM(cHTMLContainerElement& parent);
    bool ParseFORMCTRL(cHTMLContainerElement& parent);
    bool ParseHEAD();
    bool ParseHR(cHTMLContainerElement& parent);
    bool ParseHn(cHTMLContainerElement& parent);
    bool ParseIMG(cHTMLContainerElement& parent);
    bool ParseINLINE(cHTMLContainerElement& parent);
    bool ParseINPUT(cHTMLContainerElement& parent);
    bool ParseLI(cHTMLList& list, int number);
    bool ParseLINK();
    bool ParseLIST(cHTMLContainerElement& parent);
    bool ParseMAP(cHTMLContainerElement& parent);
    bool ParseMETA();
    bool ParseOL(cHTMLContainerElement& parent);
    bool ParseOPTION(cHTMLContainerElement& parent);
    bool ParseP(cHTMLContainerElement& parent);
    bool ParsePHRASE(cHTMLContainerElement& parent);
    bool ParsePRE(cHTMLContainerElement& parent);
    bool ParseSELECT(cHTMLContainerElement& parent);
    bool ParseSPAN(cHTMLContainerElement& parent);
    bool ParseSPECIAL(cHTMLContainerElement& parent);
    bool ParseTABLE(cHTMLContainerElement& parent);
    bool ParseTBODY(cHTMLTABLE& table);
    bool ParseTD(cHTMLTR& row);
    bool ParseTFOOT(cHTMLTABLE& table);
    bool ParseTH(cHTMLTR& row);
    bool ParseTHEAD(cHTMLTABLE& table);
    bool ParseTITLE();
    bool ParseTR(cHTMLTABLE& table);
    bool ParseUL(cHTMLContainerElement& parent);
    void InsertTextSegment(const cRZString& text, cHTMLContainerElement& parent);

    // ---- font / alignment state
    void PushFont();
    void PopFont();
    static int MakeOutputStyle(cRZString& value);                        // LEFT 0, CENTER 1, RIGHT 2
    static int MakeRelativeToTextAlignmentStyle(cRZString& value);       // LEFT 0, RIGHT 1, TOP 2, MIDDLE 3, BOTTOM 4

    tFontAttributes& font() { return font_; }
    int outputStyle() const { return outputStyle_; }

private:
    bool InFirstSet(const std::set<int>& set) const { return set.count(token_) != 0; }

    HtmlServices services_;
    IHtmlWindow* window_ = nullptr;
    IHtmlWebSystem* web_ = nullptr;
    cHTMLBODY* body_ = nullptr;                // +0x28
    bool initialized_ = false;                 // +0x30
    std::uint32_t refCount_ = 0;               // +0x34
    bool unget_ = false;                       // +0x40
    bool pageLayoutDone_ = false;              // +0x41
    bool blinkingPresent_ = false;             // +0x42
    std::recursive_mutex lock_;                // +0x58

    std::string variableFace_{"arial"};            // +0x78
    std::uint32_t variableStyle_ = 0;              // +0x98
    std::string fixedFace_{"courier new"};         // +0xa8
    std::uint32_t fixedStyle_ = 0;                 // +0xc8
    std::string docTypeVersion_, docTypeLanguage_; // +0xd8, +0x100
    std::string title_;                            // +0x128
    std::vector<cHTMLMETA*> metas_;                // +0x148
    std::unique_ptr<cRZString> baseUrl_;           // +0x160
    std::vector<cHTMLLink*> links_;                // +0x168
    std::vector<cHTMLAnchor*> anchors_;            // +0x180

    int token_ = 0;                                // +0x1c0
    std::string tagName_, attrText_;               // +0x198, +0x1c8
    cRZString cdata_;                              // +0x1f0
    std::unordered_map<std::string, std::string> attributes_;   // +0x218
    int olDepth_ = 0, ulDepth_ = 0;                // +0x240, +0x244
    tFontAttributes font_;                         // +0x248..
    int outputStyle_ = 0;                          // +0x288
    std::vector<tFontAttributes> fontStack_;       // +0x290
    std::vector<int> outputStyleStack_;            // +0x2a8
    cRZString documentText_;                       // +0x2d8
    std::uint32_t position_ = 0;                   // +0x300

    struct tURLToImageMapItem {
        IHtmlBitmap* bitmap = nullptr;             // +0x48
        std::vector<IHtmlBitmapReceiver*> pending; // +0x50
        std::uint32_t jobId = 0;                   // +0x68
    };
    std::map<std::string, tURLToImageMapItem> images_;   // +0x308

    std::uint32_t fontSizes_[7] = {8, 10, 12, 14, 18, 24, 36};   // +0x328
    Color defaultColor_ = 0, visitedLinkColor_ = 0, unvisitedLinkColor_ = 0, activeLinkColor_ = 0;

    struct tBulletInfo { int size; int style; Color color; IHtmlBitmap* bitmap; };
    std::vector<tBulletInfo> bullets_;
};

}  // namespace html
