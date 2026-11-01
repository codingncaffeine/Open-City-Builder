// cGZLegacyControl — implementation. See LegacyControl.h for the layout, the three interfaces and the quirks.
//
// The 21 ControlVocabulary* functions register the script keyword table and the two class tables exactly as the
// binary does (the keyword strings and ids were read out of the decompiled functions / the inline string
// buffers they build; the class ids are the vocabulary's own GZWin* / IGZWin* entries). The interpreter
// dispatch table is the original's TU-static sInterpretVocab (0x100b6eb10) and the class->view table its
// sCLSIDtoIIDMap (0x100b6eb28).
#include "gz/LegacyControl.h"

#include <cstdio>
#include <cstring>
#include <map>

#include "gz/Win.h"
#include "gz/WinBMP.h"
#include "gz/WinFlatRect.h"
#include "gz/WinGen.h"
#include "gz/WinScrollbar.h"
#include "gz/WinBtn.h"
#include "gz/WinSlider.h"
#include "gz/WinSpinner.h"
#include "gz/WinText.h"
#include "gz/WinTextEdit.h"
#include "rz/cRZVariant.h"
#include "sc4/WinProcs.h"

namespace {

struct VocabEntry {
    const char* name;
    std::uint32_t value;
};

const VocabEntry kCommonVocab[] = {
    {"clsid", 0xffffffff},
    {"iid", 0xfffffffe},
    {"yes", 1},
    {"true", 1},
    {"no", 0},
    {"false", 0},
    {"on", 1},
    {"off", 0},
    {"GZWin", 0xe2ba00ee},
    {"id", 0x100},
    {"area", 0x101},
    {"pos", 0x102},
    {"size", 0x103},
    {"fillcolor", 0x104},
    {"caption", 0x105},
    {"captionres", 0x106},
    {"transparent", 0x107},
    {"font", 0xf000},
    {"bkgcolor", 0xf001},
    {"forecolor", 0xf002},
    {"notify", 0xf003},
    {"gutters", 0xf004},
    {"style", 0xf005},
    {"left", 0xf006},
    {"right", 0xf007},
    {"center", 0xf008},
    {"flat", 0xf009},
    {"normal", 0xf00a},
    {"sunken", 0xf00b},
    {"raised", 0xf00c},
    {"fill", 0xf00d},
    {"nofill", 0xf00e},
    {"colorfontnormal", 0xf00f},
    {"colorfontdisabled", 0xf010},
    {"colorfonthilited", 0xf011},
    {"colorfontnormalbkg", 0xf012},
    {"colorfontdisabledbkg", 0xf013},
    {"colorfonthilitedbkg", 0xf014},
    {"align", 0xf015},
    {"image", 0xf016},
    {"imagetype", 0xf017},
    {"imagerect", 0xf018},
    {"outline", 0xf019},
    {"comments", 0x108},
    {"commentslmd", 0x109},
    {"commentsrmd", 0x10a},
    {"commentslmu", 0x10b},
    {"commentsrmu", 0x10c},
    {"commentsmm", 0x10d},
    {"commentsku", 0x10e},
    {"commentskd", 0x10f},
    {"winflag_visible", 0xf01a},
    {"winflag_enabled", 0xf01b},
    {"winflag_moveable", 0xf01c},
    {"winflag_sizeable", 0xf01d},
    {"winflag_sortable", 0xf01e},
    {"winflag_pbuff", 0xf01f},
    {"winflag_pbufftrans", 0xf020},
    {"winflag_pbufferase", 0xf021},
    {"winflag_pbuffvid", 0xf022},
    {"winflag_alphablend", 0xf026},
    {"winflag_mousetrans", 0xf023},
    {"winflag_ignoremouse", 0xf024},
    {"winflag_acceptfocus", 0xf025},
};

const VocabEntry kFlatRectVocab[] = {
    {"GZWinFlatRect", 0xc2afa76e},
    {"IGZWinFlatRect", 0xc2afa76f},
    {"colorleft", 0x200},
    {"colortop", 0x201},
    {"colorright", 0x202},
    {"colorbottom", 0x203},
    {"top", 0x204},
    {"bottom", 0x205},
    {"all", 0x206},
    {"lrb", 0x207},
    {"trb", 0x208},
};

const VocabEntry kTextVocab[] = {
    {"GZWinText", 0x592},
    {"IGZWinText", 0x212cdc1f},
    {"wrapped", 0x300},
    {"opaque", 0x301},
    {"defcolor", 0x302},
    {"dbgdrawarea", 0x303},
    {"autofit", 0x304},
    {"lefttop", 0},
    {"leftcenter", 1},
    {"leftbottom", 2},
    {"centertop", 3},
    {"center", 4},
    {"centerbottom", 5},
    {"righttop", 6},
    {"rightcenter", 7},
    {"rightbottom", 8},
    {"textoffsets", 0x305},
};

const VocabEntry kTextEditVocab[] = {
    {"GZWinTextEdit", 0x231a1c58},
    {"IGZWinTextEdit", 0x231a1c57},
    {"editable", 0x400},
    {"hscrollbar", 0x401},
    {"vscrollbar", 0x402},
    {"caretvisible", 0x403},
    {"allowinsert", 0x404},
    {"allowundo", 0x405},
    {"singleline", 0x406},
    {"caretcolor", 0x407},
    {"highlightcolor", 0x408},
    {"maxtext", 0x409},
    {"initvalue", 0x40a},
    {"outlinecolor", 0x40b},
    {"overwrite", 0x40c},
    {"insertindex", 0x40d},
    {"insertpos", 0x40e},
    {"caretperiod", 0x40f},
    {"maxundo", 0x410},
    {"hscrollimage", 0x413},
    {"hscrollimageRect", 0x414},
    {"vscrollimage", 0x411},
    {"vscrollimageRect", 0x412},
    {"notifyonreturn", 0x415},
    {"notifyonchange", 0x416},
    {"notifyonlostfocus", 0x417},
    {"enableclipboard", 0x418},
    {"passwordmode", 0x419},
};

const VocabEntry kListBoxVocab[] = {
    {"GZWinListBox", 0x598},
    {"IGZWinListBox", 0x4132242b},
    {"scrollbargutters", 0x500},
    {"initselection", 0x501},
    {"sort", 0x503},
    {"drop", 0x504},
    {"setlinevisible", 0x505},
    {"setlineontop", 0x506},
    {"listelement", 0x502},
};

const VocabEntry kComboVocab[] = {
    {"GZWinCombo", 0x59b},
    {"IGZWinCombo", 0x412ce496},
    {"combodownarrowimage", 0x800},
    {"combodownarrowrect", 0x801},
    {"combodowncolor", 0x802},
    {"buttongutter", 0x803},
};

const VocabEntry kBtnVocab[] = {
    {"GZWinBtn", 0x22ecfc47},
    {"IGZWinBtn", 0x8810},
    {"toggle", 0xa00},
    {"triggerondown", 0xa01},
    {"showcaption", 0xa02},
    {"autosize", 0xa03},
    {"wrapcaption", 0xa04},
    {"shiftcaption", 0xa05},
    {"tips", 0xa06},
    {"tipsdelay", 0xa07},
    {"tipstimeout", 0xa08},
    {"label", 0xa09},
    {"standard", 0xa0a},
    {"standardfixed", 0xa0b},
    {"togglebtn", 0xa0c},
    {"togglebtnfixed", 0xa0d},
    {"checkbox", 0xa0e},
    {"radiobtn", 0xa0f},
    {"tiptext", 0xa10},
    {"tipoffsets", 0xa11},
    {"tipres", 0xa12},
    {"noimage", 0xa13},
    {"standardhilite", 0xa14},
    {"standardhilitefixed", 0xa15},
    {"radiocheckall", 0xa16},
    {"radio", 0xa17},
    {"tipflag", 0xa18},
    {"btnclicksnd", 0xa19},
    {"btnupsnd", 0xa1a},
    {"radiocheck", 0xa1b},
};

const VocabEntry kBMPVocab[] = {
    {"GZWinBMP", 0x82fe68c4},
    {"IGZWinBMP", 0xc12cea13},
    {"alpha", 0xb00},
    {"transparentbkg", 0xb01},
    {"edgeimage", 0xb02},
};

const VocabEntry kSliderVocab[] = {
    {"GZWinSlider", 0x21325208},
    {"IGZWinSlider", 0x21325207},
    {"minmaxvalue", 0xc00},
    {"direction", 0xc01},
    {"horizontal", 0xc02},
    {"vertical", 0xc03},
};

const VocabEntry kScrollbarVocab[] = {
    {"GZWinScrollbar", 0x61325a2e},
    {"IGZWinScrollbar", 0x61325a2d},
    {"pagesize", 0xd00},
    {"linesize", 0xd01},
    {"linepagecount", 0xd02},
};

const VocabEntry kLineInputVocab[] = {
    {"GZWinLineInput", 0x21335c5a},
    {"IGZWinLineInput", 0x21335c59},
    {"filled", 0xe00},
    {"outline3d", 0xe01},
    {"outline3dex", 0xe02},
    {"colorlt", 0xe03},
    {"colorrb", 0xe04},
    {"colorltinside", 0xe05},
    {"colorrbinside", 0xe06},
    {"fillcolorhilite", 0xe07},
    {"fillalphahilite", 0xe08},
    {"caretpos", 0xe09},
    {"charlimit", 0xe0a},
    {"selection", 0xe0b},
};

const VocabEntry kOptGrpVocab[] = {
    {"GZWinOptGrp", 0xa1336cc1},
    {"IGZWinOptGrp", 0xa1336cc0},
    {"maincaption", 0xf00},
    {"itemcaption", 0xf01},
    {"drawfill", 0xf02},
    {"labelfont", 0xf03},
    {"buttonfont", 0xf04},
    {"labelfontcolornormal", 0xf05},
    {"labelfontcolordisabled", 0xf06},
    {"buttonfontcolornormal", 0xf07},
    {"buttonfontcolorhilited", 0xf08},
    {"captiontextresid", 0xf0a},
    {"backimage", 0xf0b},
    {"autofilltype", 0xf0c},
    {"option", 0xf0d},
    {"automatic", 0xf0e},
    {"manual", 0xf0f},
    {"autofillsize", 0xf10},
    {"optionmoveto", 0xf11},
    {"optionsetsize", 0xf12},
};

const VocabEntry kSpinnerVocab[] = {
    {"GZWinSpinner", 0x612ce0c4},
    {"IGZWinSpinner", 0x612ce0c3},
    {"digits", 0x1000},
    {"autonumber", 0x1001},
    {"autonumbercomma", 0x1002},
    {"autonumbercurrency", 0x1003},
    {"minmax", 0x1004},
    {"stepsize", 0x1005},
    {"coloroutline", 0x1006},
    {"coloroutlinel", 0x1007},
    {"coloroutlinet", 0x1008},
    {"coloroutliner", 0x1009},
    {"coloroutlineb", 0x100a},
};

const VocabEntry kOutlineVocab[] = {
    {"GZWinOutline", 0x4303e5b6},
    {"IGZWinOutline", 0xe303e5ae},
    {"colordark", 0x1100},
    {"colorlight", 0x1101},
    {"node", 0x1102},
};

const VocabEntry kTextTickerVocab[] = {
    {"GZWinTextTicker", 0xe32f0b31},
    {"IGZWinTextTicker", 0x32f0b3c},
    {"speed", 0x1200},
    {"frequency", 0x1201},
    {"message", 0x1202},
};

const VocabEntry kFileBrowserVocab[] = {
    {"GZWinFileBrowser", 0x1aa52ea4},
    {"IGZWinFileBrowser", 0x3aa52e64},
    {"defaultpath", 0x1301},
    {"backdir", 0x1302},
    {"dir", 0x1303},
    {"file", 0x1304},
    {"defaultselectiontype", 0x1305},
    {"defaultselectionname", 0x1306},
    {"bynameascending", 0x1308},
    {"bynamedescending", 0x1309},
    {"byextensionascending", 0x130a},
    {"byextensiondescending", 0x130b},
    {"filesortprimary", 0x130c},
    {"filesortsecondary", 0x130d},
    {"topascending", 0x130e},
    {"topdescending", 0x130f},
    {"bottomascending", 0x1310},
    {"bottomdescending", 0x1311},
    {"directorysort", 0x1312},
    {"icon", 0x1313},
    {"name", 0x1314},
    {"extension", 0x1315},
    {"empty", 0x1316},
    {"unused", 0x1317},
    {"default", 0x1318},
    {"column1data", 0x1319},
    {"column2data", 0x131a},
    {"column3data", 0x131b},
    {"filenamefont", 0x131c},
    {"filenamealignment", 0x1323},
    {"fileextensionfont", 0x1324},
    {"fileextensionalignment", 0x132b},
    {"drivenamefont", 0x132c},
    {"drivenamealignment", 0x1333},
    {"iconcolumnwidth", 0x1334},
    {"extensioncolumnwidth", 0x1335},
    {"rowheight", 0x1336},
    {"linegridenabled", 0x1337},
    {"linegridcolor", 0x1338},
    {"columngridenabled", 0x1339},
    {"columngridcolor", 0x133a},
    {"backdirtext", 0x133d},
    {"drivelistsize", 0x133e},
    {"backgroundcolor", 0x133f},
    {"backgroundimage", 0x1340},
    {"backgroundtransparent", 0x1341},
    {"filter", 0x1342},
};

const VocabEntry kTreeViewVocab[] = {
    {"GZWinTreeView", 0x3ae8bae1},
    {"IGZWinTreeView", 0x3ae8bad2},
    {"displayroot", 0x1401},
    {"displaynodeicons", 0x1402},
    {"displaytreelines", 0x1403},
    {"multiselect", 0x1404},
    {"icongutter", 0x1405},
    {"minmaxboxsize", 0x1406},
    {"roottext", 0x1407},
    {"rooticon", 0x1408},
    {"rooticonselected", 0x1409},
    {"rooticonexpanded", 0x140a},
    {"rootcolorforeground", 0x140c},
    {"rootcolorbackground", 0x140d},
    {"rootfont", 0x1410},
    {"rootexpanded", 0x1411},
    {"rootselected", 0x1412},
};

const VocabEntry kFoldersVocab[] = {
    {"GZWinFolders", 0x78e8055},
    {"IGZWinFolders", 0xa13351b8},
};

const VocabEntry kGridVocab[] = {
    {"GZWinGrid", 0xdaa6b9bf},
    {"IGZWinGrid", 0xdaa6b9be},
    {"fontcolor", 0x1601},
    {"textalign", 0x1602},
    {"textwrapping", 0x1603},
    {"olinecolor", 0x1604},
    {"selrule", 0x1605},
    {"maxselcount", 0x1606},
    {"dcolwidth", 0x1607},
    {"drowheight", 0x1608},
    {"enablehdr", 0x1609},
    {"colhdrsz", 0x160a},
    {"rowhdrsz", 0x160b},
    {"fnone", 0x160c},
    {"fedit", 0x160d},
    {"fhscroll", 0x160e},
    {"fvscroll", 0x160f},
    {"fcolheading", 0x1610},
    {"frowheading", 0x1611},
    {"fcolgrid", 0x1612},
    {"frowgrid", 0x1613},
    {"finsmode", 0x1614},
    {"fopqbkgnd", 0x1615},
    {"fdoutline", 0x1616},
    {"fdgridoutline", 0x1617},
    {"fcellwrap", 0x1618},
    {"fallownosel", 0x1619},
    {"ffixcolcnt", 0x161a},
    {"ffixrowcnt", 0x161b},
    {"fdpastlastcol", 0x161c},
    {"fdpastlastrow", 0x161d},
    {"fcelloverlap", 0x161e},
    {"fdrpdnmenu", 0x161f},
    {"fhlcelldata", 0x1620},
    {"fdefault", 0x1621},
    {"fall", 0x1622},
    {"colgridclr", 0x1623},
    {"rowgridclr", 0x1624},
    {"wingridcell", 0x1625},
    {"wingridrow", 0x1627},
    {"wingridcol", 0x1626},
};

const VocabEntry kCustomVocab[] = {
    {"GZWinCustom", 0x478d1e6f},
    {"IGZWinCustom", 0x678d1e84},
};

const VocabEntry kSC3WinGenVocab[] = {
    {"SC3WinGen", 0x4386d516},
    {"GZWinGen", 0x4386d516},
    {"IGZWinGen", 0x5386d516},
    {"userdata", 0x10000},
    {"blttype", 0x10001},
    {"tiled", 0},
    {"edge", 2},
    {"divider", 0x10002},
    {"bluebar", 0x10003},
    {"moveable", 0x10004},
    {"sizeable", 0x10005},
    {"defaultkeys", 0x10006},
    {"closevisible", 0x10007},
    {"gobackvisible", 0x10008},
    {"minmaxvisible", 0x10009},
    {"closedisabled", 0x1000a},
    {"gobackdisabled", 0x1000b},
    {"minmaxdisabled", 0x1000c},
    {"titlebar", 0x1000d},
    {"paint", 0x1000e},
    {"sidebar", 0x1000f},
};

IVocabularyServices* gVocabularyServices = nullptr;
IScriptControlHost* gScriptControlHost = nullptr;

IVocabulary* LocalVocabulary() {
    IVocabularyServices* services = gVocabularyServices;
    return services ? services->Cached() : nullptr;
}

template <std::size_t N>
void RegisterVocabulary(const VocabEntry (&entries)[N]) {
    IVocabulary* vocab = LocalVocabulary();
    if (!vocab) return;
    for (const VocabEntry& entry : entries) vocab->AddTranslation(cRZString(entry.name), entry.value);
}

// The original registers one (clsid, iid) pair and one (iid, interpreter) pair per control class.
void RegisterControl(std::uint32_t clsid, std::uint32_t iid, cGZLegacyControl::InterpretFn interpret) {
    cGZLegacyControl::Tables().clsidToIid[clsid] = iid;
    cGZLegacyControl::Tables().interpret[iid] = interpret;
}

// The four display-resolution resource instances the common interpreter turns into a "WxH" caption
// (resInstances @ 0x100a24030, group 0xa554ae8).
constexpr std::uint32_t kResolutionInstances[4] = {0xca7e17d8u, 0xca7e17d9u, 0xca7e17dau, 0xca7e17dbu};
constexpr std::uint32_t kResolutionGroup = 0xa554ae8u;

// Script keyword ids the common interpreter reads (see the vocabulary table).
enum : std::uint32_t {
    kPropClsid = 0xffffffffu, kPropIid = 0xfffffffeu, kPropId = 0x100, kPropArea = 0x101, kPropPos = 0x102,
    kPropSize = 0x103, kPropFillColor = 0x104, kPropCaption = 0x105, kPropCaptionRes = 0x106,
    kPropTransparent = 0x107, kPropComments = 0x108, kPropFont = 0xf000, kPropStyle = 0xf005,
    kPropCaptionResParam = 0xb31771a, kPropResourceKey = 0x3aca6906,
};

constexpr std::uint32_t kResourceTypeString = 0x2026960bu;

}  // namespace

IVocabularyServices* VocabularyServices() { return gVocabularyServices; }
void SetVocabularyServices(IVocabularyServices* services) { gVocabularyServices = services; }
IScriptControlHost* ScriptControlHost() { return gScriptControlHost; }
void SetScriptControlHost(IScriptControlHost* host) { gScriptControlHost = host; }

// ---------------------------------------------------------------------------------------------------------
// Property helpers (same translation unit as the class in the original; addresses in LegacyControl.h).
// ---------------------------------------------------------------------------------------------------------
bool GetPropertyUint32(const tScriptPropertyArray& parameters, std::uint32_t id, std::uint32_t& out) {
    for (const tScriptProperty& property : parameters) {
        if (property.id != id || !property.value) continue;
        if (property.value->GetType() != cIGZVariant::kTypeUint32) continue;
        out = property.value->GetValUint32();
        return true;
    }
    return false;
}

bool GetPropertyString(const tScriptPropertyArray& parameters, std::uint32_t id, cRZString& out) {
    out = cRZString();   // the original erases the string first
    for (const tScriptProperty& property : parameters) {
        if (property.id != id || !property.value) continue;
        if (property.value->GetType() != (cIGZVariant::kTypeRZChar | cIGZVariant::kTypeArrayFlag)) continue;
        return property.value->GetValString(out);   // variant vslot 0x1d0
    }
    return false;
}

bool GetPropertyCoords(const tScriptPropertyArray& parameters, std::uint32_t id, int& x, int& y) {
    cRZString text;
    if (!GetPropertyString(parameters, id, text)) return false;
    return std::sscanf(text.ToChar(), "(%d,%d)", &x, &y) == 2;
}

bool GetPropertyColor(const tScriptPropertyArray& parameters, std::uint32_t id, std::uint8_t& r,
                      std::uint8_t& g, std::uint8_t& b) {
    cRZString text;
    if (!GetPropertyString(parameters, id, text)) return false;
    int red = 0, green = 0, blue = 0;
    if (std::sscanf(text.ToChar(), "(%d,%d,%d)", &red, &green, &blue) != 3) return false;
    r = static_cast<std::uint8_t>(red);
    g = static_cast<std::uint8_t>(green);
    b = static_cast<std::uint8_t>(blue);
    return true;
}

bool GetPropertySint32(const tScriptPropertyArray& parameters, std::uint32_t id, int& out) {
    // Quirk: the original accepts only a uint32-typed property (variant type 6) and then reads the same storage
    // through AsSint32 ([77], vs 0x268) - see 0x100540829.
    for (const tScriptProperty& property : parameters) {
        if (property.id != id || !property.value) continue;
        if (property.value->GetType() != cIGZVariant::kTypeUint32) continue;
        out = property.value->AsSint32();
        return true;
    }
    return false;
}

bool GetPropertyRect(const tScriptPropertyArray& parameters, std::uint32_t id, cRZRect& out) {
    cRZString text;
    if (!GetPropertyString(parameters, id, text)) return false;
    return std::sscanf(text.ToChar(), "(%d,%d,%d,%d)", &out.left, &out.top, &out.right, &out.bottom) == 4;
}

// A style named in a script ("font=GenBodyMedium"): its style id through the colour manager's font mapping (WCM vslot 0x130,
// the host's FontStyleFromName); 0 when the name is unknown.
std::uint32_t NamedFontStyle(const cIGZString& name) {
    IScriptControlHost* host = gScriptControlHost;
    return host ? host->FontStyleFromName(name) : 0;
}

void SetGZWinFont(cIGZWin* window, std::uint32_t styleId, std::uint32_t index) {
    if (!window) return;
    // The font parameters start at 0xfaa3be85 (a negative id); index 0 is the control's own font.
    window->SetParam(0xfaa3be85u + index, new cRZVariant(styleId));   // setparam takes the reference
}

void SetGZWinFont(cIGZWin* window, const cIGZString& name, std::uint32_t index) {
    if (!window) return;
    window->SetParam(0xfaa3be85u + index, new cRZVariant(name));
}

void SetResourceKeyParam(cIGZWin* window, std::uint32_t key, const cGZPersistResourceKey& value) {
    if (!window) return;
    window->SetParam(key, new cRZVariant(&value, sizeof(value), 0x8000));
}

void SetGZWinImage(cIGZWin* window, std::uint32_t group, std::uint32_t instance, std::uint32_t index) {
    if (!window) return;
    std::uint32_t blob[2] = {group, instance};
    window->SetParam(0xdaa3b860u + index, new cRZVariant(blob, sizeof(blob), 0x8000));
}

bool LoadStringA(std::uint32_t group, std::uint32_t instance, cRZString& out) {
    IScriptControlHost* host = gScriptControlHost;
    if (!host) return false;
    return host->LoadScriptString(group, instance, out);
}

cIGZBuffer* LoadBuffer(std::uint32_t group, std::uint32_t instance) {
    IScriptControlHost* host = gScriptControlHost;
    if (!host) return nullptr;
    return host->LoadScriptBuffer(group, instance);
}

// ---------------------------------------------------------------------------------------------------------
// cGZLegacyControl
// ---------------------------------------------------------------------------------------------------------
cGZLegacyControl::Registries& cGZLegacyControl::Tables() {
    static Registries tables;
    return tables;
}

cGZLegacyControl::cGZLegacyControl() {
    // The vocabulary is process-wide (spLocalVocab). The first control creates it and registers every keyword
    // table once; later controls only take a reference (0x100530be0: `call *0x8(%rax)` then return).
    IVocabularyServices* services = gVocabularyServices;
    if (!services) return;
    if (services->Cached()) {
        services->AddRefCached();
        return;
    }
    if (!services->CreateCached()) return;
    ControlVocabularyGZWinCommon();
    ControlVocabularyGZWinFlatRect();
    ControlVocabularyGZWinText();
    ControlVocabularyGZWinTextEdit();
    ControlVocabularyGZWinListBox();
    ControlVocabularyGZWinCombo();
    ControlVocabularyGZWinBtn();
    ControlVocabularyGZWinBMP();
    ControlVocabularyGZWinSlider();
    ControlVocabularyGZWinScrollbar();
    ControlVocabularyGZWinLineInput();
    ControlVocabularyGZWinOptGrp();
    ControlVocabularyGZWinSpinner();
    ControlVocabularyGZWinOutline();
    ControlVocabularyGZWinTextTicker();
    ControlVocabularyGZWinFileBrowser();
    ControlVocabularyGZWinTreeView();
    ControlVocabularyGZWinFolders();
    ControlVocabularyGZWinGrid();
    ControlVocabularyGZWinCustom();
    ControlVocabularySC3WinGen();
}

cGZLegacyControl::~cGZLegacyControl() {
    if (control_) {
        if (ownControl_) {
            cIGZWin* parent = control_->GetParentWin();
            if (parent) parent->ChildRemove(control_);
            control_->Shutdown();
        }
        control_->Release();
        control_ = nullptr;
    }
    if (parentControl_) {
        parentControl_->Release();
        parentControl_ = nullptr;
    }
    if (IVocabularyServices* services = gVocabularyServices) {
        if (services->Cached()) services->ReleaseCached();   // clear the cache when the count reaches 0
    }
    if (winProc_) {
        winProc_->Release();
        winProc_ = nullptr;
    }
}

bool cGZLegacyControl::QueryInterface(std::uint32_t iid, void** out) {
    // The original compares against the three iids in this order (0x10053cd8c); iid 1 is cIGZUnknown.
    void* result = nullptr;
    if (iid == kGZIID_cIGZUnknown || iid == 0x1a356083u) {
        result = static_cast<cIGZScriptableControl*>(this);
    } else if (iid == 0x5a356159u) {
        result = static_cast<cIGZControl*>(this);
    } else if (iid == 0x9a4527cbu) {
        result = static_cast<cIGZLegacyControl*>(this);
    } else {
        return false;
    }
    *out = result;
    AddRef();
    return true;
}

std::uint32_t cGZLegacyControl::AddRef() { return ++refCount_; }

std::uint32_t cGZLegacyControl::Release() {
    // Release deletes through the deleting-destructor slot (0x58 of the primary vtable) at count 1 *or 0*.
    if (refCount_ > 1) return --refCount_;
    delete this;
    return 0;
}

// ---------------------------------------------------------------------------------------------------------
// Vocabulary bootstrap. Each function registers its keywords and, for a concrete control, its class pair.
// ---------------------------------------------------------------------------------------------------------
void cGZLegacyControl::ControlVocabularyGZWinCommon() { RegisterVocabulary(kCommonVocab); }

void cGZLegacyControl::ControlVocabularyGZWinFlatRect() {
    RegisterVocabulary(kFlatRectVocab);
    RegisterControl(0xc2afa76eu, 0xc2afa76fu, &InterpretPropertiesGZWinFlatRect);
}

void cGZLegacyControl::ControlVocabularyGZWinText() {
    RegisterVocabulary(kTextVocab);
    RegisterControl(0x592u, 0x212cdc1fu, &InterpretPropertiesGZWinText);
}

void cGZLegacyControl::ControlVocabularyGZWinTextEdit() {
    RegisterVocabulary(kTextEditVocab);
    RegisterControl(0x231a1c58u, 0x231a1c57u, &InterpretPropertiesGZWinTextEdit);
}

void cGZLegacyControl::ControlVocabularyGZWinListBox() {
    RegisterVocabulary(kListBoxVocab);
    RegisterControl(0x598u, 0x4132242bu, &InterpretPropertiesGZWinListBox);
}

void cGZLegacyControl::ControlVocabularyGZWinCombo() {
    RegisterVocabulary(kComboVocab);
    RegisterControl(0x59bu, 0x412ce496u, &InterpretPropertiesGZWinCombo);
}

void cGZLegacyControl::ControlVocabularyGZWinBtn() {
    RegisterVocabulary(kBtnVocab);
    RegisterControl(0x22ecfc47u, 0x8810u, &InterpretPropertiesGZWinBtn);
}

void cGZLegacyControl::ControlVocabularyGZWinBMP() {
    RegisterVocabulary(kBMPVocab);
    RegisterControl(0x82fe68c4u, 0xc12cea13u, &InterpretPropertiesGZWinBMP);
}

void cGZLegacyControl::ControlVocabularyGZWinSlider() {
    RegisterVocabulary(kSliderVocab);
    RegisterControl(0x21325208u, 0x21325207u, &InterpretPropertiesGZWinSlider);
}

void cGZLegacyControl::ControlVocabularyGZWinScrollbar() {
    RegisterVocabulary(kScrollbarVocab);
    RegisterControl(0x61325a2eu, 0x61325a2du, &InterpretPropertiesGZWinScrollbar);
}

void cGZLegacyControl::ControlVocabularyGZWinLineInput() {
    RegisterVocabulary(kLineInputVocab);
    RegisterControl(0x21335c5au, 0x21335c59u, &InterpretPropertiesGZWinLineInput);
}

void cGZLegacyControl::ControlVocabularyGZWinOptGrp() {
    RegisterVocabulary(kOptGrpVocab);
    RegisterControl(0xa1336cc1u, 0xa1336cc0u, &InterpretPropertiesGZWinOptGrp);
}

void cGZLegacyControl::ControlVocabularyGZWinSpinner() {
    RegisterVocabulary(kSpinnerVocab);
    RegisterControl(0x612ce0c4u, 0x612ce0c3u, &InterpretPropertiesGZWinSpinner);
}

void cGZLegacyControl::ControlVocabularyGZWinOutline() {
    RegisterVocabulary(kOutlineVocab);
    RegisterControl(0x4303e5b6u, 0xe303e5aeu, &InterpretPropertiesGZWinOutline);
}

void cGZLegacyControl::ControlVocabularyGZWinTextTicker() {
    RegisterVocabulary(kTextTickerVocab);
    RegisterControl(0xe32f0b31u, 0x32f0b3cu, &InterpretPropertiesGZWinTextTicker);
}

void cGZLegacyControl::ControlVocabularyGZWinFileBrowser() {
    RegisterVocabulary(kFileBrowserVocab);
    RegisterControl(0x1aa52ea4u, 0x3aa52e64u, &InterpretPropertiesGZWinFileBrowser);
}

void cGZLegacyControl::ControlVocabularyGZWinTreeView() {
    RegisterVocabulary(kTreeViewVocab);
    RegisterControl(0x3ae8bae1u, 0x3ae8bad2u, &InterpretPropertiesGZWinTreeView);
}

void cGZLegacyControl::ControlVocabularyGZWinFolders() {
    RegisterVocabulary(kFoldersVocab);
    RegisterControl(0x78e8055u, 0xa13351b8u, &InterpretPropertiesGZWinFolders);
}

void cGZLegacyControl::ControlVocabularyGZWinGrid() {
    RegisterVocabulary(kGridVocab);
    RegisterControl(0xdaa6b9bfu, 0xdaa6b9beu, &InterpretPropertiesGZWinGrid);
}

void cGZLegacyControl::ControlVocabularyGZWinCustom() {
    RegisterVocabulary(kCustomVocab);
    RegisterControl(0x478d1e6fu, 0x678d1e84u, &InterpretPropertiesGZWinCustom);
}

void cGZLegacyControl::ControlVocabularySC3WinGen() {
    RegisterVocabulary(kSC3WinGenVocab);
    RegisterControl(0x4386d516u, 0x5386d516u, &InterpretPropertiesSC3WinGen);
}

// ---------------------------------------------------------------------------------------------------------
// The adapter proper
// ---------------------------------------------------------------------------------------------------------
bool cGZLegacyControl::SetParameters(tScriptPropertyArray& parameters) {
    // The original returns 0 on every path (0x10053bbd2 `xorl %eax,%eax`), even when the control was built:
    // the caller only relies on GetControl() afterwards. Kept.
    std::uint32_t clsid = 0;
    if (!GetPropertyUint32(parameters, kPropClsid, clsid) || clsid == 0) return false;
    std::uint32_t iid = 0;
    if (!GetPropertyUint32(parameters, kPropIid, iid)) {
        // No explicit view interface: take the one registered for this class (0x10053b9d8 looks up
        // sCLSIDtoIIDMap by class id and copies the mapped value).
        Registries& tables = Tables();
        auto it = tables.clsidToIid.find(clsid);
        if (it != tables.clsidToIid.end()) iid = it->second;
    }

    control_ = nullptr;   // quirk: a previous control is dropped here without a Release
    IScriptControlHost* host = gScriptControlHost;
    cIGZWin* control = host ? host->CreateControl(clsid, kGZIID_cIGZWin) : nullptr;
    if (!control) {
        // Fall back to a class registered under this view interface (0x10053ba80 walks sCLSIDtoIIDMap in key
        // order and creates the first class whose mapped value equals the requested iid).
        Registries& tables = Tables();
        for (const auto& entry : tables.clsidToIid) {
            if (entry.second != iid) continue;
            control = host ? host->CreateControl(entry.first, kGZIID_cIGZWin) : nullptr;
            break;
        }
        if (!control && host) control = host->CreateControl(0x478d1e6fu, kGZIID_cIGZWin);   // cGZWinCustom
    }
    if (!control) return false;
    control_ = control;

    control_->SetParam(0xe85eebdeu, new cRZVariant(clsid));
    if (iid != 0) control_->SetParam(0x8d6e462u, new cRZVariant(iid));
    ownControl_ = true;

    InterpretFn interpret = nullptr;
    {
        Registries& tables = Tables();
        auto it = tables.interpret.find(iid);
        if (it != tables.interpret.end()) interpret = it->second;
    }
    if (interpret) interpret(control_, parameters, true);
    InterpretPropertiesGZWinCommon(control_, parameters, true);
    DoExceptions(control_, true);
    control_->Init();

    if (!parentControl_) {
        cIGZWin* main = host ? host->GetMainWindow() : nullptr;
        if (main) main->ChildAdd(control_);
    } else {
        parentControl_->ChildAdd(control_);
        parentControl_->Release();
        parentControl_ = nullptr;
    }
    control_->PullToFront();
    if (interpret) interpret(control_, parameters, false);
    InterpretPropertiesGZWinCommon(control_, parameters, false);
    return false;   // the original always reports failure here
}

void cGZLegacyControl::DoExceptions(cIGZWin* window, bool install) {
    // 0x10053cc5c: a generic window (cIGZWinGen, iid 0x5386d516) is handed the window procedure, and the
    // adapter drops its own reference to it.
    if (!install || !window) return;
    gz::cIGZWinGen* gen = nullptr;
    if (!window->QueryInterface(gz::kGZIID_cIGZWinGen, reinterpret_cast<void**>(&gen))) return;
    if (winProc_) {
        gen->SetWinProc(winProc_);   // IWinGen vslot [7] (+0x38)
        winProc_->Release();
        winProc_ = nullptr;
    }
    gen->Release();
}

IVocabulary* cGZLegacyControl::GetControlVocabulary() { return LocalVocabulary(); }

std::uint32_t cGZLegacyControl::GetID() { return control_ ? control_->GetID() : 0; }

void cGZLegacyControl::SetID(std::uint32_t id) {
    if (control_) control_->SetID(id);
}

void cGZLegacyControl::SetOwnControl(bool own) { ownControl_ = own; }

bool cGZLegacyControl::ImplementInterface(cIGZScriptableControlSet* set, std::uint32_t clsid,
                                          std::uint32_t iid, cIGZVariant* value) {
    (void)set; (void)clsid; (void)iid; (void)value;
    return true;
}

bool cGZLegacyControl::SetParent(cIGZScriptableControl* parent) {
    if (parentControl_) {
        parentControl_->Release();
        parentControl_ = nullptr;
    }
    if (parent) {
        cIGZLegacyControl* legacy = nullptr;   // iid 0x9a4527cb
        if (parent->QueryInterface(0x9a4527cbu, reinterpret_cast<void**>(&legacy))) {
            cIGZWin* window = legacy->GetControl();   // vslot [3] (+0x18), counted reference
            parentControl_ = window;
            if (window) window->AddRef();
            legacy->Release();
        }
    }
    return true;
}

bool cGZLegacyControl::Render(cIGZGDriver* driver) {
    (void)driver;
    return true;
}

bool cGZLegacyControl::SetArea(cRZRect& area) {
    if (!control_) return false;
    control_->SetArea(area);
    return true;
}

bool cGZLegacyControl::GetArea(cRZRect& area) {
    if (!control_) return false;
    control_->GetArea(area);
    return true;
}

bool cGZLegacyControl::ControlEvent(cRZPoint& point, std::uint32_t event) {
    (void)point; (void)event;
    return true;
}

void cGZLegacyControl::ForceControl(cIGZWin* control) {
    if (control_ == control) return;
    if (control) control->AddRef();
    if (control_) control_->Release();
    control_ = control;
}

void cGZLegacyControl::UseWinProc(gz::cIGZWinProc* proc) {
    if (winProc_ == proc) return;
    if (proc) proc->AddRef();
    if (winProc_) winProc_->Release();
    winProc_ = proc;
}

bool cGZLegacyControl::GetScriptFragment(cIGZString** out) {
    cIGZWin* control = GetControl();   // 0x1005522e8 calls its own GetControl through vslot 0x80
    if (!control) return false;
    cRZString* fragment = new cRZString();
    fragment->AddRef();   // the caller owns this reference (the new string starts at 0)
    IScriptControlHost* host = gScriptControlHost;
    if (host) host->UnscriptWindow(control, *fragment);
    *out = fragment;
    return true;
}

// ---------------------------------------------------------------------------------------------------------
// InterpretPropertiesGZWinCommon (0x10053bc8e) — PARTIAL: the seven comment strings (properties 0x108..0x10f)
// are packed into a variant of the (unrecovered) cRZStringArray type 0x100b0ecb8; that path is not written.
// Everything else (flags, id, area/pos/size, fill colour, caption, caption resource and its resolution
// special case, transparent) is here.
// ---------------------------------------------------------------------------------------------------------
void cGZLegacyControl::InterpretPropertiesGZWinCommon(cIGZWin* window, tScriptPropertyArray& parameters,
                                                      bool init) {
    if (!window) return;
    std::uint32_t value = 0;
    if (!init) {
        // Applied after the window is initialised, so these override the defaults set in the first pass.
        if (GetPropertyUint32(parameters, 0xf01b, value)) window->SetFlag(0x2u, value != 0);
        if (GetPropertyUint32(parameters, 0xf01f, value)) window->SetFlag(0x10000u, value != 0);
        if (GetPropertyUint32(parameters, 0xf020, value)) window->SetFlag(0x20000u, value != 0);
        if (GetPropertyUint32(parameters, 0xf021, value)) window->SetFlag(0x40000u, value != 0);
        if (GetPropertyUint32(parameters, 0xf022, value)) window->SetFlag(0x100000u, value != 0);
        return;
    }

    window->SetFlag(0x1u, true);
    window->SetFlag(0x10000u, true);
    window->SetFlag(0x800u, false);
    window->SetFlag(0x20000u, true);
    window->SetFlag(0x40000u, true);
    window->SetFlag(0x4u, false);
    window->SetFlag(0x8000u, true);

    if (GetPropertyUint32(parameters, 0xf01a, value)) window->SetFlag(0x1u, value != 0);       // winflag_visible
    if (GetPropertyUint32(parameters, 0xf01c, value)) window->SetFlag(0x100u, value != 0);     // winflag_moveable
    if (GetPropertyUint32(parameters, 0xf01d, value)) window->SetFlag(0x200u, value != 0);     // winflag_sizeable
    if (GetPropertyUint32(parameters, 0xf01e, value)) window->SetFlag(0x800u, value != 0);     // winflag_sortable
    if (GetPropertyUint32(parameters, 0xf025, value)) window->SetFlag(0x8000u, value != 0);    // winflag_acceptfocus
    if (GetPropertyUint32(parameters, 0xf026, value)) window->SetFlag(0x4u, value != 0);       // winflag_alphablend
    if (GetPropertyUint32(parameters, 0xf023, value)) window->SetFlag(0x80000u, value != 0);   // winflag_mousetrans
    if (GetPropertyUint32(parameters, 0xf024, value)) window->SetFlag(0x200000u, value != 0);  // winflag_ignoremouse

    if (GetPropertyUint32(parameters, kPropId, value)) window->SetID(value);

    cRZString text;
    if (GetPropertyString(parameters, kPropArea, text)) {
        // "area" is a string "(l,t,r,b)" (quirk: the property is a string, not a variant rectangle); only when
        // it is absent are "pos" and "size" used.
        int left = 0, top = 0, right = 0, bottom = 0;
        std::sscanf(text.ToChar(), "( %d, %d, %d, %d)", &left, &top, &right, &bottom);
        cRZRect area{left, top, right, bottom};
        window->SetArea(area);
    } else {
        int x = 0, y = 0;
        if (GetPropertyCoords(parameters, kPropPos, x, y)) window->GZWinMoveTo(x, y);
        if (GetPropertyCoords(parameters, kPropSize, x, y)) window->SetSize(x, y);
    }

    std::uint8_t red = 0, green = 0, blue = 0;
    if (GetPropertyColor(parameters, kPropFillColor, red, green, blue)) window->SetFillColor(red, green, blue);
    if (GetPropertyString(parameters, kPropCaption, text)) window->SetCaption(text);

    std::uint32_t group = 0, instance = 0;
    if (GetPropertyString(parameters, kPropCaptionRes, text)) {
        // "captionres" is "{group,instance}" in hex; the string is loaded as a localised string and the key is
        // remembered as a parameter so the dialog can be re-localised.
        std::sscanf(text.ToChar(), "{%x,%x}", &group, &instance);
        cRZString loaded;
        if (LoadStringA(group, instance, loaded)) window->SetCaption(loaded);
        cGZPersistResourceKey key{kResourceTypeString, group, instance};
        SetResourceKeyParam(window, kPropCaptionResParam, key);
    }

    if (group == kResolutionGroup) {
        // Resolution resources: instance 0x0ab6c441..3 first clear the caption (they are the "custom" entries),
        // then the four native display resolutions are matched (0xca7e17d8..db) and written as "WxH".
        if (instance + 0xf5493bbfu < 3u) window->SetCaption(cRZString());
        for (int i = 0; i < 4; ++i) {
            if (kResolutionInstances[i] != instance) continue;
            int width = 0, height = 0;
            IScriptControlHost* host = gScriptControlHost;
            if (!host) break;
            host->GetNativeResolution(i, width, height);
            char buffer[32] = "";
            if (width > 0 && height > 0) std::snprintf(buffer, sizeof(buffer), "%dx%d", width, height);   // none: no label
            window->SetCaption(cRZString(buffer));
            break;
        }
    }

    {
        // SetParam(0x3aca6906, variant(blob{group, instance})) — the original leaks no reference: the window
        // owns the created variant (SetParam adds a reference to a count-0 object).
        std::uint32_t blob[2] = {group, instance};
        window->SetParam(kPropResourceKey, new cRZVariant(blob, sizeof(blob), 0x8000));
    }

    for (const tScriptProperty& property : parameters) {
        if (property.id == kPropTransparent) {
            window->SetFlag(0x20000u, true);
            break;
        }
    }

    // PARTIAL (see LEGACY-1): the comment strings at 0x108..0x10f go into a cRZStringArray (variant vtbl
    // 0x100b0ecb8) stored at 0xbaca6879; that container type is still to be recovered.
}

// ---------------------------------------------------------------------------------------------------------
// InterpretPropertiesGZWinFlatRect (0x10053d25c): outline style bits + the four edge colours. The style
// property (0xf005) carries one of the vocabulary style ids, ORed into the outline flag byte.
// ---------------------------------------------------------------------------------------------------------
void cGZLegacyControl::InterpretPropertiesGZWinFlatRect(cIGZWin* window, tScriptPropertyArray& parameters,
                                                        bool init) {
    if (!window) return;
    gz::cIGZWinFlatRect* flat = nullptr;
    if (!window->QueryInterface(gz::kGZIID_cIGZWinFlatRect, reinterpret_cast<void**>(&flat))) return;
    if (init) {
        std::uint8_t flags = 0;
        for (const tScriptProperty& property : parameters) {
            if (property.id != kPropStyle || !property.value) continue;
            if (property.value->GetType() != cIGZVariant::kTypeUint32) continue;
            switch (property.value->GetValUint32()) {
                case 0x204: flags |= 0x02; break;   // top
                case 0x205: flags |= 0x08; break;   // bottom
                case 0x206: flags |= 0x0f; break;   // all sides
                case 0x207: flags |= 0x0d; break;   // left + right + bottom
                case 0x208: flags |= 0x0e; break;   // left + top + right
                case 0xf006: flags |= 0x01; break;  // left
                case 0xf007: flags |= 0x04; break;  // right
                case 0xf00b: flags |= 0x20; break;  // sunken
                case 0xf00c: flags |= 0x10; break;  // raised
                case 0xf00e: flags |= 0x40; break;  // no fill
                default: break;
            }
        }
        flat->SetOutlineFlags(flags);   // vslot [9] (+0x48)
        std::uint8_t red = 0, green = 0, blue = 0;
        if (GetPropertyColor(parameters, 0x200, red, green, blue)) flat->SetOutlineColor(0, red, green, blue);
        if (GetPropertyColor(parameters, 0x201, red, green, blue)) flat->SetOutlineColor(1, red, green, blue);
        if (GetPropertyColor(parameters, 0x202, red, green, blue)) flat->SetOutlineColor(2, red, green, blue);
        if (GetPropertyColor(parameters, 0x203, red, green, blue)) flat->SetOutlineColor(3, red, green, blue);
    }
    flat->Release();
}

// ---------------------------------------------------------------------------------------------------------
// InterpretPropertiesGZWinSlider (0x1005405c2): min/max (from "minmaxvalue" "(min,max)", swapped when
// reversed), direction, image resource and, in the post-init pass, the initial value (property 0x40a).
// ---------------------------------------------------------------------------------------------------------
void cGZLegacyControl::InterpretPropertiesGZWinSlider(cIGZWin* window, tScriptPropertyArray& parameters,
                                                      bool init) {
    if (!window) return;
    gz::cIGZWinSlider* slider = nullptr;
    if (!window->QueryInterface(gz::kGZIID_cIGZWinSlider, reinterpret_cast<void**>(&slider))) return;
    if (init) {
        int first = 0, second = 0;
        if (GetPropertyCoords(parameters, 0xc00, first, second)) {
            if (second < first) {
                const int swap = second;
                second = first;
                first = swap;
            }
            slider->SetMaximumValue(second);   // vslot [8] (+0x40)
            slider->SetMinimumValue(first);    // vslot [6] (+0x30)
        }
        std::uint32_t direction = 0;
        if (GetPropertyUint32(parameters, 0xc01, direction)) {
            slider->SetDirection(direction == 0xc03u ? gz::kSliderVertical : gz::kSliderHorizontal);   // +0x50
        }
        cRZString text;
        if (GetPropertyString(parameters, 0xf016, text)) {
            std::uint32_t group = 0, instance = 0;
            if (std::sscanf(text.ToChar(), "{%x,%x}", &group, &instance) == 2) {
                cIGZBuffer* image = LoadBuffer(group, instance);
                if (image) {
                    cRZRect area;
                    if (GetPropertyRect(parameters, 0xf018, area)) {
                        slider->SetImage(image, &area);
                    } else {
                        slider->SetImage(image, nullptr);
                    }
                }
                cGZPersistResourceKey key{0x856ddbacu, group, instance};
                SetResourceKeyParam(window, 0x6b4bcbd4u, key);
                SetGZWinImage(window, group, instance, 0);
                if (image) image->Release();
            }
        }
    } else {
        int value = 0;
        if (GetPropertySint32(parameters, 0x40a, value)) slider->SetValue(value);   // vslot [5] (+0x28)
    }
    slider->Release();
}

// ---------------------------------------------------------------------------------------------------------
// InterpretPropertiesGZWinScrollbar (0x1005408be): min/max, orientation, page/line size, the line and page
// count, the image resource and, post-init, the initial value.
// ---------------------------------------------------------------------------------------------------------
void cGZLegacyControl::InterpretPropertiesGZWinScrollbar(cIGZWin* window, tScriptPropertyArray& parameters,
                                                         bool init) {
    if (!window) return;
    gz::cIGZWinScrollbar* bar = nullptr;
    if (!window->QueryInterface(gz::kGZIID_cIGZWinScrollbar, reinterpret_cast<void**>(&bar))) return;
    if (init) {
        int first = 0, second = 0;
        if (GetPropertyCoords(parameters, 0xc00, first, second)) {
            if (second < first) {
                const int swap = second;
                second = first;
                first = swap;
            }
            bar->SetMinMax(first, second);   // vslot [7] (+0x38)
        }
        std::uint32_t value = 0;
        if (GetPropertyUint32(parameters, 0xc01, value)) {
            bar->SetOrientation(value == 0xc03u ? 2 : 1);   // vslot [9] (+0x48)
        }
        if (GetPropertyUint32(parameters, 0xd00, value)) bar->SetPageSize(static_cast<int>(value));   // +0x58
        if (GetPropertyUint32(parameters, 0xd01, value)) bar->SetLineSize(static_cast<int>(value));   // +0x68
        int lines = 0, pages = 0;
        if (GetPropertyCoords(parameters, 0xd02, lines, pages)) {
            bar->SetLineAndPageCount(static_cast<std::uint32_t>(lines), static_cast<std::uint32_t>(pages));   // +0x78
        }
        cRZString text;
        if (GetPropertyString(parameters, 0xf016, text)) {
            std::uint32_t group = 0, instance = 0;
            if (std::sscanf(text.ToChar(), "{%x,%x}", &group, &instance) == 2) {
                cIGZBuffer* image = LoadBuffer(group, instance);
                if (image) {
                    cRZRect area;
                    if (GetPropertyRect(parameters, 0xf018, area)) {
                        bar->ImageSetup(area, image, 0);                        // vslot [17] (+0x88)
                    } else {
                        bar->ImageSetup(*image->GetBufferArea(), image, 0);     // buffer vslot [13]
                    }
                }
                cGZPersistResourceKey key{0x856ddbacu, group, instance};
                SetResourceKeyParam(window, 0x6b4bcbd4u, key);
                SetGZWinImage(window, group, instance, 0);
                if (image) image->Release();
            }
        }
    } else {
        int value = 0;
        if (GetPropertySint32(parameters, 0x40a, value)) bar->SetValue(value);   // vslot [5] (+0x28)
    }
    bar->Release();
}

// ---------------------------------------------------------------------------------------------------------
// InterpretPropertiesGZWinBMP (0x1005402d0): alpha, the transparency/edge flags, the image resource (which also
// sizes the window to the bitmap) and, post-init, the image rectangle.
// ---------------------------------------------------------------------------------------------------------
void cGZLegacyControl::InterpretPropertiesGZWinBMP(cIGZWin* window, tScriptPropertyArray& parameters,
                                                   bool init) {
    if (!window) return;
    gz::cIGZWinBMP* bmp = nullptr;
    if (!window->QueryInterface(gz::kGZIID_cIGZWinBMP, reinterpret_cast<void**>(&bmp))) return;
    if (init) {
        std::uint32_t alpha = 0;
        if (GetPropertyUint32(parameters, 0xb00, alpha)) {
            if (alpha > 0xffu) alpha = 0xffu;
            bmp->SetAlphaBlending(true, static_cast<float>(alpha) / 255.0f);   // vslot [6] (+0x30)
        }
        std::uint32_t value = 0;
        if (GetPropertyUint32(parameters, 0xf003, value)) bmp->SetWinBMPFlag(1, value != 0);   // vslot [11]
        if (GetPropertyUint32(parameters, 0xb01, value)) bmp->SetWinBMPFlag(2, value != 0);    // transparentbkg
        if (GetPropertyUint32(parameters, 0xb02, value)) bmp->SetWinBMPFlag(8, value != 0);    // edgeimage
        cRZString text;
        if (GetPropertyString(parameters, 0xf016, text)) {
            std::uint32_t group = 0, instance = 0;
            if (std::sscanf(text.ToChar(), "{%x,%x}", &group, &instance) == 2) {
                cIGZBuffer* image = LoadBuffer(group, instance);
                if (image) {
                    bmp->SetBMP(image);                                      // vslot [4] (+0x20)
                    window->SetSize(static_cast<int>(image->Width()),        // buffer vslots 0x58/0x60
                                    static_cast<int>(image->Height()));
                }
                cGZPersistResourceKey key{0x856ddbacu, group, instance};
                SetResourceKeyParam(window, 0xab428c5du, key);
                SetGZWinImage(window, group, instance, 0);
                if (image) image->Release();
            }
        }
    } else {
        cRZRect area;
        if (GetPropertyRect(parameters, 0xf018, area)) {
            bmp->SetImageArea(area);          // vslot [8] (+0x40)
            bmp->SetWinBMPFlag(0x10, true);
        } else {
            bmp->SetWinBMPFlag(0x10, false);
        }
    }
    bmp->Release();
}

// ---------------------------------------------------------------------------------------------------------
// InterpretPropertiesGZWinOutline (0x100542a9a): style -> outline flags, the two outline colours and the shape
// polygon points ("node" strings "(x,y)", up to 32).
//
// WinOutline.h and WinFlatRect.h both declare gz::kOutlineRaised/kOutlineSunken with different values, so this
// translation unit (which needs the flat-rect interface for the interpreter above) cannot include both. The
// outline view calls are therefore made through the recovered vtable slots, exactly the ids the binary uses:
// [2] Release, [3] SetFlags, [5] SetColor, [13] SetShapePoly.
// ---------------------------------------------------------------------------------------------------------
namespace {
constexpr std::uint32_t kOutlineIid = 0xe303e5aeu;
using OutlineReleaseFn = std::uint32_t (*)(void*);
using OutlineSetFlagsFn = void (*)(void*, std::uint32_t);
using OutlineSetColorFn = void (*)(void*, int, std::uint8_t, std::uint8_t, std::uint8_t);
using OutlineSetShapePolyFn = bool (*)(void*, int, const cRZPoint*, bool, bool);

template <typename Fn>
Fn OutlineSlot(void* object, unsigned slot) {
    return reinterpret_cast<Fn>((*reinterpret_cast<void***>(object))[slot]);
}
}  // namespace

void cGZLegacyControl::InterpretPropertiesGZWinOutline(cIGZWin* window, tScriptPropertyArray& parameters,
                                                       bool init) {
    if (!window) return;
    void* outline = nullptr;
    if (!window->QueryInterface(kOutlineIid, &outline) || !outline) return;
    if (init) {
        std::uint32_t flags = 0;
        for (const tScriptProperty& property : parameters) {
            if (property.id != kPropStyle || !property.value) continue;
            if (property.value->GetType() != cIGZVariant::kTypeUint32) continue;
            switch (property.value->GetValUint32()) {
                case 0xf009: flags |= 0x01; break;   // flat
                case 0xf00b: flags |= 0x02; break;   // sunken
                case 0xf00c: flags |= 0x04; break;   // raised
                case 0xf00d: flags |= 0x20; break;   // filled
                default: break;
            }
        }
        if (flags != 0) OutlineSlot<OutlineSetFlagsFn>(outline, 3)(outline, flags);
        std::uint8_t red = 0, green = 0, blue = 0;
        if (GetPropertyColor(parameters, 0x1100, red, green, blue)) {
            OutlineSlot<OutlineSetColorFn>(outline, 5)(outline, 1, red, green, blue);
        }
        if (GetPropertyColor(parameters, 0x1101, red, green, blue)) {
            OutlineSlot<OutlineSetColorFn>(outline, 5)(outline, 2, red, green, blue);
        }
        cRZPoint points[32];
        std::uint32_t count = 0;
        for (const tScriptProperty& property : parameters) {
            if (property.id != 0x1102 || !property.value) continue;
            if (property.value->GetType() != (cIGZVariant::kTypeRZChar | cIGZVariant::kTypeArrayFlag)) continue;
            cRZString text;
            if (!property.value->GetValString(text)) continue;
            int x = 0, y = 0;
            std::sscanf(text.ToChar(), "(%d,%d)", &x, &y);
            points[count].x = x;
            points[count].y = y;
            ++count;
            if (count >= 32) break;
        }
        if (count != 0) {
            OutlineSlot<OutlineSetShapePolyFn>(outline, 13)(outline, static_cast<int>(count), points, true, true);
        }
    }
    OutlineSlot<OutlineReleaseFn>(outline, 2)(outline);
}

// ---------------------------------------------------------------------------------------------------------
// InterpretPropertiesGZWinCustom (0x10054a2fc): empty in the original (a custom control has no scripted
// drawing properties; the common ones are enough).
// ---------------------------------------------------------------------------------------------------------
void cGZLegacyControl::InterpretPropertiesGZWinCustom(cIGZWin* window, tScriptPropertyArray& parameters,
                                                      bool init) {
    (void)window; (void)parameters; (void)init;
}

// InterpretPropertiesSC3WinGen (0x1005432b8): the generic window's script keywords. The decompiler drops the
// QueryInterface out-parameter of its first call (every later `local_40->vslot` is the cIGZWinGen), so the
// mapping below was read from the vslot offsets: SetWinGenFlag is [6] (+0x30), SetUserData(value) [20] (+0xa0),
// SetBackgroundBitmap(type, image) [12] (+0x60), SetGutters(x, y) [25] (+0xc8), SetWinProc [7] (+0x38),
// AddDividerH [30] (+0xf0), AddColorBar [29] (+0xe8). Init pass: the flag keywords (the title bar, "fill", "paint"
// and the dialog bit are inverted: the keyword says "has", the flag says "has not"), user data, the background image
// {group,instance} with its blit type, the gutters, and a dummy window procedure. Second pass: "divider" and
// "bluebar" strings define the dividers and colour bars.
void cGZLegacyControl::InterpretPropertiesSC3WinGen(cIGZWin* window, tScriptPropertyArray& parameters, bool init) {
    if (!window) return;
    gz::cIGZWinGen* gen = nullptr;
    if (!window->QueryInterface(gz::kGZIID_cIGZWinGen, reinterpret_cast<void**>(&gen))) return;
    if (init) {
        static constexpr struct { std::uint32_t property; int flag; bool inverted; } kFlags[] = {
            {0x10004, 0x1, false},     // moveable
            {0x10005, 0x2, false},     // sizeable
            {0x10006, 0x4, false},     // defaultkeys
            {0x10007, 0x10, false},    // closevisible
            {0x10008, 0x20, false},    // gobackvisible
            {0x10009, 0x40, false},    // minmaxvisible
            {0x1000a, 0x100, false},   // closedisabled
            {0x1000b, 0x200, false},   // gobackdisabled
            {0x1000c, 0x400, false},   // minmaxdisabled
            {0x1000d, 0x1000, true},   // titlebar
            {0xf00d, 0x2000, true},    // fill
            {0xf019, 0x4000, true},    // outline
            {0x1000e, 0x8000, true},   // paint
            {0x1000f, 0x10000, false}, // sidebar
        };
        std::uint32_t value = 0;
        for (const auto& entry : kFlags) {
            if (GetPropertyUint32(parameters, entry.property, value)) gen->SetWinGenFlag(entry.flag, entry.inverted ? value == 0 : value != 0);
        }
        std::uint32_t userData = 0;
        if (GetPropertyUint32(parameters, 0x10000, userData)) gen->SetUserData(userData);   // vslot [20]
        cRZString text;
        if (GetPropertyString(parameters, 0xf016, text)) {
            std::uint32_t group = 0, instance = 0;
            if (std::sscanf(text.ToChar(), "{%x,%x}", &group, &instance) == 2) {
                cIGZBuffer* image = LoadBuffer(group, instance);
                std::uint32_t blit = 0;
                GetPropertyUint32(parameters, 0x10001, blit);
                if (image) {
                    // tiled (0) -> 0, edge (2) -> 2, anything else -> 1 (stretched)
                    const gz::BGBltType type = blit == 0 ? gz::kBGTiled : (blit == 2 ? gz::kBGEdges : gz::kBGStretched);
                    gen->SetBackgroundBitmap(type, image);   // vslot [12]
                }
                cGZPersistResourceKey key{0x856ddbacu, group, instance};
                SetResourceKeyParam(window, 0xab428c5du, key);
                SetGZWinImage(window, group, instance, 0);
                if (image) image->Release();
            }
        }
        int gx = 0, gy = 0;
        if (GetPropertyCoords(parameters, 0xf004, gx, gy)) gen->SetGutters(static_cast<std::uint8_t>(gx), static_cast<std::uint8_t>(gy));   // vslot [25]
        gen->SetWinProc(new sc4::cGZDummyWinProc());   // vslot [7]: takes its own reference on the new object
    } else {
        for (const tScriptProperty& property : parameters) {
            if (!property.value || (property.id != 0x10002 && property.id != 0x10003)) continue;
            if (property.value->GetType() != (cIGZVariant::kTypeRZChar | cIGZVariant::kTypeArrayFlag)) continue;
            cRZString text;
            property.value->GetValString(text);
            if (property.id == 0x10002) {   // divider "(id,y)"
                unsigned id = 0; int y = 0;
                if (std::sscanf(text.ToChar(), "(%u,%d)", &id, &y) == 2) gen->AddDividerH(int(id), y);   // vslot [30]
            } else {   // bluebar "(id,l,t,r,b)"
                unsigned id = 0; int l = 0, t = 0, r = 0, b = 0;
                if (std::sscanf(text.ToChar(), "(%u,%d,%d,%d,%d)", &id, &l, &t, &r, &b) == 5) {
                    gen->AddColorBar(int(id), 0, 0, cRZRect{l, t, r, b});   // vslot [29]
                }
            }
        }
    }
    gen->Release();
}

// InterpretPropertiesGZWinText (0x10053d4f0): the text control's keywords. As in the generic window's interpreter the
// decompiler drops the QueryInterface out-parameter (the cIGZWinText, iid 0x212cdc1f); the slots read off the vtable
// offsets are SetWinTextFlag [7] (+0x38), FitWindowToText [5] (+0x28), SetFontStyle [19] (+0x98), SetTextColor(index,
// native) [11] (+0x58), SetGutters [23] (+0xb8), SetTextOffsets [25] (+0xc8), SetAlignment [21] (+0xa8).
// Init pass: three flag properties (0xf003 -> flag 1, 0x300 -> 2, 0x301 -> 4; a 0x301 of 0 also makes the window's
// private buffer transparent), the font (a style id, else a name), the text colour (0xf002) and shadow colour
// (0xf001) converted by the graphics system, the gutters (default 0,0) and the text offsets. Second pass: the
// alignment (centre -> 4; any other keyword value is passed on as it is, the left/right ids themselves - kept).
// Both passes end with the autofit parameter 0xdacf8f9c on the window: true (and a FitWindowToText(false, false))
// when the script has property 0x304, false otherwise.
void cGZLegacyControl::InterpretPropertiesGZWinText(cIGZWin* window, tScriptPropertyArray& parameters, bool init) {
    if (!window) return;
    gz::cIGZWinText* text = nullptr;
    if (!window->QueryInterface(gz::kGZIID_cIGZWinText, reinterpret_cast<void**>(&text))) return;
    if (init) {
        std::uint32_t value = 0;
        if (GetPropertyUint32(parameters, 0xf003, value)) text->SetWinTextFlag(1, value != 0);
        if (GetPropertyUint32(parameters, 0x300, value)) text->SetWinTextFlag(2, value != 0);
        if (GetPropertyUint32(parameters, 0x301, value)) {
            text->SetWinTextFlag(4, value != 0);
            if (value == 0) window->SetFlag(kWinPrivateBufferTransparent, true);   // vslot 0x220 (0x20000, 1)
        }
        std::uint32_t styleId = 0;
        if (GetPropertyUint32(parameters, 0xf000, styleId)) {
            text->SetFontStyle(styleId);
            SetGZWinFont(window, styleId, 0);
        } else {
            cRZString name;
            if (GetPropertyString(parameters, 0xf000, name)) {
                if (const std::uint32_t named = NamedFontStyle(name)) text->SetFontStyle(named);
                SetGZWinFont(window, name, 0);
            }
        }
        std::uint8_t r = 0, g = 0, b = 0;
        if (GetPropertyColor(parameters, 0xf002, r, g, b)) {
            text->SetTextColor(gz::cIGZWinText::kColorText, WinGraphics() ? WinGraphics()->MakeNativeColor(r, g, b) : 0);
        }
        if (GetPropertyColor(parameters, 0xf001, r, g, b)) {
            text->SetTextColor(gz::cIGZWinText::kColorShadow, WinGraphics() ? WinGraphics()->MakeNativeColor(r, g, b) : 0);
        }
        int gx = 0, gy = 0;
        if (GetPropertyCoords(parameters, 0xf004, gx, gy)) text->SetGutters(static_cast<std::int8_t>(gx), static_cast<std::int8_t>(gy));
        else text->SetGutters(0, 0);
        int ox = 0, oy = 0;
        if (GetPropertyCoords(parameters, 0x305, ox, oy)) text->SetTextOffsets(static_cast<std::int8_t>(ox), static_cast<std::int8_t>(oy));
    } else {
        std::uint32_t align = 0;
        if (GetPropertyUint32(parameters, 0xf015, align)) text->SetAlignment(align == 0xf008u ? 4u : align);
    }
    bool autofit = false;
    for (const tScriptProperty& property : parameters) {
        if (property.id == 0x304) { autofit = true; break; }
    }
    auto* autofitParam = new cRZVariant();
    autofitParam->SetValBool(autofit);
    window->SetParam(0xdacf8f9cu, autofitParam);
    if (autofit) text->FitWindowToText(false, false);
    text->Release();
}

// InterpretPropertiesGZWinBtn (0x10053f238): the push button's keywords (cIGZWinBtn, iid 0x8810; the decompiler
// drops the QueryInterface out-parameter, so the slots were read off the vtable offsets: SetBtnFlag [5] +0x28,
// SetStyle(style, image, area) [12] +0x60, SetFont [19] +0x98, SetFontStyle [20] +0xa0, SetFontColor [22] +0xb0,
// SetCaptionStyle [27] +0xd8, SetGutters [29] +0xe8 / [31] +0xf8, SetTipText [35] +0x118, SetTipPlacementType [38]
// +0x130, SetTipPlacementOffsets [40] +0x140, SetSoundID [45] +0x168). Second pass: only the 0xa02 flag again.
// Init pass: flags 1, 2, 4, 0x20, 0x40, 0x80, 0x100 (plain) and 0x10, 0x200, 0x400 (inverted: the keyword says
// "has", the flag says "has not"); the style keyword -> 0..4 (unlisted keywords stay 0, 0xa00 means 3); the font
// (a style id, else a name, else the font named "main9"); six colours; the gutters ("(x,y)" or "(l,t,r,b)", decided by
// counting commas); the image {group,instance} and its resource keys; the caption alignment; the tool tip text and
// its resource key; the tip offsets and placement (default 0x1000000); two sound ids with their {group,instance} params.
// Kept: the tip text of resource {6a231eaa, 0a6cf0d3} is replaced by a CFBundle localisation in the original
// (macOS only); it is not reproduced here (see LEGACY-2).
void cGZLegacyControl::InterpretPropertiesGZWinBtn(cIGZWin* window, tScriptPropertyArray& parameters, bool init) {
    if (!window) return;
    gz::cIGZWinBtn* btn = nullptr;
    if (!window->QueryInterface(gz::kGZIID_cIGZWinBtn, reinterpret_cast<void**>(&btn))) return;
    std::uint32_t value = 0;
    if (!init) {
        if (GetPropertyUint32(parameters, 0xa02, value)) btn->SetBtnFlag(4, value != 0);
        btn->Release();
        return;
    }
    static constexpr struct { std::uint32_t property; int flag; bool inverted; } kFlags[] = {
        {0xa00, 0x1, false}, {0xa01, 0x2, false}, {0xa02, 0x4, false}, {0xf00d, 0x10, true}, {0xa03, 0x20, false},
        {0xa04, 0x40, false}, {0xa05, 0x80, false}, {0xa06, 0x100, false}, {0xa07, 0x200, true}, {0xa08, 0x400, true},
    };
    for (const auto& entry : kFlags) {
        if (GetPropertyUint32(parameters, entry.property, value)) btn->SetBtnFlag(entry.flag, entry.inverted ? value == 0 : value != 0);
    }
    int style = 0;
    if (GetPropertyUint32(parameters, 0xf005, value)) {
        switch (value) {
            case 0xa09: style = 1; break;
            case 0xa0a: case 0xa0b: style = 2; break;
            case 0xa0c: case 0xa0d: case 0xa00: style = 3; break;
            case 0xa0e: case 0xa0f: case 0xa1b: style = 4; break;
            default: break;   // 0xa10..0xa1a and anything else: 0
        }
    }
    std::uint32_t styleId = 0;
    cRZString text;
    if (GetPropertyUint32(parameters, 0xf000, styleId)) {
        btn->SetFontStyle(styleId);
        SetGZWinFont(window, styleId, 0);
    } else if (GetPropertyString(parameters, 0xf000, text)) {
        if (const std::uint32_t named = NamedFontStyle(text)) btn->SetFontStyle(named);
        SetGZWinFont(window, text, 0);
    } else {
        IScriptControlHost* host = gScriptControlHost;
        const std::uint32_t id = host ? host->FontStyleFromName(cRZString("main9")) : 0;   // WCM vslot 0x130
        gz::IWinButtonHost* buttons = gz::WinButtonHost();
        btn->SetFont(buttons ? buttons->Font(id) : nullptr);                               // WCM vslot 0x28
    }
    static constexpr struct { std::uint32_t property; int index; } kColors[] = {
        {0xf00f, 0}, {0xf010, 4}, {0xf011, 2}, {0xf012, 1}, {0xf013, 5}, {0xf014, 3},
    };
    for (const auto& entry : kColors) {
        std::uint8_t r = 0, g = 0, b = 0;
        if (GetPropertyColor(parameters, entry.property, r, g, b)) {
            btn->SetFontColor(entry.index, WinGraphics() ? WinGraphics()->MakeNativeColor(r, g, b) : 0);
        }
    }
    cRZString gutters;
    if (GetPropertyString(parameters, 0xf004, gutters) && gutters.Strlen() != 0) {
        int commas = 0;
        for (const char* c = gutters.ToChar(); *c; ++c) commas += *c == ',';
        int a = 0, b = 0, c = 0, d = 0;
        if (commas == 1) {
            std::sscanf(gutters.ToChar(), "(%d,%d)", &a, &b);
            btn->SetGutters(static_cast<std::uint8_t>(a), static_cast<std::uint8_t>(b));
        } else if (commas == 3) {
            std::sscanf(gutters.ToChar(), "(%d,%d,%d,%d)", &a, &b, &c, &d);
            btn->SetGutters(static_cast<std::uint8_t>(a), static_cast<std::uint8_t>(b), static_cast<std::uint8_t>(c), static_cast<std::uint8_t>(d));
        }
    }
    cRZString imageText;
    if (GetPropertyString(parameters, 0xf016, imageText)) {
        unsigned group = 0, instance = 0;
        std::sscanf(imageText.ToChar(), "{%x,%x}", &group, &instance);
        cIGZBuffer* image = LoadBuffer(group, instance);
        if (!image) {
            btn->SetStyle(style, nullptr, nullptr);
        } else {
            const cRZRect area = *image->GetBufferArea();   // buffer vslot 0x68
            btn->SetStyle(style, image, &area);
        }
        cGZPersistResourceKey key{0x856ddbacu, group, instance};
        SetResourceKeyParam(window, 0x2b343becu, key);
        SetGZWinImage(window, group, instance, 0);
        if (image) image->Release();
    } else {
        btn->SetStyle(style, nullptr, nullptr);
    }
    if (GetPropertyUint32(parameters, 0xf015, value)) {
        if (value == 0xf006) btn->SetCaptionStyle(1);
        else if (value == 0xf007) btn->SetCaptionStyle(2);
        else if (value == 0xf008) btn->SetCaptionStyle(0);
    }
    cRZString tip;
    if (GetPropertyString(parameters, 0xa10, tip)) btn->SetTipText(tip);
    cRZString tipResource;
    if (GetPropertyString(parameters, 0xa12, tipResource)) {
        unsigned group = 0, instance = 0;
        std::sscanf(tipResource.ToChar(), "{%x,%x}", &group, &instance);
        cRZString loaded;
        if (LoadStringA(group, instance, loaded)) btn->SetTipText(loaded);
        cGZPersistResourceKey key{0x2026960bu, group, instance};
        SetResourceKeyParam(window, 0x2b340ffeu, key);
    }
    int tx = 0, ty = 0;
    if (GetPropertyCoords(parameters, 0xa11, tx, ty)) btn->SetTipPlacementOffsets(tx, ty);
    if (GetPropertyUint32(parameters, 0xa18, value)) btn->SetTipPlacementType(int(value));
    else btn->SetTipPlacementType(0x1000000);
    cRZString sound;
    if (GetPropertyString(parameters, 0xa19, sound)) {
        unsigned group = 0, instance = 0;
        std::sscanf(sound.ToChar(), "{%x,%x}", &group, &instance);
        std::uint32_t blob[2] = {group, instance};
        window->SetParam(0xa5be39du, new cRZVariant(blob, sizeof(blob), 0x8000));
        btn->SetSoundID(0, instance);
    } else {
        btn->SetSoundID(0, 0);
    }
    if (GetPropertyString(parameters, 0xa1a, sound)) {
        unsigned group = 0, instance = 0;
        std::sscanf(sound.ToChar(), "{%x,%x}", &group, &instance);
        std::uint32_t blob[2] = {group, instance};
        window->SetParam(0x4a5be2f8u, new cRZVariant(blob, sizeof(blob), 0x8000));
        btn->SetSoundID(1, instance);
    } else {
        btn->SetSoundID(1, 0);
    }
    btn->Release();
}

// InterpretPropertiesGZWinTextEdit (0x10053d96e): the edit control's keywords (cIGZWinTextEdit, iid 0x231a1c57; slots
// read off the vtable offsets because the decompiler drops the QueryInterface out-parameter: SetFontStyle [10]
// +0x50, SetFontColor(index, c) [13] +0x68, SetCaretColor [18] +0x90, SetHighlightFillColor [23] +0xb8,
// SetOutlineUse [37] +0x128, SetMaximumTextToAllow [33] +0x108, SetTextGutterX/Y [40]/[42] +0x140/+0x150,
// SetInsertMode [64] +0x200, SetInsertionIndex [66] +0x210, SetInsertionLineAndColumn [68] +0x220,
// SetCaretPeriod [72] +0x240, SetMaxUndoLevelCount [81] +0x288, SetScrollBarImageVertical/Horizontal [87]/[88]
// +0x2b8/+0x2c0, SetFlags [77] +0x268, GetFlags [80] +0x280, ResizeWindowForExactLineHeights [43] +0x158).
// Init pass: the flag word is read, each flag keyword of the table at 0x100a24044 (property -> bit) sets (value 1) or
// clears (any other value) its bit, and the word is written back; then the font (id or name), six font colours, the
// caret / highlight / outline colours, the maximum text, gutters, insert mode, insertion index / line+column, caret
// period, undo depth and the two scroll bar images with their resource-key params. Second pass: the autofit
// parameter (property 0x304 -> true and a resize to exact line heights, else false).
// Kept: the outline switch (0xf019) is read without testing the lookup, so an absent property leaves the previous
// value; the port uses 0 (see LEGACY-3).
void cGZLegacyControl::InterpretPropertiesGZWinTextEdit(cIGZWin* window, tScriptPropertyArray& parameters, bool init) {
    if (!window) return;
    gz::cIGZWinTextEdit* edit = nullptr;
    if (!window->QueryInterface(gz::kGZIID_cIGZWinTextEdit, reinterpret_cast<void**>(&edit))) return;
    if (init) {
        static constexpr struct { std::uint32_t property; std::uint32_t mask; } kFlagTable[] = {
            {0x400, 0x1}, {0x300, 0x2}, {0x401, 0x4}, {0x402, 0x8}, {0xf019, 0x10}, {0x301, 0x20}, {0x403, 0x40},
            {0x404, 0x80}, {0x405, 0x100}, {0x406, 0x200}, {0x415, 0x400}, {0x416, 0x800}, {0x417, 0x8000},
            {0x418, 0x1000}, {0x419, 0x2000},
        };
        std::uint32_t flags = edit->GetFlags();
        for (const auto& entry : kFlagTable) {
            std::uint32_t value = 0;
            if (GetPropertyUint32(parameters, entry.property, value)) flags = value == 1 ? (flags | entry.mask) : (flags & ~entry.mask);
        }
        edit->SetFlags(flags);
        std::uint32_t styleId = 0;
        cRZString name;
        if (GetPropertyUint32(parameters, 0xf000, styleId)) {
            edit->SetFontStyle(styleId);
            SetGZWinFont(window, styleId, 0);
        } else if (GetPropertyString(parameters, 0xf000, name)) {
            if (const std::uint32_t named = NamedFontStyle(name)) edit->SetFontStyle(named);
            SetGZWinFont(window, name, 0);
        }
        auto native = [](std::uint8_t r, std::uint8_t g, std::uint8_t b) { return WinGraphics() ? WinGraphics()->MakeNativeColor(r, g, b) : 0u; };
        static constexpr struct { std::uint32_t property; gz::cIGZWinTextEdit::ColorIndex index; } kFontColors[] = {
            {0xf00f, gz::cIGZWinTextEdit::kColorText}, {0xf010, gz::cIGZWinTextEdit::kColor2},
            {0xf011, gz::cIGZWinTextEdit::kColorSelectedText}, {0xf012, gz::cIGZWinTextEdit::kColorTextBack},
            {0xf013, gz::cIGZWinTextEdit::kColor3}, {0xf014, gz::cIGZWinTextEdit::kColorSelectedBack},
        };
        std::uint8_t r = 0, g = 0, b = 0;
        for (const auto& entry : kFontColors) {
            if (GetPropertyColor(parameters, entry.property, r, g, b)) edit->SetFontColor(entry.index, native(r, g, b));
        }
        if (GetPropertyColor(parameters, 0x407, r, g, b)) edit->SetCaretColor(native(r, g, b));
        if (GetPropertyColor(parameters, 0x408, r, g, b)) edit->SetHighlightFillColor(native(r, g, b));
        if (GetPropertyColor(parameters, 0x40b, r, g, b)) {
            std::uint32_t outline = 0;
            GetPropertyUint32(parameters, 0xf019, outline);
            edit->SetOutlineUse(outline != 0, native(r, g, b));
        }
        std::uint32_t value = 0;
        if (GetPropertyUint32(parameters, 0x409, value)) edit->SetMaximumTextToAllow(value);
        int x = 0, y = 0;
        if (GetPropertyCoords(parameters, 0xf004, x, y)) {
            edit->SetTextGutterX(static_cast<std::uint32_t>(x));
            edit->SetTextGutterY(static_cast<std::uint32_t>(y));
        }
        std::uint32_t mode = 0;
        if (GetPropertyUint32(parameters, 0x40c, mode)) edit->SetInsertMode(mode == 0);
        if (GetPropertyUint32(parameters, 0x40d, value)) edit->SetInsertionIndex(value);
        if (GetPropertyCoords(parameters, 0x40e, x, y)) edit->SetInsertionLineAndColumn(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
        if (GetPropertyUint32(parameters, 0x40f, value)) edit->SetCaretPeriod(value);
        if (GetPropertyUint32(parameters, 0x410, value)) edit->SetMaxUndoLevelCount(value);
        auto scrollImage = [&](std::uint32_t property, std::uint32_t rectProperty, std::uint32_t keyParam, std::uint32_t imageIndex, bool vertical) {
            cRZString text;
            if (!GetPropertyString(parameters, property, text)) return;
            unsigned group = 0, instance = 0;
            if (std::sscanf(text.ToChar(), "{%x,%x}", &group, &instance) != 2) return;
            cIGZBuffer* image = LoadBuffer(group, instance);
            if (image) {
                cRZRect area{0, 0, 0, 0};
                GetPropertyRect(parameters, rectProperty, area);
                if (vertical) edit->SetScrollBarImageVertical(image, area);
                else edit->SetScrollBarImageHorizontal(image, area);
            }
            cGZPersistResourceKey key{0x856ddbacu, group, instance};
            SetResourceKeyParam(window, keyParam, key);
            SetGZWinImage(window, group, instance, imageIndex);
            if (image) image->Release();
        };
        scrollImage(0x413, 0x414, 0x8b4bfefcu, 0, false);
        scrollImage(0x411, 0x412, 0x8b4bfefdu, 1, true);
    } else {
        bool autofit = false;
        for (const tScriptProperty& property : parameters) {
            if (property.id == 0x304) { autofit = true; break; }
        }
        auto* autofitParam = new cRZVariant();
        autofitParam->SetValBool(autofit);
        window->SetParam(0xdacf8f9cu, autofitParam);
        if (autofit) edit->ResizeWindowForExactLineHeights();
    }
    edit->Release();
}

// InterpretPropertiesGZWinSpinner (0x1005422b8): the spinner's keywords (cIGZWinSpinner, iid 0x612ce0c3; slots read
// off the vtable offsets: SetSpinnerType [4] +0x20, SetSpinnerFlag [6] +0x30, SetValue [9] +0x48, SetMinMax [10]
// +0x50, SetStepSize [12] +0x60, SetImage [14] +0x70, SetFont [19] +0x98, SetFontColor [22] +0xb0, SetTextAlignment
// [27] +0xd8, SetGutters [29] +0xe8, SetOutlineColors [31] +0xf8). Init pass: the button placement keyword (none /
// right / left -> 0 / 1 / 2), five flags, the font (an id or name, else the font named "main9", the font object
// coming from the window colour manager's style table), three font colours, the image, the text alignment (left 0,
// right 2, centre 1), the gutters and the four outline colours (each side falls back to the first colour, or to 0
// when none of the five was written; nothing is set when none was). Second pass: min/max, step and value.
void cGZLegacyControl::InterpretPropertiesGZWinSpinner(cIGZWin* window, tScriptPropertyArray& parameters, bool init) {
    if (!window) return;
    gz::cIGZWinSpinner* spinner = nullptr;
    if (!window->QueryInterface(gz::kGZIID_cIGZWinSpinner, reinterpret_cast<void**>(&spinner))) return;
    if (!init) {
        int low = 0, high = 0, value = 0;
        if (GetPropertyCoords(parameters, 0x1004, low, high)) spinner->SetMinMax(low, high);
        if (GetPropertySint32(parameters, 0x1005, value)) spinner->SetStepSize(value);
        if (GetPropertySint32(parameters, 0x40a, value)) spinner->SetValue(value);
        spinner->Release();
        return;
    }
    std::uint32_t value = 0;
    if (GetPropertyUint32(parameters, 0x1000, value)) {
        if (value == 0) spinner->SetSpinnerType(gz::cIGZWinSpinner::kButtonsOnly);
        else if (value == 0xf006) spinner->SetSpinnerType(gz::cIGZWinSpinner::kButtonsRight);
        else if (value == 0xf007) spinner->SetSpinnerType(gz::cIGZWinSpinner::kButtonsLeft);
    }
    static constexpr struct { std::uint32_t property; std::uint32_t flag; } kFlags[] = {
        {0xf019, 1}, {0xf00d, 2}, {0x1001, 0x1000}, {0x1002, 0x2000}, {0x1003, 0x4000},
    };
    for (const auto& entry : kFlags) {
        if (GetPropertyUint32(parameters, entry.property, value)) spinner->SetSpinnerFlag(entry.flag, value != 0);
    }
    gz::IWinFontSource* fonts = gz::WinFontSource();
    std::uint32_t styleId = 0;
    cRZString name;
    if (GetPropertyUint32(parameters, 0xf000, styleId)) {
        spinner->SetFont(fonts ? fonts->Font(styleId) : nullptr);   // WCM vslot 0x28
        SetGZWinFont(window, styleId, 0);
    } else if (GetPropertyString(parameters, 0xf000, name)) {
        if (const std::uint32_t named = NamedFontStyle(name)) spinner->SetFont(fonts ? fonts->Font(named) : nullptr);
        SetGZWinFont(window, name, 0);
    } else {
        IScriptControlHost* host = gScriptControlHost;
        const std::uint32_t id = host ? host->FontStyleFromName(cRZString("main9")) : 0;   // WCM vslot 0x130
        spinner->SetFont(fonts ? fonts->Font(id) : nullptr);
    }
    auto native = [](std::uint8_t r, std::uint8_t g, std::uint8_t b) { return WinGraphics() ? WinGraphics()->MakeNativeColor(r, g, b) : 0u; };
    static constexpr struct { std::uint32_t property; gz::cIGZWinSpinner::tFontColorIdx index; } kColors[] = {
        {0xf00f, gz::cIGZWinSpinner::kColorNormal}, {0xf010, gz::cIGZWinSpinner::kColorDisabled}, {0xf011, gz::cIGZWinSpinner::kColorHover},
    };
    std::uint8_t r = 0, g = 0, b = 0;
    for (const auto& entry : kColors) {
        if (GetPropertyColor(parameters, entry.property, r, g, b)) spinner->SetFontColor(entry.index, native(r, g, b));
    }
    cRZString imageText;
    if (GetPropertyString(parameters, 0xf016, imageText)) {
        unsigned group = 0, instance = 0;
        std::sscanf(imageText.ToChar(), "{%x,%x}", &group, &instance);
        cIGZBuffer* image = LoadBuffer(group, instance);
        if (image) spinner->SetImage(image, nullptr);
        cGZPersistResourceKey key{0x856ddbacu, group, instance};
        SetResourceKeyParam(window, 0x8bd1333au, key);
        SetGZWinImage(window, group, instance, 0);
        if (image) image->Release();
    }
    if (GetPropertyUint32(parameters, 0xf015, value)) {
        if (value == 0xf006) spinner->SetTextAlignment(0);
        else if (value == 0xf007) spinner->SetTextAlignment(2);
        else if (value == 0xf008) spinner->SetTextAlignment(1);
    }
    int gx = 0, gy = 0;
    if (GetPropertyCoords(parameters, 0xf004, gx, gy)) spinner->SetGutters(static_cast<std::uint8_t>(gx), static_cast<std::uint8_t>(gy));
    // outline colours: 0x1006 is the base, 0x1007..0x100a the left / top / right / bottom overrides
    const bool hasBase = GetPropertyColor(parameters, 0x1006, r, g, b);
    const std::uint32_t base = hasBase ? native(r, g, b) : 0u;
    std::uint32_t sides[4] = {base, base, base, base};
    bool any = hasBase;
    static constexpr std::uint32_t kSideProperties[4] = {0x1007, 0x1008, 0x1009, 0x100a};
    for (int i = 0; i < 4; ++i) {
        if (GetPropertyColor(parameters, kSideProperties[i], r, g, b)) { sides[i] = native(r, g, b); any = true; }
    }
    if (any) spinner->SetOutlineColors(sides[0], sides[1], sides[2], sides[3]);
    spinner->Release();
}

// ---------------------------------------------------------------------------------------------------------
// PARTIAL per-class interpreters (addresses in LegacyControl.h). Their bodies apply the control-specific view
// interface setters; the decompiler drops the QueryInterface result base in every one of them (the `local_*`
// that receives the interface pointer is read but never assigned in the output), so each needs a reading of
// the disassembly per function before it can be written faithfully. Registered in the tables so the dispatch
// is the original one, but they currently do nothing; each is recovered-partial in
// notes/ledger_pending/cGZLegacyControl.txt with the neighbour it waits for.
// ---------------------------------------------------------------------------------------------------------
#define GZLEGACYCTRL_PARTIAL_INTERPRETER(Name)                                                   \
    void cGZLegacyControl::InterpretProperties##Name(cIGZWin* window, tScriptPropertyArray& parameters, \
                                                    bool init) {                                 \
        (void)window; (void)parameters; (void)init;                                              \
    }


GZLEGACYCTRL_PARTIAL_INTERPRETER(GZWinCombo)
GZLEGACYCTRL_PARTIAL_INTERPRETER(GZWinLineInput)
GZLEGACYCTRL_PARTIAL_INTERPRETER(GZWinOptGrp)
GZLEGACYCTRL_PARTIAL_INTERPRETER(GZWinTextTicker)
GZLEGACYCTRL_PARTIAL_INTERPRETER(GZWinFileBrowser)
GZLEGACYCTRL_PARTIAL_INTERPRETER(GZWinTreeView)
GZLEGACYCTRL_PARTIAL_INTERPRETER(GZWinFolders)

#undef GZLEGACYCTRL_PARTIAL_INTERPRETER
