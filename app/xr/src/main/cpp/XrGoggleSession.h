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

#include "XrInteraction.h"

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
        // Not a button: the video was let go of after being moved or resized with the
        // pointer, which is when its geometry is worth saving.
        BUTTON_VIDEO_MOVED = 7,
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
    /**
     * Whether hand tracking may act at all.
     *
     * <p>Off by default, because a pilot's hands are on a transmitter. Meta's thumb
     * microgestures are a thumb sliding along the index finger, which is very close to
     * what working a gimbal looks like, and a pinch-and-hold is close to gripping the
     * sticks. Every false trigger was a real one for the recogniser.
     */
    void setHandInputEnabled(bool enabled) { mHandInputEnabled.store(enabled); }

    bool handInputEnabled() const { return mHandInputEnabled.load(); }

    enum Overlay
    {
        OVERLAY_SYMBOLOGY = xri::LAYER_SYMBOLOGY,
        OVERLAY_DASHBOARD = xri::LAYER_DASHBOARD,
        OVERLAY_MINIMAP   = xri::LAYER_MINIMAP,
        OVERLAY_CHART     = xri::LAYER_CHART,
        OVERLAY_COUNT     = xri::LAYER_COUNT
    };

    /**
     * The pointer's two pictures: the beam, and the cursor's frames side by side. Handed out
     * like the overlays so the app draws them, but placed by the pointer every frame rather
     * than by the layout - and drawn once, since they never change.
     */
    enum PointerArt
    {
        POINTER_BEAM = OVERLAY_COUNT,
        POINTER_CURSOR,
        SURFACE_COUNT
    };

    static constexpr int kBeamArtWidth    = 32;
    static constexpr int kBeamArtHeight   = 256;
    static constexpr int kCursorArtCell   = 64;

    /** Producer side of an overlay or pointer layer, or null if the runtime would not give us one. */
    jobject overlaySurface(int id) const
    {
        if (id >= 0 && id < OVERLAY_COUNT) return mOverlays[id].surface;
        if (id >= POINTER_BEAM && id < SURFACE_COUNT) return mPointerArt[id - POINTER_BEAM].surface;
        return nullptr;
    }

    int overlayWidth(int id) const
    {
        if (id >= 0 && id < OVERLAY_COUNT) return mOverlays[id].width;
        if (id >= POINTER_BEAM && id < SURFACE_COUNT) return mPointerArt[id - POINTER_BEAM].width;
        return 0;
    }

    int overlayHeight(int id) const
    {
        if (id >= 0 && id < OVERLAY_COUNT) return mOverlays[id].height;
        if (id >= POINTER_BEAM && id < SURFACE_COUNT) return mPointerArt[id - POINTER_BEAM].height;
        return 0;
    }

    /** Drops a layer from the frame. Off costs nothing per frame. */
    void setOverlayVisible(int id, bool visible)
    {
        if (id >= 0 && id < OVERLAY_COUNT) mOverlays[id].visible.store(visible);
    }

    /**
     * Whether a controller may point at the instruments and the video and move them.
     *
     * <p>On by default, unlike hand input: a trigger pull aimed at a particular panel is a
     * deliberate act, where a hand gesture is something the recogniser decides you did. Off,
     * there is no beam at all - for a pilot who never rearranges anything.
     */
    void setOverlayDragEnabled(bool enabled) { mDragEnabled.store(enabled); }

    bool overlayDragEnabled() const { return mDragEnabled.load(); }

    /**
     * Whether a tracked hand may drag, as opposed to a controller.
     *
     * <p>Deliberately not the hand-gesture switch. That one is off because the microgestures
     * fire while a pilot works the sticks, and a drag has nothing in common with them: it
     * needs the pointing ray to land on a panel before it does anything at all. Coupling the
     * two would mean you could not have hand dragging without the gestures that made hand
     * input worth switching off. Still off by default, because it is the hands.
     */
    void setHandDragEnabled(bool enabled) { mHandDragEnabled.store(enabled); }

    /**
     * Places a layer, in the same units as the layout table: degrees, and metres for the
     * last two. A non-finite or non-positive size or distance means "leave that one alone",
     * so a caller restoring a saved layout does not have to know the defaults.
     */
    void setOverlayPose(int id, float yawDeg, float pitchDeg, float tiltDeg, float distance,
                        float widthM);

    /** Reads a layer's placement back, for saving it. Returns false for an unknown id. */
    bool overlayPose(int id, float* yawDeg, float* pitchDeg, float* tiltDeg, float* distance,
                     float* widthM) const;

    /** Back to the built-in arrangement. */
    void resetOverlayLayout();


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
    // Returns true on the frame a held action should fire, once per press. mayAct false holds
    // it off, and a press that began while it was false never fires.
    bool heldActionFired(XrAction action, int slot, XrTime now, bool mayAct);
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

        // Where it sits in the arrangement - see xri::Placement. Written by the frame loop
        // while a panel is being moved, and by restore and reset from other threads.
        xri::Placement place;

        std::atomic<bool> visible{true};
        const char*       name = "";
    };

    OverlayLayer mOverlays[OVERLAY_COUNT];
    bool         mHasCylinder = false;

    struct PointerArtLayer
    {
        XrSwapchain swapchain = XR_NULL_HANDLE;
        jobject     surface   = nullptr;
        int         width     = 0;
        int         height    = 0;
    };
    PointerArtLayer mPointerArt[SURFACE_COUNT - POINTER_BEAM];

    // --- pointing at things and moving them ---------------------------------------
    // The geometry and the state machine live in XrInteraction, which has no OpenXR in it
    // and is tested on a desktop. This class only feeds it and applies what it says.
    std::atomic<bool> mDragEnabled{true};
    std::atomic<bool> mHandDragEnabled{false};
    xri::Interaction  mInteraction;
    // What the pointer drew this frame, composed after everything else.
    xri::PointerOut   mPointers[2];
    // Read by syncActions(), used by the frame that follows.
    bool  mSelect[2] = {false, false};
    // A trigger counts once it has been seen up since focus or a profile change.
    bool  mSelectArmed[2] = {false, false};
    bool  mTouch[2]  = {false, false};
    float mStickX    = 0.0f;
    float mStickY    = 0.0f;
    // Whether each side is a tracked hand, from the last interaction profile change.
    bool  mSideIsHand[2] = {false, false};
    // Why each hand may not act right now, from the last frame - see xri::handBlock().
    xri::HandBlock mHandBlock[2] = {xri::HAND_NOT_TRACKED, xri::HAND_NOT_TRACKED};
    // How far a held hand gesture has got, 0..1, for the ring that shows it filling up.
    float mHoldProgress[2] = {0.0f, 0.0f};

    /**
     * Locates both hands, works out which may act, runs the pointer for this frame and
     * applies what it moved. Inside the frame loop, because it needs the anchor the layers
     * are composed against.
     */
    void updatePointer(JNIEnv* env, jobject listener, const xri::Anchor& anchor,
                       xri::Video* video, XrSpace layoutSpace, XrTime time, float dt);
    void applyHapticTo(int hand, float amplitude, int ms);
    // Only so every press can be logged once with what it was aimed at.
    bool mPointerSelectWas[2] = {false, false};

    /**
     * Listener methods, looked up once per session.
     *
     * <p>Not per frame: JNI local references live until the native call that made them
     * returns, and runLoop() does not return until the session ends - so a GetObjectClass()
     * in the frame path accumulates one reference every frame for the whole flight. A
     * jmethodID is not a reference and stays valid while the class is loaded, which it is.
     */
    void resolveListenerMethods(JNIEnv* env, jobject listener);

    jmethodID mOnButton       = nullptr;
    jmethodID mOnOverlayGrab  = nullptr;
    jmethodID mOnOverlayMoved = nullptr;
    jmethodID mOnOverlayHover = nullptr;

    /** The arrangement as xri wants it, copied out of the layers' atomic-free fields. */
    void currentPlacements(xri::Placement out[OVERLAY_COUNT]) const;


    XrPassthroughFB      mPassthrough      = XR_NULL_HANDLE;
    XrPassthroughLayerFB mPassthroughLayer = XR_NULL_HANDLE;
    bool                 mPassthroughRunning = false;

    XrActionSet mActionSet          = XR_NULL_HANDLE;
    XrAction    mActionRecenter     = XR_NULL_HANDLE;
    XrAction    mActionPassthrough  = XR_NULL_HANDLE;
    XrAction    mActionRecord       = XR_NULL_HANDLE;
    XrAction    mActionLockMode     = XR_NULL_HANDLE;
    XrAction    mActionStick        = XR_NULL_HANDLE;
    // A finger resting on a controller - trigger, stick, buttons or thumb rest - which is
    // what shows the beam. Per hand.
    XrAction    mActionTouch        = XR_NULL_HANDLE;
    XrAction    mActionHandRecenter = XR_NULL_HANDLE;
    XrAction    mActionHandPassthrough = XR_NULL_HANDLE;
    XrAction    mActionRaise        = XR_NULL_HANDLE;
    XrAction    mActionLower        = XR_NULL_HANDLE;
    XrAction    mActionExit         = XR_NULL_HANDLE;
    XrAction    mActionHaptic       = XR_NULL_HANDLE;
    XrAction    mActionAim          = XR_NULL_HANDLE;
    XrAction    mActionGrab         = XR_NULL_HANDLE;
    // One per hand, so either controller can do the pointing.
    XrSpace     mAimSpace[2]        = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    XrPath      mHandPath[2]        = {XR_NULL_PATH, XR_NULL_PATH};
    XrPath      mHandProfilePath    = XR_NULL_PATH;

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
    /** Whether the aim/grab bindings were actually accepted for the hand profile. */
    bool                     mHandDragBound      = false;
    bool                     mHasImageLayout     = false;
    bool                     mHasMicrogestures   = false;
    bool                     mHasSimultaneous    = false;
    /** Whether the capacitive touch bindings took; without them the beam always shows. */
    bool                     mHasTouchSensors    = false;
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
    std::atomic<bool>  mHandInputEnabled{false};
    std::atomic<float> mQuadWidth{2.2f};
    std::atomic<float> mQuadHeightOffset{0.0f};
    std::atomic<bool>  mHeadLocked{true};
    std::atomic<bool>  mPassthroughWanted{false};
    std::atomic<bool>  mSharpening{true};
    std::atomic<float> mWantedRefreshRate{0.0f};
    std::atomic<float> mAppliedRefreshRate{0.0f};

    std::atomic<float> mPendingHapticAmplitude{0.0f};
    std::atomic<int>   mPendingHapticMs{0};

    // Anchor for world-locked mode, in the LOCAL reference space: where the head was at the
    // last recenter, and which way it looked. The video and the instruments are both placed
    // from it, so moving the video further away does not drag the instruments with it.
    xri::Vec3  mAnchorEye;
    xri::Quat  mAnchorFacing;
    bool       mAnchorValid = false;

    std::string        mManifestDir;
    std::string        mRuntimeLibrary;
    // Press bookkeeping for the hand actions, which need a deliberate hold.
    struct HeldButton
    {
        bool   down     = false;
        bool   fired    = false;
        // Pressed while the hand was not free to act; ignored until it is let go.
        bool   blocked  = false;
        XrTime downTime = 0;
    };
    HeldButton         mHeld[2];
    std::vector<float> mRefreshRates;
    std::mutex         mErrorMutex;
    std::string        mLastError;
};

#endif  // PIXELPILOT_XRGOGGLESESSION_H
