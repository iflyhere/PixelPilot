// Immersive OpenXR presentation of the decoded video stream.
//
// The video never touches this code: xrCreateSwapchainAndroidSurfaceKHR hands out an
// Android Surface that is the producer side of a compositor swapchain, MediaCodec
// decodes straight into it, and the runtime composites it as a quad layer. There is no
// GL blit and no readback in the video path - the only reason an EGL context exists at
// all is that OpenXR requires a graphics binding for the session.

#ifndef PIXELPILOT_XRGOGGLESESSION_H
#define PIXELPILOT_XRGOGGLESESSION_H

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <jni.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

class XrGoggleSession
{
  public:
    // Keep in sync with XrGoggleSession.java
    enum Button
    {
        BUTTON_RECENTER   = 0,
        BUTTON_PASSTHROUGH = 1,
        BUTTON_RECORD     = 2,
        BUTTON_LOCK_MODE  = 3,
        BUTTON_RAISE      = 4,
        BUTTON_LOWER      = 5,
    };

    XrGoggleSession()  = default;
    ~XrGoggleSession() = default;

    // Brings up loader, instance, system, EGL, session, swapchain and actions.
    // Returns false and leaves lastError() set on any failure; the caller is expected to
    // fall back to the flat activity.
    bool create(JNIEnv* env, jobject activity);

    // Blocks until requestStop() or an unrecoverable error. Must be called from the same
    // thread that called create(), and that thread must be attached to the JVM: button
    // presses are dispatched to listener.onXrButton(int) from here.
    void runLoop(JNIEnv* env, jobject listener);

    void destroy(JNIEnv* env);

    void requestStop() { mStopRequested = true; }

    // The producer Surface for MediaCodec. Global ref, valid between create() and
    // destroy().
    jobject videoSurface() const { return mVideoSurface; }

    std::string lastError();

    std::vector<float> refreshRates();

    // --- live settings, read by the frame loop -------------------------------------
    void setVideoResolution(int width, int height);
    void setQuadDistance(float meters);
    void setQuadWidth(float meters);
    void setQuadHeightOffset(float meters);
    void setPassthrough(bool enabled);
    void setSharpening(bool enabled);
    void setHeadLocked(bool enabled);
    void requestRefreshRate(float hz);
    void requestRecenter() { mRecenterRequested = true; }
    // amplitude 0..1, duration in milliseconds
    void requestHaptic(float amplitude, int durationMs);

    float quadDistance() const { return mQuadDistance; }
    float quadWidth() const { return mQuadWidth; }
    float quadHeightOffset() const { return mQuadHeightOffset; }
    bool  headLocked() const { return mHeadLocked; }
    bool  passthroughEnabled() const { return mPassthroughWanted; }

    // Swapchain geometry is fixed at create() time; the caller seeds it from the last
    // known stream resolution.
    void setSwapchainSize(int width, int height);

    // Writable directory used to drop a runtime manifest, see resolveRuntime().
    void setManifestDir(std::string dir) { mManifestDir = std::move(dir); }

  private:
    bool initLoader(JNIEnv* env, jobject activity);
    bool resolveRuntime();
    bool runtimeAnswers();
    bool pointLoaderAt(const char* libraryPath);
    bool createInstance(JNIEnv* env, jobject activity);
    bool createEgl();
    bool createSession();
    bool createSpaces();
    bool createSwapchain(JNIEnv* env);
    bool createActions();
    bool createPassthrough();

    void pollEvents(JNIEnv* env, jobject listener, bool* exitLoop);
    void renderFrame(JNIEnv* env, jobject listener);
    void syncActions(JNIEnv* env, jobject listener);
    void applyPendingHaptic();
    void applyPassthroughState();
    void logInteractionProfiles();
    void applyPendingRefreshRate();
    void recenterAt(XrTime time);

    bool     extensionSupported(const char* name) const;
    bool     check(XrResult result, const char* what);
    void     setError(const std::string& what);
    XrPath   path(const char* str);
    XrAction boolAction(const char* name, const char* localized);

    // --- OpenXR handles -----------------------------------------------------------
    XrInstance mInstance = XR_NULL_HANDLE;
    XrSystemId mSystemId = XR_NULL_SYSTEM_ID;
    XrSession  mSession  = XR_NULL_HANDLE;
    XrSpace    mViewSpace  = XR_NULL_HANDLE;
    XrSpace    mLocalSpace = XR_NULL_HANDLE;
    XrSwapchain mSwapchain  = XR_NULL_HANDLE;
    jobject     mVideoSurface = nullptr;

    XrPassthroughFB      mPassthrough      = XR_NULL_HANDLE;
    XrPassthroughLayerFB mPassthroughLayer = XR_NULL_HANDLE;
    bool                 mPassthroughRunning = false;

    XrActionSet mActionSet          = XR_NULL_HANDLE;
    XrAction    mActionRecenter     = XR_NULL_HANDLE;
    XrAction    mActionPassthrough  = XR_NULL_HANDLE;
    XrAction    mActionRecord       = XR_NULL_HANDLE;
    XrAction    mActionLockMode     = XR_NULL_HANDLE;
    XrAction    mActionStick        = XR_NULL_HANDLE;
    XrAction    mActionNearer       = XR_NULL_HANDLE;
    XrAction    mActionFarther      = XR_NULL_HANDLE;
    XrAction    mActionRaise        = XR_NULL_HANDLE;
    XrAction    mActionLower        = XR_NULL_HANDLE;
    XrAction    mActionHaptic       = XR_NULL_HANDLE;

    // --- EGL ----------------------------------------------------------------------
    EGLDisplay mEglDisplay = EGL_NO_DISPLAY;
    EGLConfig  mEglConfig  = nullptr;
    EGLSurface mEglSurface = EGL_NO_SURFACE;
    EGLContext mEglContext = EGL_NO_CONTEXT;

    // --- extension entry points ---------------------------------------------------
    PFN_xrCreateSwapchainAndroidSurfaceKHR mXrCreateSwapchainAndroidSurfaceKHR = nullptr;
    PFN_xrCreatePassthroughFB              mXrCreatePassthroughFB              = nullptr;
    PFN_xrDestroyPassthroughFB             mXrDestroyPassthroughFB             = nullptr;
    PFN_xrPassthroughStartFB               mXrPassthroughStartFB               = nullptr;
    PFN_xrPassthroughPauseFB               mXrPassthroughPauseFB               = nullptr;
    PFN_xrCreatePassthroughLayerFB         mXrCreatePassthroughLayerFB         = nullptr;
    PFN_xrDestroyPassthroughLayerFB        mXrDestroyPassthroughLayerFB        = nullptr;
    PFN_xrPassthroughLayerResumeFB         mXrPassthroughLayerResumeFB         = nullptr;
    PFN_xrPassthroughLayerPauseFB          mXrPassthroughLayerPauseFB          = nullptr;
    PFN_xrPassthroughLayerSetStyleFB       mXrPassthroughLayerSetStyleFB       = nullptr;
    PFN_xrEnumerateDisplayRefreshRatesFB   mXrEnumerateDisplayRefreshRatesFB   = nullptr;
    PFN_xrRequestDisplayRefreshRateFB      mXrRequestDisplayRefreshRateFB      = nullptr;
    PFN_xrResumeSimultaneousHandsAndControllersTrackingMETA mXrResumeSimultaneous = nullptr;

    std::vector<std::string> mAvailableExtensions;
    bool                     mHasPassthrough    = false;
    bool                     mHasHandInteraction = false;
    bool                     mHasImageLayout     = false;
    bool                     mHasMicrogestures   = false;
    bool                     mHasSimultaneous    = false;
    bool                     mHasLayerSettings = false;
    bool                     mHasRefreshRate   = false;

    // --- state --------------------------------------------------------------------
    XrSessionState    mSessionState  = XR_SESSION_STATE_UNKNOWN;
    bool              mSessionRunning = false;
    std::atomic<bool> mStopRequested{false};
    std::atomic<bool> mRecenterRequested{false};

    std::atomic<int>   mVideoWidth{1920};
    std::atomic<int>   mVideoHeight{1080};
    int                mSwapchainWidth  = 1920;
    int                mSwapchainHeight = 1080;
    std::atomic<float> mQuadDistance{1.6f};
    std::atomic<float> mQuadWidth{2.2f};
    std::atomic<float> mQuadHeightOffset{0.0f};
    std::atomic<bool>  mHeadLocked{true};
    std::atomic<bool>  mPassthroughWanted{false};
    std::atomic<bool>  mSharpening{true};
    std::atomic<float> mWantedRefreshRate{0.0f};
    std::atomic<float> mAppliedRefreshRate{0.0f};

    std::atomic<float> mPendingHapticAmplitude{0.0f};
    std::atomic<int>   mPendingHapticMs{0};

    // Anchor for world-locked mode, in the LOCAL reference space.
    XrPosef    mAnchorPose = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
    bool       mAnchorValid = false;

    std::string        mManifestDir;
    std::string        mRuntimeLibrary;
    std::vector<float> mRefreshRates;
    std::mutex         mErrorMutex;
    std::string        mLastError;
};

#endif  // PIXELPILOT_XRGOGGLESESSION_H
