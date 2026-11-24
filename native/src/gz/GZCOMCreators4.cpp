// GZCOM creators of recovered classes that can be built without host services (occupant filters and the camera).
#include "gz/GZCOM.h"
#include "s3d/Camera.h"
#include "sc4/OccupantFilters2.h"

namespace gzcom {

void* GZCOM_CreatecSC4OccupantTypesFilter() { return static_cast<void*>(new sc4::cSC4OccupantTypesFilter()); }
void* GZCOM_CreatecSC4OccupantTypesFilterHashed() { return static_cast<void*>(new sc4::cSC4OccupantTypesFilterHashed()); }
void* GZCOM_CreatecSC4NetworkOccupantFilter() { return static_cast<void*>(new sc4::cSC4NetworkOccupantFilter()); }
void* GZCOM_CreatecSC4ViewLevelOccupantFilter() { return static_cast<void*>(new sc4::cSC4ViewLevelOccupantFilter()); }
void* GZCOM_CreatecSC4RealisticOccupantFilter() { return static_cast<void*>(new sc4::cSC4RealisticOccupantFilter()); }
void* GZCOM_CreatecS3DCamera() { return static_cast<void*>(new s3d::cS3DCamera()); }

}  // namespace gzcom
