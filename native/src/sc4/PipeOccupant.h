// cSC4PipeOccupant - one piece of underground water pipe (class id / interface id 0x49c05b9f). A cSC4UndergroundOccupant
// (network type 4, model instance offset 0x6000000) that adds one field: the watered state at +0x358 (0 dry, 1 watered;
// the draw colour is looked up by it). Recovered from SimCity 4 Deluxe Edition 1.2.1 (0x1001a0ec8..0x1001a14c6).
//
// The piece tables (sCustomGeometryInfo) are in PipeOccupantGeometry.cpp; StaticInit fills the two diagonal distance
// tables.
#pragma once
#include "sc4/PipeOccupantView.h"
#include "sc4/UndergroundOccupant.h"

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4PipeOccupant = 0x49c05b9f;
constexpr std::uint32_t kGZIID_cISC4PipeOccupantView = 0xa9764c8c;   // the second interface QueryInterface answers
constexpr std::uint32_t kPipeNetworkType = 4;
constexpr std::int32_t kPipeModelInstanceOffset = 0x6000000;

class cSC4PipeOccupant : public cSC4UndergroundOccupant {
public:
    cSC4PipeOccupant();                                                  // 0x1001a0ec8
    ~cSC4PipeOccupant() override = default;                              // 0x1001a0f6c

    bool QueryInterface(std::uint32_t iid, void** out) override;        // 0x1001a1332
    bool Init() override;                                                // 0x1001a102e
    bool Init(const float (*corners)[3]);                                // 0x1001a11ae
    bool Shutdown() override;                                            // 0x1001a1200
    bool Write(cIGZOStream& stream) const override;                      // 0x1001a1218 (record version 3)
    bool Read(cIGZIStream& stream) override;                             // 0x1001a1290
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4PipeOccupant; }   // 0x1001a14a0
    void GetSortValue(std::uint32_t& out) const override;               // 0x1001a13a0
    cS3DVector3 GetDrawColor() const override;                           // 0x1001a1434
    const tCustomGeometryInfo* GetCustomGeometryInfo() const override;  // 0x1001a14ac
    std::uint32_t GetNetworkType() const override { return kPipeNetworkType; }                 // 0x1001a14ba
    std::int32_t GetModelInstanceOffset() const override { return kPipeModelInstanceOffset; }   // 0x1001a14c6

    void SetWateredState(bool watered);                                  // 0x1001a1404
    std::int32_t WateredState() const { return wateredState_; }

    static bool StaticInit();                                            // 0x1001a106a (i386 0x001368d8)
    static tCustomGeometryInfo sCustomGeometryInfo;                      // i386 0x00ab7a20
    static float sDiagonalPieceEdgeDistancePercentages[5];              // i386 0x00bfa834
    static float sDiagonalCapEdgeDistancePercentages[5];                // i386 0x00bfa820

private:
    // The pipe-burst disaster's view of this pipe (kGZIID_PortPipeOccupantView): shares the pipe's count.
    class BurstView final : public cISC4PipeOccupantView {
    public:
        explicit BurstView(cSC4PipeOccupant* pipe) : pipe_(pipe) {}
        bool QueryInterface(std::uint32_t iid, void** out) override { return pipe_->QueryInterface(iid, out); }
        std::uint32_t AddRef() override { return pipe_->AddRef(); }
        std::uint32_t Release() override { return pipe_->Release(); }
        void GetCell(std::uint32_t& x, std::uint32_t& z) override { pipe_->GetOccupiedCell(x, z); }   // i386 +0xec
        void GetCenter(float* xyz) override { pipe_->GetPosition(xyz); }                              // i386 +0x148
        bool IsConnected(int direction, int kind) override {                                         // i386 +0xbc
            return pipe_->ConnectsToNeighbor(direction, static_cast<std::uint8_t>(kind));
        }
        cISC4Occupant* AsOccupant() override { return pipe_; }

    private:
        cSC4PipeOccupant* pipe_;
    };

    std::int32_t wateredState_ = 0;   // +0x358
    BurstView burstView_{this};
};

}  // namespace sc4
