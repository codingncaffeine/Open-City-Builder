#include "sc4/AdvisorUIImageWinProc.h"

#include <bit>

#include "gz/WinGen.h"
#include "gz/WinInterfaces.h"
#include "render/gd/IGZGDriver.h"
#include "rz/cRZMessage2Standard.h"
#include "s3d/ModelInstanceAnimatedSprite.h"
#include "sc4/Advisor.h"
#include "sc4/DrawContext.h"
#include "sc4/Render3D.h"

namespace sc4 {

namespace {

IAdvisorUIImageHost* g_host = nullptr;

// _mMoodIndexToModelIndexA / B (i386 0x008485b0 / 0x008485a0): the head animation slots of a mood.
constexpr int kMoodToModelA[4] = {0, 1, 2, 3};
constexpr int kMoodToModelB[4] = {4, 5, 6, 7};

int MoodSlot(int mood) { return mood >= 0 && mood < 4 ? mood : 0; }   // port guard: the original indexes unchecked

}  // namespace

IAdvisorUIImageHost* AdvisorUIImageHost() { return g_host; }
void SetAdvisorUIImageHost(IAdvisorUIImageHost* host) { g_host = host; }

cSC4AdvisorUIImageWinProc::cSC4AdvisorUIImageWinProc(int advisorId) : advisorId_(advisorId) {}

cSC4AdvisorUIImageWinProc::~cSC4AdvisorUIImageWinProc() { DropCamera(); }

void cSC4AdvisorUIImageWinProc::DropCamera() {
    if (camera_ && camera_->Release() == 0) delete camera_;
    camera_ = nullptr;
}

// @ 0x0021eaf8: the window procedure and the message target (iid 1 answers the window procedure).
bool cSC4AdvisorUIImageWinProc::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == gz::kGZIID_cIGZWinProc || iid == 1) {
        *out = static_cast<gz::cIGZWinProc*>(this);
    } else if (iid == 0x452294aa) {
        *out = static_cast<cIGZMessageTarget2*>(this);
    } else {
        return false;
    }
    AddRef();
    return true;
}

std::uint32_t cSC4AdvisorUIImageWinProc::AddRef() { return ++refCount_; }

std::uint32_t cSC4AdvisorUIImageWinProc::Release() {
    if (refCount_ > 1) return --refCount_;
    delete this;
    return 0;
}

// @ 0x0021eb72.
bool cSC4AdvisorUIImageWinProc::Shutdown() {
    if (initialized_) {
        initialized_ = false;
        Enable(false);
        window_ = nullptr;
        DropCamera();
        modelA_ = nullptr;
        modelB_ = nullptr;
        current_ = nullptr;
    }
    return true;
}

// @ 0x0021ec00: register with the animation tick manager and (re)load the models, or drop them.
void cSC4AdvisorUIImageWinProc::Enable(bool enable) {
    if (enabled_ == enable) return;
    enabled_ = enable;
    if (g_host) {
        if (enable) g_host->AddTickable(this, kAdvisorHeadTickPriority);
        else g_host->RemoveTickable(this, kAdvisorHeadTickPriority);
    }
    LoadModels(enable);
}

// @ 0x0021ec84: the animation tick carries the elapsed milliseconds as a float (message slot 0x4c).
bool cSC4AdvisorUIImageWinProc::DoMessage(cIGZMessage2* message) {
    if (message && message->GetType() == kAdvisorHeadTickMessage)
        DoAnimationTick(std::bit_cast<float>(static_cast<std::uint32_t>(static_cast<cIGZMessage2Standard*>(message)->GetData3())));
    return true;
}

// @ 0x0021ece8.
void cSC4AdvisorUIImageWinProc::ForceMood(int mood) {
    if (mood == 1) mood = 2;
    else if (mood == 0) mood = 3;
    else if (mood == 3) mood = 0;
    else if (mood == 5) mood = 1;
    if (mood == mood_) return;
    mood_ = mood;
    loopCount_ = 0;
    LoadModels(enabled_);
}

// @ 0x0021ed48: the window's area, the main window's size and the orthographic camera over the window.
bool cSC4AdvisorUIImageWinProc::Init(cIGZWin* window) {
    if (initialized_) return true;
    initialized_ = true;
    window_ = window;
    if (!window) return true;   // port guard
    area_ = window->GetAreaAbsolute();
    if (g_host) g_host->MainWindowSize(mainWidth_, mainHeight_);
    DropCamera();
    camera_ = new s3d::cS3DCamera();
    camera_->AddRef();
    camera_->Init();
    camera_->SetOrtho(0.0f, static_cast<float>(area_.right - area_.left), 0.0f, static_cast<float>(area_.bottom - area_.top),
                      -400.0f, 400.0f);
    const float* projection = camera_->ProjectionMatrix();
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row) projection_[column][row] = projection[row * 4 + column];
    // cS3DMatrix3::RotateY(pi) on the identity: x and z turn around.
    cS3DTransform& view = camera_->LocalXform();
    view.rotation[0][0] = -1.0f;
    view.rotation[2][2] = -1.0f;
    view.flags |= 4;
    ++view.changeCounter;
    return true;
}

// @ 0x0021eec0.
bool cSC4AdvisorUIImageWinProc::DoWinProcMessage(cIGZWin* window, cGZMessage& message) {
    if (message.type == gz::kWinProcInit) return Init(window);
    if (message.type == gz::kWinProcShutdown) return Shutdown();
    // the window's paint has finished (kWinProcPaintAfter): the head goes over it
    if (message.type != gz::kWinProcPaintAfter) return false;
    if (window_) area_ = window_->GetAreaAbsolute();
    DoPaint();
    if (window) window->InvalidateSelfAndParents();
    return true;
}

// @ 0x0021ef54: position, then the uniform scale written into the instance transform.
void cSC4AdvisorUIImageWinProc::PlaceHead(float x, float y, float z, float scale) {
    s3d::cIS3DModelInstanceAnimated* head = current_.get();
    if (!head) return;
    head->SetPosition(cS3DVector3{x, y, z});
    cS3DTransform transform;
    s3d::cIS3DModelInstance* instance = head->AsModelInstance();
    instance->GetTransform(transform);
    transform.scale = scale;
    if (scale != 1.0f) transform.flags |= 2;
    else transform.flags &= static_cast<std::uint8_t>(~2u);
    ++transform.changeCounter;
    instance->SetTransform(transform);
}

// @ 0x0021f2f8: the two head models of the advisor for the current mood (nothing changes while A stays the same).
void cSC4AdvisorUIImageWinProc::LoadModels(bool load) {
    if (!load) {
        modelIdA_ = 0;
        modelA_ = nullptr;
        modelIdB_ = 0;
        modelB_ = nullptr;
        return;
    }
    cISC4Advisor* advisor = g_host ? g_host->Advisor(advisorId_) : nullptr;
    if (!advisor) return;
    const int slot = MoodSlot(mood_);
    const std::uint32_t idA = static_cast<std::uint32_t>(advisor->GetHeadAnimID(kMoodToModelA[slot]));
    const std::uint32_t idB = static_cast<std::uint32_t>(advisor->GetHeadAnimID(kMoodToModelB[slot]));
    if (idA == modelIdA_) return;
    modelIdA_ = idA;
    modelA_ = nullptr;
    modelIdB_ = idB;
    modelB_ = nullptr;

    const bool llama = g_host->LlamaCheatActive();
    auto load1 = [&](std::uint32_t id, cRZAutoRefCount<s3d::cIS3DModelInstanceAnimated>& slotRef) {
        s3d::cIS3DModelInstanceAnimated* model = nullptr;
        if (llama) model = g_host->CreateModel(cGZPersistResourceKey{kAdvisorHeadModelType, kAdvisorHeadModelGroup, id + kAdvisorHeadLlamaOffset});
        if (!model) model = g_host->CreateModel(cGZPersistResourceKey{kAdvisorHeadModelType, kAdvisorHeadModelGroup, id});
        if (!model) return;
        slotRef = model;
        model->Release();   // the slot holds its own reference
        model->SetSelfIlluminated(true);
    };
    load1(modelIdA_, modelA_);
    load1(modelIdB_, modelB_);
    current_ = modelA_.get();
    if (current_) current_->SetFrame(0);
}

// @ 0x0021effc: the head over the window, with this camera, on a cleared depth buffer.
void cSC4AdvisorUIImageWinProc::DoPaint() {
    const int width = area_.right - area_.left;
    const int height = area_.bottom - area_.top;
    PlaceHead(static_cast<float>((area_.left - area_.right) / 2), static_cast<float>(-offset_), 0.0f,
              static_cast<float>(static_cast<double>(height) * kAdvisorHeadScale));
    cISC43DRender* renderer = g_host ? g_host->Renderer() : nullptr;
    gd::cIGZGDriver* driver = g_host ? g_host->Driver() : nullptr;
    if (!camera_ || !current_ || !renderer || !driver) return;
    SC4DrawContext* context = renderer->GetDrawContext();
    if (!context) return;   // port guard
    s3d::cS3DCamera* saved = context->Camera();
    renderer->BorrowRenderSystem();
    driver->SetViewport(static_cast<std::uint32_t>(area_.left), static_cast<std::uint32_t>(mainHeight_ - area_.bottom),
                        static_cast<std::uint32_t>(width - offset_), static_cast<std::uint32_t>(height));
    camera_->SetViewport(static_cast<float>(width - offset_), static_cast<float>(height));
    driver->DepthMask(true);
    driver->ClearDepth(1.0);
    driver->Clear(kAdvisorHeadClearMask);
    context->SetDefaultRenderStateUnilaterally();
    context->SetCamera(camera_);
    context->SetDepthOffset(0);
    current_->AsModelInstance()->Draw(context, false);
    context->SetCamera(saved);
    driver->SetViewport();
    renderer->ReturnRenderSystem();
}

// @ 0x0021f1d2: advance the playing model; on each wrap to frame 0 restart A with a new loop count, or count a loop and
// switch to B when the count is reached.
void cSC4AdvisorUIImageWinProc::DoAnimationTick(float milliseconds) {
    if (!current_) return;
    current_->DoAutoFrameUpdate(milliseconds);
    if (current_->GetFrame() == 0 && lastFrame_ != 0) {
        if (loopCount_ == 0) {
            loopCount_ = 1;
            current_ = modelA_.get();
            if (current_) current_->SetFrame(0);
            loopTarget_ = random_.RandomSint32RangeUniform(kAdvisorHeadLoopsMin, kAdvisorHeadLoopsMax);
        } else if (++loopCount_ == loopTarget_) {
            loopCount_ = 0;
            current_ = modelB_.get();
            if (current_) current_->SetFrame(0);
        }
    }
    if (current_) lastFrame_ = current_->GetFrame();
}

}  // namespace sc4
