// cGZScriptTerp — implementation. See ScriptTerp.h for the layout, the state machine, the interfaces and the
// list of quirks kept. Addresses in the trailing comments are the originals.
//
// The parser is a hand-written character scanner: GetToken hands out one token at a time and ExecuteScript
// feeds it to the eleven-state machine. The vocabulary stack (cGZScriptVocab, an IScriptVocab seam) resolves
// the words of a script to the numeric ids the UI layer uses: the target may push its shared vocabulary for
// the duration of a script (cGZUIScriptService::ScriptBegin does exactly that), and the interpreter's own
// — nearly empty — vocabulary stays at the bottom of the stack.
#include "gz/ScriptTerp.h"

#include <cctype>

#include "gz/cIGZPersistDBSerialRecord.h"
#include "gz/cIGZPersistRecord.h"
#include "gz/cIGZPersistResourceManager.h"
#include "rz/RZConvert.h"
#include "rz/cRZFile.h"
#include "rz/cRZString.h"
#include "rz/cRZVariant.h"
#include "rz/cRZSysServPtr.h"

namespace {

// Parser states (+0x48). 9 is in the original's switch but never assigned; 10 is the error state.
constexpr std::int32_t kStateIdle = 0;
constexpr std::int32_t kStateText = 1;
constexpr std::int32_t kStateOpenTag = 2;
constexpr std::int32_t kStateParameters = 3;
constexpr std::int32_t kStateParmName = 4;
constexpr std::int32_t kStateParmValue = 5;
constexpr std::int32_t kStateSkipTag = 6;
constexpr std::int32_t kStateUnknownParm = 7;
constexpr std::int32_t kStateUnknownValue = 8;
constexpr std::int32_t kStateError = 10;

// The characters that end a bare word: NUL, tab, LF, CR, space, '"', '<', '=', '>' (the bit mask the original
// tests at 0x10054f38b; chars >= 0x40 are never delimiters).
constexpr std::uint64_t kTokenDelimiters = 0x7000000500002601ULL;
bool IsTokenDelimiter(unsigned char c) {
    return c < 0x40 && ((kTokenDelimiters >> c) & 1) != 0;
}

// The persistence service ExecuteScriptResource opens the script's DB record through (cRZSysServPtr).
constexpr std::uint32_t kPersistResourceManagerService = 0x056b906e;
constexpr std::uint32_t kPersistResourceManagerIid = 0x656b8efc;

IScriptTerpHost* gScriptTerpHost = nullptr;

}  // namespace

IScriptTerpHost* ScriptTerpHost() { return gScriptTerpHost; }
void SetScriptTerpHost(IScriptTerpHost* host) { gScriptTerpHost = host; }

// ---------------------------------------------------------------------------------------------------------
// Construction / lifetime
// ---------------------------------------------------------------------------------------------------------
cGZScriptTerp::cGZScriptTerp() {   // @0x10054e6e4
    // The original allocates a fresh cGZScriptVocab (operator new(0x28) + cGZScriptVocab::cGZScriptVocab
    // @ 0x10054ddf4) and pushes it as the bottom of the vocabulary stack. The class is not recovered, so the
    // host supplies it (docs/SEAMS.md: IScriptTerpHost). Without a host the stack stays empty, which only
    // matters for a bare object the original never creates.
    if (IScriptTerpHost* host = gScriptTerpHost) {
        if (IScriptVocab* vocab = host->CreateVocabulary()) vocabularies_.push_back(vocab);
    }
}

cGZScriptTerp::~cGZScriptTerp() {   // @0x10054e818
    if (parmValue_) {
        parmValue_->Release();
        parmValue_ = nullptr;
    }
    SetScriptTarget(nullptr);
    while (PopVocabulary()) {
    }
    // The object always holds its own vocabulary (the constructor pushed it); release that one too.
    if (!vocabularies_.empty()) {
        vocabularies_.back()->Release();
        vocabularies_.clear();
    }
    // Quirks kept: the parameter array is dropped without releasing its variants (the original only frees
    // the block through the array's allocator, 0x10054e87f) and the context in +0x40 is never released at all.
    parameters_.clear();
}

// ---------------------------------------------------------------------------------------------------------
// cIGZUnknown
// ---------------------------------------------------------------------------------------------------------
bool cGZScriptTerp::QueryInterface(std::uint32_t iid, void** out) {   // @0x10054f756
    if (!out || (iid != kGZIID_cIGZUnknown && iid != kGZIID_cIGZScriptTerp)) return false;
    *out = this;
    AddRef();
    return true;
}

std::uint32_t cGZScriptTerp::AddRef() {   // @0x10054f778
    return ++refCount_;
}

std::uint32_t cGZScriptTerp::Release() {   // @0x10054f786
    // The original deletes for a count of 1 *or* 0 (it compares against 2, 0x10054f789).
    if (refCount_ >= 2) return --refCount_;
    delete this;
    return 0;
}

// ---------------------------------------------------------------------------------------------------------
// Target / context
// ---------------------------------------------------------------------------------------------------------
bool cGZScriptTerp::SetScriptTarget(cIGZScriptTarget* target) {   // @0x10054e964
    if (target_) {
        target_->Release();
        target_ = nullptr;
    }
    if (interpreting_) {
        interpreting_->Release();
        interpreting_ = nullptr;
    }
    if (compiling_) {
        compiling_->Release();
        compiling_ = nullptr;
    }
    if (target) {
        target->AddRef();
        target_ = target;
        // The two sub-interfaces are optional: the original never checks whether the QueryInterface
        // succeeded and simply leaves the slot null (0x10054e9e1/0x10054e9f2).
        target->QueryInterface(kGZIID_cIGZScriptInterpretingTarget, reinterpret_cast<void**>(&interpreting_));
        target->QueryInterface(kGZIID_cIGZScriptCompilingTarget, reinterpret_cast<void**>(&compiling_));
    }
    return true;
}

bool cGZScriptTerp::SetContext(cIGZVariant* context) {   // @0x10054ea00
    // Quirk: the old context is released before the new one is referenced, so SetContext(GetContext()) with a
    // single reference frees the variant and then AddRefs freed memory.
    if (context_) {
        context_->Release();
        context_ = nullptr;
    }
    context_ = context;
    if (context) context->AddRef();
    return true;
}

cIGZVariant* cGZScriptTerp::GetContext() {   // @0x10054f7a6
    return context_;   // borrowed: the original does not AddRef here
}

// ---------------------------------------------------------------------------------------------------------
// Vocabulary stack and the token forwarders
// ---------------------------------------------------------------------------------------------------------
bool cGZScriptTerp::PushVocabulary(IScriptVocab* vocab) {   // @0x10054eae8
    if (!vocab) return false;
    vocab->AddRef();
    vocabularies_.push_back(vocab);
    return true;
}

bool cGZScriptTerp::PopVocabulary() {   // @0x10054eb3e
    // Never pops the interpreter's own (bottom) vocabulary.
    if (vocabularies_.size() < 2) return false;
    vocabularies_.back()->Release();
    vocabularies_.pop_back();
    return true;
}

bool cGZScriptTerp::AddToken(const cIGZString& name, cIGZVariant* value) {   // @0x10054ea3e
    // The original tail-jumps into the top vocabulary's AddTranslation(vslot 0x18).
    IScriptVocab* vocab = CurrentVocabulary();
    if (!vocab) return false;
    return vocab->AddTranslation(name, value);
}

bool cGZScriptTerp::AddToken(const cIGZString& name, std::uint32_t value) {   // @0x10054ea54
    // Also a tail call: the temporary cRZVariant (operator new(0x18) + cRZVariant(unsigned int)) is owned by
    // the vocabulary from then on, AddTranslation takes the reference.
    IScriptVocab* vocab = CurrentVocabulary();
    if (!vocab) return false;
    cRZVariant* variant = new cRZVariant(value);
    variant->AddRef();   // held across the call: a name already in the table leaves the variant unowned (the original leaks it)
    const bool added = vocab->AddTranslation(name, variant);
    variant->Release();
    return added;
}

bool cGZScriptTerp::RemoveToken(const cIGZString& name) {   // @0x10054eabe
    IScriptVocab* vocab = CurrentVocabulary();
    if (!vocab) return false;
    return vocab->RemoveTranslation(name);
}

bool cGZScriptTerp::RemoveAllTokens() {   // @0x10054ead4
    IScriptVocab* vocab = CurrentVocabulary();
    if (!vocab) return false;
    vocab->Clear();
    return true;
}

bool cGZScriptTerp::GetLocalTranslation(const cIGZString& name, cIGZVariant** out) {   // @0x10054eb7e
    // Top of the stack first (the target's shared vocabulary shadows the interpreter's own).
    for (std::size_t i = vocabularies_.size(); i > 0; --i) {
        if (vocabularies_[i - 1]->GetTranslation(name, out)) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------
// Tokenizer
// ---------------------------------------------------------------------------------------------------------
bool cGZScriptTerp::GetToken(cIGZString** out, eTType& type, bool atTopLevel) {   // @0x10054f1be
    if (!cursor_) return false;
    cRZString* token = new cRZString();
    *out = token;
    token->AddRef();

    // At the top level everything up to the next '<' is literal text, whitespace and all.
    if (atTopLevel && *cursor_ != '<' && *cursor_ != '\0') {
        const char* start = cursor_;
        const char* scan = start + 1;
        const char* end = start;
        char ch = *start;
        while (ch != '\0' && ch != '<') {
            cursor_ = scan;
            ch = *scan;
            end = scan;
            scan += 1;
        }
        token->AssignFromChars(start, static_cast<std::uint32_t>(end - start));
        type = kTTypeLiteral;
        return true;
    }

    for (;;) {
        const unsigned char c = static_cast<unsigned char>(*cursor_);
        if (c == '\0') {
            token->Release();
            *out = nullptr;   // the original clears it here too (0x10054f2f4)
            return false;
        }
        if (c == '\t' || c == '\n' || c == '\r' || c == ' ') {   // the four whitespace characters
            cursor_ += 1;
            continue;
        }
        if (c == '"') {
            cursor_ += 1;
            const char* content = cursor_;
            if (*content == '\0') {
                token->Release();
                *out = nullptr;   // deviation: the original releases but leaves *out dangling (0x10054f3b8)
                return false;
            }
            const char* scan = content + 1;
            char ch = *content;
            for (;;) {
                if (ch == '\0' || ch == '"') break;
                cursor_ = scan;
                ch = *scan;
                scan += 1;
            }
            const std::uint32_t length = static_cast<std::uint32_t>((scan - 1) - content);
            if (ch == '"') cursor_ = scan;
            token->AssignFromChars(content, length);
            type = kTTypeLiteral;   // a quoted string is reported as a literal, not as a word
            return true;
        }
        if (c == '<') {
            token->AssignFromCString("<");
            type = kTTypeOpen;
            cursor_ += 1;
            return true;
        }
        if (c == '=') {
            token->AssignFromCString("=");
            type = kTTypeEquals;
            cursor_ += 1;
            return true;
        }
        if (c == '>') {
            token->AssignFromCString(">");
            type = kTTypeClose;
            cursor_ += 1;
            return true;
        }
        break;
    }

    // A bare word: everything up to the next delimiter (or the end of the script).
    const char* start = cursor_;
    const char* scan = start;
    for (;;) {
        if (IsTokenDelimiter(static_cast<unsigned char>(*scan))) break;
        cursor_ = scan + 1;
        scan += 1;
    }
    token->AssignFromChars(start, static_cast<std::uint32_t>(scan - start));
    type = kTTypeToken;
    return true;
}

// ---------------------------------------------------------------------------------------------------------
// Word -> id resolution
// ---------------------------------------------------------------------------------------------------------
bool cGZScriptTerp::GetUint32(const cIGZString& text, std::uint32_t& out) {   // @0x10054f6b2
    cIGZVariant* translation = nullptr;
    if (GetLocalTranslation(text, &translation)) {
        if (translation->GetType() != cIGZVariant::kTypeUint32) {   // a string-typed keyword is not an id
            translation->Release();
            return false;
        }
        out = translation->AsUint32();
        translation->Release();
        return true;
    }
    const char* chars = text.Data();
    // The original requires an ASCII digit (the ctype _CTYPE_D bit) and rejects a non-ASCII first byte, so
    // "-5"/"+5" are not numbers; RZConvertToUint then parses decimal or 0x-prefixed text.
    if (!chars || !std::isdigit(static_cast<unsigned char>(chars[0]))) return false;
    out = RZ::ConvertToUint(chars);
    return true;
}

// ---------------------------------------------------------------------------------------------------------
// The state machine's helpers
// ---------------------------------------------------------------------------------------------------------
bool cGZScriptTerp::parseOpenTag(cIGZString* text) {   // @0x10054f3d0
    std::uint32_t id = 0;
    if (!GetUint32(*text, id)) return false;
    tagId_ = id;
    inTag_ = true;
    if (interpreting_) interpreting_->BeginExecuteTag(this, id);
    ClearParams();
    return true;
}

bool cGZScriptTerp::parseFlushLastParm() {   // @0x10054f418
    if (!hasParm_) return true;
    tScriptProperty property;
    property.id = parmId_;
    property.value = parmValue_;
    parameters_.push_back(property);
    if (parmValue_) parmValue_->AddRef();   // the array's reference
    if (interpreting_) interpreting_->ApplyProperty(this, parmId_, parmValue_);
    if (parmValue_) {
        parmValue_->Release();   // drop the pending reference, the array keeps the value alive
        parmValue_ = nullptr;
    }
    hasParm_ = false;
    return true;
}

bool cGZScriptTerp::parseCloseTag() {   // @0x10054f4e6
    if (!inTag_) return true;
    if (interpreting_) interpreting_->EndExecuteTag(this, tagId_);
    if (compiling_) compiling_->ScriptTag(this, tagId_, parameters_);
    inTag_ = false;
    return true;
}

bool cGZScriptTerp::parseParmValue(cIGZString* text) {   // @0x10054f538
    if (!GetLocalTranslation(*text, &parmValue_)) {
        std::uint32_t number = 0;
        const bool numeric = GetUint32(*text, number);
        cRZVariant* variant = numeric ? new cRZVariant(number) : new cRZVariant(*text);
        parmValue_ = variant;
        variant->AddRef();
    }
    return true;
}

bool cGZScriptTerp::parseParmStrValue(cIGZString* text) {   // @0x10054f5c8
    cRZVariant* variant = new cRZVariant();   // an empty variant, then SetValString (vslot 0x1d8)
    parmValue_ = variant;
    variant->AddRef();
    variant->SetValString(*text);
    return true;
}

bool cGZScriptTerp::ClearParams() {   // @0x10054f634
    for (tScriptProperty& property : parameters_) {
        if (property.value) property.value->Release();
    }
    parameters_.clear();
    if (parmValue_) {
        parmValue_->Release();
        parmValue_ = nullptr;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------
// ExecuteScript and the entry points that feed it
// ---------------------------------------------------------------------------------------------------------
bool cGZScriptTerp::ExecuteScript() {   // @0x10054ecf0
    if (state_ != kStateIdle) return false;   // no re-entrant scripts
    cursor_ = scriptText_;
    state_ = kStateText;
    inTag_ = false;
    bool atTopLevel;
    if (cIGZScriptTarget* target = target_) {
        target->ScriptBegin(this);
        atTopLevel = state_ == kStateText;
    } else {
        atTopLevel = true;
    }

    cIGZString* token = nullptr;
    eTType type = kTTypeLiteral;
    bool more = GetToken(&token, type, atTopLevel);
    std::int32_t state = state_;
    while (more) {
        switch (state) {
        case kStateText:
            if (type < kTTypeOpen) {
                // +0x10 is reloaded per call (the target may rebind itself from a callback).
                if (cIGZScriptTarget* target = target_) target->ScriptLiteral(this, token);
            } else if (type == kTTypeOpen) {
                state_ = kStateOpenTag;
            } else {
                state_ = kStateError;
            }
            break;
        case kStateOpenTag:
            if (type == kTTypeToken) {
                state_ = parseOpenTag(token) ? kStateParameters : kStateSkipTag;
            } else if (type == kTTypeClose) {
                state_ = kStateText;   // "<>": an empty tag is ignored
            } else {
                state_ = kStateError;
            }
            break;
        case kStateParameters:
            if (type == kTTypeToken) {
                parseFlushLastParm();
                if (GetUint32(*token, parmId_)) {
                    hasParm_ = true;
                    state_ = kStateParmName;
                } else {
                    state_ = kStateUnknownParm;
                }
            } else if (type == kTTypeClose) {
                parseFlushLastParm();
                parseCloseTag();
                state_ = kStateText;
            } else {
                state_ = kStateError;
            }
            break;
        case kStateParmName:
            if (type == kTTypeToken) {
                parseFlushLastParm();
                if (GetUint32(*token, parmId_)) {
                    hasParm_ = true;
                    state_ = kStateParmName;
                } else {
                    state_ = kStateUnknownParm;
                }
            } else if (type == kTTypeEquals) {
                state_ = kStateParmValue;
            } else if (type == kTTypeClose) {
                parseFlushLastParm();
                parseCloseTag();
                state_ = kStateText;
            } else {
                state_ = kStateError;
            }
            break;
        case kStateParmValue:
            if (type == kTTypeLiteral) {
                parseParmStrValue(token);
            } else if (type == kTTypeToken) {
                parseParmValue(token);
            } else {
                state_ = kStateError;
                break;
            }
            state_ = kStateParameters;
            break;
        case kStateSkipTag:
            if (type == kTTypeClose) state_ = kStateText;
            break;
        case kStateUnknownParm:
            // No flush here: nothing is pending after the unknown name (0x10054ee45).
            if (type == kTTypeToken) {
                if (GetUint32(*token, parmId_)) {
                    hasParm_ = true;
                    state_ = kStateParmName;
                } else {
                    state_ = kStateUnknownParm;
                }
            } else if (type == kTTypeClose) {
                parseCloseTag();
                state_ = kStateText;
            } else if (type == kTTypeEquals) {
                state_ = kStateUnknownValue;
            } else {
                state_ = kStateError;
            }
            break;
        case kStateUnknownValue:
            // The value of an unknown property is swallowed whole: no parseParmValue/parseParmStrValue call.
            state_ = type < kTTypeOpen ? kStateParameters : kStateError;
            break;
        case kStateError:
        default:
            goto finished;
        }
        token->Release();
        token = nullptr;
        more = GetToken(&token, type, state_ == kStateText);
        state = state_;
    }

finished:
    if (token) token->Release();   // an error leaves the loop holding the current token (the original leaks it)
    cursor_ = nullptr;
    ClearParams();
    const bool success = state != kStateError;
    if (cIGZScriptTarget* target = target_) target->ScriptEnd(this, success);
    const std::int32_t finalState = state_;
    state_ = kStateIdle;
    return finalState != kStateError;
}

bool cGZScriptTerp::ExecuteScriptString(const cIGZString& script) {   // @0x10054efb8
    scriptText_ = script.Data();   // no copy: the caller's string must outlive the call
    return ExecuteScript();
}

bool cGZScriptTerp::ExecuteScriptFile(const cIGZString& path) {   // @0x10054ec00
    cRZFile file(path);
    if (!file.Exists()) return false;
    file.Open(1, 2, 1);   // read, open existing, share read (the recovered Open maps access/creation)
    if (file.Length() == 0) return false;
    std::uint32_t size = file.Length();   // quirk: 0xffffffff (an error) passes the test above
    char* buffer = new char[size + 1];
    scriptText_ = buffer;
    file.ReadWithCount(buffer, size);
    buffer[size] = '\0';
    const bool ok = ExecuteScript();
    if (scriptText_) delete[] scriptText_;   // quirk: +0x50 is left dangling afterwards
    return ok;
}

bool cGZScriptTerp::ExecuteScriptResource(const cGZPersistResourceKey& key) {   // @0x10054efdc
    cRZSysServPtr<cIGZPersistResourceManager, kPersistResourceManagerService, kPersistResourceManagerIid>
        manager;
    // The original dereferences the service unconditionally (0x10054f031); a missing service returns false
    // here instead of crashing.
    if (!manager) return false;
    cIGZPersistDBRecord* record = nullptr;
    if (!manager->OpenDBRecord(key, &record, false)) return false;

    bool ok = false;
    cIGZPersistDBSerialRecord* serial = nullptr;
    if (record->QueryInterface(kGZIID_cIGZPersistDBSerialRecord, reinterpret_cast<void**>(&serial))) {
        const std::uint32_t size = serial->GetSize();   // slot 0x1a0
        std::uint8_t* data = new std::uint8_t[size ? size : 1];
        serial->GetFieldVoid(data, size);               // slot 0xd8
        serial->Release();
        // The original builds the cRZString by hand (SSO/heap), NUL-terminates it and parses that.
        cRZString script;
        script.AssignFromChars(reinterpret_cast<const char*>(data), size);
        delete[] data;
        ok = ExecuteScriptString(script);
    }
    // Quirk: on the QueryInterface failure path the record's own Release is skipped (0x10054f0d3 jumps
    // straight here); CloseDBRecord releases the caller's reference, so nothing leaks.
    manager->CloseDBRecord(key, &record);
    return ok;
}
