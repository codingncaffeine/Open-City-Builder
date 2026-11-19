// The snapshot taker and album hosts (NativeSnapshot.cpp).
#pragma once
#include <cstdint>
#include <vector>

class cIGZWin;

namespace native {
// The frame's pixels as top-down BGRA bytes (the GL driver's read-back); false when there is no frame.
using FrameReader = bool (*)(std::vector<std::uint8_t>& bgra, std::uint32_t& width, std::uint32_t& height);
void SetSnapshotFrameReader(FrameReader reader);
// Installs the snapshot host and the picture view's host.
void InstallSnapshotHost();
// cSC4WinSnapshotTaker::CreateWinSnapshotTaker(parent, 0): the frame over the city view.
bool CreateSnapshotTaker(cIGZWin* parent);
// cSC4WinSnapshotTaker::ShowSnapshotAlbum + DoModalWin: the album at the current city's album.
void ShowSnapshotAlbum(cIGZWin* parent);
}  // namespace native
