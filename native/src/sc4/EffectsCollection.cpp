#include "sc4/EffectsCollection.h"

#include <cctype>
#include <utility>

namespace sc4 {

namespace {

// The original lowercases the scratch string in place with __tolower (0x10092af64), passing the sign-extended char.
// The port goes through unsigned char: identical in the C locale for the ASCII names effects resources use.
void LowercaseInPlace(std::string& text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

}  // namespace

// ---- construction ----------------------------------------------------------------------------------------------
// @ 0x100576350. The compiler's inlined default construction of the six name maps (load factor 1.0) and the six
// description vectors is all the constructor's other stores are; the only real work is the 13-slot component table.
// The camera-params flag at +0x1a0 is not written by the original at all (see the header).
cSC4EffectsCollection::cSC4EffectsCollection() : components_(13) {}

// @ 0x1005765bc. Destroys the two record vectors, the six description vectors, the component table (releasing every
// component reference) and the scratch string, exactly like the compiler-generated reverse-order teardown.
cSC4EffectsCollection::~cSC4EffectsCollection() = default;

const std::string& cSC4EffectsCollection::LowercasedName(const char* name) {
    scratch_.assign(name);
    LowercaseInPlace(scratch_);
    return scratch_;
}

int cSC4EffectsCollection::ComponentIndex(std::uint32_t type) {
    if (type < 3 || type > 15) return -1;   // the original computes (type - 3) * 0x40 unchecked
    return static_cast<int>(type) - 3;
}

// ---- collection lifetime ---------------------------------------------------------------------------------------
// @ 0x1005767d8. Clears the message triggers (+0x1e8) and the effect bindings (+0x1d0) and invalidates the camera
// parameters (+0x1a0); the description tables survive, which is the original's behaviour.
void cSC4EffectsCollection::BeginCollection() {
    messageTriggers_.clear();
    effectBindings_.clear();
    cameraParamsSet_ = false;
}

// @ 0x100576862. An empty virtual: the original returns immediately.
void cSC4EffectsCollection::EndCollection() {}

// ---- particles -------------------------------------------------------------------------------------------------
// @ 0x100576868. A new name allocates a cSC4ParticlesDescription (0x2c0 bytes in the original, refcount at +0x2bc),
// registers its index and then copies the incoming record into it; an existing name just overwrites the record.
void cSC4EffectsCollection::AddParticleDescription(const char* name, const cSC4ParticlesDescription& description) {
    const std::string& key = LowercasedName(name);
    const auto found = particleNames_.find(key);
    if (found == particleNames_.end()) {
        const auto index = static_cast<int>(particles_.size());
        particleNames_.emplace(key, index);
        particles_.emplace_back();
        particles_[static_cast<std::size_t>(index)] = description;   // operator= @ 0x1005b5432
        return;
    }
    particles_[static_cast<std::size_t>(found->second)] = description;
}

// @ 0x100576a42. Copies the stored record out; a null output pointer only tests the name.
bool cSC4EffectsCollection::GetParticleDescription(const char* name, cSC4ParticlesDescription* description) {
    const std::string& key = LowercasedName(name);
    const auto found = particleNames_.find(key);
    if (found == particleNames_.end()) return false;
    if (description != nullptr) *description = particles_[static_cast<std::size_t>(found->second)];
    return true;
}

// ---- dynamic particles -----------------------------------------------------------------------------------------
// @ 0x100576af2. The original allocates a cSC4DynamicParticleDescription (0x68 bytes, cRZRefCount base) and keeps a
// reference; the record itself is copied in with operator= @ 0x1006fd016.
void cSC4EffectsCollection::AddDynamicParticleDescription(const char* name,
                                                           const cSC4DynamicParticleDescription& description) {
    const std::string& key = LowercasedName(name);
    const auto found = dynamicParticleNames_.find(key);
    if (found == dynamicParticleNames_.end()) {
        const auto index = static_cast<int>(dynamicParticles_.size());
        dynamicParticleNames_.emplace(key, index);
        dynamicParticles_.emplace_back();
        dynamicParticles_[static_cast<std::size_t>(index)] = description;
        return;
    }
    dynamicParticles_[static_cast<std::size_t>(found->second)] = description;
}

// @ 0x100576c54
bool cSC4EffectsCollection::GetDynamicParticleDescription(const char* name,
                                                           cSC4DynamicParticleDescription* description) {
    const std::string& key = LowercasedName(name);
    const auto found = dynamicParticleNames_.find(key);
    if (found == dynamicParticleNames_.end()) return false;
    if (description != nullptr) *description = dynamicParticles_[static_cast<std::size_t>(found->second)];
    return true;
}

// ---- decals ----------------------------------------------------------------------------------------------------
// @ 0x100576d0a. The table is a vector of plain records (0xa8 each): a new name resizes it by one and registers the
// old element count as the index, then the five curves and the scalar tail are copied.
void cSC4EffectsCollection::AddDecalDescription(const char* name, const cSC4DecalDescription& description) {
    const std::string& key = LowercasedName(name);
    const auto found = decalNames_.find(key);
    int index;
    if (found == decalNames_.end()) {
        index = static_cast<int>(decals_.size());
        decals_.resize(decals_.size() + 1);
        decalNames_.emplace(key, index);
    } else {
        index = found->second;
    }
    cSC4DecalDescription& target = decals_[static_cast<std::size_t>(index)];
    target.flags = description.flags;
    target.textureId = description.textureId;
    target.layer = description.layer;
    target.lifetime = description.lifetime;
    target.life = description.life;
    if (&target != &description) {
        target.rotationCurve = description.rotationCurve;
        target.sizeCurve = description.sizeCurve;
        target.opacityCurve = description.opacityCurve;
        target.colorCurve = description.colorCurve;
        target.aspectCurve = description.aspectCurve;
    }
    target.opacityVariance = description.opacityVariance;
    target.sizeVariance = description.sizeVariance;
    target.rotationVariance = description.rotationVariance;
    target.mapping = description.mapping;
    target.offset = description.offset;
}

// @ 0x100576f8e
bool cSC4EffectsCollection::GetDecalDescription(const char* name, cSC4DecalDescription* description) {
    const std::string& key = LowercasedName(name);
    const auto found = decalNames_.find(key);
    if (found == decalNames_.end()) return false;
    if (description != nullptr) *description = decals_[static_cast<std::size_t>(found->second)];
    return true;
}

// ---- shakes ----------------------------------------------------------------------------------------------------
// @ 0x100577106. 0x40 bytes per record: two floats, two curves and a trailing float / flag pair.
void cSC4EffectsCollection::AddShakeDescription(const char* name, const cSC4ShakeDescription& description) {
    const std::string& key = LowercasedName(name);
    const auto found = shakeNames_.find(key);
    int index;
    if (found == shakeNames_.end()) {
        index = static_cast<int>(shakes_.size());
        shakes_.resize(shakes_.size() + 1);
        shakeNames_.emplace(key, index);
    } else {
        index = found->second;
    }
    cSC4ShakeDescription& target = shakes_[static_cast<std::size_t>(index)];
    target.amplitude = description.amplitude;
    target.frequency = description.frequency;
    if (&target != &description) {
        target.curveA = description.curveA;
        target.curveB = description.curveB;
    }
    target.falloff = description.falloff;
    target.flags = description.flags;
}

// @ 0x1005772a6
bool cSC4EffectsCollection::GetShakeDescription(const char* name, cSC4ShakeDescription* description) {
    const std::string& key = LowercasedName(name);
    const auto found = shakeNames_.find(key);
    if (found == shakeNames_.end()) return false;
    if (description != nullptr) *description = shakes_[static_cast<std::size_t>(found->second)];
    return true;
}

// ---- lights ----------------------------------------------------------------------------------------------------
// @ 0x100577392. 0x38 bytes per record: the colour vector, the intensity vector and the range float.
void cSC4EffectsCollection::AddLightDescription(const char* name, const cSC4LightDescription& description) {
    const std::string& key = LowercasedName(name);
    const auto found = lightNames_.find(key);
    int index;
    if (found == lightNames_.end()) {
        index = static_cast<int>(lights_.size());
        lights_.resize(lights_.size() + 1);
        lightNames_.emplace(key, index);
    } else {
        index = found->second;
    }
    cSC4LightDescription& target = lights_[static_cast<std::size_t>(index)];
    if (&target != &description) {
        target.colors = description.colors;
        target.intensities = description.intensities;
    }
    target.range = description.range;
}

// @ 0x1005774b2
bool cSC4EffectsCollection::GetLightDescription(const char* name, cSC4LightDescription* description) {
    const std::string& key = LowercasedName(name);
    const auto found = lightNames_.find(key);
    if (found == lightNames_.end()) return false;
    if (description != nullptr) *description = lights_[static_cast<std::size_t>(found->second)];
    return true;
}

// ---- effect descriptions ---------------------------------------------------------------------------------------
// @ 0x100577594. The effects are the one table keyed by (name, version): the multimap holds one entry per version and
// the lookup walks the name's range comparing the description's version at +0x08. After the copy the negative record
// and event indices are resolved against the other tables (0x1005777xx..0x1005779b0).
void cSC4EffectsCollection::AddEffectDescription(const char* name, const cSC4EffectDescription& description) {
    const std::string& key = LowercasedName(name);
    int index = -1;
    const auto range = effectNames_.equal_range(key);
    for (auto it = range.first; it != range.second; ++it) {
        index = it->second;
        if (effects_[static_cast<std::size_t>(index)].version == description.version) break;
        index = -1;
    }
    if (index < 0) {
        index = static_cast<int>(effects_.size());
        effects_.resize(effects_.size() + 1);
        effectNames_.emplace(std::string(key), index);
    }
    cSC4EffectDescription& target = effects_[static_cast<std::size_t>(index)];
    target.id = description.id;
    target.version = description.version;
    if (&target != &description) {
        target.records = description.records;
        target.events = description.events;
    }
    for (int i = 0; i < 3; ++i) target.values[i] = description.values[i];
    target.label = description.label;

    // The label and every record / event name are lowercased in the stored copy.
    LowercaseInPlace(target.label);
    for (auto& record : target.records) {
        if (record.intValue >= 0) continue;   // already resolved: the original leaves it alone
        LowercaseInPlace(record.name);
        if (record.kind == 2) {               // kind 2 (a nested effect) has no index
            record.intValue = 0;
            continue;
        }
        const NameMap* names = nullptr;
        switch (record.kind) {
            case 0: names = &particleNames_; break;
            case 1: names = &decalNames_; break;
            case 16: names = &dynamicParticleNames_; break;
            default:
                if (record.kind >= 3 && record.kind <= 15)
                    names = &components_[static_cast<std::size_t>(record.kind - 3)].names;
                break;
        }
        if (names != nullptr) {
            const auto found = names->find(record.name);
            if (found != names->end()) record.intValue = found->second;
        }
    }
    // Event types with bit 0 name a shake, types with (type & 0xc) a light.
    for (auto& event : target.events) {
        LowercaseInPlace(event.name);
        const NameMap* names = nullptr;
        if ((event.type & 1u) != 0) names = &shakeNames_;
        else if ((event.type & 0xcu) != 0) names = &lightNames_;
        if (names != nullptr) {
            const auto found = names->find(event.name);
            if (found != names->end()) event.flags = static_cast<std::uint32_t>(found->second);
        }
    }
}

// @ 0x1005779fe. A plain find on the multimap: the first entry carrying the name, whatever version it has.
bool cSC4EffectsCollection::GetEffectDescription(const char* name, cSC4EffectDescription* description) {
    const std::string& key = LowercasedName(name);
    const auto found = effectNames_.find(key);
    if (found == effectNames_.end()) return false;
    if (description != nullptr) *description = effects_[static_cast<std::size_t>(found->second)];
    return true;
}

// ---- component descriptions ------------------------------------------------------------------------------------
// @ 0x100577b04. Components live in one of thirteen buckets (types 3..15). A null name appends anonymously; a new name
// registers its index; an existing name replaces the stored reference (AddRef new, Release old) at the same index.
std::uint32_t cSC4EffectsCollection::AddComponentDescription(const char* name,
                                                             cISC4ComponentDescription* description) {
    if (description == nullptr) return 0;   // the original dereferences it (vslot 0x00 = Type)
    const int slot = ComponentIndex(description->Type());
    if (slot < 0) return 0;                 // the original indexes the table unchecked
    cCompDescsRec& record = components_[static_cast<std::size_t>(slot)];
    if (name == nullptr) {
        const auto index = static_cast<std::uint32_t>(record.descriptions.size());
        record.descriptions.push_back(cRZAutoRefCount<cISC4ComponentDescription>(description));
        return index;
    }
    const std::string& key = LowercasedName(name);
    const auto found = record.names.find(key);
    if (found == record.names.end()) {
        const auto index = static_cast<std::uint32_t>(record.descriptions.size());
        record.descriptions.push_back(cRZAutoRefCount<cISC4ComponentDescription>(description));
        record.names.emplace(key, static_cast<int>(index));
        return index;
    }
    const auto index = static_cast<std::uint32_t>(found->second);
    record.descriptions[static_cast<std::size_t>(index)] = description;
    return index;
}

// @ 0x100577d24. Hands out the stored component with a reference added (the caller releases it).
bool cSC4EffectsCollection::GetComponentDescription(const char* name, std::uint32_t type,
                                                     cISC4ComponentDescription** description) {
    const int slot = ComponentIndex(type);
    if (slot < 0) return false;             // the original indexes the table unchecked
    const std::string& key = LowercasedName(name);
    const cCompDescsRec& record = components_[static_cast<std::size_t>(slot)];
    const auto found = record.names.find(key);
    if (found == record.names.end()) return false;
    if (description != nullptr) {
        cISC4ComponentDescription* component = record.descriptions[static_cast<std::size_t>(found->second)];
        *description = component;
        if (component != nullptr) component->AddRef();
    }
    return true;
}

// ---- camera parameters, bindings, triggers ---------------------------------------------------------------------
// @ 0x100577df6
void cSC4EffectsCollection::SetCameraParams(const cSC4CameraParams& params) {
    if (&cameraParams_ != &params) cameraParams_.keys = params.keys;
    for (int i = 0; i < 4; ++i) cameraParams_.values[i] = params.values[i];
    cameraParamsSet_ = true;
}

// @ 0x100577e3e. No lowercasing here: the binding name is stored as given.
void cSC4EffectsCollection::AddEffectBinding(const char* name, std::uint32_t id, std::uint32_t value) {
    effectBindings_.emplace_back(std::string(name), cEffectKey{id, value});
}

// @ 0x100577f9e
void cSC4EffectsCollection::AddMessageTrigger(const cSC4MessageTriggerDescription& trigger) {
    messageTriggers_.push_back(trigger);
}

// @ 0x100577fec. Writes the version of the first entry of the name range and, given an alias, registers alias -> the
// same description for that version. An alias that already names a different entry makes the call fail — after the
// version has been written.
bool cSC4EffectsCollection::UpdateEffectPriority(const char* name, std::uint32_t version, const char* aliasName) {
    const std::string& key = LowercasedName(name);
    const auto range = effectNames_.equal_range(key);
    if (range.first == range.second) return false;

    // The original counts the entries whose index differs from the first one (always >= 1 here).
    const int firstIndex = range.first->second;
    int distinct = 0;
    for (auto it = range.first; it != range.second; ++it)
        if (it == range.first || it->second != firstIndex) ++distinct;
    if (distinct == 0) return false;

    effects_[static_cast<std::size_t>(firstIndex)].version = version;
    // The alias is looked up for that version; without one the original looks the same name up again (the scratch is
    // simply lowercased a second time). A different entry that already carries the version makes the call fail — after
    // the version has been written.
    if (aliasName != nullptr) LowercasedName(aliasName);
    else LowercaseInPlace(scratch_);
    const auto aliasRange = effectNames_.equal_range(scratch_);
    int foundIndex = -1;
    for (auto it = aliasRange.first; it != aliasRange.second; ++it) {
        foundIndex = it->second;
        if (effects_[static_cast<std::size_t>(foundIndex)].version == version) break;
        foundIndex = -1;
    }
    if (foundIndex < 0) {
        effectNames_.emplace(std::string(scratch_), firstIndex);
    } else if (foundIndex != firstIndex) {
        return false;
    }
    return true;
}

// ---- merging collections ---------------------------------------------------------------------------------------
// @ 0x10057825c. Merges *this* into the argument. A destination without effect descriptions takes everything by swap
// (the camera parameters and the flag are the two members that are not swapped); otherwise the merge either keeps the
// names (the +0x200 mode) or renumbers the merged indices.
void cSC4EffectsCollection::AddToCollection(cSC4EffectsCollection* target) {
    if (target->effects_.empty()) {
        effectNames_.swap(target->effectNames_);
        effects_.swap(target->effects_);
        particleNames_.swap(target->particleNames_);
        particles_.swap(target->particles_);
        dynamicParticleNames_.swap(target->dynamicParticleNames_);
        dynamicParticles_.swap(target->dynamicParticles_);
        decalNames_.swap(target->decalNames_);
        decals_.swap(target->decals_);
        shakeNames_.swap(target->shakeNames_);
        shakes_.swap(target->shakes_);
        lightNames_.swap(target->lightNames_);
        lights_.swap(target->lights_);
        components_.swap(target->components_);
        effectBindings_.swap(target->effectBindings_);
        messageTriggers_.swap(target->messageTriggers_);
        // The flag is not swapped: it still describes this collection, and the copy does not make the parameters valid
        // in the destination.
        if (cameraParamsSet_ && target != this) target->cameraParams_ = cameraParams_;
        return;
    }
    if (addWithNames_) {
        AddToCollectionWithNames(target);
        return;
    }
    AddToCollectionWithRenumbering(target);
}

// @ 0x100578608. Forwards every named description of this collection through the interface's Add* slots, in the
// original's order: particles, dynamic particles, decals, shakes, lights, all thirteen component buckets, then effects.
void cSC4EffectsCollection::AddToCollectionWithNames(IEffectsCollectionHost* target) {
    for (const auto& entry : particleNames_)
        target->AddParticleDescription(entry.first.c_str(), particles_[static_cast<std::size_t>(entry.second)]);
    for (const auto& entry : dynamicParticleNames_)
        target->AddDynamicParticleDescription(entry.first.c_str(),
                                              dynamicParticles_[static_cast<std::size_t>(entry.second)]);
    for (const auto& entry : decalNames_)
        target->AddDecalDescription(entry.first.c_str(), decals_[static_cast<std::size_t>(entry.second)]);
    for (const auto& entry : shakeNames_)
        target->AddShakeDescription(entry.first.c_str(), shakes_[static_cast<std::size_t>(entry.second)]);
    for (const auto& entry : lightNames_)
        target->AddLightDescription(entry.first.c_str(), lights_[static_cast<std::size_t>(entry.second)]);
    for (const cCompDescsRec& record : components_)
        for (const auto& entry : record.names)
            target->AddComponentDescription(entry.first.c_str(),
                                            record.descriptions[static_cast<std::size_t>(entry.second)]);
    for (const auto& entry : effectNames_)
        target->AddEffectDescription(entry.first.c_str(), effects_[static_cast<std::size_t>(entry.second)]);
}

// @ 0x100578826. Appends the value tables to the argument, shifts the merged effect indices by the argument's table
// sizes and merges the effect name map. Everything else keeps the original's gaps: the other name maps are not
// merged, kind 16 (dynamic particles) is not shifted, and this collection's effect records are renumbered in place.
void cSC4EffectsCollection::AddToCollectionWithRenumbering(cSC4EffectsCollection* target) {
    const auto particleOffset = static_cast<int>(target->particles_.size());
    target->particles_.insert(target->particles_.end(), particles_.begin(), particles_.end());
    target->dynamicParticles_.insert(target->dynamicParticles_.end(), dynamicParticles_.begin(),
                                     dynamicParticles_.end());
    const auto decalOffset = static_cast<int>(target->decals_.size());
    target->decals_.insert(target->decals_.end(), decals_.begin(), decals_.end());
    const auto shakeOffset = static_cast<int>(target->shakes_.size());
    target->shakes_.insert(target->shakes_.end(), shakes_.begin(), shakes_.end());
    const auto lightOffset = static_cast<int>(target->lights_.size());
    target->lights_.insert(target->lights_.end(), lights_.begin(), lights_.end());

    // The effect records' component indices shift by the destination's bucket sizes (captured before the append); the
    // buckets then grow by this collection's counts.
    std::vector<int> componentOffsets(components_.size());
    for (std::size_t i = 0; i < components_.size(); ++i) {
        componentOffsets[i] = static_cast<int>(target->components_[i].descriptions.size());
        target->components_[i].descriptions.insert(target->components_[i].descriptions.end(),
                                                   components_[i].descriptions.begin(),
                                                   components_[i].descriptions.end());
    }

    const auto effectOffset = static_cast<int>(target->effects_.size());
    for (cSC4EffectDescription& effect : effects_) {
        for (auto& record : effect.records) {
            switch (record.kind) {
                case 0: record.intValue += particleOffset; break;
                case 1: record.intValue += decalOffset; break;
                case 2: record.intValue += effectOffset; break;
                default:
                    if (record.kind >= 3 && record.kind <= 15)
                        record.intValue += componentOffsets[static_cast<std::size_t>(record.kind - 3)];
                    break;   // kind 16 and the unknown kinds keep their index (the original's gap)
            }
        }
        for (auto& event : effect.events) {
            if ((event.type & 1u) != 0) event.flags += static_cast<std::uint32_t>(shakeOffset);
            else if ((event.type & 0xcu) != 0) event.flags += static_cast<std::uint32_t>(lightOffset);
        }
    }
    target->effects_.insert(target->effects_.end(), effects_.begin(), effects_.end());

    // Merge the effect name map: a destination entry with the same (name, version) is dropped, then the shifted
    // source entry is inserted (the original walks the multimap buckets by hand).
    for (const auto& entry : effectNames_) {
        const auto sourceIndex = static_cast<std::size_t>(entry.second);
        const std::uint32_t version = effects_[sourceIndex].version;
        const auto range = target->effectNames_.equal_range(entry.first);
        for (auto it = range.first; it != range.second; ++it) {
            if (target->effects_[static_cast<std::size_t>(it->second)].version == version) {
                target->effectNames_.erase(it);
                break;
            }
        }
        target->effectNames_.emplace(entry.first, entry.second + effectOffset);
    }

    target->messageTriggers_.insert(target->messageTriggers_.end(), messageTriggers_.begin(), messageTriggers_.end());
    target->effectBindings_.insert(target->effectBindings_.end(), effectBindings_.begin(), effectBindings_.end());
}

}  // namespace sc4
