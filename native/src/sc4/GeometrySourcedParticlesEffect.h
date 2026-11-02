// cSC4GeometrySourcedParticlesEffect — a particle system whose particles are the triangles of a model (Mac i386
// 0x0040c3c4..0x0040fe9e; scratch notes: geo_plan.md). The demolition effects hand it the demolished building's or tree's
// model (void parameter 7 from cSC4VisualEffect::SetModelFromOccupant): until it starts it draws the model whole, then it
// dices every textured triangle down to the description's piece size and blows the pieces outward from an explosion point
// near the model's foot (a front that travels at description +0x1e4), with gravity, wind and damping.
//
// Own fields of the original (i386): +0x214 vector<cQuadTri>, +0x220 quads instead of triangles (SetDescription clears it),
// +0x224 one primitive pool per textured material, +0x230 their texture ids, +0x23c the materials' texture bindings,
// +0x248 the cIS3DMeshAndMaterial, +0x24c pieces made, +0x250 explosion point, +0x25c radius, +0x260 front radius,
// +0x264 previous front radius, +0x268 front strength, +0x26c explosion depth offset (float parameter 0x101).
// PARTIAL: the quad path (CreateParticlesFromModelQuads / DiceAndAddQuad: unreachable once SetDescription clears +0x220),
// the vertex-buffer render (RenderActiveParticlesVB draws through the plain path).
#pragma once
#include <cstdint>
#include <vector>

#include "render/PrimitivePool.h"
#include "sc4/ParticlesEffect.h"

namespace s3d { class cIS3DModelInstance; class cS3DModelInstance; }

namespace sc4 {

struct cSC4GSParticle : cSC4EffectsParticle {   // reserved18 = the material index
    std::uint32_t quadTri = 0;                     // +0x44
};

class cSC4GeometrySourcedParticlesEffect final : public cSC4ParticlesEffect {
public:
    struct cQuadTri {
        cS3DVector3 offset;       // minus the triangle's centroid, from its second vertex
        cS3DVector3 edgeB;        // third vertex - second (transformed)
        cS3DVector3 edgeA;        // first vertex - second (transformed)
        float uv1[2], uv2[2], uv0[2];
    };

    cSC4GeometrySourcedParticlesEffect();                                   // @ 0x0040e866
    ~cSC4GeometrySourcedParticlesEffect() override;

    void SetDescription(cSC4ParticlesDescription* description);             // @ 0x0040e372
    void SetModel(s3d::cIS3DModelInstance* model);                          // @ 0x0040e922
    bool SetFloatParams(int type, const float* values, int count) override; // @ 0x0040e7e4
    bool SetVoidParams(int type, void** values, int count) override;        // void parameter 7: the model (SetModelFromOccupant)
    void GenerateNewParticles() override;                                   // @ 0x0040fd54
    void DestroyParticle(cSCParticle* particle) override { std::free(particle); }   // @ 0x0040fe9e
    void RenderActiveParticles(IParticlesDrawContext* context) override;    // @ 0x0040c428
    void RenderActiveParticlesVB(IParticlesDrawContext* context) override { RenderActiveParticles(context); }
    bool UpdateParticlePhysics(cSC4EffectsParticle* particle, float elapsed) override;   // @ 0x0040dc2a

    bool HasModel() const { return model_ != nullptr; }
    std::size_t PieceCount() const { return pieces_.size(); }

private:
    void SetupArrays();                                                     // @ 0x0040e0a4
    void SetExplosionPointFromViewDir(const cS3DVector3& eye, const cS3DVector3& direction);   // @ 0x0040e4e6
    cSC4GSParticle* NewParticle(const cS3DVector3& position);               // @ 0x0040e9f2
    void CreateParticlesFromModelTriangles();                               // @ 0x0040f348
    void DiceAndAddTriangle(std::vector<cS3DVector3>& points, std::vector<cS3DVector2>& uvs, const int corners[3], float maxEdge,
                            std::uint32_t material);                        // @ 0x0040effc
    void AddTriangle(const std::vector<cS3DVector3>& points, const std::vector<cS3DVector2>& uvs, const int corners[3],
                     std::uint32_t material);                               // @ 0x0040ec96

    std::vector<cQuadTri> pieces_;                  // +0x214
    std::vector<cSC4PrimitivePool> pools_;          // +0x224
    std::vector<std::uint32_t> textures_;           // +0x230 (from the materials' bindings, +0x23c)
    s3d::cS3DModelInstance* model_ = nullptr;       // +0x248 (its mesh and material lists; one reference)
    bool generated_ = false;                        // +0x24c
    cS3DVector3 explosionPoint_;                    // +0x250
    float radius_ = 10.0f;                          // +0x25c
    float front_ = 0.0f;                            // +0x260
    float previousFront_ = 0.0f;                    // +0x264
    float frontStrength_ = 0.0f;                    // +0x268
    float depthOffset_ = 0.0f;                      // +0x26c
};

}  // namespace sc4
