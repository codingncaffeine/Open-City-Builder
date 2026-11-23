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
#include "rz/cRZAutoRefCount.h"
#include "sc4/MySim.h"

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4MyStreetSim = 0x0bf606a5;
constexpr std::uint32_t kGZIID_cISC4MyStreetSim = 0x0bf61276;
constexpr std::uint32_t kMyStreetSimBitmapId = 0x0bfa331e;
constexpr std::uint32_t kMessageMyStreetSimRemoved = 0x09d92fc3;   // data2 2, 3 or 5: the automaton is going away
constexpr int kMyStreetSimTrackerType = 3;

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
