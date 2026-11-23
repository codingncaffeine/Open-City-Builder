// cSC4MyStreetSim - a MySim that lives on the street (class id 0x0bf606a5): a car or pedestrian the player picks with the
// street-sim tool becomes a sim with a biography and balloons that rides along with that automaton. A cSC4MySim whose
// constructor stamps the street bitmap id 0x0bfa331e and the "no track" handle -1 and keeps one extra interface
// (cISC4MyStreetSim, iid 0x0bf61276) with its own string (the landmark name the trip script sees when there is no landmark).
// Mac i386 0x00487c50..0x00489a40 (with cSC4MyStreetSimInputControl); the 1.2.1 x64 addresses are in the comments.
//
// The trip: AttachAutomaton keeps the automaton (a car's cell lifespan is maxed and its flag 0x2000000 set), listens to it,
// fills the trip variables of the sim's script table from the automaton's occupant group (on foot, car, bus, train,
// elevated train, monorail; state 6 on foot, else 4 or 5 at random), samples the surroundings and tracks the occupant
// in the dispatch vehicle view (tracker type 3, the sim's bitmap). DetachAutomaton undoes it. The day / month
// simulation runs only while an automaton is attached. The surroundings are sampled again when the sim posts its own
// "changed" message, and every 2 seconds while the game is paused.
#pragma once
#include <vector>

#include "rz/cRZAutoRefCount.h"
#include "sc4/MySim.h"
#include "sc4/MySimDispatch.h"
#include "sc4/ViewInputControl.h"

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4MyStreetSim = 0x0bf606a5;
constexpr std::uint32_t kGZIID_cISC4MyStreetSim = 0x0bf61276;
constexpr std::uint32_t kMyStreetSimBitmapId = 0x0bfa331e;
constexpr std::uint32_t kMessageMyStreetSimRemoved = 0x09d92fc3;   // data2 2, 3 or 5: the automaton is going away
constexpr int kMyStreetSimTrackerType = 3;
constexpr std::uint32_t kMyStreetSimToolId = 0xcc23d564;        // the street-sim tool's control id
constexpr std::uint32_t kMyStreetSimCursor = 0xebf9c962;        // nothing to pick under the pointer
constexpr std::uint32_t kMyStreetSimCursorPick = 0xebf9c971;    // a car or person to pick
constexpr std::uint32_t kPropertyNoStreetSim = 0x1bc42c49;      // an automaton exemplar that carries it cannot be picked
constexpr std::uint32_t kMyStreetSimAdvisorId = 0x1e;           // the street sim's slot in the advisor system

class cISC4View3DWin;
class cSC4MyStreetSim;

// What the street-sim tool reaches outside itself.
class IMyStreetSimToolHost {
public:
    virtual ~IMyStreetSimToolHost() = default;
    virtual cISC4View3DWin* View() = 0;   // SL::spView3D (the control's view; picks, the current control, the tool stack)
    // spOccupantManager vslot 0x90 GetOccupantsInBox(list, min, max, 0, -1), then vslot 0xc0 drops the list's references:
    // the occupants are borrowed for the call.
    virtual void OccupantsInBox(const float min[3], const float max[3], std::vector<cISC4Occupant*>& out) = 0;
    // QueryInterface(0x8b99ee7c) of the occupant (cars and people are traffic automata) as the street sim's automaton
    // view; one reference, null for anything else.
    virtual IMySimDispatchAutomaton* StreetAutomaton(cISC4Occupant* occupant) = 0;
};
IMyStreetSimToolHost* MyStreetSimToolHost();
void SetMyStreetSimToolHost(IMyStreetSimToolHost* host);

// cSC4MyStreetSimInputControl (i386 0x004894cc..0x00489a40, 0x30 bytes): hovering marks a car, bus, train or person
// under the pointer (or a person within 3 x 7 x 3 m of the picked ground), a click makes it the street sim's automaton,
// Escape ends the tool.
class cSC4MyStreetSimInputControl final : public cSC4ViewInputControl {
public:
    cSC4MyStreetSimInputControl() { id_ = kMyStreetSimToolId; }   // i386 0x00489a0a
    ~cSC4MyStreetSimInputControl() override = default;
    bool Init() override;                                         // i386 0x0048953a
    bool Init(cSC4MyStreetSim* sim);                              // i386 0x00489566
    bool Shutdown() override;                                     // i386 0x004895ae
    bool ShouldStack() override { return false; }                 // i386 0x004895de
    bool OnKeyDown(std::uint32_t key, std::uint32_t modifiers) override;   // i386 0x004895e6
    bool OnMouseExit() override { return true; }                  // i386 0x00489608
    bool OnMouseDownL(int x, int y, std::uint32_t modifiers) override;     // i386 0x00489612
    bool OnMouseMove(int x, int y, std::uint32_t modifiers) override;      // i386 0x0048968a
    IMySimDispatchAutomaton* Hovered() const { return hovered_.get(); }    // harness view

private:
    cRZAutoRefCount<cSC4MyStreetSim> sim_;                 // +0x28 (the street view)
    cRZAutoRefCount<IMySimDispatchAutomaton> hovered_;     // +0x2c
};

class cSC4MyStreetSim : public cSC4MySim {
public:
    cSC4MyStreetSim();                                              // i386 0x00489340 (0x1006b9ae4)
    ~cSC4MyStreetSim() override = default;                          // i386 0x0048945c
    bool QueryInterface(std::uint32_t iid, void** out) override;    // i386 0x00487cd0
    bool Init(int advisorType) override;                            // i386 0x00487d38
    bool Shutdown() override;                                       // i386 0x00487d76
    bool Write(cIGZOStream& stream) const override;                 // i386 0x00487d8c
    bool Read(cIGZIStream& stream) override;                        // i386 0x00487db8
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4MyStreetSim; }
    bool DoMessage(cIGZMessage2* message) override;                 // i386 0x00488c34
    void simulate_day() override;                                   // i386 0x00487f3e
    void simulate_day_events() override;                            // i386 0x00487f74
    void simulate_month() override;                                 // i386 0x00487fa6
    void ShowLocation() override;                                   // i386 0x00487fbe
    void HideLocation() override;                                   // i386 0x00488d36

    // cISC4MyStreetSim (i386 vslots 0x1a8 / 0x1ac of the primary table; 0xc through the street view).
    virtual bool AttachAutomaton(IMySimDispatchAutomaton* automaton);   // i386 0x00488e1a
    virtual bool DetachAutomaton();                                     // i386 0x0048801e
    int get_track_handle() const { return trackHandle_; }               // i386 0x007f3944
    void update_trip_variables();                                       // i386 0x00488112
    bool CreateMyStreetSimTool();                                       // i386 0x00487ea2 (street view vslot 0x28)

    // Harness views.
    IMySimDispatchAutomaton* Automaton() const { return automaton_.get(); }
    bool Attached() const { return attached_; }
    int TravelType() const { return travelType_; }

private:
    cRZAutoRefCount<IMySimDispatchAutomaton> automaton_;   // i386 +0x104
    bool attached_ = false;                                // i386 +0x208
    int travelType_ = 0;                                   // i386 +0x2bc (9 = none)
    cRZAutoRefCount<cRZString> extra_;                     // i386 +0x2c0 (x64 +0x440)
};

}  // namespace sc4
