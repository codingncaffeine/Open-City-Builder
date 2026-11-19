// cGZGraph — the chart window (148 functions 0x1002e2ce8..0x100386fdc plus the five MiscGraph* overlay classes).
// A graph is a window with a background, a plotting rectangle, two axes (0 = horizontal, 1 = vertical) each
// with a value range, major / minor ticks, optional grid lines, tick text and a label, a title, a list of
// dynamic graph objects (lines, rects, arrows, pictures, text) and an optional hit-test result buffer.
// Subclasses (bar / line / scatter / pie) override DrawGraphData.
//
// "auto" settings use sentinels that CommitDefaultValues resolves on the first redraw: a value / spacing equal
// to FLT_MIN (0x00800000), a rectangle whose left is 0x7fffffff, decimal places 0x80. A colour of INT_MIN
// means "the font's own colour" (kFontDefaultColor).
//
// Layout (0x320 bytes): cGZWin (0x150), +0x150 cIGZGraph view, +0x15c plotting area, +0x16c fonts loaded,
// +0x170 title, +0x198 title area, +0x1a8 title font, +0x1b0 title colour, +0x1b4 title height, +0x1b8 title
// alignment, +0x1bc/+0x1c0/+0x1c1/+0x1c4 background fill colour / fill / outline / outline colour, +0x1c8..+0x1d4
// plot-area fill / colour / outline / colour, +0x1d8 / +0x1e0 background patterns, +0x1e8 draw axes, +0x1e9 major
// tick usage[2], +0x1eb minor[2], +0x1ed major grid[2], +0x1ef minor grid[2], +0x1f1 tick text[2], +0x1f4 axis
// colour, +0x1f8 min[2], +0x200 max[2], +0x208 major spacing[2], +0x210 minor spacing[2], +0x218 decimals[2],
// +0x21c major tick length[2], +0x224 minor tick length[2], +0x22c tick colour, +0x230 tick text alignment[2],
// +0x238 zero position[2], +0x240 axis labels[2], +0x290 label areas[2], +0x2b0 label alignment[2], +0x2b8 label
// fonts[2], +0x2c8 label colours[2], +0x2d0 tick fonts[2], +0x2e0 tick colours[2], +0x2e8 label rotation[2],
// +0x2f0 transparent key colour, +0x2f8 hit-test buffer, +0x300 its size, +0x308 dynamic objects.
#pragma once
#include <cfloat>
#include <climits>
#include <list>
#include <memory>
#include <string>

#include "gz/Buffer.h"
#include "gz/Font.h"
#include "gz/Win.h"
#include "rz/cRZString.h"

namespace gz {

constexpr std::uint32_t kGZIID_cIGZGraph = 0x02cf2351;

enum Axis : std::int32_t { kAxisHorizontal = 0, kAxisVertical = 1 };
enum GraphRotation : std::int32_t { kRotate0 = 0, kRotate90 = 1, kRotate180 = 2, kRotate270 = 3 };
enum Usage : std::int32_t { kUsageOff = 0, kUsageOn = 1 };

constexpr float kAutoValue = FLT_MIN;                       // 0x00800000
constexpr std::int32_t kAutoRect = 0x7fffffff;
constexpr std::uint32_t kAutoColor = 0x80000000;
constexpr std::uint8_t kAutoDecimals = 0x80;

class cIGZGraph;

// An overlay the graph draws after its data (vtable: 0 / 1 destructors, 2 DrawSelf).
class DynamicGraphObject {
public:
    virtual ~DynamicGraphObject() = default;
    virtual void DrawSelf(cIGZGraph* graph, cIGZBuffer* buffer) = 0;
};

// The services a graph needs from the framework: fonts (RZ::FO()), scratch buffers and colours (RZ::GS()).
class IGraphHost {
public:
    virtual ~IGraphHost() = default;
    virtual cIGZFont* CreateFont(const char* face, std::uint32_t size) = 0;     // one reference, null when unavailable
    virtual bool FontsAvailable() = 0;                                          // RZ::FO() != null
    virtual cIGZBuffer* CreateBuffer() = 0;                                     // one reference
    virtual std::uint32_t MakeNativeColor(std::uint8_t r, std::uint8_t g, std::uint8_t b) = 0;
    virtual cIGZBuffer* WindowBuffer(IWinBuffer* buffer) = 0;                   // a window's draw buffer as a cIGZBuffer
};
IGraphHost* GraphHost();
void SetGraphHost(IGraphHost* host);

class MiscGraphTextInfo;
class MiscGraphLine;
class MiscGraphRect;
class MiscGraphArrow;
class MiscGraphPicture;

// cIGZGraph (iid 0x02cf2351): the chart interface; slot n at 8n.
class cIGZGraph : public cIGZUnknown {
public:
    virtual void SetBackgroundFill(bool fill) = 0;                                          // [3]
    virtual void SetBackgroundOutline(bool outline) = 0;                                    // [4]
    virtual void SetBackgroundFillColor(std::uint32_t color) = 0;                           // [5]
    virtual void SetBackgroundOutlineColor(std::uint32_t color) = 0;                        // [6]
    virtual void SetBackgroundPattern(cIGZBuffer* pattern) = 0;                             // [7]
    virtual void SetGraphAreaOutlineAndFill(bool outline, bool fill) = 0;                   // [8]
    virtual void SetGraphAreaOutlineColor(std::uint32_t color) = 0;                         // [9]
    virtual void SetGraphAreaFillColor(std::uint32_t color) = 0;                            // [10]
    virtual void SetGraphBackgroundPattern(cIGZBuffer* pattern) = 0;                        // [11]
    virtual void SetGraphArea(const cRZRect& area) = 0;                                     // [12]
    virtual void GetGraphArea(cRZRect& area) = 0;                                           // [13]
    virtual void CompleteRedraw(cIGZBuffer* buffer) = 0;                                    // [14]
    virtual void SetAxisMinValue(Axis axis, float value) = 0;                               // [15]
    virtual void SetAxisMaxValue(Axis axis, float value) = 0;                               // [16]
    virtual void SetAxisZeroPosition(Axis axis, int position) = 0;                          // [17]
    virtual void SetAxisLineColor(std::uint32_t color) = 0;                                 // [18]
    virtual void SetAxisLabel(Axis axis, const cRZString& label, bool autoPosition) = 0;    // [19]
    virtual void SetAxisLabelPosition(Axis axis, cRZRect& area) = 0;                        // [20]
    virtual void SetAxisLabelFont(Axis axis, cIGZFont* font, std::uint32_t color) = 0;      // [21]
    virtual void SetAxisLabelRotation(Axis axis, GraphRotation rotation) = 0;               // [22]
    virtual void SetMajorTickUsage(Axis axis, Usage usage) = 0;                             // [23]
    virtual void SetMajorTickSpacing(Axis axis, float spacing) = 0;                         // [24]
    virtual void SetMajorTickDivisions(Axis axis, std::uint32_t divisions, bool minPixels) = 0;   // [25]
    virtual void SetMajorTickGridUsage(Axis axis, Usage usage) = 0;                         // [26]
    virtual void SetMajorTickDecimalPlaces(Axis axis, int places) = 0;                      // [27]
    virtual void SetMajorTickLength(Axis axis, int length) = 0;                             // [28]
    virtual void SetAxisTickFont(Axis axis, cIGZFont* font, std::uint32_t color) = 0;       // [29]
    virtual void SetAxisTickTextDrawing(Axis axis, Usage usage) = 0;                        // [30]
    virtual void SetTickColor(std::uint32_t color) = 0;                                     // [31]
    virtual void SetMinorTickUsage(Axis axis, Usage usage) = 0;                             // [32]
    virtual void SetMinorTickSpacing(Axis axis, float spacing) = 0;                         // [33]
    virtual void SetMinorTickDivisions(Axis axis, std::uint32_t divisions, bool minPixels) = 0;   // [34]
    virtual void SetMinorTickGridUsage(Axis axis, Usage usage) = 0;                         // [35]
    virtual void SetMinorTickLength(Axis axis, int length) = 0;                             // [36]
    virtual void SetTitle(const cRZString& title, bool autoPosition) = 0;                   // [37]
    virtual void SetTitlePosition(cRZRect& area) = 0;                                       // [38]
    virtual void SetTitleFont(cIGZFont* font, std::uint32_t color) = 0;                     // [39]
    virtual bool DoHitTest(cRZPoint& point, std::uint32_t& result) = 0;                     // [40]
    virtual bool AllocateHitTestData(std::uint32_t size) = 0;                               // [41]
    virtual void* GetHitTestResultData() = 0;                                               // [42]
    virtual bool DrawLine(cIGZBuffer* buffer, int x0, int y0, int x1, int y1, int color, std::uint32_t thickness) = 0;   // [43]
    virtual void DrawRectangleOutline(cIGZBuffer* buffer, const cRZRect& rect, std::uint32_t color, std::uint32_t thickness) = 0;   // [44]
    virtual bool Fill(cIGZBuffer* buffer, std::uint32_t color, const cRZRect& rect, float unused) = 0;   // [45]
    virtual bool Blt(cIGZBuffer* source, cIGZBuffer* destination, const cRZRect& sourceRect, const cRZRect& destRect) = 0;   // [46]
    virtual void AddDynamicGraphObject(DynamicGraphObject* object) = 0;                     // [47]
    virtual void RemoveDynamicGraphObject(DynamicGraphObject* object) = 0;                  // [48]
    virtual MiscGraphTextInfo* AddMiscGraphTextInfo() = 0;                                  // [49]
    virtual MiscGraphLine* AddMiscGraphLine() = 0;                                          // [50]
    virtual MiscGraphRect* AddMiscGraphRect() = 0;                                          // [51]
    virtual MiscGraphArrow* AddMiscGraphArrow() = 0;                                        // [52]
    virtual MiscGraphPicture* AddMiscGraphPicture() = 0;                                    // [53]
    virtual cIGZWin* AsIGZWin() = 0;                                                        // [54]
};

// The five ready-made overlay objects (AddMiscGraph*). Layout: vptr, +0xc first field.
class MiscGraphLine final : public DynamicGraphObject {
public:
    void DrawSelf(cIGZGraph* graph, cIGZBuffer* buffer) override;   // @ 0x10037bd06
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;       // +0xc
    std::uint32_t color = 0;                   // +0x1c
    std::uint32_t thickness = 1;               // +0x20
};

class MiscGraphRect final : public DynamicGraphObject {
public:
    void DrawSelf(cIGZGraph* graph, cIGZBuffer* buffer) override;   // @ 0x10037c092
    cRZRect rect;                              // +0xc
    bool outline = true;                       // +0x1c
    std::uint32_t outlineThickness = 1;        // +0x20
    std::uint32_t outlineColor = 0;            // +0x24
    bool fill = false;                         // +0x28
    std::uint32_t fillColor = 0xffffffff;      // +0x2c
};

class MiscGraphArrow final : public DynamicGraphObject {
public:
    void DrawSelf(cIGZGraph* graph, cIGZBuffer* buffer) override;   // @ 0x10037c11c
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;       // +0xc
    std::uint32_t color = 0;                   // +0x1c
    std::uint32_t thickness = 1;               // +0x20
    bool startHead = true;                     // +0x24
    bool endHead = false;                      // +0x25
    std::int32_t headLength = 8;               // +0x28
    float headAngle = 0.78539819f;             // +0x2c (pi / 4)
};

class MiscGraphPicture final : public DynamicGraphObject {
public:
    ~MiscGraphPicture() override;              // @ 0x10037c1a8
    bool Init() { return true; }               // @ 0x10037c21c
    void SetBuffer(cIGZBuffer* buffer);        // @ 0x10037c224
    void DrawSelf(cIGZGraph* graph, cIGZBuffer* buffer) override;   // @ 0x10037c258
    cIGZBuffer* picture = nullptr;             // +0x10
    int x = 0, y = 0;                          // +0x18
    bool relativeToGraphArea = false;          // +0x20
};

class MiscGraphTextInfo final : public DynamicGraphObject {
public:
    ~MiscGraphTextInfo() override;             // @ 0x10037bd7c
    bool Init();                               // @ 0x10037be12 (default font: Arial 12, else Helvetica)
    void SetFont(cIGZFont* font, std::uint32_t color);   // @ 0x10037bf46
    void DrawSelf(cIGZGraph* graph, cIGZBuffer* buffer) override;   // @ 0x10037bf9c
    cRZString text;                            // +0x10
    cRZRect rect;                              // +0x38
    std::uint32_t textAlignment = 0;           // +0x48
    bool outline = false;                      // +0x4c
    std::uint32_t outlineColor = 0;            // +0x50
    bool fill = false;                         // +0x54
    std::uint32_t fillColor = 0xffffffff;      // +0x58
    cIGZFont* font = nullptr;                  // +0x60
    std::uint32_t fontColor = kAutoColor;      // +0x68
};

// The free function DrawRectangleOutline(cIGZBuffer*, rect, colour, thickness) @ 0x1002b3deb.
bool DrawRectangleOutline(cIGZBuffer* buffer, const cRZRect& rect, std::uint32_t color, std::uint32_t thickness);

class cGZGraph : public cGZWin, public cIGZGraph {
public:
    explicit cGZGraph(cIGZWinMgr* mgr);
    virtual ~cGZGraph();

    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return cGZWin::AddRef(); }
    std::uint32_t Release() override { return cGZWin::Release(); }

    bool Init() override;
    bool Shutdown() override;
    void SetArea(int l, int t, int r, int b) override;
    void SetArea(const cRZRect& area) override { SetArea(area.left, area.top, area.right, area.bottom); }
    bool GZPaint() override;
    bool GZOnMouseDownL(int x, int y, std::uint32_t flags) override;
    bool GZOnMouseUpL(int x, int y, std::uint32_t flags) override;
    bool GZOnMouseMove(int x, int y, std::uint32_t flags) override;

    // cIGZGraph
    void SetBackgroundFill(bool fill) override { backgroundFill_ = fill; }
    void SetBackgroundOutline(bool outline) override { backgroundOutline_ = outline; }
    void SetBackgroundFillColor(std::uint32_t color) override { backgroundFillColor_ = color; }
    void SetBackgroundOutlineColor(std::uint32_t color) override { backgroundOutlineColor_ = color; }
    void SetBackgroundPattern(cIGZBuffer* pattern) override;
    void SetGraphAreaOutlineAndFill(bool outline, bool fill) override { areaOutline_ = outline; areaFill_ = fill; }
    void SetGraphAreaOutlineColor(std::uint32_t color) override { areaOutlineColor_ = color; }
    void SetGraphAreaFillColor(std::uint32_t color) override { areaFillColor_ = color; }
    void SetGraphBackgroundPattern(cIGZBuffer* pattern) override;
    void SetGraphArea(const cRZRect& area) override { graphArea_ = area; }
    void GetGraphArea(cRZRect& area) override { area = graphArea_; }
    void CompleteRedraw(cIGZBuffer* buffer) override;
    void SetAxisMinValue(Axis axis, float value) override { min_[axis] = value; }
    void SetAxisMaxValue(Axis axis, float value) override { max_[axis] = value; }
    void SetAxisZeroPosition(Axis axis, int position) override { zeroPosition_[axis] = position; }
    void SetAxisLineColor(std::uint32_t color) override { axisColor_ = color; }
    void SetAxisLabel(Axis axis, const cRZString& label, bool autoPosition) override;
    void SetAxisLabelPosition(Axis axis, cRZRect& area) override { labelArea_[axis] = area; }
    void SetAxisLabelFont(Axis axis, cIGZFont* font, std::uint32_t color) override;
    void SetAxisLabelRotation(Axis axis, GraphRotation rotation) override { labelRotation_[axis] = rotation; }
    void SetMajorTickUsage(Axis axis, Usage usage) override { majorTicks_[axis] = usage != 0; }
    void SetMajorTickSpacing(Axis axis, float spacing) override { majorSpacing_[axis] = spacing; }
    float MajorTickSpacing(Axis axis) const { return majorSpacing_[axis]; }   // (port accessor: kAutoValue until committed)
    void SetMajorTickDivisions(Axis axis, std::uint32_t divisions, bool minPixels) override;
    void SetMajorTickGridUsage(Axis axis, Usage usage) override { majorGrid_[axis] = usage != 0; }
    void SetMajorTickDecimalPlaces(Axis axis, int places) override;
    void SetMajorTickLength(Axis axis, int length) override { majorLength_[axis] = length; }
    void SetAxisTickFont(Axis axis, cIGZFont* font, std::uint32_t color) override;
    void SetAxisTickTextDrawing(Axis axis, Usage usage) override { tickText_[axis] = usage != 0; }
    void SetTickColor(std::uint32_t color) override { tickColor_ = color; }
    void SetMinorTickUsage(Axis axis, Usage usage) override { minorTicks_[axis] = usage == kUsageOn; }
    void SetMinorTickSpacing(Axis axis, float spacing) override { minorSpacing_[axis] = spacing; }
    void SetMinorTickDivisions(Axis axis, std::uint32_t divisions, bool minPixels) override;
    void SetMinorTickGridUsage(Axis axis, Usage usage) override { minorGrid_[axis] = usage == kUsageOn; }
    void SetMinorTickLength(Axis axis, int length) override { minorLength_[axis] = length; }
    void SetTitle(const cRZString& title, bool autoPosition) override;
    void SetTitlePosition(cRZRect& area) override { titleArea_ = area; }
    void SetTitleFont(cIGZFont* font, std::uint32_t color) override;
    bool DoHitTest(cRZPoint&, std::uint32_t&) override { return false; }
    bool AllocateHitTestData(std::uint32_t size) override;
    void* GetHitTestResultData() override { return hitData_; }
    bool DrawLine(cIGZBuffer* buffer, int x0, int y0, int x1, int y1, int color, std::uint32_t thickness) override;
    void DrawRectangleOutline(cIGZBuffer* buffer, const cRZRect& rect, std::uint32_t color, std::uint32_t thickness) override;
    bool Fill(cIGZBuffer* buffer, std::uint32_t color, const cRZRect& rect, float unused) override;
    bool Blt(cIGZBuffer* source, cIGZBuffer* destination, const cRZRect& sourceRect, const cRZRect& destRect) override;
    void AddDynamicGraphObject(DynamicGraphObject* object) override { dynamicObjects_.push_back(object); }
    void RemoveDynamicGraphObject(DynamicGraphObject* object) override { dynamicObjects_.remove(object); }
    MiscGraphTextInfo* AddMiscGraphTextInfo() override;
    MiscGraphLine* AddMiscGraphLine() override;
    MiscGraphRect* AddMiscGraphRect() override;
    MiscGraphArrow* AddMiscGraphArrow() override;
    MiscGraphPicture* AddMiscGraphPicture() override;
    cIGZWin* AsIGZWin() override { return this; }

    // Overridable drawing steps
    virtual void LoadDefaultFonts();
    virtual void DrawBackground(cIGZBuffer* buffer);
    virtual void DrawGraphAreaBackground(cIGZBuffer* buffer);
    virtual void DrawAxes(cIGZBuffer* buffer);
    virtual void DrawTicks(cIGZBuffer* buffer, std::uint32_t axisMask);
    virtual void DrawTickText(cIGZBuffer* buffer, std::uint32_t axisMask);
    virtual void DrawGraphData(cIGZBuffer*) {}
    virtual void DrawDynamicObjects(cIGZBuffer* buffer);
    virtual void DrawTitles(cIGZBuffer* buffer);
    virtual int ValueToPixel(float value, Axis axis, bool clamp);
    virtual float PixelToValue(Axis axis, int pixel, bool clamp);
    virtual int ValueAmountToPixelAmount(float amount, Axis axis);
    virtual float PixelAmountToValueAmount(Axis axis, int pixels);
    virtual float GetHighestAxisValue(Axis) { return 90.0f; }
    virtual float GetLowestAxisValue(Axis) { return 0.0f; }
    virtual void CalculateAxisZeroPosition(Axis axis);
    virtual int WindowPixelToGraphPixel(Axis axis, int pixel);
    virtual int GraphPixelToWindowPixel(Axis axis, int pixel);
    virtual int GetMaxTickLengthForAxis(Axis axis);
    virtual void CommitDefaultValues();
    virtual void GetAutomaticTitleRect(cRZRect& out);
    virtual void GetAutomaticGraphAreaRect(cRZRect& out);
    virtual void GetAutomaticAxisLabelRect(Axis axis, cRZRect& out);
    virtual void GetAutomaticAxisMinValue(Axis axis, float& out);
    virtual void GetAutomaticAxisMaxValue(Axis axis, float& out);
    virtual void GetAutomaticAxisMajorTickSpacing(Axis axis, float& out);
    virtual void GetAutomaticAxisMinorTickSpacing(Axis axis, float& out);

protected:
    static std::string FormatTick(float value, std::uint8_t decimals);
    static void ReplaceFont(cIGZFont*& slot, cIGZFont* font);
    static void ReplacePattern(cIGZBuffer*& slot, cIGZBuffer* pattern);
    static std::int32_t TruncToInt(float value);
    void DrawRotatedAxisLabel(cIGZBuffer* buffer);

    std::int32_t version_ = 1;                                   // +0x158
    cRZRect graphArea_{kAutoRect, kAutoRect, kAutoRect, kAutoRect};   // +0x15c
    bool fontsLoaded_ = false;                                   // +0x16c
    cRZString title_;                                            // +0x170
    cRZRect titleArea_{kAutoRect, kAutoRect, kAutoRect, kAutoRect};   // +0x198
    cIGZFont* titleFont_ = nullptr;                              // +0x1a8
    std::uint32_t titleColor_ = kAutoColor;                      // +0x1b0
    std::int32_t titleHeight_ = 0x20;                            // +0x1b4
    std::int32_t titleAlignment_ = 1;                            // +0x1b8
    std::uint32_t backgroundFillColor_ = 0xffffffff;             // +0x1bc
    bool backgroundFill_ = true, backgroundOutline_ = true;      // +0x1c0, +0x1c1
    std::uint32_t backgroundOutlineColor_ = 0;                   // +0x1c4
    bool areaFill_ = false;                                      // +0x1c8
    std::uint32_t areaFillColor_ = 0x12345678;                   // +0x1cc
    bool areaOutline_ = true;                                    // +0x1d0
    std::uint32_t areaOutlineColor_ = 0;                         // +0x1d4
    cIGZBuffer* backgroundPattern_ = nullptr;                    // +0x1d8
    cIGZBuffer* areaPattern_ = nullptr;                          // +0x1e0
    bool drawAxes_ = false;                                      // +0x1e8
    bool majorTicks_[2] = {true, true};                          // +0x1e9
    bool minorTicks_[2] = {false, false};                        // +0x1eb
    bool majorGrid_[2] = {true, true};                           // +0x1ed
    bool minorGrid_[2] = {false, false};                         // +0x1ef
    bool tickText_[2] = {false, true};                           // +0x1f1
    std::uint32_t axisColor_ = 0;                                // +0x1f4
    float min_[2] = {kAutoValue, kAutoValue};                    // +0x1f8
    float max_[2] = {kAutoValue, kAutoValue};                    // +0x200
    float majorSpacing_[2] = {kAutoValue, kAutoValue};           // +0x208
    float minorSpacing_[2] = {kAutoValue, kAutoValue};           // +0x210
    std::uint8_t decimals_[2] = {kAutoDecimals, kAutoDecimals};  // +0x218
    std::int32_t majorLength_[2] = {4, 4};                       // +0x21c
    std::int32_t minorLength_[2] = {2, 2};                       // +0x224
    std::uint32_t tickColor_ = 0;                                // +0x22c
    std::int32_t tickTextAlignment_[2] = {1, 2};                 // +0x230
    std::int32_t zeroPosition_[2] = {kAutoRect, kAutoRect};      // +0x238
    cRZString axisLabel_[2];                                     // +0x240
    cRZRect labelArea_[2] = {{kAutoRect, kAutoRect, kAutoRect, kAutoRect}, {kAutoRect, kAutoRect, kAutoRect, kAutoRect}};   // +0x290
    std::int32_t labelAlignment_[2] = {1, 0};                    // +0x2b0
    cIGZFont* labelFont_[2] = {nullptr, nullptr};                // +0x2b8
    std::uint32_t labelColor_[2] = {kAutoColor, kAutoColor};     // +0x2c8
    cIGZFont* tickFont_[2] = {nullptr, nullptr};                 // +0x2d0
    std::uint32_t tickFontColor_[2] = {kAutoColor, kAutoColor};  // +0x2e0
    std::int32_t labelRotation_[2] = {0, 0};                     // +0x2e8
    std::uint32_t keyColor_ = 0;                                 // +0x2f0 transparent key of rotated labels
    std::uint8_t* hitData_ = nullptr;                            // +0x2f8
    std::uint32_t hitDataSize_ = 0;                              // +0x300
    std::list<DynamicGraphObject*> dynamicObjects_;              // +0x308
};

}  // namespace gz
