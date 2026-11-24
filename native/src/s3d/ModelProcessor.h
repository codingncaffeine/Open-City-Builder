// cS3DModelProcessor (11 symbols, 0x100165910..0x10016667e): the object that turns one cIS3DModelInstance into the
// form the renderer's lighting pass draws. It takes the instance's mesh list (the cIS3DMeshAndMaterial view of the
// instance), keeps a reference to the instance and to that mesh list, and rewrites every mesh's vertex array from the
// accepted standard format 2 (position + two texture coordinates, stride 0x14) into the destination format
// (eGDVertexFormat 10 by default: position + diffuse colour + two texture coordinates, stride 0x18), filling the new
// colour bytes from the city lighting. It also computes per-vertex normals for the lighting call and keeps a per-mesh
// draw order sorted by view depth. The automaton uses it twice (cSC4Automaton +0x130 for the auxiliary/ghost model and
// +0x1d8 for the primary model) and the model-effects classes embed it as well.
//
// Layout (0x80 bytes, the size cSC4Automaton reserves per sub-object; every field default from the constructor at
// 0x100165910):
//   +0x00 cIS3DModelInstance* model (counted)            +0x08 cS3DMeshAndMaterial* mesh (counted)
//   +0x10 eGDVertexFormat destination format (0xa)       +0x18 std::vector<cRZAutoRefCount<cS3DVertexArray>> originals
//   +0x30 std::vector<std::vector<uint8_t>> (never used) +0x48 std::vector<std::vector<cS3DVector3>> normals
//   +0x60 std::vector<int> sorted mesh indices           +0x78 bool "already released" (see ReleaseModel)
//
// The two references are held as raw pointers here (the original holds cRZAutoRefCount objects) so that the release
// quirks below can be reproduced exactly; every assignment goes through the same AddRef-then-Release-the-old order.
//
// Quirks kept (with their addresses):
//  * SetModel copies the vertex data with a *fixed* 0x18-byte record stride (0x100165f91) while it sizes the buffer
//    with nRZSimGL::StandardVFStride(destination format); SetDestFormat(11) (stride 0x20) therefore leaves the last
//    eight bytes of every record uninitialised. The default format 10 is the only one the game passes.
//  * SetModel refuses an instance that answers cIS3DModelInstanceAnimated with a frame count of 2 or more
//    (0x100165cf8): such an instance is animated, not a static model, and the processor releases everything and
//    returns false.
//  * Every mesh's vertex array must already be in the standard format 2 (0x100165dba); one array in another format
//    drops the whole mesh list and the model (returns false).
//  * The vertex arrays borrowed from the mesh are given back when the processor is reset or destroyed (0x100165b93 /
//    0x100165be8 restore them through the mesh record, 0x100165a02 in the destructor): after the processor dies the
//    mesh records are back on the arrays the model resource owns.
//  * ReleaseModel (0x100166036) releases the model *and* the mesh, leaves the mesh pointer dangling and never
//    restores the borrowed arrays; it then sets +0x78 so the destructor clears the pointer without releasing it a
//    second time (0x100165a38 / 0x100165c6b). Destroying a processor after ReleaseModel with borrowed arrays still
//    writes through the dangling mesh pointer, exactly as the original does.
//  * SortMeshes inserts each mesh after every entry whose depth is smaller *or equal* (0x100166746), so equal depths
//    keep the mesh order, and it treats a NaN depth as "not greater" (the unordered flag of ucomiss falls into jbe).
//  * The 0x30 byte-array vector is written by the assignment operator (0x100108e91) and destroyed, but nothing in
//    this build fills it.
//
// Seams: the lighting pass is cISC4LightingManager vslot 0xd8 (GetVertexColors, recovered in src/sc4/LightingManager.h);
// the mesh list is the recovered cS3DMeshAndMaterial. cS3DVertexArray and the 0x18-byte mesh record are recovered here
// because the mesh list keeps them as raw records.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "render/gd/IGZGDriver.h"
#include "rz/cRZAutoRefCount.h"
#include "s3d/MeshAndMaterial.h"
#include "s3d/ModelInstance.h"
#include "s3d/S3DMath.h"
#include "s3d/TriangleIterator.h"

namespace sc4 {
class cISC4LightingManager;
}

namespace s3d {

// The vertex format the processor accepts from a mesh (nRZSimGL::PackStandardVertexFormat(2) = -0x7fffbfff: position
// + two texture coordinates) and the one it writes (eGDVertexFormat 10, the 0x18-byte record below).
constexpr std::int32_t kModelProcessorAcceptedFormat = 2;
constexpr gd::eGDVertexFormat kModelProcessorDestFormat = 10;
constexpr int kModelProcessorVertexStride = 0x18;   // the record the copy loop writes: xyz, colour, u, v
constexpr int kModelProcessorColourOffset = 0xc;    // colour of one record
constexpr int kModelProcessorNormalStride = 0xc;    // sizeof(cS3DVector3)

// cS3DVertexArray (0x40 bytes): one mesh's interleaved vertex data, its local bounds and its reference count. The
// constructor the processor inlines (0x100165e5a) writes format 2, stride 0x14, the empty box {1,1,1}/{0,0,0} and a
// zero reference count; the released object frees the data buffer and itself (0x100165be8 / 0x100165c27).
class cS3DVertexArray {
public:
    cS3DVertexArray() = default;
    ~cS3DVertexArray() = default;

    std::uint32_t AddRef() { return ++refCount; }
    std::uint32_t Release() {
        if (refCount > 1) return --refCount;
        delete this;
        return 0;
    }

    std::uint32_t format = std::uint32_t(kModelProcessorAcceptedFormat);   // +0x00 eGDVertexFormat
    std::uint16_t stride = 0x14;                                           // +0x04 nRZSimGL::StandardVFStride(2)
    std::uint16_t count = 0;                                               // +0x06 vertices
    std::vector<std::uint8_t> data;                                        // +0x08 interleaved vertices
    float boundsMin[3] = {1.0f, 1.0f, 1.0f};                               // +0x20
    float boundsMax[3] = {0.0f, 0.0f, 0.0f};                               // +0x2c
    std::uint16_t refCount = 0;                                            // +0x38
};
static_assert(sizeof(cS3DVertexArray) == 0x40, "the original vertex array is 0x40 bytes");

// The draw-record container a mesh points at (sc4::SC4MeshDrawList is the same record): two pointers at +8 / +0x10.
struct ModelProcessorDrawList {
    const void* reserved0 = nullptr;         // +0
    const MeshPrimitive* begin = nullptr;    // +8  (eGDPrimType, first element, element count)
    const MeshPrimitive* end = nullptr;      // +0x10
};

// One 0x18-byte mesh record of cS3DMeshAndMaterial as the processor reads it (sc4::SC4Mesh is the same record). The
// vertex-array field is a cRZAutoRefCount<cS3DVertexArray> in the original; it is kept as a raw pointer and every
// assignment goes through AssignMeshVertexArray.
struct ModelProcessorMesh {
    cS3DVertexArray* vertexArray = nullptr;               // +0  (counted)
    const std::uint16_t* const* indexGroup = nullptr;     // +8  (the index buffer is *indexGroup)
    const ModelProcessorDrawList* draws = nullptr;        // +0x10
};
static_assert(sizeof(ModelProcessorMesh) == 0x18, "the original mesh record is 0x18 bytes");

// The typed view of one cS3DMeshAndMaterial mesh record. The records are raw 0x18-byte arrays, so this reinterprets
// them; the elements of the mesh vector are 0x18 bytes apart and therefore at least 8-byte aligned.
inline ModelProcessorMesh& MeshRecordAt(cS3DMeshAndMaterial& mesh, unsigned index) {
    return *reinterpret_cast<ModelProcessorMesh*>(mesh.Mesh(index).data());
}
inline const ModelProcessorMesh& MeshRecordAt(const cS3DMeshAndMaterial& mesh, unsigned index) {
    return *reinterpret_cast<const ModelProcessorMesh*>(mesh.Mesh(index).data());
}

// cRZAutoRefCount<cS3DVertexArray>::operator= as the mesh record uses it: AddRef the new array, then release the old.
inline void AssignMeshVertexArray(cS3DVertexArray*& slot, cS3DVertexArray* value) {
    if (value == slot) return;
    if (value) value->AddRef();
    cS3DVertexArray* previous = slot;
    slot = value;
    if (previous) previous->Release();
}

class cS3DModelProcessor {
public:
    cS3DModelProcessor();                                             // @ 0x100165910 / 0x10016596c
    // Never instantiated by this build (no symbol); kept so the class does not shallow-copy its counted pointers.
    cS3DModelProcessor(const cS3DModelProcessor& other);
    ~cS3DModelProcessor();                                            // @ 0x1001659b6 (alias 0x100165b3e)
    cS3DModelProcessor& operator=(const cS3DModelProcessor& other);    // @ 0x100108e06

    void SetDestFormat(gd::eGDVertexFormat format) { destFormat_ = format; }   // @ 0x100165b48
    bool SetModel(cIS3DModelInstance* model);                         // @ 0x100165b52
    void ReleaseModel();                                              // @ 0x100166036
    // `flipRotation` negates the model transform's 3x3 rotation before the lighting pass (0x1001660f4).
    void LightModel(sc4::cISC4LightingManager* lighting, bool flipRotation);   // @ 0x10016606a
    void CreateVertexNormals();                                       // @ 0x1001661bc
    // Descending depth order of the meshes (front to back) for the view direction.
    void SortMeshes(const cS3DVector3& direction);                    // @ 0x10016667e

    // ---- accessors for the neighbours and the tests ----
    cIS3DModelInstance* Model() const { return model_; }
    cS3DMeshAndMaterial* MeshAndMaterial() const { return mesh_; }
    gd::eGDVertexFormat DestFormat() const { return destFormat_; }
    bool IsReleased() const { return released_; }
    const std::vector<cRZAutoRefCount<cS3DVertexArray>>& VertexArrays() const { return vertexArrays_; }
    const std::vector<std::vector<std::uint8_t>>& ByteArrays() const { return byteArrays_; }
    const std::vector<std::vector<cS3DVector3>>& Normals() const { return normals_; }
    // The converted copy the mesh record of `index` holds (what LightModel colours and the effects processor deforms).
    cS3DVertexArray* ConvertedVertexArray(std::size_t index) const;
    const std::vector<int>& SortedMeshIndices() const { return sortedMeshIndices_; }

private:
    cIS3DModelInstance* model_ = nullptr;                             // +0x00 (counted)
    cS3DMeshAndMaterial* mesh_ = nullptr;                             // +0x08 (counted)
    gd::eGDVertexFormat destFormat_ = kModelProcessorDestFormat;      // +0x10
    std::vector<cRZAutoRefCount<cS3DVertexArray>> vertexArrays_;      // +0x18 the arrays borrowed from the mesh
    std::vector<std::vector<std::uint8_t>> byteArrays_;               // +0x30 never filled in this build
    std::vector<std::vector<cS3DVector3>> normals_;                   // +0x48 one vector per mesh
    std::vector<int> sortedMeshIndices_;                              // +0x60 mesh index per sorted position
    bool released_ = false;                                           // +0x78 ReleaseModel already ran
};
static_assert(sizeof(cS3DModelProcessor) == 0x80, "cSC4Automaton reserves 0x80 bytes per processor");

}  // namespace s3d
