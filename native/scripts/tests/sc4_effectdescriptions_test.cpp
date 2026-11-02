#include <cstdio>

#include "rz/PortMemoryStreams.h"
#include "sc4/EffectDescriptions.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

int main() {
    {   // shake
        cSC4ShakeDescription a; a.amplitude = 1.5f; a.frequency = 2; a.curveA = {1, 2}; a.curveB = {3}; a.falloff = 4; a.flags = 7;
        PortMemoryOStream os; os << a;
        PortMemoryIStream is(os.bytes().data(), os.bytes().size());
        cSC4ShakeDescription b; is >> b;
        CHECK(b.amplitude == 1.5f && b.frequency == 2 && b.curveA.size() == 2 && b.curveB[0] == 3 && b.falloff == 4 && b.flags == 7 && !is.GetError());
    }
    {   // decal: kind 0 becomes 2 with flag 0x40
        cSC4DecalDescription a; a.flags = 3; a.textureId = 9; a.layer = 1; a.lifetime = 0; a.life = 0.5f; a.rotationCurve = {1}; a.colorCurve = {{1, 2, 3}, {4, 5, 6}};
        a.rotationVariance = 8; a.offset = {7, 9};
        PortMemoryOStream os; os << a;
        PortMemoryIStream is(os.bytes().data(), os.bytes().size());
        cSC4DecalDescription b; is >> b;
        CHECK(b.lifetime == 2 && (b.flags & 0x40) && (b.flags & 3) == 3 && b.textureId == 9 && b.colorCurve.size() == 2 && b.colorCurve[1].z == 6 && b.rotationVariance == 8 && b.offset.y == 9);
        a.lifetime = 4;
        PortMemoryOStream os2; os2 << a;
        PortMemoryIStream is2(os2.bytes().data(), os2.bytes().size());
        cSC4DecalDescription c; is2 >> c;
        CHECK(c.lifetime == 4 && c.flags == 3);
    }
    {   // camera + light + message trigger
        cSC4CameraParams a; a.keys = {1, 2, 3}; a.values[3] = 5;
        cSC4LightDescription l; l.colors = {{1, 0, 0}}; l.intensities = {9}; l.range = 2;
        cSC4MessageTriggerDescription m; m.message = 0xabcdef; m.name = "hello";
        PortMemoryOStream os; os << a << l << m;
        PortMemoryIStream is(os.bytes().data(), os.bytes().size());
        cSC4CameraParams b; cSC4LightDescription l2; cSC4MessageTriggerDescription m2;
        is >> b >> l2 >> m2;
        CHECK(b.keys.size() == 3 && b.values[3] == 5 && l2.colors.size() == 1 && l2.colors[0].x == 1 && l2.range == 2 && m2.message == 0xabcdef && m2.name == "hello");
    }
    {   // effect with records and events
        cSC4EffectDescription e; e.id = 5; e.version = 3; e.label = "boom"; e.values[1] = 11;
        cSC4EffectDescription::cDescriptionRec r; r.name = "part"; r.kind = 2; r.type = 77; r.transform.translation[0] = 4; r.transform.scale = 2; r.transform.changeCounter = 9;
        r.flagA = 1; r.shortB = 300; r.floats[1] = 0.25f; r.intValue = -5;
        e.records.push_back(r);
        cSC4EffectDescription::cEventRec ev; ev.type = 8; ev.name = "go"; ev.delay = 1.5f; ev.flags = 6;
        e.events.push_back(ev); e.events.push_back(ev);
        PortMemoryOStream os; os << e;
        PortMemoryIStream is(os.bytes().data(), os.bytes().size());
        cSC4EffectDescription f; is >> f;
        CHECK(f.id == 5 && f.version == 3 && f.label == "boom" && f.values[1] == 11 && f.records.size() == 1 && f.events.size() == 2);
        CHECK(f.records[0].name == "part" && f.records[0].kind == 2 && f.records[0].type == 77 && f.records[0].transform.translation[0] == 4);
        CHECK(f.records[0].transform.scale == 2 && f.records[0].transform.changeCounter == 9 && f.records[0].shortB == 300 && f.records[0].floats[1] == 0.25f && f.records[0].intValue == -5);
        CHECK(f.events[1].name == "go" && f.events[1].delay == 1.5f && f.events[1].flags == 6 && !is.GetError());
    }
    {   // tractor point, timed effect, item, dynamic particle
        cSC4ParticlesDescriptionBase::cTractorPoint t; t.position = {1, 2, 3}; t.direction = {4, 5, 6}; t.strength = 7; t.radius = 8;
        cSC4DynamicParticleDescription d; d.type = 4; d.name = "spark"; d.count = 3; d.curve = {1, 2}; d.values[5] = 6;
        PortMemoryOStream os; os << t << d;
        os.SetRZCharStr("late");   // a timed effect: name then time
        os.SetFloat32(2.5f);
        PortMemoryIStream is(os.bytes().data(), os.bytes().size());
        cSC4ParticlesDescriptionBase::cTractorPoint t2; cSC4DynamicParticleDescription d2; cSC4ParticlesDescriptionBase::cTimedEffect te;
        is >> t2 >> d2 >> te;
        CHECK(t2.direction.z == 6 && t2.radius == 8 && d2.type == 4 && d2.name == "spark" && d2.count == 3 && d2.curve.size() == 2 && d2.values[5] == 6);
        CHECK(te.name == "late" && te.time == 2.5f);
        PortMemoryOStream os2; os2 << cS3DVector2{1, 2}; os2.SetRZCharStr("item");
        PortMemoryIStream is2(os2.bytes().data(), os2.bytes().size());
        sc4::cSC4SequenceDescription::cItemRec item; is2 >> item;
        CHECK(item.range.y == 2 && item.name == "item");
    }
    std::printf(failures ? "FAILED\n" : "sc4_effectdescriptions ok\n");
    return failures != 0;
}
