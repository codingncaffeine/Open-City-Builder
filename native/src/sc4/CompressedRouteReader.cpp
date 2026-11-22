// cSC4CompressedRouteReader — see CompressedRouteReader.h for the format and the layout.
#include "sc4/CompressedRouteReader.h"

namespace sc4 {
namespace {

// _kDX / _kDZ: the cell step of each packed side.
constexpr std::int16_t kDX[4] = {-1, 0, 1, 0};
constexpr std::int16_t kDZ[4] = {0, -1, 0, 1};

}  // namespace

// i386 0x000990b0. The header is read at once: the first record is the start cell with no entry side (0xff).
void cSC4CompressedRouteReader::SetData(const std::vector<std::uint8_t>* data) {
    data_ = data;
    cursor_ = 0;
    stepsLeft_ = 0;
    if (data && !data->empty()) {
        next_.type = static_cast<std::uint8_t>((*data)[cursor_++] & 0x3f);
        next_.x = static_cast<std::int16_t>(cursor_ < data->size() ? (*data)[cursor_] : 0);
        ++cursor_;
        next_.z = static_cast<std::int16_t>(cursor_ < data->size() ? (*data)[cursor_] : 0);
        ++cursor_;
        next_.runStep = 0;
        next_.entrySide = 0xff;
        done_ = false;
    } else {
        done_ = true;
    }
}

// i386 0x00098d40. The record the reader stands on is handed out first (x, z, type, run flag, entry side, crossing flag),
// then the reader moves on and fills in the record's next type and exit side from the step it took.
bool cSC4CompressedRouteReader::GetNext(CompressedRouteCell& out) {
    if (done_ || !data_) return false;
    out = next_;
    const std::vector<std::uint8_t>& data = *data_;
    const auto takeStep = [&]() {
        stepIndex_ = static_cast<std::uint8_t>((stepIndex_ + 1) & 3);
        if (stepIndex_ == 0) packed_ = cursor_ < data.size() ? data[cursor_++] : 0;
        const std::uint8_t side = static_cast<std::uint8_t>(packed_ & 3);
        out.exitSide = side;
        next_.entrySide = static_cast<std::uint8_t>((side + 2) & 3);
        packed_ = static_cast<std::uint8_t>(packed_ >> 2);
        --stepsLeft_;
        next_.x = static_cast<std::int16_t>(next_.x + kDX[side]);
        next_.z = static_cast<std::int16_t>(next_.z + kDZ[side]);
        next_.runStep = 1;
        out.nextType = next_.type;
        return true;
    };
    if (stepsLeft_ != 0) return takeStep();
    if (cursor_ != data.size()) {
        const std::uint8_t token = data[cursor_++];
        next_.type = static_cast<std::uint8_t>(token & 0x3f);
        out.nextType = static_cast<std::uint8_t>(token & 0x3f);
        if ((token & 0x40) == 0) {
            // A jump: the next cell is given outright. The exit side is the straight side towards it (two cells or more away
            // on one axis still counts), 0xff for a diagonal.
            const std::uint8_t nx = cursor_ < data.size() ? data[cursor_] : 0;
            ++cursor_;
            const std::uint8_t nz = cursor_ < data.size() ? data[cursor_] : 0;
            ++cursor_;
            next_.x = static_cast<std::int16_t>(nx);
            next_.z = static_cast<std::int16_t>(nz);
            next_.runStep = 0;
            next_.crossing = static_cast<std::uint8_t>(token >> 7);
            // The larger axis wins when both differ (a tie goes to z). Two or more cells along x leave through that x side; one
            // cell along x is a sideways hop with no exit side: the entry is blanked and carries over to the next cell. Along z
            // the side is -z only for two or more cells back (traffic-simulator.md 4.3; i386 0x00098e1c..0x00098ebc).
            int dx = static_cast<int>(static_cast<std::int16_t>(nx)) - out.x;
            int dz = static_cast<int>(static_cast<std::int16_t>(nz)) - out.z;
            if (dx != 0 && dz != 0) {
                const int ax = dx < 0 ? -dx : dx, az = dz < 0 ? -dz : dz;
                if (az < ax) dz = 0;
                else dx = 0;
            }
            if (dx >= 2) out.exitSide = 2;
            else if (dx <= -2) out.exitSide = 0;
            else if (dx != 0) {
                out.exitSide = 0xff;
                out.entrySide = 0xff;
                return true;
            } else out.exitSide = static_cast<std::uint8_t>(dz <= -2 ? 1 : 3);
            next_.entrySide = static_cast<std::uint8_t>((out.exitSide + 2) & 3);
            return true;
        }
        stepsLeft_ = cursor_ < data.size() ? data[cursor_] : 0;
        ++cursor_;
        stepIndex_ = 3;
        next_.crossing = 0;
        if (stepsLeft_ != 0) return takeStep();
    }
    done_ = true;
    out.exitSide = 0xff;
    out.nextType = out.type;
    return true;
}

}  // namespace sc4
