// cSC4PipeOccupant's piece tables (Mac i386 data 0x00ab7bec..0x00ab83b0, the record cSC4PipeOccupant::sCustomGeometryInfo
// at 0x00ab7a20): the straight piece and its cap, the diagonal piece and its cap (vertices as sVF_V3F_C4UB_T2F, the
// edge / hinge vertex indices, the straight edge distances) and the 17 texture records. The counts the original's
// static initializer copies in (0x00a7ee52) are written into the record directly; the two diagonal distance tables are
// filled by cSC4PipeOccupant::StaticInit.
#include "sc4/PipeOccupant.h"

namespace sc4 {
namespace {
const UndergroundVertex kStraightPieceVerts[12] = {          // 0x00ab8260
    {-8.0f, 0.0f, -8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-8.0f, 0.0f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-1.67900002f, 0.0f, -8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-1.67900002f, 0.0f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-0.697000027f, 1.13300002f, -8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-0.697000027f, 1.13300002f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {0.697000027f, 1.13300002f, -8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {0.697000027f, 1.13300002f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {1.67900002f, 0.0f, -8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {1.67900002f, 0.0f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {8.0f, 0.0f, -8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {8.0f, 0.0f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
};
const std::int32_t kStraightPieceEdge0VertIndices[6] = {1, 3, 5, 7, 9, 11};   // 0x00ab8230
const std::int32_t kStraightPieceEdge1VertIndices[6] = {0, 2, 4, 6, 8, 10};   // 0x00ab8218
const float kStraightPieceEdgeDistancePercentages[6] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};   // 0x00ab8200
const UndergroundVertex kStraightCapTopVerts[12] = {          // 0x00ab80e0
    {-8.0f, 0.0f, 0.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-8.0f, 0.0f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-1.67900002f, 0.0f, 0.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-1.67900002f, 0.0f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-0.697000027f, 1.13300002f, 0.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-0.697000027f, 1.13300002f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {0.697000027f, 1.13300002f, 0.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {0.697000027f, 1.13300002f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {1.67900002f, 0.0f, 0.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {1.67900002f, 0.0f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {8.0f, 0.0f, 0.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {8.0f, 0.0f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
};
const UndergroundVertex kStraightCapEndVerts[6] = {           // 0x00ab8040
    {-1.67900002f, 0.0f, 0.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-0.697000027f, -1.13300002f, 0.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-0.697000027f, 1.13300002f, 0.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {0.697000027f, -1.13300002f, 0.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {0.697000027f, 1.13300002f, 0.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {1.67900002f, 0.0f, 0.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
};
const std::int32_t kStraightCapHingeVertIndices[6] = {0, 2, 4, 6, 8, 10};   // 0x00ab8018
const UndergroundVertex kDiagonalPieceVerts[21] = {           // 0x00ab7e20
    {0.0f, 0.0f, 0.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-8.0f, 0.0f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {2.81299996f, 0.0f, 2.81299996f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-2.37400007f, 0.0f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {3.50699997f, 1.13300002f, 3.50699997f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-0.985000014f, 1.13300002f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {4.49300003f, 1.13300002f, 4.49300003f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {0.985000014f, 1.13300002f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {5.1869998f, 0.0f, 5.1869998f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {2.37400007f, 0.0f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {8.0f, 0.0f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {5.1869998f, 0.0f, 5.1869998f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {8.0f, 0.0f, 2.37400007f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {4.49300003f, 1.13300002f, 4.49300003f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {8.0f, 1.13300002f, 0.985000014f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {3.50699997f, 1.13300002f, 3.50699997f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {8.0f, 1.13300002f, -0.985000014f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {2.81299996f, 0.0f, 2.81299996f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {8.0f, 0.0f, -2.37400007f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {0.0f, 0.0f, 0.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {8.0f, 0.0f, -8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
};
const std::int32_t kDiagonalPieceEdge0VertIndices[5] = {1, 3, 5, 7, 9};   // 0x00ab7df4
const std::int32_t kDiagonalPieceEdge1VertIndices[5] = {20, 18, 16, 14, 12};   // 0x00ab7de0
const std::int32_t kDiagonalPieceHingeVertIndices[6] = {0, 2, 4, 6, 8, 10};   // 0x00ab7dc8
const UndergroundVertex kDiagonalCapTopVerts[11] = {          // 0x00ab7cc0
    {-8.0f, 0.0f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-5.1869998f, 0.0f, 5.1869998f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-2.37400007f, 0.0f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-4.49300003f, 1.13300002f, 4.49300003f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-0.985000014f, 1.13300002f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-3.50699997f, 1.13300002f, 3.50699997f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {0.985000014f, 1.13300002f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-2.81299996f, 0.0f, 2.81299996f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {2.37400007f, 0.0f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {0.0f, 0.0f, 0.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {8.0f, 0.0f, 8.0f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
};
const UndergroundVertex kDiagonalCapEndVerts[6] = {           // 0x00ab7c20
    {-5.1869998f, 0.0f, 5.1869998f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-4.49300003f, -1.13300002f, 4.49300003f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-4.49300003f, 1.13300002f, 4.49300003f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-3.50699997f, -1.13300002f, 3.50699997f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-3.50699997f, 1.13300002f, 3.50699997f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
    {-2.81299996f, 0.0f, 2.81299996f, {0x00, 0x00, 0x00, 0x00}, 0.0f, 0.0f},
};
const std::int32_t kDiagonalCapEdge0VertIndices[5] = {2, 4, 6, 8, 10};   // 0x00ab7c04
const std::int32_t kDiagonalCapHingeVertIndices[6] = {1, 3, 5, 7, 9, 0};   // 0x00ab7bec
}  // namespace

// Written by StaticInit (bss 0x00bfa834 / 0x00bfa820 in the original).
float cSC4PipeOccupant::sDiagonalPieceEdgeDistancePercentages[5] = {};
float cSC4PipeOccupant::sDiagonalCapEdgeDistancePercentages[5] = {};

tCustomGeometryInfo cSC4PipeOccupant::sCustomGeometryInfo = {
    {kStraightPieceVerts, nullptr, nullptr, kStraightPieceEdge0VertIndices, kStraightPieceEdge1VertIndices,
     kStraightPieceEdgeDistancePercentages, kStraightCapTopVerts, kStraightCapEndVerts, kStraightCapHingeVertIndices},
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
    {0x8a1b1d4a, {0x00000000, 0x00000000, 0x00000000, 0x00000000}},
    {0xea38125b, {0x00000000, 0x00000000, 0x00000000, 0x00000000}},
    {0xea38125b, {0x00000000, 0x00000000, 0x00000000, 0x00000000}},
    {0x00000000, {0x00000000, 0x00000000, 0x00000000, 0x00000000}},
    {0x00000000, {0x00000000, 0x00000000, 0x00000000, 0x00000000}},
    {0x00000000, {0x00000000, 0x00000000, 0x00000000, 0x00000000}},
    {0x00000000, {0x00000000, 0x00000000, 0x00000000, 0x00000000}},
    {0x00000000, {0x00000000, 0x00000000, 0x00000000, 0x00000000}},
    {0x00000000, {0x00000000, 0x00000000, 0x00000000, 0x00000000}},
    {0x00000000, {0x00000000, 0x00000000, 0x00000000, 0x00000000}},
    {0x00000000, {0x00000000, 0x00000000, 0x00000000, 0x00000000}},
    {0x00000000, {0x00000000, 0x00000000, 0x00000000, 0x00000000}},
    {0xea1b27f1, {0x00000000, 0x00000000, 0x00000000, 0x00000000}},
    {0x0a3813e1, {0x00000000, 0x00000000, 0x00000000, 0x00000000}},
    {0x0a3813e1, {0x00000000, 0x00000000, 0x00000000, 0x00000000}},
    },
};

}  // namespace sc4
