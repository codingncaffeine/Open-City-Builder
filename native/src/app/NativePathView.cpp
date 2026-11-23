// cSC4PathView (Mac i386 0x0051b1f0..0x0051c0ba; OpenSC4 docs/specs/overlay-views.md §1): the renderer view object that draws the
// route query's routes as ribbons over the city. Paths are kept in priority order; each one is a triangle strip two vertices per
// point (vertex format 10: position, colour, uv), with miter joints, butt ends and optional arrowheads at either end.
// Not carried over: SetPathTexture (no caller in the game), so the paths always use the plain state.
#include <cmath>
#include <cstdint>
#include <deque>
#include <list>
#include <vector>

#include "app/NativeCityView.h"
#include "sc4/DrawContext.h"
#include "sc4/PackedRenderState.h"
#include "sc4/Render3D.h"
#include "sc4/RenderProperties.h"
#include "sc4/ServiceLocator.h"
#include "sc4/ViewInputControlQuery.h"

namespace native {
namespace {
using namespace sc4;

struct PathVertex {   // sVF_V3F_C4UB_T2F (vertex format 10)
    float x, y, z;
    std::uint32_t color;
    float u, v;
};
static_assert(sizeof(PathVertex) == 24);

struct Vec { float x = 0, y = 0, z = 0; };
Vec Sub(const Vec& a, const Vec& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec Add(const Vec& a, const Vec& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec Mul(const Vec& a, float k) { return {a.x * k, a.y * k, a.z * k}; }
float Dot(const Vec& a, const Vec& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float Length(const Vec& a) { return std::sqrt(Dot(a, a)); }
// N(A, B) = (dz, 0, -dx) / |(dx, dz)|: the horizontal unit normal of A -> B
Vec Normal(const Vec& a, const Vec& b) {
    const float dx = b.x - a.x, dz = b.z - a.z;
    const float length = std::sqrt(dx * dx + dz * dz);
    return {dz / length, 0.0f, -dx / length};
}

class PathView final : public IQueryPathView, public cISC4ViewObject3D {
public:
    struct PathInfo {
        std::uint32_t color = 0;    // +0
        std::uint32_t flags = 0;    // +4
        float priority = 0.0f;      // +8
        std::deque<Vec> points;     // +0x0c
        float repeat = 0.0f;        // +0x38 (the original copies an uninitialized slot; no texture is ever set, so unused)
    };

    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (--refs_ != 0) return refs_;
        delete this;
        return 0;
    }

    // i386 0x0051b2b4: the plain and the textured state (colour write, no depth write or test, culling, blended unless kPreferOpaque).
    bool Init() {
        if (initialized_) return true;
        auto* properties = dynamic_cast<cSC4RenderProperties*>(SL::spRenderProperties);
        const bool blend = !(properties && properties->NumBoolProperties() > 33 && properties->BoolValue(33));
        plain_.PackRenderState(true, false, false, false, blend, 0, true, -1, 4, 5, -1, -1.0f, 1, -1);
        textured_.PackRenderState(true, false, false, false, blend, 1, true, -1, 4, 5, -1, -1.0f, 1, -1);
        initialized_ = true;
        return true;
    }
    bool ShutdownView() override {   // i386 0x0051b1f0
        initialized_ = false;
        return true;
    }
    void SetThickness(float thickness) override { thickness_ = thickness; }
    // i386 0x0051bf62: inserted before the first path whose priority is not below the new one (equal: newest first).
    void NewPath(std::uint32_t color, float priority, std::uint32_t flags) override {
        auto it = paths_.begin();
        while (it != paths_.end() && !(it->priority >= priority)) ++it;
        PathInfo info;
        info.color = color;
        info.flags = flags;
        info.priority = priority;
        selected_ = paths_.insert(it, std::move(info));
        hasSelected_ = true;
    }
    // i386 0x0051b456: appended unless within sqrt(1e-5) of the previous point in x and z.
    void AddPathVertex(const float* xyz) override {
        if (!hasSelected_) return;
        std::deque<Vec>& points = selected_->points;
        const Vec p{xyz[0], xyz[1], xyz[2]};
        if (!points.empty()) {
            const Vec& last = points.back();
            const double dx = last.x - p.x, dz = last.z - p.z;
            if (dx * dx + dz * dz < 1e-5) return;
        }
        points.push_back(p);
    }
    void ClearPaths() override {   // i386 0x0051bfe6 (the selection is not reset, as in the original; NewPath sets it again)
        paths_.clear();
        hasSelected_ = false;   // DIVERGENCE: the original keeps a stale pointer; with none the next AddPathVertex is dropped
    }

    // Update (i386 0x0051becc): depth offset 15, the identity model transform, every path in order.
    void DrawViewObject(SC4DrawContext& context) override {
        if (paths_.empty()) return;
        const int depthOffset = context.GetDepthOffset();
        context.SetDepthOffset(15);
        context.ResetModelViewTransform();
        const cS3DTransform identity;
        context.SetModelTransform(identity);
        for (PathInfo& path : paths_) DrawPath(context, path);
        context.SetDepthOffset(depthOffset);
    }

private:
    // CalcButtEnds (i386 0x0051b674): P +- t N(A, B).
    void ButtEnds(const Vec& a, const Vec& b, const Vec& p, Vec& plus, Vec& minus) const {
        const Vec n = Mul(Normal(a, b), thickness_);
        plus = Add(p, n);
        minus = Sub(p, n);
    }
    // CalcMiterJoint (i386 0x0051b4da): M = (N(prev, P) + N(P, next)) / 2, q = M.M; q < 1e-4: P +- N(P, next) (unscaled), else
    // P +- M x (t / q).
    void MiterJoint(const Vec& prev, const Vec& p, const Vec& next, Vec& plus, Vec& minus) const {
        const Vec n1 = Normal(prev, p), n2 = Normal(p, next);
        const Vec m = Mul(Add(n1, n2), 0.5f);
        const double q = Dot(m, m);
        if (q < 0.0001) {
            plus = Add(p, n2);
            minus = Sub(p, n2);
            return;
        }
        const Vec offset = Mul(m, static_cast<float>(thickness_ / q));
        plus = Add(p, offset);
        minus = Sub(p, offset);
    }
    // DrawArrowhead (i386 0x0051b728): L = |Q - P| (3D), a = min(L - t, 4.5 t); a < t: nothing. Else B = P + (Q - P) a / L and the
    // triangle P, B - 2t N(P, Q), B + 2t N(P, Q); P moves to B.
    void Arrowhead(SC4DrawContext& context, Vec& p, const Vec& q, std::uint32_t color) const {
        const float t = thickness_;
        const float length = Length(Sub(q, p));
        float a = length - t;
        const float cap = static_cast<float>(4.5 * static_cast<double>(t));
        if (cap < a) a = cap;
        if (a < t) return;
        const Vec base = Add(p, Mul(Sub(q, p), a / length));
        const Vec side = Mul(Normal(p, q), 2.0f * t);
        const Vec left = Sub(base, side), right = Add(base, side);
        PathVertex triangle[3] = {{p.x, p.y, p.z, color, 0, 0}, {left.x, left.y, left.z, color, 0, 0}, {right.x, right.y, right.z, color, 0, 0}};
        context.DrawPrims(0, 10, 3, triangle);
        p = base;
    }
    // DrawPath (i386 0x0051b9e4; overlay-views.md §1.1-1.2). One strip pair per point: the first point's butt end (after the start
    // arrow moved it), a miter at every inner point, the last point's butt end (after the end arrow). The pair at a point carries
    // the longitudinal coordinate of the segment leaving it (the last pair keeps the last segment's).
    void DrawPath(SC4DrawContext& context, PathInfo& path) {
        const std::size_t n = path.points.size();
        if (n < 2) return;
        context.SetRenderState(plain_);
        std::vector<PathVertex> strip;
        strip.reserve(n * 2);
        const float k = path.repeat / (thickness_ + thickness_);
        float s = 0.0f, b = 0.0f;
        Vec dir;
        const auto emit = [&](const Vec& plus, const Vec& minus) {
            strip.push_back({plus.x, plus.y, plus.z, path.color, Dot(dir, plus) + b, 0.0f});
            strip.push_back({minus.x, minus.y, minus.z, path.color, Dot(dir, minus) + b, 1.0f});
        };
        Vec first = path.points[0], last = path.points[n - 1];   // the working end points (the arrows move them)
        const auto point = [&](std::size_t i) -> const Vec& { return i == 0 ? first : path.points[i]; };
        for (std::size_t i = 1; i < n; ++i) {
            Vec plus, minus;
            if (i == 1) {
                if (path.flags & 1) Arrowhead(context, first, path.points[1], path.color);
                ButtEnds(first, path.points[1], first, plus, minus);
            } else {
                MiterJoint(point(i - 2), point(i - 1), path.points[i], plus, minus);
            }
            const Vec& a = point(i - 1);
            const Vec segment = Sub(path.points[i], a);
            const float length3 = Length(segment);
            dir = Mul(segment, k / length3);
            b = s - Dot(dir, a);
            emit(plus, minus);
            s += length3 * k;
        }
        const Vec& before = point(n - 2);
        if (path.flags & 2) Arrowhead(context, last, before, path.color);
        Vec plus, minus;
        ButtEnds(before, last, last, plus, minus);
        emit(plus, minus);
        context.DrawPrims(1, 10, static_cast<int>(strip.size()), strip.data());
    }

    std::uint32_t refs_ = 0;
    bool initialized_ = false;          // +0x0c
    float thickness_ = 2.0f;            // +0x10
    std::list<PathInfo> paths_;         // +0x14
    std::list<PathInfo>::iterator selected_;   // +0x1c
    bool hasSelected_ = false;
    cSC4PackedRenderState plain_;       // +0x20
    cSC4PackedRenderState textured_;    // +0x28
};

}  // namespace

// new cSC4PathView + Init (the query tool's ShowAllRoutesThatIntersectCityRect).
sc4::IQueryPathView* NativeCreatePathView() {
    auto* view = new PathView();
    view->AddRef();
    view->Init();
    return view;
}
// SL::spRenderer vslot 0x80 / 0x84 (i386): AddViewObject(view, 5, 1000) / RemoveViewObject.
bool NativeAddPathView(sc4::IQueryPathView* view) {
    sc4::cSC43DRender* renderer = CityRenderer();
    auto* object = dynamic_cast<cISC4ViewObject3D*>(view);
    return renderer && object && renderer->AddViewObject(object, 5, 1000);
}
bool NativeRemovePathView(sc4::IQueryPathView* view) {
    sc4::cSC43DRender* renderer = CityRenderer();
    auto* object = dynamic_cast<cISC4ViewObject3D*>(view);
    return renderer && object && renderer->RemoveViewObject(object);
}

}  // namespace native
