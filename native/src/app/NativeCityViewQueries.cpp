// The city renderer's view utilities (SL::spViewUtilities): the eye position (+0x44), the zoom (+0x14) and the rotation (+0x18).
#include "app/NativeCityView.h"
#include "sc4/Render3D.h"
#include "sc4/ViewUtilities.h"

namespace native {

const float* CityViewEye() {
    static const float none[3] = {0, 0, 0};
    sc4::cSC43DRender* render = CityRenderer();
    SC4ViewUtilities* view = render ? render->GetViewUtilities() : nullptr;
    return view ? &view->EyePosition().x : none;
}

int CityViewZoom() {
    sc4::cSC43DRender* render = CityRenderer();
    SC4ViewUtilities* view = render ? render->GetViewUtilities() : nullptr;
    return view ? view->Zoom() : 0;
}

int CityViewRotation() {
    sc4::cSC43DRender* render = CityRenderer();
    SC4ViewUtilities* view = render ? render->GetViewUtilities() : nullptr;
    return view ? view->Rotation() : 0;
}

}  // namespace native
