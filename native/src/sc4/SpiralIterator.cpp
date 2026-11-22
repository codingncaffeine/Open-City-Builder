#include "sc4/SpiralIterator.h"

#include <cstdint>

namespace sc4 {

SpiralIterator SpiralIterator::Over(int startX, int startZ, int width, int height) {
    SpiralIterator it;
    it.x = startX;
    it.z = startZ;
    it.targetX = startX + 1;
    it.targetZ = startZ;
    it.minX = it.minZ = 0;
    it.maxX = width < 1 ? 0 : width - 1;
    it.maxZ = height < 1 ? 0 : height - 1;
    it.mode = 1;
    it.direction = 1;
    it.state = 0;
    it.onZ = 1;
    it.edgesHit = 0;
    it.turns = 0;
    if ((startX | startZ) < 0 || it.maxX < startX || it.maxZ < startZ) {
        it.state = 2;
        it.edgesHit = 0xf;
    }
    return it;
}

SpiralIterator SpiralIterator::Unbounded() {
    SpiralIterator it;
    it.minX = it.minZ = INT32_MIN;
    it.maxX = it.maxZ = INT32_MAX;
    it.mode = 1;
    it.turns = 0;
    it.Reset(0, 0);
    return it;
}

// A start outside the clip rect finishes the walk at once.
void SpiralIterator::Reset(int startX, int startZ) {
    x = startX;
    z = startZ;
    turns = 0;
    targetX = mode == 1 ? startX + 1 : startX - 1;
    targetZ = startZ;
    onZ = 1;
    direction = mode == 1 ? 1 : -1;
    edgesHit = 0;
    state = 0;
    if (x < minX || z < minZ || maxX < x || maxZ < z) {
        state = 2;
        edgesHit = 0xf;
    }
}

void SpiralIterator::SetClipRect(int x0, int z0, int x1, int z1) {
    minX = x0;
    minZ = z0;
    maxX = x1 < x0 ? x0 : x1;
    maxZ = z1 < z0 ? z0 : z1;
}

bool SpiralIterator::GetNext(int& outX, int& outZ) {
    if (state == 2) return false;
    bool advance = true;
    if (state == 0) {
        state = 1;
        advance = false;
    }
    if (advance) {
        bool finishedRun = false;
        if (!onZ) {
            const int old = z;
            int next;
            if (direction < 1) {
                next = old - 1;
                z = next;
                finishedRun = old <= minZ;
            } else {
                next = old + 1;
                z = next;
                finishedRun = maxZ <= old;
            }
            if (finishedRun) z = targetZ;
            else finishedRun = next == targetZ;
        } else {
            const int old = x;
            int next;
            if (direction < 1) {
                next = old - 1;
                x = next;
                finishedRun = !(minX < old);
            } else {
                next = old + 1;
                x = next;
                finishedRun = !(old < maxX);
            }
            if (finishedRun) x = targetX;
            else finishedRun = next == targetX;
        }
        if (finishedRun) DoXZEnd();
    }
    if (edgesHit == 0xf) {
        state = 2;
        return false;
    }
    outX = x;
    outZ = z;
    return true;
}

// Called when the current run reaches its end: turns the walker onto the other axis, lengthens the run and keeps
// going until the new run touches the rectangle (a run entirely outside it only records which edge it crossed).
bool SpiralIterator::DoXZEnd() {
    if (edgesHit == 0xf) return false;
    std::uint8_t bits = edgesHit;
    std::uint8_t axis = onZ;
    int dir = direction;
    do {
        if (axis == 0) {
            const int cz = z;
            if (dir < 1) {
                if (minZ <= cz) {
                    onZ = 1;
                    if (mode == 1) { dir = -dir; direction = dir; }
                    const int step = dir > 0 ? 1 : -1;
                    direction = step + dir;
                    targetX = step + dir + x;
                    targetZ = cz;
                    if (!(minX <= x)) x = minX;
                    return true;
                }
                bits |= 8;
            } else {
                if (cz <= maxZ) {
                    onZ = 1;
                    if (mode == 1) { dir = -dir; direction = dir; }
                    const int step = dir > 0 ? 1 : -1;
                    direction = step + dir;
                    targetX = step + dir + x;
                    targetZ = cz;
                    if (!(x <= maxX)) x = maxX;
                    return true;
                }
                bits |= 4;
            }
            edgesHit = bits;
            onZ = 1;
            if (mode == 1) { dir = -dir; direction = dir; }
            dir += dir < 1 ? -1 : 1;
            direction = dir;
            targetX = x + dir;
            targetZ = cz;
            x += dir;
            axis = 1;
        } else {
            const int cx = x;
            if (dir < 1) {
                if (minX <= cx) {
                    if (mode == 0) {
                        if (dir < 0) ++turns;
                        onZ = 0;
                        targetX = cx;
                        dir = -dir;
                        direction = dir;
                    } else {
                        onZ = 0;
                        targetX = cx;
                    }
                    targetZ = dir + z;
                    if (!(z <= maxZ)) z = maxZ;
                    return true;
                }
                bits |= 2;
                edgesHit = bits;
                if (mode == 0) {
                    if (dir < 0) ++turns;
                    onZ = 0;
                    targetX = cx;
                    dir = -dir;
                    direction = dir;
                } else {
                    onZ = 0;
                    targetX = cx;
                }
            } else {
                if (cx <= maxX) {
                    if (mode == 0) {
                        onZ = 0;
                        targetX = cx;
                        dir = -dir;
                        direction = dir;
                    } else {
                        if (mode == 1) ++turns;
                        onZ = 0;
                        targetX = cx;
                    }
                    targetZ = dir + z;
                    if (minZ > z) z = minZ;
                    return true;
                }
                bits |= 1;
                edgesHit = bits;
                if (mode != 0) {
                    if (mode == 1) ++turns;
                    onZ = 0;
                    targetX = cx;
                } else {
                    onZ = 0;
                    targetX = cx;
                    dir = -dir;
                    direction = dir;
                }
            }
            targetZ = z + dir;
            z += dir;
            axis = 0;
        }
    } while (bits != 0xf);
    return false;
}

}  // namespace sc4
