// The makers of the six classes cGZSoundCOMDirector registers (the director and its framework hooks are in
// rz/COMDirectors.{h,cpp}; its class table names the classes and these are their Create functions).
#include "audio/GZSndCaches.h"
#include "audio/GZSndDefaultEventHandler.h"
#include "audio/GZSndMgr.h"
#include "audio/GZSndSys.h"
#include "audio/SegmentDataSources.h"
#include "rz/COMDirectors.h"

namespace gz {

void RegisterGZSoundClasses() {
    RegisterDirectorMaker("cGZSndSys", +[]() -> cIGZUnknown* { return static_cast<cIGZSndSys*>(cGZSndSys::Create()); });
    RegisterDirectorMaker("cGZSndMgr", +[]() -> cIGZUnknown* { return static_cast<cIGZSndMgr*>(cGZSndMgr::Create()); });
    RegisterDirectorMaker("cGZSndSegmentDataSourceCreator", +[]() -> cIGZUnknown* { return cGZSndSegmentDataSourceCreator::Create(); });
    RegisterDirectorMaker("cGZSndPropertyCache", +[]() -> cIGZUnknown* { return cGZSndPropertyCache::Create(); });
    RegisterDirectorMaker("cGZSndDefaultEventHandler", +[]() -> cIGZUnknown* { return cGZSndDefaultEventHandler::Create(); });
    RegisterDirectorMaker("cGZSndInstanceCache", +[]() -> cIGZUnknown* { return cGZSndInstanceCache::Create(); });
}

}  // namespace gz
