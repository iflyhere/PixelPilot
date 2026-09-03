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
        BUTTON_EXIT       = 6,
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

    /**
     * Producer side of a second, transparent quad layer for the HUD. Kept separate from
     * the video so the overlay is composited at panel resolution rather than being drawn
     * into a stream that has already been through an encoder - and so it can be redrawn
     * at its own rate without touching the decoder's swapchain.
     *
     * Null when the runtime would not give us a second surface swapchain.
     */
    /**
     * The overlay layers, each its own compositor layer with its own pose.
     *
     * <p>Depth in a headset comes from parallax, not from painted shadows - a bevel on a flat
     * plane reads as a bevel on a flat plane, because the stereo pair contradicts it. So the
     * instruments are separate layers at separate distances and angles, and the runtime
     * renders each one per eye from its own pose.
     *
     * <p>SYMBOLOGY has to stay a flat quad exactly over the video: the reticle and the horizon
     * are only meaningful if they sit where the image sits, and on a curved layer they would
     * drift off the video centre towards the edges. The rest is free, so the dashboard wraps
     * around the pilot and the map and the chart lie back like instruments on a panel.
     */
    enum Overlay
    {
        OVERLAY_SYMBOLOGY = 0,
        OVERLAY_DASHBOARD,
        OVERLAY_MINIMAP,
        OVERLAY_CHART,
        OVERLAY_COUNT
    };

    /** Producer side of an overlay layer, or null if the runtime would not give us one. */
    jobject overlaySurface(int id) const
    {
        return (id >= 0 && id < OVERLAY_COUNT) ? mOverlays[id].surface : nullptr;
    }

    int overlayWidth(int id) const
    {
        return (id >= 0 && id < OVERLAY_COUNT) ? mOverlays[id].width : 0;
    }

    int overlayHeight(int id) const
    {
        return (id >= 0 && id < OVERLAY_COUNT) ? mOverlays[id].height : 0;
    }

    /** Drops a layer from the frame. Off costs nothing per frame. */
    void setOverlayVisible(int id, bool visible)
    {
        if (id >= 0 && id < OVERLAY_COUNT) mOverlays[id].visible.store(visible);
    }


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
    bool createSurfaceSwapchain(
        JNIEnv* env, int width, int height, const char* what, XrSwapchain* outSwapchain, jobject* outSurface);
    void describeOverlays();
    bool createActions();
    bool createPassthrough();

    void pollEvents(JNIEnv* env, jobject listener, bool* exitLoop);
    void renderFrame(JNIEnv* env, jobject listener);
    void syncActions(JNIEnv* env, jobject listener);
    void applyPendingHaptic();
    void applyPassthroughState();
    // Returns true on the frame a held action should fire, once per press.
    bool heldActionFired(XrAction action, int slot, XrTime now);
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
    struct OverlayLayer
    {
        // Producer side.
        XrSwapchain swapchain = XR_NULL_HANDLE;
        jobject     surface   = nullptr;
        int         width     = 0;
        int         height    = 0;

        // Placement, relative to where the video quad sits. Angles in degrees.
        float yawDeg          = 0.0f;  // positive to the right
        float pitchDeg        = 0.0f;  // positive up
        float tiltDeg         = 0.0f;  // about its own X, to lay a panel back
        float distance        = 0.0f;  // metres; 0 means "same as the video"
        float widthM          = 0.0f;  // metres; 0 means "same as the video"
        float aspect          = 0.0f;  // height/width; 0 means "from the pixel size"
        bool  cylinder        = false;
        float centralAngleDeg = 0.0f;

        std::atomic<bool> visible{true};
        const char*       name = "";
    };

    OverlayLayer mOverlays[OVERLAY_COUNT];
    bool         mHasCylinder = false;


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
    XrAction    mActionHandRecenter = XR_NULL_HANDLE;
    XrAction    mActionHandPassthrough = XR_NULL_HANDLE;
    XrAction    mActionRaise        = XR_NULL_HANDLE;
    XrAction    mActionLower        = XR_NULL_HANDLE;
    XrAction    mActionExit         = XR_NULL_HANDLE;
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
    XrTime            mLastPredictedDisplayTime = 0;
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
    // Press bookkeeping for the hand actions, which need a deliberate hold.
    struct HeldButton
    {
        bool   down     = false;
        bool   fired    = false;
        XrTime downTime = 0;
    };
    HeldButton         mHeld[2];
    std::vector<float> mRefreshRates;
    std::mutex         mErrorMutex;
    std::string        mLastError;
};

#endif  // PIXELPILOT_XRGOGGLESESSION_H
