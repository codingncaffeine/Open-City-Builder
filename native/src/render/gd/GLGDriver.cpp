#include "render/gd/GLGDriver.h"

#include <epoxy/gl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include "render/GDIndexOps.h"
#include "render/SimGL.h"
#include "render/TextureStageData.h"
#include "render/gd/TextureTable.h"
#include "render/dx7/dx7_to_metal_bridge.h"

namespace gd {

namespace {

// DX7Emulator.metal's gd_vs / gd_fs in GLSL. The state block is dx7::MetalRenderState (std140 lays it out byte for byte:
// five mat4, five vec4, eight uint). Specialisation is by #defines in place of Metal's function constants. The game's
// projection matrices are OpenGL style already (clip z in [-w, w]), so unlike the Metal shader there is no z remap.
const char* const kStateBlock = R"(
layout(std140) uniform RenderState {
    mat4 world, view, projection, texMat0, texMat1;
    vec4 textureFactor, colorMul, ambient, lightDirection, lightDiffuse;
    uint stride, alphaRef, texMode, flags, alphaFunc, fvf, pad0, pad1;
} c;
)";

const char* const kVertexShader = R"(
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 2) in vec2 aUv0;
layout(location = 3) in vec2 aUv1;
out vec4 vColor;
out vec2 vUv0, vUv1;
// The game's viewport as an exact transform inside the full frame (x, y scale; x, y offset, applied in clip space).
uniform vec4 uViewport;
void main() {
    vec4 eye = c.view * (c.world * vec4(aPos, 1.0));
    gl_Position = c.projection * eye;
    gl_Position.xy = gl_Position.xy * uViewport.xy + uViewport.zw * gl_Position.w;
    vColor = aColor * c.colorMul;
    vUv0 = c.stride >= 24u || c.stride == 20u ? aUv0 : vec2(0.0);
    vUv1 = c.stride >= 32u ? aUv1 : vUv0;
    if ((c.texMode & 4u) != 0u) vUv0 = (c.texMat0 * vec4(eye.xyz, 1.0)).xy;
    else if ((c.texMode & 1u) != 0u) vUv0 = (c.texMat0 * vec4(vUv0, 0.0, 1.0)).xy;
    if ((c.texMode & 8u) != 0u) vUv1 = (c.texMat1 * vec4(eye.xyz, 1.0)).xy;
    else if ((c.texMode & 2u) != 0u) vUv1 = (c.texMat1 * vec4(vUv1, 0.0, 1.0)).xy;
}
)";

const char* const kFragmentShader = R"(
in vec4 vColor;
in vec2 vUv0, vUv1;
out vec4 fragColor;
uniform sampler2D t0, t1;
vec4 stageArg(int a, vec4 diffuse, vec4 current, vec4 tex, vec4 tf) {
    int base = a & 0xf;
    vec4 v = base == 0 ? diffuse : (base == 1 ? current : (base == 2 ? tex : tf));
    if ((a & 0x10) != 0) v = 1.0 - v;
    if ((a & 0x20) != 0) v = vec4(v.a);
    return v;
}
vec4 stageOp(int op, vec4 a1, vec4 a2, vec4 current, vec4 diffuse, vec4 tex, vec4 tf) {
    vec4 r;
    switch (op) {
    case 2: r = a1; break;
    case 3: r = a2; break;
    case 4: r = a1 * a2; break;
    case 5: r = a1 * a2 * 2.0; break;
    case 6: r = a1 * a2 * 4.0; break;
    case 7: r = a1 + a2; break;
    case 8: r = a1 + a2 - 0.5; break;
    case 9: r = (a1 + a2 - 0.5) * 2.0; break;
    case 12: r = a1 * diffuse.a + a2 * (1.0 - diffuse.a); break;
    case 13: r = a1 * tex.a + a2 * (1.0 - tex.a); break;
    case 14: r = a1 * tf.a + a2 * (1.0 - tf.a); break;
    case 16: r = a1 * current.a + a2 * (1.0 - current.a); break;
    case 24: r = vec4(4.0 * dot(a1.rgb - 0.5, a2.rgb - 0.5)); break;
    default: r = current; break;
    }
    return clamp(r, 0.0, 1.0);
}
vec4 runStage(int cop, int ca1, int ca2, int aop, int aa1, int aa2, vec4 diffuse, vec4 current, vec4 tex, vec4 tf) {
    vec4 o = current;
    if (cop > 1) o.rgb = stageOp(cop, stageArg(ca1, diffuse, current, tex, tf), stageArg(ca2, diffuse, current, tex, tf), current, diffuse, tex, tf).rgb;
    if (cop > 1 && aop > 1) o.a = stageOp(aop, stageArg(aa1, diffuse, current, tex, tf), stageArg(aa2, diffuse, current, tex, tf), current, diffuse, tex, tf).a;
    return o;
}
void main() {
    vec4 diffuse = vColor;
    vec4 color = diffuse;
#if STAGE0
    color = runStage(S0_COP, S0_CA1, S0_CA2, S0_AOP, S0_AA1, S0_AA2, diffuse, color, texture(t0, vUv0), c.textureFactor);
#endif
#if STAGE1
    color = runStage(S1_COP, S1_CA1, S1_CA2, S1_AOP, S1_AA1, S1_AA2, diffuse, color, texture(t1, vUv1), c.textureFactor);
#endif
#if ALPHA_TEST
    uint a = uint(round(color.a * 255.0));
    bool pass;
    switch (ALPHA_FUNC) {
    case 0: pass = false; break;
    case 1: pass = a <  c.alphaRef; break;
    case 2: pass = a == c.alphaRef; break;
    case 3: pass = a <= c.alphaRef; break;
    case 4: pass = a >  c.alphaRef; break;
    case 5: pass = a != c.alphaRef; break;
    case 6: pass = a >= c.alphaRef; break;
    default: pass = true; break;
    }
    if (!pass) discard;
#endif
    if ((c.texMode & 0x100u) != 0u) { fragColor = vec4(1.0, 0.0, 0.0, 1.0); return; }   // debug: geometry in red
    if ((c.texMode & 0x200u) != 0u) { fragColor = vec4(vColor.rgb, 1.0); return; }        // debug: the vertex colour only
    if ((c.texMode & 0x400u) != 0u) { fragColor = vec4(texture(t0, vUv0).rgb, 1.0); return; }   // debug: the first texture only
    if ((c.texMode & 0x800u) != 0u) { fragColor = vec4(fract(vUv0), 0.0, 1.0); return; }  // debug: the first coordinate set
    fragColor = color;
}
)";

GLenum Factor(eGDBlend b) {
    static const GLenum table[11] = {GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                                     GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA_SATURATE};
    return b < 11 ? table[b] : GL_ONE;
}
GLenum Compare(eGDTestFunc f) {
    static const GLenum table[8] = {GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
    return table[f & 7];
}

// One texture stage as the D3D texture-stage states describe it (cTextureStageData::Apply* emit these).
struct StageCfg {
    std::uint32_t texture = 0;
    int colorOp = 1, colorArg1 = 2, colorArg2 = 1, alphaOp = 1, alphaArg1 = 2, alphaArg2 = 1;   // the shim's start-up values
    int magFilter = 1, minFilter = 1, mipFilter = 1, addressU = 1, addressV = 1;
    bool Active() const { return colorOp > 1; }
};

struct ProgramKey {
    bool alphaTest;
    std::uint8_t alphaFunc;
    bool on[2];
    std::uint8_t ops[2][6];   // colour op, args, alpha op, args
    bool operator<(const ProgramKey& o) const { return std::memcmp(this, &o, sizeof *this) < 0; }
};

GLuint Compile(GLenum type, const std::string& source) {
    const GLuint s = glCreateShader(type);
    const char* text = source.c_str();
    glShaderSource(s, 1, &text, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        std::fprintf(stderr, "cGLGDriver shader error: %s\n", log);
    }
    return s;
}

struct Texture {
    GLuint name = 0;
    std::uint32_t format = 0, levels = 1;
};

}  // namespace

struct cGLGDriver::Impl {
    std::uint32_t width = 0, height = 0;             // the game's (logical) resolution
    std::uint32_t pixelWidth = 0, pixelHeight = 0;   // the frame target
    // logical -> frame target pixels (x edges by the width ratio, y by the height ratio; rounded so adjacent rectangles meet)
    int PX(std::uint32_t x) const { return int(std::lround(double(x) * pixelWidth / width)); }
    int PY(std::uint32_t y) const { return int(std::lround(double(y) * pixelHeight / height)); }
    GLuint frameFbo = 0, frameColor = 0, frameDepth = 0;
    GLuint vao = 0, vbo = 0, ibo = 0, ubo = 0;
    GLuint whiteTexture = 0;

    float colorMulRgba[4] = {1, 1, 1, 1};
    dx7::StateRecorder recorder;   // transforms, texture factor, colour multiplier, texture matrices / coordinate sources

    // viewport / scissor in the game's GL-style coordinates (origin bottom-left)
    std::uint32_t vx = 0, vy = 0, vw = 0, vh = 0;
    bool scissor = false;

    eGDMatrixTarget matrixTarget = 0;

    // interleaved arrays
    eGDVertexFormat format = 0;
    int stride = 0;
    const std::uint8_t* base = nullptr;
    int error = 0;

    std::map<ProgramKey, GLuint> programs;
    std::map<GLuint, GLint> viewportUniform;   // program -> uViewport
    std::map<std::uint64_t, GLuint> samplers;
    std::uint32_t draws = 0;

    // ---- textures and stages ----
    struct Backend final : ITextureBackend {
        Impl* m;
        explicit Backend(Impl* impl) : m(impl) {}
        bool CreateSurface(std::uint32_t texId, std::uint32_t format, std::uint32_t w, std::uint32_t h, std::uint32_t mips) override {
            GLenum internal = GL_RGBA8;
            if (format == kFmtDXT1) internal = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
            else if (format == kFmtDXT3) internal = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
            else if (format == kFmtDXT5) internal = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
            std::uint32_t maxLevels = 1;
            for (std::uint32_t d = std::max(w, h); d > 1; d >>= 1) ++maxLevels;
            const std::uint32_t levels = std::max<std::uint32_t>(1, std::min(mips, maxLevels));
            DestroySurface(texId);
            Texture t;
            t.format = format;
            t.levels = levels;
            glGenTextures(1, &t.name);
            glBindTexture(GL_TEXTURE_2D, t.name);
            glTexStorage2D(GL_TEXTURE_2D, GLsizei(levels), internal, GLsizei(w), GLsizei(h));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, GLint(levels - 1));
            if (glGetError() != GL_NO_ERROR) { glDeleteTextures(1, &t.name); return false; }
            m->textures[texId] = t;
            return true;
        }
        void DestroySurface(std::uint32_t texId) override {
            auto it = m->textures.find(texId);
            if (it == m->textures.end()) return;
            glDeleteTextures(1, &it->second.name);
            m->textures.erase(it);
        }
        bool UploadLevel(std::uint32_t texId, std::uint32_t level, std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h, const std::uint8_t* data, bool compressed) override {
            auto it = m->textures.find(texId);
            if (it == m->textures.end() || level >= it->second.levels) return false;
            glBindTexture(GL_TEXTURE_2D, it->second.name);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            if (compressed) {
                const std::uint32_t f = it->second.format;
                const GLenum internal = f == kFmtDXT1 ? GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
                                        : f == kFmtDXT3 ? GL_COMPRESSED_RGBA_S3TC_DXT3_EXT : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
                const GLsizei bytes = GLsizei(((w + 3) / 4) * ((h + 3) / 4) * (f == kFmtDXT1 ? 8 : 16));
                glCompressedTexSubImage2D(GL_TEXTURE_2D, GLint(level), GLint(x), GLint(y), GLsizei(w), GLsizei(h), internal, bytes, data);
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, GLint(level), GLint(x), GLint(y), GLsizei(w), GLsizei(h), GL_BGRA, GL_UNSIGNED_BYTE, data);
            }
            return true;
        }
    } backend{this};
    cTextureTable table{&backend};
    std::unique_ptr<cVideoModeList> modeList;

    // ---- backing store: persistent colour + depth copies of the frame (IBackingStore: top-left origin) ----
    struct Store final : IBackingStore {
        Impl* m;
        GLuint fbo = 0, color = 0, depth = 0;
        std::uint32_t w = 0, h = 0;
        explicit Store(Impl* impl) : m(impl) {}
        bool Create(std::uint32_t width, std::uint32_t height) override {
            if (width == 0 || height == 0) return false;
            Destroy();
            w = std::uint32_t(m->PX(width));   // the store holds pixels; its rectangles arrive logical
            h = std::uint32_t(m->PY(height));
            m->MakeTarget(w, h, fbo, color, depth);
            return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        }
        void Destroy() override {
            if (fbo) glDeleteFramebuffers(1, &fbo);
            if (color) glDeleteTextures(1, &color);
            if (depth) glDeleteTextures(1, &depth);
            fbo = color = depth = 0;
            w = h = 0;
            glBindFramebuffer(GL_FRAMEBUFFER, m->frameFbo);
        }
        bool Exists() const override { return fbo != 0; }
        void Copy(bool toStore, int sx, int sy, int cw, int ch, int dx, int dy) {
            if (!Exists() || cw <= 0 || ch <= 0 || sx < 0 || sy < 0 || dx < 0 || dy < 0) return;
            // logical rectangles -> pixels
            const int x0 = m->PX(std::uint32_t(sx)), y0 = m->PY(std::uint32_t(sy));
            const int x1 = m->PX(std::uint32_t(dx)), y1 = m->PY(std::uint32_t(dy));
            cw = m->PX(std::uint32_t(sx + cw)) - x0;
            ch = m->PY(std::uint32_t(sy + ch)) - y0;
            sx = x0; sy = y0; dx = x1; dy = y1;
            const int srcH = int(toStore ? m->pixelHeight : h), dstH = int(toStore ? h : m->pixelHeight);
            const int srcW = int(toStore ? m->pixelWidth : w), dstW = int(toStore ? w : m->pixelWidth);
            cw = std::min(cw, std::min(srcW - sx, dstW - dx));
            ch = std::min(ch, std::min(srcH - sy, dstH - dy));
            if (cw <= 0 || ch <= 0) return;
            // top-left rectangles -> GL's bottom-left rows
            const int gsy = srcH - sy - ch, gdy = dstH - dy - ch;
            glBindFramebuffer(GL_READ_FRAMEBUFFER, toStore ? m->frameFbo : fbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, toStore ? fbo : m->frameFbo);
            glDisable(GL_SCISSOR_TEST);
            glBlitFramebuffer(sx, gsy, sx + cw, gsy + ch, dx, gdy, dx + cw, gdy + ch, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST);
            glBindFramebuffer(GL_FRAMEBUFFER, m->frameFbo);
        }
        void SaveFromFrame(int sx, int sy, int cw, int ch, int dx, int dy) override { Copy(true, sx, sy, cw, ch, dx, dy); }
        void RestoreToFrame(int sx, int sy, int cw, int ch, int dx, int dy) override { Copy(false, sx, sy, cw, ch, dx, dy); }
    } store{this};
    sGDMode currentMode{};
    std::map<std::uint32_t, Texture> textures;
    nSGLDX7::cTextureStageData records[2];
    StageCfg cfg[2];
    bool anyStageDirty = false, bindMode = false;
    std::uint32_t stageCount = 2;

    // Applies a stage record's D3D texture-stage states to the stage configuration.
    struct StageHost final : nSGLDX7::ITextureStageHost {
        Impl* m;
        explicit StageHost(Impl* impl) : m(impl) {}
        bool IsTextureUsable(std::uint32_t id) override { return m->table.HasTexture(id); }
        void BindTexture(int stage, std::uint32_t id) override { m->cfg[stage & 1].texture = id; }
        void SetTextureStageState(int stage, int state, int value) override {
            StageCfg& c = m->cfg[stage & 1];
            switch (state) {
            case nSGLDX7::kTssColorOp: c.colorOp = value; break;
            case nSGLDX7::kTssColorArg1: c.colorArg1 = value; break;
            case nSGLDX7::kTssColorArg2: c.colorArg2 = value; break;
            case nSGLDX7::kTssAlphaOp: c.alphaOp = value; break;
            case nSGLDX7::kTssAlphaArg1: c.alphaArg1 = value; break;
            case nSGLDX7::kTssAlphaArg2: c.alphaArg2 = value; break;
            case nSGLDX7::kTssAddressU: c.addressU = value; break;
            case nSGLDX7::kTssAddressV: c.addressV = value; break;
            case nSGLDX7::kTssMagFilter: c.magFilter = value; break;
            case nSGLDX7::kTssMinFilter: c.minFilter = value; break;
            case nSGLDX7::kTssMipFilter: c.mipFilter = value; break;
            default: break;
            }
        }
    } stageHost{this};

    void ApplyDirtyStages() {
        if (!anyStageDirty) return;
        for (std::uint32_t i = 0; i < stageCount; ++i)
            if (records[i].dirty) records[i].ApplyState(static_cast<int>(i), stageHost);
        anyStageDirty = false;
    }

    // A colour (RGBA8) + depth (32-bit float) target; leaves its framebuffer bound.
    void MakeTarget(std::uint32_t w, std::uint32_t h, GLuint& fbo, GLuint& color, GLuint& depth) {
        glGenTextures(1, &color);
        glBindTexture(GL_TEXTURE_2D, color);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, GLsizei(w), GLsizei(h));
        glGenTextures(1, &depth);
        glBindTexture(GL_TEXTURE_2D, depth);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH_COMPONENT32F, GLsizei(w), GLsizei(h));
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
    }
    void MakeFrame() {
        if (frameFbo) {
            glDeleteFramebuffers(1, &frameFbo);
            glDeleteTextures(1, &frameColor);
            glDeleteTextures(1, &frameDepth);
        }
        MakeTarget(pixelWidth, pixelHeight, frameFbo, frameColor, frameDepth);
        glDisable(GL_SCISSOR_TEST);
        glDepthMask(GL_TRUE);
        glClearColor(0, 0, 0, 0);
        glClearDepth(1.0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }

    GLuint Program(const ProgramKey& k) {
        auto it = programs.find(k);
        if (it != programs.end()) return it->second;
        std::string defines = "#version 330 core\n";
        char line[96];
        std::snprintf(line, sizeof line, "#define ALPHA_TEST %d\n#define ALPHA_FUNC %d\n", int(k.alphaTest), int(k.alphaFunc));
        defines += line;
        static const char* const kNames[6] = {"COP", "CA1", "CA2", "AOP", "AA1", "AA2"};
        for (int st = 0; st < 2; ++st) {
            std::snprintf(line, sizeof line, "#define STAGE%d %d\n", st, int(k.on[st]));
            defines += line;
            for (int i = 0; i < 6; ++i) {
                std::snprintf(line, sizeof line, "#define S%d_%s %d\n", st, kNames[i], int(k.ops[st][i]));
                defines += line;
            }
        }
        const GLuint vs = Compile(GL_VERTEX_SHADER, defines + kStateBlock + kVertexShader);
        const GLuint fs = Compile(GL_FRAGMENT_SHADER, defines + kStateBlock + kFragmentShader);
        const GLuint p = glCreateProgram();
        glAttachShader(p, vs);
        glAttachShader(p, fs);
        glLinkProgram(p);
        glDeleteShader(vs);
        glDeleteShader(fs);
        GLint ok = 0;
        glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[2048];
            glGetProgramInfoLog(p, sizeof log, nullptr, log);
            std::fprintf(stderr, "cGLGDriver program error: %s\n", log);
        }
        glUniformBlockBinding(p, glGetUniformBlockIndex(p, "RenderState"), 0);
        glUseProgram(p);
        glUniform1i(glGetUniformLocation(p, "t0"), 0);
        glUniform1i(glGetUniformLocation(p, "t1"), 1);
        viewportUniform[p] = glGetUniformLocation(p, "uViewport");
        programs[k] = p;
        return p;
    }
    GLuint Sampler(const StageCfg& c) {
        const std::uint64_t key = std::uint64_t(c.magFilter) | std::uint64_t(c.minFilter) << 4 | std::uint64_t(c.mipFilter) << 8 |
                                  std::uint64_t(c.addressU) << 12 | std::uint64_t(c.addressV) << 16;
        auto it = samplers.find(key);
        if (it != samplers.end()) return it->second;
        GLuint s = 0;
        glGenSamplers(1, &s);
        const bool linMin = c.minFilter == 2;
        GLenum minFilter = linMin ? GL_LINEAR : GL_NEAREST;
        if (c.mipFilter == 3) minFilter = linMin ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
        else if (c.mipFilter == 2) minFilter = linMin ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
        glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, c.magFilter == 2 ? GL_LINEAR : GL_NEAREST);
        glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, GLint(minFilter));
        glSamplerParameteri(s, GL_TEXTURE_WRAP_S, c.addressU == 3 ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        glSamplerParameteri(s, GL_TEXTURE_WRAP_T, c.addressV == 3 ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        samplers[key] = s;
        return s;
    }
    GLuint DefaultSampler() {
        StageCfg c;
        return Sampler(c);
    }

    void SetRasterState(const StateMirror& m) {
        if (m.cull && !std::getenv("SC4_GDNOCULL")) {
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
            glFrontFace(GL_CCW);   // Enable(CullFace) = D3DCULL_CW -> GL FrontFace(CCW) + cull back
        } else {
            glDisable(GL_CULL_FACE);
        }
        // The GL viewport is always the whole frame: the game's viewport is an exact (fractional) transform in the vertex
        // shader (ViewportTransform), so every draw shares one logical -> pixel mapping. Rounding each viewport's origin
        // to whole pixels shifted draws with different viewports by up to half a pixel, and at a fractional UI scale
        // adjacent region-map tiles left one-pixel black columns between them.
        glViewport(0, 0, GLsizei(pixelWidth), GLsizei(pixelHeight));
        if (scissor) {
            const int x0 = PX(vx), y0 = PY(vy), x1 = PX(vx + vw), y1 = PY(vy + vh);
            glEnable(GL_SCISSOR_TEST);
            glScissor(x0, y0, x1 - x0, y1 - y0);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }
    }
    // NDC of the game's viewport (vx, vy bottom-left, vw x vh logical) -> NDC of the whole frame: scale x, y and offset x, y.
    void ViewportTransform(float out[4]) const {
        out[0] = float(vw) / float(width);
        out[1] = float(vh) / float(height);
        out[2] = float(2.0 * vx + vw) / float(width) - 1.0f;
        out[3] = float(2.0 * vy + vh) / float(height) - 1.0f;
    }
    void SetDepthState(bool test, eGDTestFunc func, bool write) {
        if (std::getenv("SC4_GDNODEPTH")) { test = false; write = false; }
        // depth is only written while the depth test is on; GL skips depth writes too with the test off, so keep it on with ALWAYS
        if (test) {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(Compare(func));
            glDepthMask(write ? GL_TRUE : GL_FALSE);
        } else {
            glDisable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
        }
    }
};

// ---------------------------------------------------------------------------------------------------------------------

cGLGDriver::cGLGDriver() : impl_(new Impl) { impl_->currentMode.id = -1; }
cGLGDriver::~cGLGDriver() = default;   // GL objects go with the context

std::uint32_t cGLGDriver::PixelWidth() const { return impl_->pixelWidth; }
std::uint32_t cGLGDriver::PixelHeight() const { return impl_->pixelHeight; }

std::unique_ptr<cGLGDriver> cGLGDriver::Create(std::uint32_t width, std::uint32_t height, std::uint32_t pixelWidth,
                                               std::uint32_t pixelHeight, IModeSource* modes) {
    if (epoxy_gl_version() < 33) {
        std::fprintf(stderr, "cGLGDriver: OpenGL 3.3 is required (the context offers %d.%d)\n", epoxy_gl_version() / 10, epoxy_gl_version() % 10);
        return nullptr;
    }
    if (!epoxy_has_gl_extension("GL_EXT_texture_compression_s3tc"))
        std::fprintf(stderr, "cGLGDriver: warning: no S3TC texture compression; compressed textures will not show\n");
    std::unique_ptr<cGLGDriver> d(new cGLGDriver);
    Impl& m = *d->impl_;
    m.width = width;
    m.height = height;
    m.pixelWidth = pixelWidth ? pixelWidth : width;
    m.pixelHeight = pixelHeight ? pixelHeight : height;
    m.vw = width;
    m.vh = height;
    m.MakeFrame();
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::fprintf(stderr, "cGLGDriver: the frame target is incomplete\n");
        return nullptr;
    }
    glGenVertexArrays(1, &m.vao);
    glBindVertexArray(m.vao);
    glGenBuffers(1, &m.vbo);
    glGenBuffers(1, &m.ibo);
    glGenBuffers(1, &m.ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, m.ubo);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(dx7::MetalRenderState), nullptr, GL_STREAM_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, m.ubo);
    glGenTextures(1, &m.whiteTexture);
    glBindTexture(GL_TEXTURE_2D, m.whiteTexture);
    const std::uint32_t white = 0xffffffff;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_BGRA, GL_UNSIGNED_BYTE, &white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    m.table.Init();
    // The mode the window already is: this is always the "hardware driver" (the application reads byte 0 of +0x30 as
    // "accelerated" and would otherwise fall back to the software one); +0x2e, the 2-D blit capability, stays 0.
    m.currentMode = sGDMode{};
    m.currentMode.id = 0;
    m.currentMode.width = width;
    m.currentMode.height = height;
    m.currentMode.bitsPerPixel = 32;
    m.currentMode.flags[0] = 1u << 16;   // byte 2 = 1 for every entry; byte 0 (full screen) = 0: windowed
    m.currentMode.reserved30 = 1;
    if (modes) m.modeList = std::make_unique<cVideoModeList>(modes);
    return d;
}

IBackingStore* cGLGDriver::BackingStore() { return &impl_->store; }

std::uint32_t cGLGDriver::DrawCalls() const { return impl_->draws; }
std::uint32_t cGLGDriver::PipelineCount() const { return static_cast<std::uint32_t>(impl_->programs.size()); }

// ---- state ----
void cGLGDriver::ColorMask(bool enable) { cGDriverBase::ColorMask(enable); }   // the shipped driver only flushes here
void cGLGDriver::DepthFunc(eGDTestFunc f) { cGDriverBase::DepthFunc(f); }
void cGLGDriver::DepthMask(bool e) { cGDriverBase::DepthMask(e); }
void cGLGDriver::BlendFunc(eGDBlend s, eGDBlend d) { cGDriverBase::BlendFunc(s, d); }
void cGLGDriver::AlphaFunc(eGDTestFunc f, float ref) { cGDriverBase::AlphaFunc(f, ref); }
void cGLGDriver::Enable(eGDDriverState s) {
    cGDriverBase::Enable(s);
    if (s == state::Texturing) MarkStage(true);
}
void cGLGDriver::Disable(eGDDriverState s) {
    cGDriverBase::Disable(s);
    if (s == state::Texturing) MarkStage(false);
}

// ---- textures (nSGLDX7::cGDriver slots; the stage records are the shipped cTextureStageData) ----
void cGLGDriver::MarkStage(bool enabled) {
    Impl& m = *impl_;
    auto& r = m.records[mirror_.currentStage & 1];
    r.textureEnabled = enabled ? 1 : 0;
    r.dirty = 1;
    m.anyStageDirty = true;
}

void cGLGDriver::TexStageCoord(eGDTexCoordSource source) {
    // SGLDX7Driver: the table index is ((source & 8) << 28) | (source >> 4): 0 = the vertex's own coordinate set, 1 = camera-space position
    const std::uint32_t index = ((source & 8) << 28) | (source >> 4);
    if (impl_->stageCount > mirror_.currentStage && mirror_.currentStage < 2) impl_->recorder.SetStageCameraSpaceCoords(mirror_.currentStage, index == 1);
}

void cGLGDriver::TexStageMatrix(const float* m, std::uint32_t, std::uint32_t, eGDTexMatFlags) {
    const std::uint32_t stage = mirror_.currentStage;
    if (stage >= 2) return;
    impl_->recorder.SetStageTransform(stage, m);
}

void cGLGDriver::ColorMultiplier(float r, float g, float b) { impl_->colorMulRgba[0] = r; impl_->colorMulRgba[1] = g; impl_->colorMulRgba[2] = b; impl_->recorder.SetColorMultiplier(impl_->colorMulRgba); }
void cGLGDriver::AlphaMultiplier(float a) { impl_->colorMulRgba[3] = a; impl_->recorder.SetColorMultiplier(impl_->colorMulRgba); }

void cGLGDriver::TexStage(std::uint32_t stage) {
    if (stage < impl_->stageCount) cGDriverBase::TexStage(stage);
    else impl_->error = 2;
}

void cGLGDriver::GenTextures(int count, std::uint32_t* ids) {
    for (int i = 0; i < count; ++i) ids[i] = impl_->table.NewTexture();
}
void cGLGDriver::DeleteTextures(int count, const std::uint32_t* ids) {
    for (int i = 0; i < count; ++i)
        if (ids[i] != 0) impl_->table.FreeTexture(ids[i]);
}
bool cGLGDriver::IsTexture(std::uint32_t id) { return impl_->table.IsValid(id); }

void cGLGDriver::BindTexture(eGDTextureTarget target, std::uint32_t id) {
    Impl& m = *impl_;
    if (target != 0) {
        m.error = 1;
        return;
    }
    m.bindMode = true;
    TextureParams p;
    if (!m.table.GetTextureParams(id, p)) m.table.GetTextureParams(0, p);
    auto& r = m.records[mirror_.currentStage & 1];
    m.anyStageDirty = true;
    r.dirty = 1;
    r.appliedEnvMode = 0xffffffffu;
    r.magFilter = p.magFilter;
    r.minFilter = p.minFilter;
    r.wrapS = p.wrapS;
    r.wrapT = p.wrapT;
    r.textureId = id;
    r.textureHasAlpha = m.table.HasAlpha(id) ? 1 : 0;
}

void cGLGDriver::SetTexture(std::uint32_t id, std::uint32_t stage) {
    Impl& m = *impl_;
    auto& r = m.records[stage & 1];
    if (r.textureId == id) return;
    m.bindMode = false;
    m.anyStageDirty = true;
    r.dirty = 1;
    r.appliedEnvMode = 0xffffffffu;
    r.textureId = id;
    r.textureHasAlpha = m.table.HasAlpha(id) ? 1 : 0;
}
std::uint32_t cGLGDriver::GetTexture(std::uint32_t stage) const { return impl_->records[stage & 1].textureId; }

void cGLGDriver::TexParameter(eGDTextureTarget, eGDTextureParamType param, eGDTextureParam value) {
    Impl& m = *impl_;
    auto& r = m.records[mirror_.currentStage & 1];
    if (m.bindMode && !m.table.SetTextureParam(r.textureId, param, value)) {
        m.error = 1;
        return;
    }
    std::uint32_t* field = nullptr;
    switch (param) {
    case 0: field = &r.magFilter; break;
    case 1: field = &r.minFilter; break;
    case 2: field = &r.wrapS; break;
    case 3: field = &r.wrapT; break;
    default: break;
    }
    if (field) {
        if (*field == value) return;
        *field = value;
    }
    m.anyStageDirty = true;
    r.dirty = 1;
}

void cGLGDriver::TexEnv(eGDTextureEnvTarget target, eGDTextureEnvParamType param, eGDTextureEnvModeParam mode) {
    cGDriverBase::TexEnv(target, param, mode);   // keeps the mirror the render-state assertion checks
    Impl& m = *impl_;
    auto& r = m.records[mirror_.currentStage & 1];
    m.anyStageDirty = true;
    r.dirty = 1;
    r.envMode = mode;
}

// The texture environment colour: the texture factor the stage operations read as their TFACTOR argument
// (cGDriver::TexEnv(float*) sets D3DRS_TEXTUREFACTOR from it).
void cGLGDriver::TexEnv(eGDTextureEnvTarget, eGDTextureEnvParamType, const float* values) {
    impl_->recorder.SetTextureFactor(values);
}

// cGDriver::SetCombiner (the DX7 driver's decoding): the stage switches to the combiner environment (mode 4) and takes
// the record's colour / alpha operations, sources, operands and scales.
void cGLGDriver::SetCombiner(const cGDCombiner& combiner, std::uint32_t stageIndex) {
    Impl& m = *impl_;
    auto& stage = m.records[stageIndex & 1];
    stage.envMode = 4;
    stage.dirty = 1;
    stage.appliedEnvMode = 0xffffffffu;
    m.anyStageDirty = true;
    const auto* raw = reinterpret_cast<const std::uint8_t*>(&combiner);
    if (raw[0] == 0xff) return;
    // the two operand tables of SetCombiner (0x100a26770 / 0x100a26780, nSGLDX7::lookup in the DX7 driver)
    static constexpr std::uint32_t kColorOperand[4] = {2, 3, 4, 5}, kAlphaOperand[4] = {4, 5, 4, 5};
    stage.rgbCombine = raw[0];
    stage.alphaCombine = raw[4];
    for (int i = 0; i < 3; ++i) {
        stage.rgbSource[i] = raw[1 + i] & 0xf;
        stage.alphaSource[i] = raw[5 + i] & 0xf;
        stage.rgbOperand[i] = kColorOperand[(raw[1 + i] >> 4) & 3];
        stage.alphaOperand[i] = kAlphaOperand[(raw[5 + i] >> 4) & 3];
    }
    stage.rgbScale = raw[8];
    stage.alphaScale = raw[9];
}

std::uint32_t cGLGDriver::CreateTexture(eGDInternalTextureFormat format, std::uint32_t width, std::uint32_t height, std::uint32_t mips, eGDTextureHintFlags hint) {
    Impl& m = *impl_;
    const std::uint32_t id = m.table.NewTexture();
    if (!m.table.CreateTexture(id, hint, format, width, height, mips)) m.error = 3;
    return id;
}

void cGLGDriver::LoadTextureLevel(std::uint32_t id, std::uint32_t level, std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h,
                                  eGDTextureFormat format, eGDType type, std::uint32_t rowLength, const void* pixels) {
    Impl& m = *impl_;
    if (!m.table.LoadTextureLevel(id, level, x, y, w, h, format, type, rowLength, pixels)) m.error = 3;
}
void cGLGDriver::ClearColor(float r, float g, float b, float a) { cGDriverBase::ClearColor(r, g, b, a); }
void cGLGDriver::Flush() {}   // draws are issued immediately; nothing is queued
int cGLGDriver::GetError() { int e = impl_->error; impl_->error = 0; return e; }

int cGLGDriver::VertexFormatStride(eGDVertexFormat f) const { return nRZSimGL::StandardVFStride(f); }

void cGLGDriver::MatrixMode(eGDMatrixTarget t) { impl_->matrixTarget = t; }
void cGLGDriver::LoadMatrix(const float* m) {
    Impl& s = *impl_;
    if (s.matrixTarget == 0) s.recorder.SetTransform(dx7::kTransformView, m);
    else if (s.matrixTarget == 1) s.recorder.SetTransform(dx7::kTransformProjection, m);   // other targets are ignored by the shipped driver
}
void cGLGDriver::LoadIdentity() {
    static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    LoadMatrix(identity);
}

void cGLGDriver::SetViewport() {
    Impl& s = *impl_;
    s.vx = s.vy = 0;
    s.vw = s.width;
    s.vh = s.height;
    s.scissor = false;
}
void cGLGDriver::SetViewport(std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h) {
    Impl& s = *impl_;
    s.vx = x; s.vy = y; s.vw = w; s.vh = h;
    s.scissor = true;
}
void cGLGDriver::GetViewport(std::uint32_t out[4]) {
    const Impl& s = *impl_;
    out[0] = s.vx; out[1] = s.vy; out[2] = s.vx + s.vw; out[3] = s.vy + s.vh;   // (x, y, x+w, y+h)
}

// ---- clear ----
// The shipped driver clears with D3D Clear(): colour from ClearColor (quantised to 8 bits), depth from ClearDepth. The
// shim implements it with glClear, so an active scissor (SetViewport(x,y,w,h)) limits it; colour/depth write masks and the
// depth test do not matter.
void cGLGDriver::Clear(std::uint32_t mask) {
    Impl& s = *impl_;
    const bool doColor = (mask & kClearColor) != 0, doDepth = (mask & kClearDepth) != 0;
    if (!doColor && !doDepth) return;
    const std::uint32_t argb = mirror_.clearColor;
    glBindFramebuffer(GL_FRAMEBUFFER, s.frameFbo);
    if (s.scissor) {
        const int x0 = s.PX(s.vx), y0 = s.PY(s.vy);
        glEnable(GL_SCISSOR_TEST);
        glScissor(x0, y0, s.PX(s.vx + s.vw) - x0, s.PY(s.vy + s.vh) - y0);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glClearColor(float(argb >> 16 & 0xff) / 255.0f, float(argb >> 8 & 0xff) / 255.0f, float(argb & 0xff) / 255.0f, float(argb >> 24) / 255.0f);
    glClearDepth(mirror_.clearDepth);
    glClear((doColor ? GL_COLOR_BUFFER_BIT : 0) | (doDepth ? GL_DEPTH_BUFFER_BIT : 0));
    ++s.draws;
}

// ---- drawing ----
void cGLGDriver::InterleavedArrays(eGDVertexFormat format, int stride, const void* data) {
    Impl& s = *impl_;
    s.format = format;
    const int natural = nRZSimGL::StandardVFStride(format);
    s.stride = stride == 0 ? natural : stride;
    s.base = static_cast<const std::uint8_t*>(data);
}

void cGLGDriver::DrawArrays(eGDPrimType mode, int first, int count) {
    Impl& s = *impl_;
    if (count < 1 || mode > 7 || !s.base) return;
    if ((s.format != vf::V3F_C4UB && s.format != vf::V3F_T2F && s.format != vf::V3F_C4UB_T2F && s.format != vf::V3F_C4UB_2T2F)) {
        { char note[64]; std::snprintf(note, sizeof note, "DrawArrays(vertex format %#x)", unsigned(s.format)); Unsupported(note); }
        return;
    }
    if (mode == prim::Points || mode == prim::Lines || mode == prim::LineStrip) {
        Unsupported("DrawArrays(points/lines)");
        return;
    }
    glBindVertexArray(s.vao);
    glBindBuffer(GL_ARRAY_BUFFER, s.vbo);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(count) * s.stride, s.base + std::size_t(first) * s.stride, GL_STREAM_DRAW);
    std::vector<std::uint16_t> indices;
    GLenum type = GL_TRIANGLES;
    switch (mode) {
    case prim::Triangles: break;
    case prim::TriangleStrip: type = GL_TRIANGLE_STRIP; break;
    case prim::TriangleFan:
        indices.resize(static_cast<std::size_t>(count - 2) * 3);
        nSGLDX7::GenerateIndexedTriListForTriangleFan(static_cast<int>(indices.size()), indices.data());
        break;
    case prim::Quads:
        indices.resize(static_cast<std::size_t>(count >> 1) * 3);
        nSGLDX7::GenerateIndexedTriListForQuads(count, indices.data());
        break;
    case prim::QuadStrip:
        indices.resize(static_cast<std::size_t>(count - 2) * 3);
        nSGLDX7::GenerateIndexedTriListForQuadStrip(count, indices.data());
        break;
    }
    Encode(type, count, indices.empty() ? nullptr : indices.data(), static_cast<int>(indices.size()));
}

void cGLGDriver::DrawElements(eGDPrimType mode, int count, eGDType indexType, const void* indices) {
    Impl& s = *impl_;
    if (count < 1 || mode > 7 || !s.base) return;
    if ((s.format != vf::V3F_C4UB && s.format != vf::V3F_T2F && s.format != vf::V3F_C4UB_T2F && s.format != vf::V3F_C4UB_2T2F)) {
        { char note[64]; std::snprintf(note, sizeof note, "DrawElements(vertex format %#x)", unsigned(s.format)); Unsupported(note); }
        return;
    }
    if (mode != prim::Triangles || (indexType != 1 && indexType != 3 && indexType != 5)) {
        Unsupported("DrawElements(mode/index type)");
        return;
    }
    std::vector<std::uint32_t> idx(static_cast<std::size_t>(count));
    std::uint32_t lo = 0xffffffff, hi = 0;
    for (int i = 0; i < count; ++i) {
        std::uint32_t v = indexType == 1 ? static_cast<const std::uint8_t*>(indices)[i]
                          : indexType == 3 ? static_cast<const std::uint16_t*>(indices)[i]
                                           : static_cast<const std::uint32_t*>(indices)[i];
        idx[static_cast<std::size_t>(i)] = v;
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    const int vertexCount = static_cast<int>(hi - lo + 1);
    glBindVertexArray(s.vao);
    glBindBuffer(GL_ARRAY_BUFFER, s.vbo);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(vertexCount) * s.stride, s.base + std::size_t(lo) * s.stride, GL_STREAM_DRAW);
    std::vector<std::uint16_t> rebased(idx.size());
    for (std::size_t i = 0; i < idx.size(); ++i) rebased[i] = static_cast<std::uint16_t>(idx[i] - lo);
    Encode(GL_TRIANGLES, vertexCount, rebased.data(), count);
}

// Issues one draw of the vertices just uploaded to the stream buffer, with the mirrored state.
void cGLGDriver::Encode(unsigned primitive, int vertexCount, const std::uint16_t* indices, int indexCount) {
    Impl& s = *impl_;
    glBindFramebuffer(GL_FRAMEBUFFER, s.frameFbo);
    s.ApplyDirtyStages();
    ProgramKey key{};
    key.alphaTest = mirror_.alphaTest;
    key.alphaFunc = static_cast<std::uint8_t>(mirror_.alphaFunc);
    for (int st = 0; st < 2; ++st) {
        const StageCfg& c = s.cfg[st];
        const bool on = c.Active() && s.table.HasTexture(c.texture) && s.textures.count(c.texture);
        key.on[st] = on;
        if (on) {
            const std::uint8_t ops[6] = {std::uint8_t(c.colorOp), std::uint8_t(c.colorArg1), std::uint8_t(c.colorArg2),
                                         std::uint8_t(c.alphaOp), std::uint8_t(c.alphaArg1), std::uint8_t(c.alphaArg2)};
            std::memcpy(key.ops[st], ops, 6);
        }
    }
    const GLuint program = s.Program(key);
    glUseProgram(program);
    float viewport[4];
    s.ViewportTransform(viewport);
    glUniform4f(s.viewportUniform[program], viewport[0], viewport[1], viewport[2], viewport[3]);
    if (mirror_.blend) {
        glEnable(GL_BLEND);
        glBlendFunc(Factor(mirror_.blendSrc), Factor(mirror_.blendDst));
    } else {
        glDisable(GL_BLEND);
    }
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    s.SetDepthState(mirror_.depthTest, mirror_.depthFunc, mirror_.depthWrite);
    s.SetRasterState(mirror_);

    dx7::MetalRenderState dc = s.recorder.Snapshot();
    dc.stride = static_cast<std::uint32_t>(s.stride);
    dc.alphaRef = AlphaRefByte(mirror_.alphaRef);
    if (std::getenv("SC4_GDRED")) dc.texMode |= 0x100u;
    if (const char* mode = std::getenv("SC4_GDMODE")) dc.texMode |= static_cast<std::uint32_t>(std::atoi(mode));
    glBindBuffer(GL_UNIFORM_BUFFER, s.ubo);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof dc, &dc);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, s.ubo);

    // V3F, C4UB, then one or two T2F sets (the Metal shader reads the same bytes by stride). V3F_T2F has no colour: the
    // vertices are white, as Direct3D lights a vertex without a diffuse component, and its T2F follows the position.
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, s.stride, reinterpret_cast<const void*>(0));
    if (s.format == vf::V3F_T2F) {
        glDisableVertexAttribArray(1);
        glVertexAttrib4f(1, 1, 1, 1, 1);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, s.stride, reinterpret_cast<const void*>(12));
        glDisableVertexAttribArray(3);
        glVertexAttrib2f(3, 0, 0);
    } else {
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, s.stride, reinterpret_cast<const void*>(12));
        if (s.stride >= 24) { glEnableVertexAttribArray(2); glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, s.stride, reinterpret_cast<const void*>(16)); }
        else { glDisableVertexAttribArray(2); glVertexAttrib2f(2, 0, 0); }
        if (s.stride >= 32) { glEnableVertexAttribArray(3); glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, s.stride, reinterpret_cast<const void*>(24)); }
        else { glDisableVertexAttribArray(3); glVertexAttrib2f(3, 0, 0); }
    }

    for (int st = 0; st < 2; ++st) {
        glActiveTexture(GL_TEXTURE0 + st);
        glBindTexture(GL_TEXTURE_2D, key.on[st] ? s.textures[s.cfg[st].texture].name : s.whiteTexture);
        glBindSampler(GLuint(st), key.on[st] ? s.Sampler(s.cfg[st]) : s.DefaultSampler());
    }
    glActiveTexture(GL_TEXTURE0);
    if (indices) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s.ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(indexCount) * 2, indices, GL_STREAM_DRAW);
        glDrawElements(primitive, indexCount, GL_UNSIGNED_SHORT, nullptr);
    } else {
        glDrawArrays(primitive, 0, vertexCount);
    }
    ++s.draws;
}

// ---- presentation ----
void cGLGDriver::Present(int x, int y, int width, int height) {
    Impl& s = *impl_;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s.frameFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    const bool exact = width == int(s.pixelWidth) && height == int(s.pixelHeight);
    glBlitFramebuffer(0, 0, GLint(s.pixelWidth), GLint(s.pixelHeight), x, y, x + width, y + height, GL_COLOR_BUFFER_BIT,
                      exact ? GL_NEAREST : GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, s.frameFbo);
}

bool cGLGDriver::ReadBack(std::vector<std::uint8_t>& bgra, std::uint32_t& width, std::uint32_t& height) {
    Impl& s = *impl_;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s.frameFbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    const std::size_t rowBytes = std::size_t(s.pixelWidth) * 4;
    std::vector<std::uint8_t> bottomUp(rowBytes * s.pixelHeight);
    glReadPixels(0, 0, GLsizei(s.pixelWidth), GLsizei(s.pixelHeight), GL_BGRA, GL_UNSIGNED_BYTE, bottomUp.data());
    glBindFramebuffer(GL_FRAMEBUFFER, s.frameFbo);
    width = s.pixelWidth;
    height = s.pixelHeight;
    bgra.resize(bottomUp.size());
    for (std::uint32_t y = 0; y < s.pixelHeight; ++y)
        std::memcpy(&bgra[y * rowBytes], &bottomUp[(s.pixelHeight - 1 - y) * rowBytes], rowBytes);
    return glGetError() == GL_NO_ERROR;
}

// ---- video modes (list contents and order: VideoModes.h) ----
int cGLGDriver::CountVideoModes() { return impl_->modeList ? impl_->modeList->CountVideoModes() : 0; }

bool cGLGDriver::GetVideoModeInfo(std::uint32_t index, sGDMode& mode) {
    if (!impl_->modeList || !impl_->modeList->GetVideoModeInfo(index, mode)) {
        impl_->error = 2;
        return false;
    }
    return true;
}

bool cGLGDriver::GetVideoModeInfo(sGDMode& mode) {
    mode = impl_->currentMode;
    return true;
}

// Selects a mode: the frame target takes the mode's size. -1 releases the current mode.
bool cGLGDriver::SetVideoMode(int mode, void*, bool, bool) {
    Impl& m = *impl_;
    if (mode == -1) {
        m.currentMode.id = -1;
        return true;
    }
    sGDMode info{};
    if (!m.modeList || mode < 0 || !m.modeList->GetVideoModeInfo(static_cast<std::uint32_t>(mode), info)) {
        m.error = 2;
        return false;
    }
    m.currentMode = info;
    if (info.width != m.width || info.height != m.height) {
        m.pixelWidth = std::uint32_t(std::lround(double(m.pixelWidth) * info.width / m.width));   // the same scale
        m.pixelHeight = std::uint32_t(std::lround(double(m.pixelHeight) * info.height / m.height));
        m.width = info.width;
        m.height = info.height;
        m.MakeFrame();
        m.vx = m.vy = 0;
        m.vw = m.width;
        m.vh = m.height;
        m.scissor = false;
    }
    return true;
}

}  // namespace gd
