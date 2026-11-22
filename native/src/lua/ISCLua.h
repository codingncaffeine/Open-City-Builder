// cISCLua — the scripting interface SimCity 4 code talks to (cSCLua's secondary vtable at +0x80, 0x1005e669c..).
// A flat Lua-4 style stack API; slot offsets are the original vtable offsets (verified with scripts/vtable.py cSCLua).
// Return types of the slots no recovered caller uses yet are inferred from the Lua 4 API.
#pragma once
#include <cstdint>

class cISC4GameDataRegistry;
class cIGZUnknown;
struct lua_State;

class cISCLua {
public:
    using CFunction = int (*)(lua_State*);
    // No virtual destructor: slot 0x00 is QueryInterface, so the original has none in this view.
    virtual bool QueryInterface(std::uint32_t iid, void** out) = 0;   // 0x00
    virtual std::uint32_t AddRef() = 0;                               // 0x08
    virtual std::uint32_t Release() = 0;                              // 0x10
    virtual bool Open(int libraries) = 0;                             // 0x18
    virtual bool Close() = 0;                                         // 0x20
    virtual void* AsIGZLua5() = 0;                                    // 0x28
    virtual int GetTop() = 0;                                         // 0x30
    virtual void SetTop(int index) = 0;                               // 0x38
    virtual void PushValue(int index) = 0;                            // 0x40
    virtual void Remove(int index) = 0;                               // 0x48
    virtual void Insert(int index) = 0;                               // 0x50
    virtual void Pop(int count) = 0;                                  // 0x58
    virtual int Type(int index) = 0;                                  // 0x60
    virtual const char* TypeName(int type) = 0;                       // 0x68
    virtual bool IsNumber(int index) = 0;                             // 0x70
    virtual bool IsString(int index) = 0;                             // 0x78
    virtual bool IsCFunction(int index) = 0;                          // 0x80
    virtual int Tag(int index) = 0;                                   // 0x88
    virtual bool IsBoolean(int index) = 0;                            // 0x90
    virtual bool Equal(int a, int b) = 0;                             // 0x98
    virtual bool LessThan(int a, int b) = 0;                          // 0xa0
    virtual std::uint32_t ToUint32(int index) = 0;                    // 0xa8
    virtual std::int32_t ToSint32(int index) = 0;                     // 0xb0
    virtual double ToNumber(int index) = 0;                           // 0xb8
    virtual const char* ToString(int index) = 0;                      // 0xc0
    virtual int Strlen(int index) = 0;                                // 0xc8
    virtual CFunction ToCFunction(int index) = 0;                     // 0xd0
    virtual void* ToUserData(int index) = 0;                          // 0xd8
    virtual void* ToUserTag(int index) = 0;                           // 0xe0
    virtual bool ToBoolean(int index) = 0;                            // 0xe8
    virtual void PushNil() = 0;                                       // 0xf0
    virtual void PushNumber(double value) = 0;                        // 0xf8
    virtual void PushLString(const char* s, int length) = 0;          // 0x100
    virtual void PushString(const char* s) = 0;                       // 0x108
    virtual void PushCClosure(CFunction fn, int upvalues) = 0;        // 0x110
    virtual void PushUserTag(void* p, int tag) = 0;                   // 0x118
    virtual void PushBoolean(bool value) = 0;                         // 0x120
    virtual void GetGlobal(const char* name) = 0;                     // 0x128
    virtual void GetTable(int index) = 0;                             // 0x130
    virtual void RawGet(int index) = 0;                               // 0x138
    virtual void RawGetI(int index, int n) = 0;                       // 0x140
    virtual void GetGlobals() = 0;                                    // 0x148
    virtual void GetTagMethod(int tag, const char* event) = 0;        // 0x150
    virtual bool GetRef(int ref) = 0;                                 // 0x158
    virtual void NewTable() = 0;                                      // 0x160
    virtual void SetGlobal(const char* name) = 0;                     // 0x168
    virtual void SetTable(int index) = 0;                             // 0x170
    virtual void RawSet(int index) = 0;                               // 0x178
    virtual void RawSetI(int index, int n) = 0;                       // 0x180
    virtual void SetTagMethod(int tag, const char* event) = 0;        // 0x188
    virtual int Ref(int lockIt) = 0;                                  // 0x190
    virtual int CallProtected(int args, int results) = 0;             // 0x198
    virtual void RawCall(int args, int results) = 0;                  // 0x1a0
    virtual int DoFile(const char* path) = 0;                         // 0x1a8
    virtual int DoString(const char* code) = 0;                       // 0x1b0
    virtual int DoBuffer(const char* buffer, int size, const char* name) = 0;   // 0x1b8
    virtual int GetGCCount() = 0;                                     // 0x1c0
    virtual int GetGCThreshold() = 0;                                 // 0x1c8
    virtual void SetGCThreshold(int value) = 0;                       // 0x1d0
    virtual int NewTag() = 0;                                         // 0x1d8
    virtual void CopyTagMethods(int from, int to) = 0;                // 0x1e0
    virtual void SetTag(int tag) = 0;                                 // 0x1e8
    virtual void Unref(int ref) = 0;                                  // 0x1f0
    virtual int Next(int index) = 0;                                  // 0x1f8 pops a key, pushes key,value; 0 at the end
    virtual int GetN(int index) = 0;                                  // 0x200
    virtual void Error(const char* message) = 0;                      // 0x208
    virtual void Concat(int count) = 0;                               // 0x210
    virtual void* NewUserData(int size) = 0;                          // 0x218
    virtual void Register(CFunction fn, const char* name) = 0;        // 0x220
    virtual void PushUserData(void* p, int tag) = 0;                  // 0x228
    virtual int CloneTag(int tag) = 0;                                // 0x230
    virtual bool IsFunction(int index) = 0;                           // 0x238
    virtual bool IsTable(int index) = 0;                              // 0x240
    virtual bool IsUserData(int index) = 0;                           // 0x248
    virtual bool IsNil(int index) = 0;                                // 0x250
    virtual std::uintptr_t ToDataRegistryEntryID(cISC4GameDataRegistry* registry, int index) = 0;   // 0x258 the entry id of a node table (0: none)
    virtual cIGZUnknown* ToDataRegistryNode(cISC4GameDataRegistry* registry, int index) = 0;      // 0x260 its node object (borrowed)
    virtual void* ToDataRegistryValue(cISC4GameDataRegistry* registry, int index) = 0;     // 0x268 its value handle

protected:
    ~cISCLua() = default;
};
