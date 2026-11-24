// cS3DModelProcessor — recovered from the x86_64 disassembly (otool -arch x86_64 -tV): the assignment operator at
// 0x100108e06 and the body of the class at 0x100165910..0x10016667e. The class has no virtual functions: the
// constructor writes no vtable pointer, and every call site addresses it statically.
#include "s3d/ModelProcessor.h"

#include <cmath>
#include <cstddef>
#include <cstring>

#include "gz/GZCOMInterface.h"
#include "render/SimGL.h"
#include "sc4/LightingManager.h"
#include "s3d/ModelInstanceAnimatedSprite.h"

namespace s3d {

namespace {

// The colour the processor writes into the diffuse byte of every converted vertex (0x100165f71: 0xffffffff).
constexpr std::uint32_t kOpaqueWhite = 0xffffffffu;
// Bytes of the source vertex the copy loop moves: xyz (12) and the two texture coordinates (8) at +0xc.
constexpr int kSourcePositionBytes = 0xc;
constexpr int kSourceTextureOffset = 0xc;
constexpr int kSourceTextureBytes = 0x8;
// Where the two texture coordinates land in the destination record (xyz, colour, u, v).
constexpr int kModelProcessorTextureOffset = 0x10;

}  // namespace

cS3DModelProcessor::cS3DModelProcessor() = default;   // @ 0x100165910 (the field initialisers are the original's defaults)

// No symbol in this build (never instantiated); it exists so the class does not shallow-copy its counted pointers.
cS3DModelProcessor::cS3DModelProcessor(const cS3DModelProcessor& other) : cS3DModelProcessor() { *this = other; }

cS3DModelProcessor::~cS3DModelProcessor() {   // @ 0x1001659b6 (the complete-object alias is 0x100165b3e)
    if (model_) {   // 0x1001659c7
        cIS3DModelInstance* model = model_;
        model_ = nullptr;
        model->Release();
    }
    if (mesh_) {   // 0x1001659dc
        // give the borrowed arrays back to the mesh records (0x1001659f3..0x100165be8), then drop our references
        for (std::size_t i = 0; i < vertexArrays_.size(); ++i)
            AssignMeshVertexArray(MeshRecordAt(*mesh_, unsigned(i)).vertexArray, vertexArrays_[i].get());
        vertexArrays_.clear();
        if (released_) {
            mesh_ = nullptr;   // QUIRK: ReleaseModel already released it, do not release it again (0x100165a3f)
        } else {
            cS3DMeshAndMaterial* mesh = mesh_;
            mesh_ = nullptr;
            mesh->Release();   // 0x100165a5d
        }
    }
}

cS3DModelProcessor& cS3DModelProcessor::operator=(const cS3DModelProcessor& other) {   // @ 0x100108e06
    if (this != &other) {
        if (other.model_ != model_) {   // 0x100108e20 (AddRef the new one, then release the old one)
            if (other.model_) other.model_->AddRef();
            cIS3DModelInstance* previous = model_;
            model_ = other.model_;
            if (previous) previous->Release();
        }
        if (other.mesh_ != mesh_) {   // 0x100108e4b
            if (other.mesh_) other.mesh_->AddRef();
            cS3DMeshAndMaterial* previous = mesh_;
            mesh_ = other.mesh_;
            if (previous) previous->Release();
        }
        vertexArrays_ = other.vertexArrays_;       // 0x100108e80
        byteArrays_ = other.byteArrays_;           // 0x100108e91
        normals_ = other.normals_;                 // 0x100108ea2
        sortedMeshIndices_ = other.sortedMeshIndices_;   // 0x100108eb3
    }
    destFormat_ = other.destFormat_;   // 0x100108e79 / 0x100108ec6
    released_ = other.released_;       // 0x100108ecd
    return *this;
}

bool cS3DModelProcessor::SetModel(cIS3DModelInstance* model) {   // @ 0x100165b52
    // ---- reset: return the borrowed arrays, drop the old mesh, then store the new model (0x100165b69..0x100165cc1)
    if (mesh_) {
        for (std::size_t i = 0; i < vertexArrays_.size(); ++i)
            AssignMeshVertexArray(MeshRecordAt(*mesh_, unsigned(i)).vertexArray, vertexArrays_[i].get());
        vertexArrays_.clear();       // 0x100165c30
        normals_.clear();            // 0x100165c34
        sortedMeshIndices_.clear();  // 0x100165c63
        if (released_) {
            mesh_ = nullptr;   // QUIRK: ReleaseModel already released it (0x100165c72)
        } else {
            cS3DMeshAndMaterial* previous = mesh_;
            mesh_ = nullptr;
            previous->Release();
        }
    }
    if (model != model_) {   // 0x100165c98
        if (model) model->AddRef();
        cIS3DModelInstance* previous = model_;
        model_ = model;
        if (previous) previous->Release();
    }
    released_ = false;       // 0x100165cc1
    if (!model) return false;   // the mesh is already gone, so the shared tail has nothing left to release

    // An animated instance with a real frame animation is not a static model (0x100165ccf..0x100165d45).
    {
        cRZAutoRefCount<cIS3DModelInstanceAnimated> animated =
            gz::GZCOM_Interface<cIS3DModelInstanceAnimated>(model, kGZIID_cIS3DModelInstanceAnimated);
        if (animated && animated->GetFrameCount() >= 2) {
            cIS3DModelInstance* previous = model_;
            model_ = nullptr;
            if (previous) previous->Release();
            return false;
        }
    }

    // The mesh view of the instance (0x100165d48).
    {
        cRZAutoRefCount<cS3DMeshAndMaterial> found =
            gz::GZCOM_Interface<cS3DMeshAndMaterial>(model, kGZIID_cIS3DMeshAndMaterial);
        if (found.get() != mesh_) {
            if (found) found->AddRef();
            cS3DMeshAndMaterial* previous = mesh_;
            mesh_ = found.get();
            if (previous) previous->Release();
        }
    }
    if (!mesh_ || mesh_->MeshCount() == 0) {   // 0x100165da0 / 0x100165df7
        cIS3DModelInstance* previousModel = model_;
        model_ = nullptr;
        if (previousModel) previousModel->Release();
        if (mesh_) {
            cS3DMeshAndMaterial* previousMesh = mesh_;
            mesh_ = nullptr;
            previousMesh->Release();
        }
        return false;
    }

    // Every mesh must already be in the accepted standard format (0x100165db4).
    for (int i = 0; i < mesh_->MeshCount(); ++i) {
        const ModelProcessorMesh& record = MeshRecordAt(*mesh_, unsigned(i));
        if (static_cast<std::int32_t>(record.vertexArray->format) !=
            nRZSimGL::PackStandardVertexFormat(kModelProcessorAcceptedFormat)) {
            cS3DMeshAndMaterial* previousMesh = mesh_;
            mesh_ = nullptr;
            previousMesh->Release();
            break;
        }
    }
    if (!mesh_) {
        cIS3DModelInstance* previousModel = model_;
        model_ = nullptr;
        if (previousModel) previousModel->Release();
        return false;
    }

    // Build one destination vertex array per mesh (0x100165e11..0x100165fbd).
    const int meshCount = mesh_->MeshCount();
    vertexArrays_.resize(std::size_t(meshCount));
    for (int i = 0; i < meshCount; ++i) {
        ModelProcessorMesh& record = MeshRecordAt(*mesh_, unsigned(i));
        cS3DVertexArray* source = record.vertexArray;
        cS3DVertexArray* converted = new cS3DVertexArray();
        vertexArrays_[std::size_t(i)] = source;
        converted->format = destFormat_;
        converted->stride = std::uint16_t(nRZSimGL::StandardVFStride(destFormat_));
        converted->count = source->count;
        converted->data.resize(std::size_t(converted->stride) * converted->count);
        for (int k = 0; k < 3; ++k) {
            converted->boundsMin[k] = source->boundsMin[k];
            converted->boundsMax[k] = source->boundsMax[k];
        }
        const std::uint8_t* sourceData = source->data.data();
        const std::size_t sourceStride = source->stride;
        std::uint8_t* destData = converted->data.data();
        for (std::uint16_t vertex = 0; vertex < source->count; ++vertex) {
            const std::uint8_t* in = sourceData + std::size_t(vertex) * sourceStride;
            std::uint8_t* out = destData + std::size_t(vertex) * kModelProcessorVertexStride;
            std::memcpy(out, in, kSourcePositionBytes);   // the position is copied, not converted
            std::memcpy(out + kModelProcessorColourOffset, &kOpaqueWhite, sizeof kOpaqueWhite);
            std::memcpy(out + kModelProcessorTextureOffset, in + kSourceTextureOffset, kSourceTextureBytes);
        }
        AssignMeshVertexArray(record.vertexArray, converted);
    }
    return true;
}

void cS3DModelProcessor::ReleaseModel() {   // @ 0x100166036
    if (model_) {
        cIS3DModelInstance* model = model_;
        model_ = nullptr;
        model->Release();
    }
    // QUIRKS: no null check, the mesh pointer is left dangling and the borrowed arrays are not returned (0x100166054).
    mesh_->Release();
    released_ = true;   // 0x10016605e: the destructor will only clear the pointer
}

void cS3DModelProcessor::LightModel(sc4::cISC4LightingManager* lighting, bool flipRotation) {   // @ 0x10016606a
    if (normals_.empty()) CreateVertexNormals();   // 0x100166084
    MaterialRecord& material = mesh_->Material(0);   // 0x100166096: byte +4 of the first material |= 8
    material[4] = std::uint8_t(material[4] | 0x08);

    cS3DTransform transform;   // identity; the model overwrites it below
    model_->GetTransform(transform);   // 0x1001660cd (cIS3DModelInstance slot 0x48)
    nS3D::Matrix3 rotation;
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column) rotation.m[3 * row + column] = transform.rotation[row][column];
    if (flipRotation) {
        for (int k = 0; k < 9; ++k) rotation.m[k] = -rotation.m[k];   // 0x1001660f4 flips the sign bits
    }

    for (std::size_t i = 0; i < vertexArrays_.size(); ++i) {   // 0x100166127
        if (static_cast<std::int32_t>(vertexArrays_[i]->format) !=
            nRZSimGL::PackStandardVertexFormat(kModelProcessorAcceptedFormat))
            continue;
        // The colours go into the converted copy the mesh record holds (0x100166149 reads *Mesh(i)).
        cS3DVertexArray* converted = MeshRecordAt(*mesh_, unsigned(i)).vertexArray;
        std::vector<cS3DVector3>& normals = normals_[i];
        lighting->GetVertexColors(int(normals.size()), normals.data(), kModelProcessorNormalStride,
                                  converted->data.data() + kModelProcessorColourOffset, kModelProcessorVertexStride,
                                  rotation);
    }
}

cS3DVertexArray* cS3DModelProcessor::ConvertedVertexArray(std::size_t index) const {
    return mesh_ && index < vertexArrays_.size() ? MeshRecordAt(*mesh_, unsigned(index)).vertexArray : nullptr;
}

void cS3DModelProcessor::CreateVertexNormals() {   // @ 0x1001661bc
    normals_.clear();                            // 0x10016620e frees the inner vectors
    normals_.resize(vertexArrays_.size());       // 0x10016624d
    if (normals_.empty()) return;
    cS3DTriangleIterator iterator;   // 0x1001661e1, reused for every mesh
    for (std::size_t meshIndex = 0; meshIndex < vertexArrays_.size(); ++meshIndex) {   // 0x1001662af
        cS3DVertexArray* array = vertexArrays_[meshIndex].get();
        const std::uint16_t vertexCount = array->count;
        normals_[meshIndex].resize(vertexCount, cS3DVector3{0.0f, 0.0f, 0.0f});
        std::vector<float> weights(vertexCount, 0.0f);   // accumulated double triangle area per vertex
        const std::uint8_t* vertexData = array->data.data();
        const std::size_t stride = array->stride;
        const ModelProcessorMesh& record = MeshRecordAt(*mesh_, unsigned(meshIndex));
        MeshView view;
        view.vertexFormat = record.vertexArray->format;
        view.vertexStride = record.vertexArray->stride;
        view.vertexData = record.vertexArray->data.data();
        view.indexData = record.indexGroup ? *record.indexGroup : nullptr;
        view.primitives = record.draws->begin;
        const int primitiveCount = int(record.draws->end - record.draws->begin);
        for (int submesh = 0; submesh < primitiveCount; ++submesh) {   // 0x100166544
            iterator.Init(view, submesh);
            int indices[3];
            while (iterator.GetNextTriangleIndices(indices)) {   // 0x10016651f
                cS3DVector3 points[3];
                for (int k = 0; k < 3; ++k) {
                    const std::uint8_t* vertex = vertexData + std::size_t(indices[k]) * stride;
                    std::memcpy(&points[k], vertex, sizeof points[k]);
                }
                cS3DVector3 normal{0.0f, 0.0f, 0.0f};
                nS3D::TriangleDoubleAreaNormal(points[0], points[1], points[2], normal);
                for (int k = 0; k < 3; ++k) {
                    cS3DVector3& accumulated = normals_[meshIndex][std::size_t(indices[k])];
                    accumulated.x += normal.x;
                    accumulated.y += normal.y;
                    accumulated.z += normal.z;
                }
                const float length =
                    std::sqrt(normal.z * normal.z + (normal.y * normal.y + normal.x * normal.x));
                weights[indices[0]] += length;
                weights[indices[1]] += length;
                weights[indices[2]] += length;
            }
        }
        // normalise by the summed double area (0x10016656d); a vertex no triangle touched keeps a zero normal
        for (std::size_t vertex = 0; vertex < vertexCount; ++vertex) {
            if (!(weights[vertex] > 0.0f)) continue;
            const float scale = 1.0f / weights[vertex];
            normals_[meshIndex][vertex].x *= scale;
            normals_[meshIndex][vertex].y *= scale;
            normals_[meshIndex][vertex].z *= scale;
        }
    }
}

void cS3DModelProcessor::SortMeshes(const cS3DVector3& direction) {   // @ 0x10016667e
    if (!mesh_) return;   // 0x100166695
    std::vector<float> depths;
    sortedMeshIndices_.clear();   // 0x1001666bc
    const int meshCount = mesh_->MeshCount();
    for (int i = 0; i < meshCount; ++i) {
        const cS3DVertexArray* array = MeshRecordAt(*mesh_, unsigned(i)).vertexArray;
        const float centreX = (array->boundsMin[0] + array->boundsMax[0]) * 0.5f;   // 0x1001666d7
        const float centreY = (array->boundsMin[1] + array->boundsMax[1]) * 0.5f;
        const float centreZ = (array->boundsMin[2] + array->boundsMax[2]) * 0.5f;
        const float depth = centreZ * direction.z + (centreY * direction.y + centreX * direction.x);
        // descending: skip the strictly greater entries and insert before the first one that is not greater
        // (ucomiss + jbe at 0x100166746 skips while depths[position] > depth, so equal and NaN depths go first)
        std::size_t position = 0;
        while (position < depths.size() && depths[position] > depth) ++position;
        depths.insert(depths.begin() + std::ptrdiff_t(position), depth);                          // 0x10016675f
        sortedMeshIndices_.insert(sortedMeshIndices_.begin() + std::ptrdiff_t(position), i);      // 0x100166773
    }
}

}  // namespace s3d
