// cSC4NetworkManager — the city's network ownership layer. 39 symbols at 0x10016dffe..0x100171e14,
// three vtables: 0x100a796f0 (cISC4NetworkManager, 30 slots), 0x100a797f0 (cIGZMessageTarget2, 4 slots),
// 0x100a79820 (cIGZSerializable, 6 slots). Class id 0xc990bd46, interface id 0xa9764c8c.
//
// The object does *not* own the per-cell network storage: roads, pipes and wires live in the occupants
// (cSC4NetworkOccupant and friends) and are indexed by cSC4OccupantManager / cSC4TrafficNetworkMap. What the
// manager owns is:
//   * the thirteen per-network-type tools (cSC4NetworkTool / cSC4PipeTool / cSC4PowerLineTool /
//     cSC4SubwayTool) the network tools and the UI borrow (GetNetworkTool),
//   * the occupants that ask to be tracked (+0x30, driven by messages 0x99ef1142/0x99ef1143),
//   * the retrofit list (+0xd8): network occupants whose old intersection piece has to be replaced,
//   * the batch removal queue (+0xb8, QueueRemovals) and the running construction crews (+0xf0),
//   * the texture / shadow / path remap tables Reload reads from the game's ini resource
//     {0x8a5971c5, 0, 0x8a5993b9} (same resource cSC4PowerLineTool reads its poles from).
//
// Object layout (0x170 bytes, the size GZCOM records for the class; offsets are the original ones):
//   +0x000 vtable cISC4NetworkManager   +0x008 vtable cIGZMessageTarget2   +0x010 vtable cIGZSerializable
//   +0x018 uint32 ref count (starts at 0)  +0x01c bool inited  +0x01d bool apply UK path remap
//   +0x01e bool extend-region flag (0xea3b0af1 sets it, the city-size fan-out clears it)
//   +0x020 int32 region minX   +0x024 int32 region minZ  +0x028 int32 region maxX  +0x02c int32 region maxZ
//        (INT_MAX, INT_MAX, -1, -1 from the ctor; messages grow/shrink the rectangle)
//   +0x030 std::vector<cISC4NetworkOccupant*> tracked occupants (one reference each)
//   +0x048 std::unordered_multimap<uint32, ShadowRemapByEdgesEntry>  ("Shadow remap by edges")
//   +0x070 std::vector<TextureRemapEntry> preview remaps              ("Texture remapping")
//   +0x088 std::vector<TextureRemapEntry> shadow remaps               ("Shadow remap")
//   +0x0a0 std::vector<PathRemapEntry> path remaps                    ("UK path mapping")
//   +0x0b8 std::vector<cISC4Occupant*> pending removal queue (each entry has one reference)
//   +0x0d0 bool the removal queue is active (QueueRemovals argument)
//   +0x0d8 std::list<cISC4NetworkOccupant*> retrofit list (one reference each)
//   +0x0e8 size of that list
//   +0x0f0 std::vector<cSC4NetworkConstructionCrew*> running crews (one reference each)
//   +0x100 end of the vector; +0x108..+0x168 the 13 tools (one pointer per eNetworkType, borrowed)
//
// Quirks kept on purpose (each is asserted by the test):
//  * The constructor starts the reference count at 0, so Release() on a never-AddRef'd object destroys it.
//  * Init() only registers its eleven messages *and* creates the thirteen tools when the message server
//    exists; without it the object stays "inited" with no tools (0x1001709d6 guard).
//  * The removal queue keeps the tracked occupants' last two slots uninitialised: AddTextureMapping copies two
//    bytes of stack (+0x16/+0x17); the port zeroes them (never read).
//  * MapPreviewTexture/MapShadowTexture mask the alpha with 0x83 (not 0x7f), and the "subtract" branch adds
//    seven to the (signed) delta before subtracting.
//  * A shadow-remap entry whose +0x08 word is non-zero is used directly; when it is zero the "by edges" map is
//    consulted with the fourth argument as key, and the alpha delta comes from the *vector* entry, not the map.
//  * The construction-crew clear in DoMessage (0x26d31ec2) calls crew->Shutdown() without removing the vector
//    entry; the original relies on Shutdown calling UnregisterConstructionCrew.
//  * DoViewRotationChange ignores its own arguments (the decompiler passes the *message* pointer as `this`).
//
// PARTIAL (the recovered-partial rows in notes/ledger.csv): DoMessage's city-size / underground-height
// fan-out, Retrofit, RemoveOrReplaceOccupant, GetAllIntersectionPieces(2), CreateConnectionNode and
// AutoconnectLotNodes need the network-occupant slots 0x180/0x210, the cSC4NetworkORFilter, the pipe/subway
// tools' placement pass and the underground tool's height update, none of which are recovered yet.
#pragma once
#include <cstddef>
#include <cstdint>
#include <list>
#include <unordered_map>
#include <vector>

#include "gz/cIGZMessage2.h"
#include "gz/cIGZSerializable.h"
#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "rz/cRZIniFile.h"
#include "rz/cRZString.h"
#include "sc4/Occupant.h"

namespace sc4 {

// The engine's network type enum and point template (cISC4NetworkManager::eNetworkType, defined in
// src/sc4/NetworkTool.h). They are left as opaque declarations here so this header does not pull in the whole
// tool class; the .cpp includes NetworkTool.h for the enumerators and asserts the counts match.
enum eNetworkType : std::uint32_t;
template <typename T>
struct SC4PointT;

// cISC4NetworkManager::eNetworkType has 13 values (Road..GroundHighway); the tool array is indexed by it.
constexpr std::size_t kNetworkToolCount = 13;

constexpr std::uint32_t kGZIID_cISC4NetworkManager = 0xa9764c8c;
constexpr std::uint32_t kGZCLSID_cSC4NetworkManager = 0xc990bd46;
constexpr std::uint32_t kGZIID_cISC4NetworkOccupant = 0xa821ef94;
constexpr std::uint32_t kGZIID_cSC4NetworkOccupantWithPreBuiltModel = 0x49c1a034;

// The ini resource the remap tables come from (SC4GetResourceAsFile @ 0x10003a3d2; the call site is
// 0x10016e22e) and the occupant type the network pieces report.
constexpr std::uint32_t kNetworkManagerIniType = 0x8a5971c5;
constexpr std::uint32_t kNetworkManagerIniGroup = 0;
constexpr std::uint32_t kNetworkManagerIniInstance = 0x8a5993b9;
constexpr std::uint32_t kNetworkOccupantType = 0xc772bf98;

// cISC4NetworkOccupant::GetNetworkFlags (+0x60) mask groups DestroyNetworkOccupants sweeps
// (kNetworkClasses @ 0x100a194f0); the resolver types 0, 4 and 7 are in the .cpp.
constexpr std::uint32_t kNetworkDestroyClassMasks[3] = {0x1f4f, 0x10, 0x80};

// The eleven messages Init registers on the message server (0x100170a4e..0x100170ac8).
constexpr std::uint32_t kNetworkManagerMessages[11] = {
    0xa6b79602, 0x278e6003, 0xea3b0af1, 0x6a3b0afd, 0x6ab97795, 0x26d31ec2,
    0x26d31ec1, 0x99ef1142, 0x99ef1143, 0xc9da96e8, 0xc9da96e9,
};

// The name table GetEnglishNetworkName returns (cSC4NetworkTool::kNetworkTypeNames @ 0x100a7a8a0, the same
// 13 strings the "Shadow remap by edges" section matches by name). The array itself lives in the .cpp.
extern const char* const kNetworkTypeNameTable[kNetworkToolCount];

// One "Texture remapping"/"Shadow remap" line: mask match, transform, alpha delta, stop flag (0x18 bytes, the
// stride is the original's; +0x16/+0x17 are uninitialised stack bytes in the binary).
struct TextureRemapEntry {
    std::uint32_t matchValue = 0;    // +0x00  (value & matchMask) == matchValue selects the entry
    std::uint32_t matchMask = 0;     // +0x04
    std::uint32_t addValue = 0;      // +0x08  result = ((value & transformMask) + addValue) - (value & subtractMask)
    std::uint32_t transformMask = 0; // +0x0c
    std::uint32_t subtractMask = 0;  // +0x10  (the eighth ini number)
    std::uint8_t alphaDelta = 0;     // +0x14  bit 7 clear: add, set: (delta + 8) - alpha; then & 0x83
    bool stop = false;               // +0x15  stop looking after this entry
    std::uint16_t padding = 0;       // +0x16  uninitialised in the original
};
static_assert(sizeof(TextureRemapEntry) == 0x18, "TextureRemapEntry is the original's 0x18-byte record");
static_assert(offsetof(TextureRemapEntry, alphaDelta) == 0x14, "alpha delta is at +0x14");
static_assert(offsetof(TextureRemapEntry, stop) == 0x15, "the stop flag is at +0x15");

// One "Shadow remap by edges" value: the map key is the first hex number of the line (the piece id).
struct ShadowRemapByEdgesEntry {
    std::uint32_t networkType = 0;   // +0x00 inside the value (node +0x14): index of the type name
    std::uint32_t addValue = 0;      // +0x04 (node +0x18)
    std::uint32_t transformMask = 0; // +0x08 (node +0x1c)
    std::uint8_t alphaDelta = 0;     // +0x0c (node +0x20)
    std::uint8_t padding[3] = {};
};
static_assert(sizeof(ShadowRemapByEdgesEntry) == 0x10, "the map value is 0x10 bytes");
static_assert(offsetof(ShadowRemapByEdgesEntry, alphaDelta) == 0xc, "alpha delta is value+0xc");

// One "UK path mapping" line (0x10 bytes).
struct PathRemapEntry {
    std::uint32_t matchValue = 0;   // +0x00
    std::uint32_t matchMask = 0;    // +0x04
    std::uint32_t addValue = 0;     // +0x08
    std::uint32_t transformMask = 0;// +0x0c
};
static_assert(sizeof(PathRemapEntry) == 0x10, "PathRemapEntry is 0x10 bytes");

// The engine's intrusive list of pointers (the original's ilist<T>: sentinel node + count, elements at node
// +0x10). The recovered functions only walk it, count it and insert the head; std::list gives the same
// iteration order and reference ownership (each entry owns one reference), so the port uses it directly.
template <typename T>
using ilist = std::list<T>;

// cSC4NetworkConstructionCrew (vtable 0x100a84160) is not recovered. The manager only keeps a counted
// reference and calls its vslot 0x28 (Shutdown) when a game reset destroys the crews. This placeholder holds
// the two cIGZUnknown counters; replace it with the real class when that is recovered.
class cSC4NetworkConstructionCrew : public cIGZUnknown {
public:
    virtual void Shutdown() = 0;   // [5] +0x28; the original implementation unregisters itself
};

class cSC4PipeOccupant;             // forward: the manager only hands the out-parameter through
class cSC4PipeConnectionOccupant;   // forward: ditto
class cISC4Lot;                     // forward: AutoconnectLotNodes only (PARTIAL)
class cISC4OccupantFilter;          // cSC4OccupantFilter / cSC4WaterOccupantFilter / cSC4SubwayOccupantFilter

// cISC4NetworkOccupant (iid 0xa821ef94) as this class uses it. The comments are the original byte offsets.
class INetworkManagerOccupant : public cIGZUnknown {
public:
    virtual bool IsManagedGlobally() const = 0;    // +0x28  message 0x99ef1142/43 track only these
    virtual bool HasNetworkFlag(std::uint32_t flag) = 0;   // +0x40
    virtual std::uint32_t GetNetworkFlags() = 0;   // +0x60
    virtual void SetLit(bool lit) = 0;             // +0x1e8 (cSC4NetworkOccupantWithPreBuiltModel::SetLit)
    virtual bool SupportsLighting() = 0;           // +0x1f0
    virtual void UpdateViewRotation() = 0;         // +0x170 (DoViewRotationChange)
    virtual cISC4Occupant* AsOccupant() { return nullptr; }   // slot 48 (not counted)
};

// cISC4OccupantManager (SL::spOccupantManager) as the three queries of this slice use it. The real class is
// recovered (src/sc4/OccupantManager.h) but its header repeats kGZIID_cIGZMessageTarget2, which NetworkTool.h
// (the source of the shared eNetworkType enum) also defines, so the manager reaches it through this seam and
// an adapter. The comments are the original vtable slots.
class INetworkManagerOccupantManager {
public:
    virtual ~INetworkManagerOccupantManager() = default;
    // +0x120: occupants whose bounding box overlaps the rectangle (null pointers = the whole city), filtered
    // by occupant type (0 = any) and capped at maxCount (0xffffffff = unlimited). Every entry has a reference.
    virtual void GetOccupantsByBBox(ilist<cISC4Occupant*>& out, const float* min, const float* max,
                                    std::uint32_t type, std::uint32_t maxCount) = 0;
    // +0x180: release every entry's reference and clear the list.
    virtual void ReleaseOccupantList(ilist<cISC4Occupant*>& list) = 0;
    // +0x160 (table index 0x2c): walk the occupants of one type; the callback returns false to stop.
    virtual void IterateOccupants(bool (*callback)(cISC4Occupant*, void*), void* context,
                                  std::uint32_t type) = 0;
};

// The network tools Init creates and GetNetworkTool hands out (cSC4NetworkTool / cSC4PipeTool /
// cSC4PowerLineTool / cSC4SubwayTool). Only the slots the recovered manager calls are declared; the original
// byte offsets are in the comments. A fresh tool comes back with a zero reference count, like the original
// constructors leave it.
class INetworkManagerTool : public cIGZUnknown {
public:
    virtual bool Init() = 0;                                                  // +0x18
    virtual bool Shutdown() = 0;                                              // +0x20
    virtual void DestroyOccupantsAndResolve(std::vector<INetworkManagerOccupant*>& occupants) = 0;  // +0x190
};

// Everything outside the object. One method per original call; the global/slot is in the comment.
class INetworkManagerHost {
public:
    virtual ~INetworkManagerHost() = default;

    virtual cIGZMessageServer2* MessageServer() = 0;            // SL::spMS2 (0x100041... ; null is allowed)
    // RZGetFramework service 0x441e5070 / iid 0x54b7d5 -> vslot 0x128(0) -> vslot 0xc8: the language flag
    // Init inverts into +0x1d (the UK path-remap switch).
    virtual bool LanguageFlag() = 0;
    // SC4GetResourceAsFile({0x8a5971c5, 0, 0x8a5993b9}, &file, false) opened as an ini file. The caller
    // deletes it (cRZIniFile holds one reference to the file and releases it in its destructor).
    virtual cRZIniFile* OpenNetworkIni() = 0;
    // The constructor GetNetworkTool(type, true) / Init performs: new cSC4NetworkTool(0x638) for the road
    // types, new cSC4PipeTool(0x698) for pipe, new cSC4PowerLineTool(0x6e8), new cSC4SubwayTool(0x698).
    // A fresh tool starts at reference count 0, exactly like those constructors; the caller AddRefs it.
    virtual INetworkManagerTool* CreateNetworkTool(eNetworkType type) = 0;
    // cSC4WaterOccupantFilter (vtable 0x100a7b328) / cSC4SubwayOccupantFilter (0x100a7af58): new(0x18) plus
    // the cSC4OccupantFilter constructor, one reference.
    virtual cISC4OccupantFilter* CreateOccupantFilter(bool subway) = 0;
    virtual bool IsOccupantSubwayStation(cISC4Occupant* occupant) = 0;         // cSC4SubwayOccupantFilter static
    virtual void FindPipeOccupant(int x, int z, cSC4PipeOccupant*& out) = 0;   // cSC4PipeTool::FindPipeOccupant
    virtual void FindPipeConnectionOccupant(int x, int z, cSC4PipeConnectionOccupant*& out) = 0;
    virtual INetworkManagerOccupantManager* OccupantManager() = 0;   // SL::spOccupantManager
    // SL::sp24HourClock vslot 0xe0. The message branch 0x26d31ec1 sets the lights on when it is non-zero; that
    // it means "night" is inferred from the branch (the clock class is not recovered).
    virtual bool IsNight() = 0;
    // SC4WriteGZSerializable(stream, object) @ 0x1000435e3 and SC4ReadGZSerializable(stream, iid, &out, false)
    // @ 0x100043267. Both need a DB segment stream, so they stay a seam.
    virtual bool WriteGZSerializable(cIGZOStream& stream, const cIGZUnknown* object) = 0;
    virtual bool ReadGZSerializable(cIGZIStream& stream, std::uint32_t iid, void** out) = 0;
};

void SetNetworkManagerHost(INetworkManagerHost* host);
// PORT: the port's network occupants do not answer cISC4NetworkOccupant (0xa821ef94; their QueryInterface is shared
// with other seams), so a host can supply the manager's view of one: one reference, or null.
using NetworkOccupantViewProvider = INetworkManagerOccupant* (*)(cISC4Occupant* occupant);
void SetNetworkOccupantViewProvider(NetworkOccupantViewProvider provider);
INetworkManagerHost* NetworkManagerHost();

// cISC4NetworkManager: the primary interface (vtable 0x100a796f0, 30 slots). Only the virtuals this slice
// recovers are declared, in the original order; the unimplemented slots are listed in the comments so the
// gaps stay visible. Like the other interfaces in this port it is NOT layout-compatible with the game.
class cISC4NetworkManager : public cIGZUnknown {
public:
    virtual bool Init() = 0;                                                   // [3]  0x018
    virtual bool Shutdown() = 0;                                               // [4]  0x020
    virtual INetworkManagerTool* GetNetworkTool(eNetworkType type, bool create) = 0;  // [5] 0x028
    virtual void DestroyNetworkOccupants(const ilist<cISC4Occupant*>& occupants) = 0;  // [6] 0x030
    virtual void QueueRemovals(bool queue) = 0;                                // [7]  0x038
    virtual bool RemoveOrReplaceOccupant(INetworkManagerTool* tool, cISC4Occupant* occupant,
                                         bool& replaced) = 0;                  // [8]  0x040
    virtual bool IsOccupantSubwayStation(cISC4Occupant* occupant) = 0;         // [9]  0x048
    virtual void FindPipeOccupant(int x, int z, cSC4PipeOccupant*& out) = 0;   // [10] 0x050
    virtual void FindPipeConnectionOccupant(int x, int z, cSC4PipeConnectionOccupant*& out) = 0;  // [11] 0x058
    virtual void GetAllIntersectionPieces(INetworkManagerOccupant* occupant, ilist<cISC4Occupant*>& out,
                                          INetworkManagerOccupantManager* manager,
                                          INetworkManagerOccupant* root) = 0;  // [12] 0x060
    virtual std::uint32_t MapPreviewTexture(std::uint32_t texture, std::uint8_t& alpha) = 0;   // [13] 0x068
    virtual std::uint32_t MapShadowTexture(std::uint32_t texture, std::uint8_t& alpha, std::uint32_t network,
                                           std::uint32_t piece) = 0;           // [14] 0x070
    virtual std::uint32_t MapPath(std::uint32_t path) = 0;                     // [15] 0x078
    virtual void RegisterConstructionCrew(cSC4NetworkConstructionCrew* crew) = 0;    // [16] 0x080
    virtual void UnregisterConstructionCrew(cSC4NetworkConstructionCrew* crew) = 0;  // [17] 0x088
    virtual cISC4OccupantFilter* GetWaterOccupantFilter() = 0;                 // [18] 0x090
    virtual cISC4OccupantFilter* GetSubwayOccupantFilter() = 0;                // [19] 0x098
    virtual bool CreateConnectionNode(cISC4Occupant*& out, eNetworkType type, const SC4PointT<int>& cell,
                                      std::uint32_t a, std::uint32_t b, std::uint32_t c,
                                      std::uint8_t d) = 0;                     // [20] 0x0a0
    virtual void AutoconnectLotNodes(cISC4Lot* lot, const ilist<cISC4Occupant*>& occupants) = 0;  // [21] 0x0a8
    virtual const char* GetEnglishNetworkName(eNetworkType type) = 0;          // [22] 0x0b0
    virtual void AddToRetrofitList(cISC4Occupant* occupant) = 0;               // [23] 0x0b8
    // [24]/[25] 0x0c0/0x0c8 destructors; [26]/[27]/[28] are cIGZSerializable's Read/Write/GetGZCLSID;
    // [29] 0x0e8 DoMessage is declared by cIGZMessageTarget2.
    virtual ~cISC4NetworkManager() = default;
};

class cSC4NetworkManager final : public cISC4NetworkManager,
                                 public cIGZMessageTarget2,
                                 public cIGZSerializable {
public:
    cSC4NetworkManager();
    ~cSC4NetworkManager() override;
    cSC4NetworkManager(const cSC4NetworkManager&) = delete;
    cSC4NetworkManager& operator=(const cSC4NetworkManager&) = delete;

    // ---- cIGZUnknown ----
    bool QueryInterface(std::uint32_t iid, void** out) override;   // 0x100170e1e
    std::uint32_t AddRef() override;                               // 0x10016f38a
    std::uint32_t Release() override;                              // 0x100170e7e

    // ---- cIGZSerializable ----
    bool Write(cIGZOStream& stream) const override;                // 0x100171d8c
    bool Read(cIGZIStream& stream) override;                       // 0x100171cb2
    std::uint32_t GetGZCLSID() const override;                     // 0x100171e08

    // ---- cIGZMessageTarget2 ----
    bool DoMessage(cIGZMessage2* message) override;                // 0x10016f3b4

    // ---- cISC4NetworkManager ----
    bool Init() override;                                                          // 0x100170928
    bool Shutdown() override;                                                      // 0x100170ee6
    INetworkManagerTool* GetNetworkTool(eNetworkType type, bool create) override;  // 0x10016e6c8
    // PORT: a tool the host supplies after Init ran (the city loads before the view's hosts are installed); an
    // occupied slot keeps its tool.
    void AdoptNetworkTool(eNetworkType type, INetworkManagerTool* tool);
    void DestroyNetworkOccupants(const ilist<cISC4Occupant*>& occupants) override; // 0x10016e8d8
    void QueueRemovals(bool queue) override;                                       // 0x10016eaee
    bool RemoveOrReplaceOccupant(INetworkManagerTool* tool, cISC4Occupant* occupant,
                                 bool& replaced) override;                          // 0x100171064 (PARTIAL)
    bool IsOccupantSubwayStation(cISC4Occupant* occupant) override;                // 0x10016f2a8
    void FindPipeOccupant(int x, int z, cSC4PipeOccupant*& out) override;          // 0x10016f2b6
    void FindPipeConnectionOccupant(int x, int z, cSC4PipeConnectionOccupant*& out) override;  // 0x10016f2c8
    void GetAllIntersectionPieces(INetworkManagerOccupant* occupant, ilist<cISC4Occupant*>& out,
                                  INetworkManagerOccupantManager* manager,
                                  INetworkManagerOccupant* root) override;         // 0x1001711c2 (PARTIAL)
    std::uint32_t MapPreviewTexture(std::uint32_t texture, std::uint8_t& alpha) override;   // 0x10016ec20
    std::uint32_t MapShadowTexture(std::uint32_t texture, std::uint8_t& alpha, std::uint32_t network,
                                   std::uint32_t piece) override;                  // 0x10016ece4
    std::uint32_t MapPath(std::uint32_t path) override;                            // 0x10016ee44
    void RegisterConstructionCrew(cSC4NetworkConstructionCrew* crew) override;     // 0x10016f1dc
    void UnregisterConstructionCrew(cSC4NetworkConstructionCrew* crew) override;   // 0x10016f226
    cISC4OccupantFilter* GetWaterOccupantFilter() override;                        // 0x10016f2da
    cISC4OccupantFilter* GetSubwayOccupantFilter() override;                       // 0x10016f332
    bool CreateConnectionNode(cISC4Occupant*& out, eNetworkType type, const SC4PointT<int>& cell,
                              std::uint32_t a, std::uint32_t b, std::uint32_t c,
                              std::uint8_t d) override;                            // 0x100171494 (PARTIAL)
    void AutoconnectLotNodes(cISC4Lot* lot, const ilist<cISC4Occupant*>& occupants) override;  // 0x100171780 (PARTIAL)
    const char* GetEnglishNetworkName(eNetworkType type) override;                 // 0x100171c3c
    void AddToRetrofitList(cISC4Occupant* occupant) override;                      // 0x100171c50

    // ---- non-virtual entry points of the class ----
    void Reload();                                                          // 0x10016e1ec
    void Retrofit();                                                        // 0x100170230 (PARTIAL)
    void DoViewRotationChange(int oldRotation, int newRotation);            // 0x1001700d8
    void AddTextureMapping(std::uint32_t matchValue, std::uint32_t matchMask, std::uint32_t addValue,
                           std::uint32_t transformMask, std::uint8_t alphaDelta, bool stop,
                           std::uint32_t subtractMask);                     // 0x10016ec7e
    void AddShadowMapping(std::uint32_t matchValue, std::uint32_t matchMask, std::uint32_t addValue,
                          std::uint32_t transformMask, std::uint8_t alphaDelta, bool stop,
                          std::uint32_t subtractMask);                      // 0x10016edc8
    void GetAllIntersectionPieces2(INetworkManagerOccupant* occupant, ilist<cISC4Occupant*>& out,
                                   INetworkManagerOccupantManager* manager, INetworkManagerOccupant* root,
                                   cISC4OccupantFilter* filter);             // 0x1001712aa (PARTIAL)

private:
    // The four Reload section callbacks (anonymous namespace in the original, 0x10016ee80..0x10016f138).
    static void AddTextureRemapEntryCallback(const cRZString& key, const cRZString& value, void* context);
    static void AddShadowRemapEntryCallback(const cRZString& key, const cRZString& value, void* context);
    static void AddShadowRemapByEdgesEntryCallback(const cRZString& key, const cRZString& value, void* context);
    static void AddPathRemapEntryCallback(const cRZString& key, const cRZString& value, void* context);

    // Init's "store the freshly created tool, release the previous one" block (0x100170bxx pattern).
    void SetNetworkTool(eNetworkType type, INetworkManagerTool* tool);

    // scripts/tests/sc4_networkmanager_test.cpp checks the original field offsets and the private state the
    // public interface cannot reach (the original declares the members private too).
    friend struct NetworkManagerTestAccess;

    std::uint32_t refCount_ = 0;                       // +0x18
    bool inited_ = false;                              // +0x1c
    bool ukPathMapping_ = false;                       // +0x1d
    bool extendRegion_ = false;                        // +0x1e
    std::int32_t regionMinX_ = 0x7fffffff;             // +0x20
    std::int32_t regionMinZ_ = 0x7fffffff;             // +0x24
    std::int32_t regionMaxX_ = -1;                     // +0x28
    std::int32_t regionMaxZ_ = -1;                     // +0x2c
    std::vector<INetworkManagerOccupant*> trackedOccupants_;  // +0x30
    std::unordered_multimap<std::uint32_t, ShadowRemapByEdgesEntry> shadowRemapByEdges_;  // +0x48
    std::vector<TextureRemapEntry> textureRemaps_;           // +0x70
    std::vector<TextureRemapEntry> shadowTextureRemaps_;     // +0x88
    std::vector<PathRemapEntry> pathRemaps_;                  // +0xa0
    std::vector<cISC4Occupant*> removalQueue_;                 // +0xb8
    bool removalQueueActive_ = false;                         // +0xd0
    std::list<cISC4Occupant*> retrofitList_;                  // +0xd8
    std::vector<cSC4NetworkConstructionCrew*> constructionCrews_;  // +0xf0
    INetworkManagerTool* networkTools_[kNetworkToolCount] = {};    // +0x108 (13 pointers, 0x108..0x16c)
};

}  // namespace sc4
