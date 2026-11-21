// cSC4SubwayOccupant's piece tables (Mac i386 data 0x00ab858c..0x00ab8f60, the record cSC4SubwayOccupant::sCustomGeometryInfo
// at 0x00ab83c0): the straight piece, the two zipper halves of a straight piece that changes depth (top and bottom), the
// straight cap, the diagonal piece and the diagonal cap (vertices as sVF_V3F_C4UB_T2F, the edge / hinge vertex indices, the
// straight edge distances) and the 17 texture records. The counts the original's static initializer (0x00a7f812) copies in
// from the file's count words (0x00ab8f64..0x00ab8f8c) are written into the record directly; the two diagonal distance
// tables are filled by cSC4SubwayOccupant::StaticInit.
#include "sc4/SubwayOccupant.h"

namespace sc4 {
namespace {
const UndergroundVertex kStraightPieceVerts[12] = {          // 0x00ab8e40
    {-8.0f, 0.0f, -8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-8.0f, 0.0f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-4.19700003f, 0.0f, -8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-4.19700003f, 0.0f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-1.74100006f, 2.97099996f, -8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-1.74100006f, 2.97099996f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {1.74100006f, 2.97099996f, -8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {1.74100006f, 2.97099996f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {4.19700003f, 0.0f, -8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {4.19700003f, 0.0f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {8.0f, 0.0f, -8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {8.0f, 0.0f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
};
const UndergroundVertex kStraightZipperPieceTopVerts[12] = {          // 0x00ab8d20
    {-8.0f, 0.0f, -8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-8.0f, 0.0f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-4.19700003f, 0.0f, -8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-4.19700003f, 0.0f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-1.74100006f, 2.97099996f, -8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-1.74100006f, 2.97099996f, 4.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {1.74100006f, 2.97099996f, -8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {1.74100006f, 2.97099996f, 4.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {4.19700003f, 0.0f, -8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {4.19700003f, 0.0f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {8.0f, 0.0f, -8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {8.0f, 0.0f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
};
const UndergroundVertex kStraightZipperPieceBottomVerts[12] = {          // 0x00ab8c00
    {-8.0f, 0.0f, -8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-8.0f, 0.0f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-4.19700003f, 0.0f, -8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-4.19700003f, 0.0f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-3.48300004f, -3.55500007f, -8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-3.48300004f, -3.55500007f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {3.48300004f, -3.55500007f, -8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {3.48300004f, -3.55500007f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {4.19700003f, 0.0f, -8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {4.19700003f, 0.0f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {8.0f, 0.0f, -8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {8.0f, 0.0f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
};
const std::int32_t kStraightPieceEdge0VertIndices[6] = {1, 3, 5, 7, 9, 11};   // 0x00ab8bd0
const std::int32_t kStraightPieceEdge1VertIndices[6] = {0, 2, 4, 6, 8, 10};   // 0x00ab8bb8
const float kStraightPieceEdgeDistancePercentages[6] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};   // 0x00ab8ba0
const UndergroundVertex kStraightCapTopVerts[12] = {          // 0x00ab8a80
    {-8.0f, 0.0f, 0.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-8.0f, 0.0f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-4.19700003f, 0.0f, 0.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-4.19700003f, 0.0f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-1.74100006f, 2.97099996f, 0.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-1.74100006f, 2.97099996f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {1.74100006f, 2.97099996f, 0.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {1.74100006f, 2.97099996f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {4.19700003f, 0.0f, 0.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {4.19700003f, 0.0f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {8.0f, 0.0f, 0.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {8.0f, 0.0f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
};
const UndergroundVertex kStraightCapEndVerts[6] = {          // 0x00ab89e0
    {-4.19700003f, 0.0f, 0.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-2.61199999f, -4.52799988f, 0.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-1.74100006f, 2.97099996f, 0.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {2.61199999f, -4.52799988f, 0.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {1.74100006f, 2.97099996f, 0.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {4.19700003f, 0.0f, 0.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
};
const std::int32_t kStraightCapHingeVertIndices[6] = {0, 2, 4, 6, 8, 10};   // 0x00ab89b8
const UndergroundVertex kDiagonalPieceVerts[21] = {          // 0x00ab87c0
    {0.0f, 0.0f, 0.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-8.0f, 0.0f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {1.03299999f, 0.0f, 1.03299999f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-5.93499994f, 0.0f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {2.76900005f, 2.97099996f, 2.76900005f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-2.46300006f, 2.97099996f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {5.23099995f, 2.97099996f, 5.23099995f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {2.46300006f, 2.97099996f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {6.96700001f, 0.0f, 6.96700001f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {5.93499994f, 0.0f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {8.0f, 0.0f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {6.96700001f, 0.0f, 6.96700001f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {8.0f, 0.0f, 5.93499994f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {5.23099995f, 2.97099996f, 5.23099995f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {8.0f, 2.97099996f, 2.46300006f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {2.76900005f, 2.97099996f, 2.76900005f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {8.0f, 2.97099996f, -2.46300006f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {1.03299999f, 0.0f, 1.03299999f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {8.0f, 0.0f, -5.93499994f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {0.0f, 0.0f, 0.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {8.0f, 0.0f, -8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
};
const std::int32_t kDiagonalPieceEdge0VertIndices[5] = {1, 3, 5, 7, 9};   // 0x00ab8794
const std::int32_t kDiagonalPieceEdge1VertIndices[5] = {20, 18, 16, 14, 12};   // 0x00ab8780
const std::int32_t kDiagonalPieceHingeVertIndices[6] = {0, 2, 4, 6, 8, 10};   // 0x00ab8768
const UndergroundVertex kDiagonalCapTopVerts[11] = {          // 0x00ab8660
    {-8.0f, 0.0f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-6.96700001f, 0.0f, 6.96700001f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-5.93499994f, 0.0f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-5.23099995f, 2.97099996f, 5.23099995f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-2.46300006f, 2.97099996f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-2.76900005f, 2.97099996f, 2.76900005f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {2.46300006f, 2.97099996f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-1.03299999f, 0.0f, 1.03299999f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {5.93499994f, 0.0f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {0.0f, 0.0f, 0.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {8.0f, 0.0f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
};
const UndergroundVertex kDiagonalCapEndVerts[6] = {          // 0x00ab85c0
    {-6.96700001f, 0.0f, 6.96700001f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-5.74900007f, -3.44899988f, 5.74900007f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-5.23099995f, 2.97099996f, 5.23099995f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-2.25099993f, -3.44899988f, 2.25099993f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-2.76900005f, 2.97099996f, 2.76900005f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-1.03299999f, 0.0f, 1.03299999f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
};
const std::int32_t kDiagonalCapEdge0VertIndices[5] = {2, 4, 6, 8, 10};   // 0x00ab85a4
const std::int32_t kDiagonalCapHingeVertIndices[6] = {1, 3, 5, 7, 9, 0};   // 0x00ab858c
}  // namespace

// Written by StaticInit (bss 0x00bfaa34 / 0x00bfaa20 in the original).
float cSC4SubwayOccupant::sDiagonalPieceEdgeDistancePercentages[5] = {};
float cSC4SubwayOccupant::sDiagonalCapEdgeDistancePercentages[5] = {};

tCustomGeometryInfo cSC4SubwayOccupant::sCustomGeometryInfo = {
    {kStraightPieceVerts, kStraightZipperPieceTopVerts, kStraightZipperPieceBottomVerts, kStraightPieceEdge0VertIndices,
     kStraightPieceEdge1VertIndices, kStraightPieceEdgeDistancePercentages, kStraightCapTopVerts, kStraightCapEndVerts,
     kStraightCapHingeVertIndices},
    {12, 6, 12, 6, 6, 0},   // StraightPieceVertCount, StraightPieceEdgeVertCount, StraightCapTop/EndVertCount, StraightCapHingeVertIndexCount
    kDiagonalPieceVerts,
    kDiagonalPieceEdge0VertIndices,
    kDiagonalPieceEdge1VertIndices,
    kDiagonalPieceHingeVertIndices,
    sDiagonalPieceEdgeDistancePercentages,
    kDiagonalCapTopVerts,
    kDiagonalCapEndVerts,
    kDiagonalCapEdge0VertIndices,
    kDiagonalCapHingeVertIndices,
    sDiagonalCapEdgeDistancePercentages,
    21, 5, 11, 6, 5, 6,       // DiagonalPieceVertCount, DiagonalPieceEdgeVertCount, DiagonalCapTop/EndVertCount, DiagonalCapEdge/HingeVertIndexCount
    {
    {0x095a8d70, {0x00000000, 0x00000000, 0x00000000, 0x00000000}},
    {0xe95a8d8d, {0x00000000, 0x00000000, 0x00000000, 0x00000000}},
    {0xe953f219, {0x00000000, 0x00000000, 0x00000000, 0x00000000}},
    {0xe953f219, {0x00000000, 0x00000000, 0x00000000, 0x00000000}},
    {0xe953f219, {0x00000000, 0x00000000, 0x00000000, 0x00000000}},
    {0x8a11ce6f, {0x00000000, 0x00000000, 0x00000000, 0x00000000}},
    {0x8a11ce6f, {0x00000000, 0x00000000, 0x00000000, 0x00000000}},
    {0x8a11ce6f, {0x00000000, 0x00000000, 0x00000000, 0x00000000}},
    {0xaa1b0446, {0x00000000, 0x00000000, 0x00000000, 0x00000000}},
    {0xaa1b0446, {0x00000000, 0x00000000, 0x00000000, 0x00000000}},
    {0xaa1b0446, {0x00000000, 0x00000000, 0x00000000, 0x00000000}},
    {0x6a1b0358, {0x00000000, 0x00000000, 0x00000000, 0x00000000}},
    {0x6a1b0358, {0x00000000, 0x00000000, 0x00000000, 0x00000000}},
    {0x6a1b0358, {0x00000000, 0x00000000, 0x00000000, 0x00000000}},
    {0xa961018a, {0x00000000, 0x00000000, 0x00000000, 0x00000000}},
    {0xa961018a, {0x00000000, 0x00000000, 0x00000000, 0x00000000}},
    {0xa961018a, {0x00000000, 0x00000000, 0x00000000, 0x00000000}},
    },
};

}  // namespace sc4
