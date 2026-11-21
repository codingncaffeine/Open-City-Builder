#include "sc4/PipeConnectionOccupant.h"

namespace sc4 {

namespace {
// The pipe stub (i386 0x00ab9c00): half a pipe of radius 2, as vertex pairs (top, bottom); it turns to face the camera.
const UndergroundVertex kPipeVertices[12] = {
    {-2.0f, 0.0f, 0.0f, {0, 0, 0, 0}, 0.0f, 0.0f},         {-2.0f, 0.0f, 0.0f, {0, 0, 0, 0}, 0.0f, 0.0f},
    {-1.61800003f, 0.0f, 1.176f, {0, 0, 0, 0}, 0.0f, 0.0f}, {-1.61800003f, 0.0f, 1.176f, {0, 0, 0, 0}, 0.0f, 0.0f},
    {-0.617999971f, 0.0f, 1.90199995f, {0, 0, 0, 0}, 0.0f, 0.0f}, {-0.617999971f, 0.0f, 1.90199995f, {0, 0, 0, 0}, 0.0f, 0.0f},
    {0.617999971f, 0.0f, 1.90199995f, {0, 0, 0, 0}, 0.0f, 0.0f},  {0.617999971f, 0.0f, 1.90199995f, {0, 0, 0, 0}, 0.0f, 0.0f},
    {1.61800003f, 0.0f, 1.176f, {0, 0, 0, 0}, 0.0f, 0.0f},  {1.61800003f, 0.0f, 1.176f, {0, 0, 0, 0}, 0.0f, 0.0f},
    {2.0f, 0.0f, 0.0f, {0, 0, 0, 0}, 0.0f, 0.0f},          {2.0f, 0.0f, 0.0f, {0, 0, 0, 0}, 0.0f, 0.0f},
};
const std::uint32_t kPipeTopIndices[6] = {0, 2, 4, 6, 8, 10};      // i386 0x00ab9be8
const std::uint32_t kPipeBottomIndices[6] = {1, 3, 5, 7, 9, 11};   // i386 0x00ab9bd0
}  // namespace

// cSC4PipeConnectionOccupant::sCustomGeometryInfo (i386 0x00ab9ba0).
const tUndergroundConnectionGeometryInfo cSC4PipeConnectionOccupant::sCustomGeometryInfo = {
    0x095a8d70, 0xe95a8d8d, 0x0a08b06e, 0x6a089741, 0x6a089740, kPipeVertices, 12, kPipeTopIndices, kPipeBottomIndices, 6,
    true, false, 0.0f};

bool cSC4PipeConnectionOccupant::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZCLSID_cSC4PipeConnectionOccupant) {
        *out = static_cast<cSC4PipeConnectionOccupant*>(this);
        AddRef();
        return true;
    }
    return cSC4UndergroundConnectionOccupant::QueryInterface(iid, out);
}

bool cSC4PipeConnectionOccupant::Init() {
    type_ = kPipeConnectionOccupantType;
    return cSC4UndergroundConnectionOccupant::Init(kGZCLSID_cSC4PipeConnectionOccupant, &sCustomGeometryInfo);
}

}  // namespace sc4
