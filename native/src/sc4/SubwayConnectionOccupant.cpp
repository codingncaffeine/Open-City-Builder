#include "sc4/SubwayConnectionOccupant.h"

namespace sc4 {

namespace {
// The shaft (i386 0x00ab9d80): a closed square tube 15 units wide, as vertex pairs (top, bottom) around it.
const UndergroundVertex kShaftVertices[10] = {
    {-7.5f, 0.0f, -7.5f, {0, 0, 0, 0}, 0.0f, 0.0f}, {-7.5f, 0.0f, -7.5f, {0, 0, 0, 0}, 0.0f, 0.0f},
    {-7.5f, 0.0f, 7.5f, {0, 0, 0, 0}, 0.0f, 0.0f},  {-7.5f, 0.0f, 7.5f, {0, 0, 0, 0}, 0.0f, 0.0f},
    {7.5f, 0.0f, 7.5f, {0, 0, 0, 0}, 0.0f, 0.0f},   {7.5f, 0.0f, 7.5f, {0, 0, 0, 0}, 0.0f, 0.0f},
    {7.5f, 0.0f, -7.5f, {0, 0, 0, 0}, 0.0f, 0.0f},  {7.5f, 0.0f, -7.5f, {0, 0, 0, 0}, 0.0f, 0.0f},
    {-7.5f, 0.0f, -7.5f, {0, 0, 0, 0}, 0.0f, 0.0f}, {-7.5f, 0.0f, -7.5f, {0, 0, 0, 0}, 0.0f, 0.0f},
};
const std::uint32_t kShaftTopIndices[5] = {0, 2, 4, 6, 8};      // i386 0x00ab9d64
const std::uint32_t kShaftBottomIndices[5] = {1, 3, 5, 7, 9};   // i386 0x00ab9d50
}  // namespace

// cSC4SubwayConnectionOccupant::sCustomGeometryInfo (i386 0x00ab9d20).
const tUndergroundConnectionGeometryInfo cSC4SubwayConnectionOccupant::sCustomGeometryInfo = {
    0x095a8d70, 0xe95a8d8d, 0x6a4e6066, 0x0ad80000, 0, kShaftVertices, 10, kShaftTopIndices, kShaftBottomIndices, 5,
    false, true, 3.0f};

bool cSC4SubwayConnectionOccupant::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZCLSID_cSC4SubwayConnectionOccupant) {
        *out = static_cast<cSC4SubwayConnectionOccupant*>(this);
        AddRef();
        return true;
    }
    return cSC4UndergroundConnectionOccupant::QueryInterface(iid, out);
}

bool cSC4SubwayConnectionOccupant::Init() {
    type_ = kSubwayConnectionOccupantType;
    return cSC4UndergroundConnectionOccupant::Init(kGZCLSID_cSC4SubwayConnectionOccupant, &sCustomGeometryInfo);
}

}  // namespace sc4
