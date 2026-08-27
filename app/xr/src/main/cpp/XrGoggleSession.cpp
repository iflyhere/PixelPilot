#include "XrGoggleSession.h"

#include <android/log.h>
#include <time.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#define TAG "pixelpilot-xr"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace
{
constexpr float kStickDeadzone   = 0.30f;
constexpr float kDistancePerSec  = 0.9f;   // meters per second of full stick deflection
constexpr float kWidthPerSec     = 1.4f;
constexpr float kMinDistance     = 0.4f;
constexpr float kMaxDistance     = 8.0f;
constexpr float kMinWidth        = 0.4f;
constexpr float kMaxWidth        = 12.0f;

float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Yaw-only quaternion, so the panel never ends up tilted when it is anchored.
XrQuaternionf quatFromYaw(float yaw)
{
    return XrQuaternionf{0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
}

float yawOf(const XrQuaternionf& q)
{
    return std::atan2(2.0f * (q.w * q.y + q.x * q.z), 1.0f - 2.0f * (q.y * q.y + q.z * q.z));
}
}  // namespace

// ---------------------------------------------------------------------------------
// error handling
// ---------------------------------------------------------------------------------

void XrGoggleSession::setError(const std::string& what)
{
    std::lock_guard<std::mutex> lock(mErrorMutex);
    if (mLastError.empty())
    {
        mLastError = what;
    }
    LOGE("%s", what.c_str());
}

std::string XrGoggleSession::lastError()
{
    std::lock_guard<std::mutex> lock(mErrorMutex);
    return mLastError;
}

bool XrGoggleSession::check(XrResult result, const char* what)
{
    if (XR_SUCCEEDED(result))
    {
        return true;
    }
    char name[XR_MAX_RESULT_STRING_SIZE] = {0};
    if (mInstance != XR_NULL_HANDLE)
    {
        xrResultToString(mInstance, result, name);
    }
    else
    {
        snprintf(name, sizeof(name), "%d", (int) result);
    }
    setError(std::string(what) + " failed: " + name);
    return false;
}

XrPath XrGoggleSession::path(const char* str)
{
    XrPath p = XR_NULL_PATH;
    XrResult r = xrStringToPath(mInstance, str, &p);
    if (XR_FAILED(r))
    {
        LOGW("xrStringToPath(%s) failed", str);
        return XR_NULL_PATH;
    }
    return p;
}

bool XrGoggleSession::extensionSupported(const char* name) const
{
    for (const auto& e : mAvailableExtensions)
    {
        if (e == name) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------
// bring-up
// ---------------------------------------------------------------------------------

bool XrGoggleSession::create(JNIEnv* env, jobject activity)
{
    if (!initLoader(env, activity)) return false;
    if (!createInstance(env, activity)) return false;
    if (!createEgl()) return false;
    if (!createSession()) return false;
    if (!createSpaces()) return false;
    if (!createSwapchain(env)) return false;
    if (!createActions()) return false;
    // Passthrough is optional: a runtime without XR_FB_passthrough still flies.
    if (mHasPassthrough && !createPassthrough())
    {
        LOGW("passthrough unavailable, continuing without it");
        mHasPassthrough = false;
    }
    if (mHasRefreshRate)
    {
        uint32_t count = 0;
        if (check(mXrEnumerateDisplayRefreshRatesFB(mSession, 0, &count, nullptr),
                  "xrEnumerateDisplayRefreshRatesFB") &&
            count > 0)
        {
            mRefreshRates.resize(count);
            mXrEnumerateDisplayRefreshRatesFB(mSession, count, &count, mRefreshRates.data());
            for (float hz : mRefreshRates)
            {
                LOGI("display refresh rate available: %.1f Hz", hz);
            }
        }
    }
    return true;
}

bool XrGoggleSession::initLoader(JNIEnv* env, jobject activity)
{
    PFN_xrInitializeLoaderKHR initializeLoader = nullptr;
    XrResult r = xrGetInstanceProcAddr(
        XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction*) &initializeLoader);
    if (XR_FAILED(r) || initializeLoader == nullptr)
    {
        // Not fatal on every runtime, but on Android it usually is.
        LOGW("xrInitializeLoaderKHR not exposed by the loader");
        return true;
    }

    JavaVM* vm = nullptr;
    env->GetJavaVM(&vm);

    XrLoaderInitInfoAndroidKHR init{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    init.applicationVM      = vm;
    init.applicationContext = env->NewGlobalRef(activity);
    r = initializeLoader((const XrLoaderInitInfoBaseHeaderKHR*) &init);
    if (XR_FAILED(r))
    {
        setError("xrInitializeLoaderKHR failed");
        return false;
    }
    return true;
}

bool XrGoggleSession::createInstance(JNIEnv* env, jobject activity)
{
    uint32_t extCount = 0;
    if (!check(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr),
               "xrEnumerateInstanceExtensionProperties"))
    {
        return false;
    }
    std::vector<XrExtensionProperties> props(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
    if (extCount > 0)
    {
        if (!check(
                xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, props.data()),
                "xrEnumerateInstanceExtensionProperties"))
        {
            return false;
        }
    }
    mAvailableExtensions.clear();
    for (const auto& p : props)
    {
        mAvailableExtensions.emplace_back(p.extensionName);
    }

    // Without these three there is no immersive video path at all.
    const char* required[] = {
        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
        XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
        XR_KHR_ANDROID_SURFACE_SWAPCHAIN_EXTENSION_NAME,
    };
    std::vector<const char*> enabled;
    for (const char* name : required)
    {
        if (!extensionSupported(name))
        {
            setError(std::string("OpenXR runtime does not support ") + name);
            return false;
        }
        enabled.push_back(name);
    }

    mHasPassthrough   = extensionSupported(XR_FB_PASSTHROUGH_EXTENSION_NAME);
    mHasLayerSettings = extensionSupported(XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME);
    mHasRefreshRate   = extensionSupported(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    if (mHasPassthrough) enabled.push_back(XR_FB_PASSTHROUGH_EXTENSION_NAME);
    if (mHasLayerSettings) enabled.push_back(XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME);
    if (mHasRefreshRate) enabled.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    LOGI("optional extensions: passthrough=%d layerSettings=%d refreshRate=%d",
         (int) mHasPassthrough,
         (int) mHasLayerSettings,
         (int) mHasRefreshRate);

    JavaVM* vm = nullptr;
    env->GetJavaVM(&vm);

    XrInstanceCreateInfoAndroidKHR androidInfo{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    androidInfo.applicationVM       = vm;
    androidInfo.applicationActivity = env->NewGlobalRef(activity);

    XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
    info.next = &androidInfo;
    std::strncpy(info.applicationInfo.applicationName, "PixelPilot", XR_MAX_APPLICATION_NAME_SIZE - 1);
    std::strncpy(info.applicationInfo.engineName, "PixelPilot", XR_MAX_ENGINE_NAME_SIZE - 1);
    info.applicationInfo.applicationVersion = 1;
    info.applicationInfo.engineVersion      = 1;
    // Ask for 1.0: every shipping Android runtime implements it, and nothing here needs 1.1.
    info.applicationInfo.apiVersion   = XR_API_VERSION_1_0;
    info.enabledExtensionCount        = (uint32_t) enabled.size();
    info.enabledExtensionNames        = enabled.data();

    if (!check(xrCreateInstance(&info, &mInstance), "xrCreateInstance")) return false;

    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!check(xrGetSystem(mInstance, &systemInfo, &mSystemId), "xrGetSystem")) return false;

    xrGetInstanceProcAddr(mInstance,
                          "xrCreateSwapchainAndroidSurfaceKHR",
                          (PFN_xrVoidFunction*) &mXrCreateSwapchainAndroidSurfaceKHR);
    if (mXrCreateSwapchainAndroidSurfaceKHR == nullptr)
    {
        setError("xrCreateSwapchainAndroidSurfaceKHR not available");
        return false;
    }
    if (mHasPassthrough)
    {
        xrGetInstanceProcAddr(
            mInstance, "xrCreatePassthroughFB", (PFN_xrVoidFunction*) &mXrCreatePassthroughFB);
        xrGetInstanceProcAddr(
            mInstance, "xrDestroyPassthroughFB", (PFN_xrVoidFunction*) &mXrDestroyPassthroughFB);
        xrGetInstanceProcAddr(
            mInstance, "xrPassthroughStartFB", (PFN_xrVoidFunction*) &mXrPassthroughStartFB);
        xrGetInstanceProcAddr(
            mInstance, "xrPassthroughPauseFB", (PFN_xrVoidFunction*) &mXrPassthroughPauseFB);
        xrGetInstanceProcAddr(mInstance,
                              "xrCreatePassthroughLayerFB",
                              (PFN_xrVoidFunction*) &mXrCreatePassthroughLayerFB);
        xrGetInstanceProcAddr(mInstance,
                              "xrDestroyPassthroughLayerFB",
                              (PFN_xrVoidFunction*) &mXrDestroyPassthroughLayerFB);
        xrGetInstanceProcAddr(mInstance,
                              "xrPassthroughLayerResumeFB",
                              (PFN_xrVoidFunction*) &mXrPassthroughLayerResumeFB);
        xrGetInstanceProcAddr(mInstance,
                              "xrPassthroughLayerPauseFB",
                              (PFN_xrVoidFunction*) &mXrPassthroughLayerPauseFB);
        xrGetInstanceProcAddr(mInstance,
                              "xrPassthroughLayerSetStyleFB",
                              (PFN_xrVoidFunction*) &mXrPassthroughLayerSetStyleFB);
        mHasPassthrough = mXrCreatePassthroughFB != nullptr && mXrCreatePassthroughLayerFB != nullptr &&
                          mXrPassthroughStartFB != nullptr;
    }
    if (mHasRefreshRate)
    {
        xrGetInstanceProcAddr(mInstance,
                              "xrEnumerateDisplayRefreshRatesFB",
                              (PFN_xrVoidFunction*) &mXrEnumerateDisplayRefreshRatesFB);
        xrGetInstanceProcAddr(mInstance,
                              "xrRequestDisplayRefreshRateFB",
                              (PFN_xrVoidFunction*) &mXrRequestDisplayRefreshRateFB);
        mHasRefreshRate =
            mXrEnumerateDisplayRefreshRatesFB != nullptr && mXrRequestDisplayRefreshRateFB != nullptr;
    }
    return true;
}

bool XrGoggleSession::createEgl()
{
    // Nothing is rendered through this context - OpenXR just requires a graphics binding,
    // and the runtime needs a current context on the session thread.
    mEglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (mEglDisplay == EGL_NO_DISPLAY)
    {
        setError("eglGetDisplay failed");
        return false;
    }
    if (eglInitialize(mEglDisplay, nullptr, nullptr) == EGL_FALSE)
    {
        setError("eglInitialize failed");
        return false;
    }

    const EGLint configAttribs[] = {EGL_RENDERABLE_TYPE,
                                    EGL_OPENGL_ES3_BIT_KHR,
                                    EGL_SURFACE_TYPE,
                                    EGL_PBUFFER_BIT,
                                    EGL_RED_SIZE,
                                    8,
                                    EGL_GREEN_SIZE,
                                    8,
                                    EGL_BLUE_SIZE,
                                    8,
                                    EGL_ALPHA_SIZE,
                                    8,
                                    EGL_DEPTH_SIZE,
                                    0,
                                    EGL_STENCIL_SIZE,
                                    0,
                                    EGL_SAMPLES,
                                    0,
                                    EGL_NONE};
    EGLint numConfigs = 0;
    if (eglChooseConfig(mEglDisplay, configAttribs, &mEglConfig, 1, &numConfigs) == EGL_FALSE ||
        numConfigs < 1)
    {
        setError("eglChooseConfig found no usable config");
        return false;
    }

    const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    mEglContext = eglCreateContext(mEglDisplay, mEglConfig, EGL_NO_CONTEXT, contextAttribs);
    if (mEglContext == EGL_NO_CONTEXT)
    {
        setError("eglCreateContext failed");
        return false;
    }

    const EGLint surfaceAttribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    mEglSurface = eglCreatePbufferSurface(mEglDisplay, mEglConfig, surfaceAttribs);
    if (mEglSurface == EGL_NO_SURFACE)
    {
        setError("eglCreatePbufferSurface failed");
        return false;
    }
    if (eglMakeCurrent(mEglDisplay, mEglSurface, mEglSurface, mEglContext) == EGL_FALSE)
    {
        setError("eglMakeCurrent failed");
        return false;
    }
    return true;
}

bool XrGoggleSession::createSession()
{
    // Required by the spec before xrCreateSession, even though we never render.
    PFN_xrGetOpenGLESGraphicsRequirementsKHR getRequirements = nullptr;
    xrGetInstanceProcAddr(mInstance,
                          "xrGetOpenGLESGraphicsRequirementsKHR",
                          (PFN_xrVoidFunction*) &getRequirements);
    if (getRequirements != nullptr)
    {
        XrGraphicsRequirementsOpenGLESKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
        getRequirements(mInstance, mSystemId, &requirements);
    }

    XrGraphicsBindingOpenGLESAndroidKHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    binding.display = mEglDisplay;
    binding.config  = mEglConfig;
    binding.context = mEglContext;

    XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
    info.next     = &binding;
    info.systemId = mSystemId;
    return check(xrCreateSession(mInstance, &info, &mSession), "xrCreateSession");
}

bool XrGoggleSession::createSpaces()
{
    XrReferenceSpaceCreateInfo viewInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    viewInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    viewInfo.poseInReferenceSpace = XrPosef{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
    if (!check(xrCreateReferenceSpace(mSession, &viewInfo, &mViewSpace),
               "xrCreateReferenceSpace(VIEW)"))
    {
        return false;
    }

    XrReferenceSpaceCreateInfo localInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    localInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    localInfo.poseInReferenceSpace = XrPosef{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
    return check(xrCreateReferenceSpace(mSession, &localInfo, &mLocalSpace),
                 "xrCreateReferenceSpace(LOCAL)");
}

bool XrGoggleSession::createSwapchain(JNIEnv* env)
{
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    // An Android surface swapchain is produced into by MediaCodec, so format and the
    // image-layout fields are not ours to pick.
    info.usageFlags  = XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format      = 0;
    info.sampleCount = 1;
    info.width       = (uint32_t) mSwapchainWidth;
    info.height      = (uint32_t) mSwapchainHeight;
    info.faceCount   = 1;
    info.arraySize   = 1;
    info.mipCount    = 1;

    jobject surface = nullptr;
    if (!check(mXrCreateSwapchainAndroidSurfaceKHR(mSession, &info, &mSwapchain, &surface),
               "xrCreateSwapchainAndroidSurfaceKHR"))
    {
        return false;
    }
    if (surface == nullptr)
    {
        setError("xrCreateSwapchainAndroidSurfaceKHR returned a null Surface");
        return false;
    }
    mVideoSurface = env->NewGlobalRef(surface);
    LOGI("android surface swapchain %dx%d created", mSwapchainWidth, mSwapchainHeight);
    return true;
}

XrAction XrGoggleSession::boolAction(const char* name, const char* localized)
{
    XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
    info.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    std::strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(info.localizedActionName, localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    XrAction action = XR_NULL_HANDLE;
    if (!check(xrCreateAction(mActionSet, &info, &action), "xrCreateAction"))
    {
        return XR_NULL_HANDLE;
    }
    return action;
}

bool XrGoggleSession::createActions()
{
    XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strncpy(setInfo.actionSetName, "goggle", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    std::strncpy(setInfo.localizedActionSetName, "Goggle", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    setInfo.priority = 0;
    if (!check(xrCreateActionSet(mInstance, &setInfo, &mActionSet), "xrCreateActionSet"))
    {
        return false;
    }

    mActionRecenter    = boolAction("recenter", "Recenter view");
    mActionPassthrough = boolAction("passthrough", "Toggle passthrough");
    mActionRecord      = boolAction("record", "Toggle recording");
    mActionLockMode    = boolAction("lock_mode", "Toggle head lock");

    XrActionCreateInfo stick{XR_TYPE_ACTION_CREATE_INFO};
    stick.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
    std::strncpy(stick.actionName, "size", XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(stick.localizedActionName, "Screen size", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    if (!check(xrCreateAction(mActionSet, &stick, &mActionSizeStick), "xrCreateAction(size)"))
    {
        return false;
    }
    std::strncpy(stick.actionName, "place", XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(stick.localizedActionName, "Screen distance", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    if (!check(xrCreateAction(mActionSet, &stick, &mActionPlaceStick), "xrCreateAction(place)"))
    {
        return false;
    }

    XrActionCreateInfo haptic{XR_TYPE_ACTION_CREATE_INFO};
    haptic.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
    std::strncpy(haptic.actionName, "haptic", XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(haptic.localizedActionName, "Haptic feedback", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    if (!check(xrCreateAction(mActionSet, &haptic, &mActionHaptic), "xrCreateAction(haptic)"))
    {
        return false;
    }

    // Touch controller: A/B live on the right hand, X/Y on the left.
    const XrActionSuggestedBinding touchBindings[] = {
        {mActionRecenter, path("/user/hand/right/input/a/click")},
        {mActionPassthrough, path("/user/hand/right/input/b/click")},
        {mActionRecord, path("/user/hand/left/input/x/click")},
        {mActionLockMode, path("/user/hand/left/input/y/click")},
        {mActionSizeStick, path("/user/hand/right/input/thumbstick")},
        {mActionPlaceStick, path("/user/hand/left/input/thumbstick")},
        {mActionHaptic, path("/user/hand/left/output/haptic")},
        {mActionHaptic, path("/user/hand/right/output/haptic")},
    };
    XrInteractionProfileSuggestedBinding touch{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    touch.interactionProfile = path("/interaction_profiles/oculus/touch_controller");
    touch.suggestedBindings  = touchBindings;
    touch.countSuggestedBindings = (uint32_t) (sizeof(touchBindings) / sizeof(touchBindings[0]));
    if (XR_FAILED(xrSuggestInteractionProfileBindings(mInstance, &touch)))
    {
        LOGW("touch controller bindings rejected, controls may be unavailable");
    }

    // Bare minimum so something still works on a runtime without Touch controllers.
    const XrActionSuggestedBinding simpleBindings[] = {
        {mActionRecenter, path("/user/hand/right/input/select/click")},
        {mActionPassthrough, path("/user/hand/left/input/select/click")},
        {mActionHaptic, path("/user/hand/left/output/haptic")},
        {mActionHaptic, path("/user/hand/right/output/haptic")},
    };
    XrInteractionProfileSuggestedBinding simple{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    simple.interactionProfile = path("/interaction_profiles/khr/simple_controller");
    simple.suggestedBindings  = simpleBindings;
    simple.countSuggestedBindings = (uint32_t) (sizeof(simpleBindings) / sizeof(simpleBindings[0]));
    if (XR_FAILED(xrSuggestInteractionProfileBindings(mInstance, &simple)))
    {
        LOGW("simple controller bindings rejected");
    }

    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets      = &mActionSet;
    return check(xrAttachSessionActionSets(mSession, &attach), "xrAttachSessionActionSets");
}

bool XrGoggleSession::createPassthrough()
{
    XrPassthroughCreateInfoFB info{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
    info.flags = 0;
    if (!check(mXrCreatePassthroughFB(mSession, &info, &mPassthrough), "xrCreatePassthroughFB"))
    {
        return false;
    }

    XrPassthroughLayerCreateInfoFB layerInfo{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
    layerInfo.passthrough = mPassthrough;
    layerInfo.purpose     = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
    layerInfo.flags       = 0;
    if (!check(mXrCreatePassthroughLayerFB(mSession, &layerInfo, &mPassthroughLayer),
               "xrCreatePassthroughLayerFB"))
    {
        return false;
    }

    if (mXrPassthroughLayerSetStyleFB != nullptr)
    {
        XrPassthroughStyleFB style{XR_TYPE_PASSTHROUGH_STYLE_FB};
        style.textureOpacityFactor = 1.0f;
        style.edgeColor            = XrColor4f{0.0f, 0.0f, 0.0f, 0.0f};
        mXrPassthroughLayerSetStyleFB(mPassthroughLayer, &style);
    }
    return true;
}

// ---------------------------------------------------------------------------------
// settings
// ---------------------------------------------------------------------------------

void XrGoggleSession::setSwapchainSize(int width, int height)
{
    if (width >= 320 && height >= 240 && width <= 4096 && height <= 4096)
    {
        mSwapchainWidth  = width;
        mSwapchainHeight = height;
        mVideoWidth      = width;
        mVideoHeight     = height;
    }
}

void XrGoggleSession::setVideoResolution(int width, int height)
{
    if (width > 0 && height > 0)
    {
        mVideoWidth  = width;
        mVideoHeight = height;
    }
}

void XrGoggleSession::setQuadDistance(float meters)
{
    mQuadDistance = clampf(meters, kMinDistance, kMaxDistance);
}

void XrGoggleSession::setQuadWidth(float meters)
{
    mQuadWidth = clampf(meters, kMinWidth, kMaxWidth);
}

void XrGoggleSession::setPassthrough(bool enabled)
{
    mPassthroughWanted = enabled;
}

void XrGoggleSession::setSharpening(bool enabled)
{
    mSharpening = enabled;
}

void XrGoggleSession::setHeadLocked(bool enabled)
{
    mHeadLocked = enabled;
    if (!enabled)
    {
        // Anchor where the pilot is looking right now instead of at the origin.
        mRecenterRequested = true;
    }
}

void XrGoggleSession::requestRefreshRate(float hz)
{
    mWantedRefreshRate = hz;
}

std::vector<float> XrGoggleSession::refreshRates()
{
    return mRefreshRates;
}

void XrGoggleSession::requestHaptic(float amplitude, int durationMs)
{
    mPendingHapticAmplitude = clampf(amplitude, 0.0f, 1.0f);
    mPendingHapticMs        = durationMs < 1 ? 1 : durationMs;
}

// ---------------------------------------------------------------------------------
// frame loop
// ---------------------------------------------------------------------------------

void XrGoggleSession::runLoop(JNIEnv* env, jobject listener)
{
    bool exitLoop = false;
    while (!exitLoop && !mStopRequested)
    {
        pollEvents(env, listener, &exitLoop);
        if (exitLoop) break;

        if (!mSessionRunning)
        {
            // Idle without burning a core while the runtime has us backgrounded.
            struct timespec ts = {0, 20 * 1000 * 1000};
            nanosleep(&ts, nullptr);
            continue;
        }

        applyPendingRefreshRate();
        syncActions(env, listener);
        applyPassthroughState();
        renderFrame(env, listener);
    }

    if (mSessionRunning && mSession != XR_NULL_HANDLE)
    {
        xrRequestExitSession(mSession);
        // Drain until the runtime actually stops the session, so teardown is clean.
        for (int i = 0; i < 200 && mSessionRunning; ++i)
        {
            bool ignored = false;
            pollEvents(env, listener, &ignored);
            struct timespec ts = {0, 10 * 1000 * 1000};
            nanosleep(&ts, nullptr);
        }
    }
}

void XrGoggleSession::pollEvents(JNIEnv* env, jobject listener, bool* exitLoop)
{
    (void) env;
    (void) listener;
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (true)
    {
        event.type = XR_TYPE_EVENT_DATA_BUFFER;
        XrResult r = xrPollEvent(mInstance, &event);
        if (r != XR_SUCCESS) break;

        switch (event.type)
        {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
            {
                const auto* changed = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
                mSessionState       = changed->state;
                LOGI("session state -> %d", (int) mSessionState);
                if (mSessionState == XR_SESSION_STATE_READY)
                {
                    XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                    begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    if (check(xrBeginSession(mSession, &begin), "xrBeginSession"))
                    {
                        mSessionRunning = true;
                    }
                }
                else if (mSessionState == XR_SESSION_STATE_STOPPING)
                {
                    mSessionRunning = false;
                    xrEndSession(mSession);
                }
                else if (mSessionState == XR_SESSION_STATE_EXITING ||
                         mSessionState == XR_SESSION_STATE_LOSS_PENDING)
                {
                    mSessionRunning = false;
                    *exitLoop       = true;
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            {
                LOGW("instance loss pending");
                mSessionRunning = false;
                *exitLoop       = true;
                break;
            }
            case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
            {
                // The runtime recentered underneath us; re-anchor on the next frame.
                if (!mHeadLocked) mRecenterRequested = true;
                break;
            }
            default:
                break;
        }
    }
}

void XrGoggleSession::applyPendingRefreshRate()
{
    if (!mHasRefreshRate) return;
    const float wanted = mWantedRefreshRate.load();
    if (wanted <= 0.0f || std::fabs(wanted - mAppliedRefreshRate.load()) < 0.01f) return;

    // Only ask for a rate the runtime actually offers, otherwise it returns an error.
    bool supported = mRefreshRates.empty();
    for (float hz : mRefreshRates)
    {
        if (std::fabs(hz - wanted) < 0.51f)
        {
            supported = true;
            break;
        }
    }
    if (!supported)
    {
        LOGW("refresh rate %.1f Hz not offered by the runtime", wanted);
        mAppliedRefreshRate = wanted;
        return;
    }
    if (XR_SUCCEEDED(mXrRequestDisplayRefreshRateFB(mSession, wanted)))
    {
        LOGI("display refresh rate set to %.1f Hz", wanted);
    }
    else
    {
        LOGW("xrRequestDisplayRefreshRateFB(%.1f) failed", wanted);
    }
    mAppliedRefreshRate = wanted;
}

// Passthrough keeps the cameras and reconstruction running, so only pay for it while it
// is on screen.
void XrGoggleSession::applyPassthroughState()
{
    if (!mHasPassthrough || mPassthrough == XR_NULL_HANDLE) return;
    const bool wanted = mPassthroughWanted.load();
    if (wanted == mPassthroughRunning) return;

    if (wanted)
    {
        if (mXrPassthroughStartFB == nullptr) return;
        if (!XR_SUCCEEDED(mXrPassthroughStartFB(mPassthrough)))
        {
            LOGW("xrPassthroughStartFB failed");
            mPassthroughWanted = false;
            return;
        }
        if (mXrPassthroughLayerResumeFB != nullptr)
        {
            mXrPassthroughLayerResumeFB(mPassthroughLayer);
        }
        mPassthroughRunning = true;
        LOGI("passthrough started");
    }
    else
    {
        if (mXrPassthroughLayerPauseFB != nullptr)
        {
            mXrPassthroughLayerPauseFB(mPassthroughLayer);
        }
        if (mXrPassthroughPauseFB != nullptr)
        {
            mXrPassthroughPauseFB(mPassthrough);
        }
        mPassthroughRunning = false;
        LOGI("passthrough paused");
    }
}

void XrGoggleSession::applyPendingHaptic()
{
    const float amplitude = mPendingHapticAmplitude.exchange(0.0f);
    const int   ms        = mPendingHapticMs.exchange(0);
    if (amplitude <= 0.0f || ms <= 0 || mActionHaptic == XR_NULL_HANDLE) return;

    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
    vibration.amplitude = amplitude;
    vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
    vibration.duration  = (XrDuration) ms * 1000000LL;

    XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
    info.action        = mActionHaptic;
    info.subactionPath = XR_NULL_PATH;
    xrApplyHapticFeedback(mSession, &info, (const XrHapticBaseHeader*) &vibration);
}

void XrGoggleSession::syncActions(JNIEnv* env, jobject listener)
{
    if (mActionSet == XR_NULL_HANDLE) return;

    XrActiveActionSet active{mActionSet, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets      = &active;
    if (XR_FAILED(xrSyncActions(mSession, &sync))) return;

    jclass    cls    = env->GetObjectClass(listener);
    jmethodID onButton = env->GetMethodID(cls, "onXrButton", "(I)V");

    struct
    {
        XrAction action;
        int      button;
    } buttons[] = {
        {mActionRecenter, BUTTON_RECENTER},
        {mActionPassthrough, BUTTON_PASSTHROUGH},
        {mActionRecord, BUTTON_RECORD},
        {mActionLockMode, BUTTON_LOCK_MODE},
    };

    for (const auto& b : buttons)
    {
        if (b.action == XR_NULL_HANDLE) continue;
        XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
        get.action = b.action;
        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        if (XR_FAILED(xrGetActionStateBoolean(mSession, &get, &state))) continue;
        // Rising edge only.
        if (state.isActive && state.changedSinceLastSync && state.currentState)
        {
            switch (b.button)
            {
                case BUTTON_RECENTER:
                    mRecenterRequested = true;
                    requestHaptic(0.4f, 30);
                    break;
                case BUTTON_PASSTHROUGH:
                    mPassthroughWanted = !mPassthroughWanted.load();
                    requestHaptic(0.4f, 30);
                    break;
                case BUTTON_LOCK_MODE:
                    setHeadLocked(!mHeadLocked.load());
                    requestHaptic(0.4f, 30);
                    break;
                default:
                    break;
            }
            if (onButton != nullptr)
            {
                env->CallVoidMethod(listener, onButton, (jint) b.button);
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
        }
    }

    const float dt = 1.0f / 72.0f;  // good enough for a manual adjustment ramp

    XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
    XrActionStateVector2f stickState{XR_TYPE_ACTION_STATE_VECTOR2F};
    if (mActionSizeStick != XR_NULL_HANDLE)
    {
        get.action = mActionSizeStick;
        stickState = XrActionStateVector2f{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(xrGetActionStateVector2f(mSession, &get, &stickState)) && stickState.isActive)
        {
            const float y = stickState.currentState.y;
            if (std::fabs(y) > kStickDeadzone)
            {
                setQuadWidth(mQuadWidth.load() + y * kWidthPerSec * dt);
            }
        }
    }
    if (mActionPlaceStick != XR_NULL_HANDLE)
    {
        get.action = mActionPlaceStick;
        stickState = XrActionStateVector2f{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(xrGetActionStateVector2f(mSession, &get, &stickState)) && stickState.isActive)
        {
            const float y = stickState.currentState.y;
            if (std::fabs(y) > kStickDeadzone)
            {
                setQuadDistance(mQuadDistance.load() - y * kDistancePerSec * dt);
            }
        }
    }
}

void XrGoggleSession::recenterAt(XrTime time)
{
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(mViewSpace, mLocalSpace, time, &location)))
    {
        return;
    }
    if ((location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) == 0 ||
        (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) == 0)
    {
        return;
    }

    const float yaw      = yawOf(location.pose.orientation);
    const float distance = mQuadDistance.load();
    mAnchorPose.orientation = quatFromYaw(yaw);
    mAnchorPose.position    = XrVector3f{location.pose.position.x - std::sin(yaw) * distance,
                                      location.pose.position.y,
                                      location.pose.position.z - std::cos(yaw) * distance};
    mAnchorValid = true;
}

void XrGoggleSession::renderFrame(JNIEnv* env, jobject listener)
{
    (void) env;
    (void) listener;

    XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState    frameState{XR_TYPE_FRAME_STATE};
    if (XR_FAILED(xrWaitFrame(mSession, &waitInfo, &frameState))) return;

    XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    if (XR_FAILED(xrBeginFrame(mSession, &beginInfo))) return;

    applyPendingHaptic();

    if (mRecenterRequested.exchange(false) || (!mHeadLocked.load() && !mAnchorValid))
    {
        recenterAt(frameState.predictedDisplayTime);
    }

    const bool headLocked = mHeadLocked.load();
    const bool wantPassthrough =
        mHasPassthrough && mPassthroughRunning && mPassthroughWanted.load();

    // Video quad. Width is in meters at the configured distance, height follows the
    // stream's aspect ratio so nothing is stretched.
    const int   vw = mVideoWidth.load();
    const int   vh = mVideoHeight.load();
    const float aspect = (vw > 0 && vh > 0) ? (float) vh / (float) vw : 9.0f / 16.0f;
    const float widthM = mQuadWidth.load();

    XrCompositionLayerSettingsFB layerSettings{XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB};
    layerSettings.layerFlags = XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SHARPENING_BIT_FB;

    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad.next         = (mHasLayerSettings && mSharpening.load()) ? &layerSettings : nullptr;
    quad.layerFlags   = 0;  // opaque: the video has to cover passthrough behind it
    quad.space        = headLocked ? mViewSpace : mLocalSpace;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain       = mSwapchain;
    quad.subImage.imageArrayIndex = 0;
    // Clamp to the swapchain the runtime handed us; MediaCodec resizes the buffers to the
    // stream resolution, which is normally the size this was created with.
    quad.subImage.imageRect.offset = XrOffset2Di{0, 0};
    quad.subImage.imageRect.extent =
        XrExtent2Di{vw > 0 && vw <= mSwapchainWidth ? vw : mSwapchainWidth,
                    vh > 0 && vh <= mSwapchainHeight ? vh : mSwapchainHeight};
    if (headLocked)
    {
        quad.pose = XrPosef{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -mQuadDistance.load()}};
    }
    else
    {
        quad.pose = mAnchorPose;
    }
    quad.size = XrExtent2Df{widthM, widthM * aspect};

    XrCompositionLayerPassthroughFB passthroughLayer{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
    passthroughLayer.flags       = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    passthroughLayer.space       = XR_NULL_HANDLE;
    passthroughLayer.layerHandle = mPassthroughLayer;

    const XrCompositionLayerBaseHeader* layers[2];
    uint32_t                            layerCount = 0;
    if (wantPassthrough)
    {
        layers[layerCount++] = (const XrCompositionLayerBaseHeader*) &passthroughLayer;
    }
    layers[layerCount++] = (const XrCompositionLayerBaseHeader*) &quad;

    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime          = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    // shouldRender false means the runtime wants nothing on screen (e.g. system overlay
    // in front of us); submitting zero layers keeps the frame loop pacing intact.
    endInfo.layerCount = frameState.shouldRender ? layerCount : 0;
    endInfo.layers     = frameState.shouldRender ? layers : nullptr;
    xrEndFrame(mSession, &endInfo);
}

// ---------------------------------------------------------------------------------
// teardown
// ---------------------------------------------------------------------------------

void XrGoggleSession::destroy(JNIEnv* env)
{
    if (mPassthroughLayer != XR_NULL_HANDLE && mXrDestroyPassthroughLayerFB != nullptr)
    {
        mXrDestroyPassthroughLayerFB(mPassthroughLayer);
        mPassthroughLayer = XR_NULL_HANDLE;
    }
    if (mPassthrough != XR_NULL_HANDLE && mXrDestroyPassthroughFB != nullptr)
    {
        if (mPassthroughRunning && mXrPassthroughPauseFB != nullptr)
        {
            mXrPassthroughPauseFB(mPassthrough);
        }
        mXrDestroyPassthroughFB(mPassthrough);
        mPassthrough        = XR_NULL_HANDLE;
        mPassthroughRunning = false;
    }
    if (mActionSet != XR_NULL_HANDLE)
    {
        xrDestroyActionSet(mActionSet);
        mActionSet = XR_NULL_HANDLE;
    }
    if (mSwapchain != XR_NULL_HANDLE)
    {
        xrDestroySwapchain(mSwapchain);
        mSwapchain = XR_NULL_HANDLE;
    }
    if (mVideoSurface != nullptr)
    {
        env->DeleteGlobalRef(mVideoSurface);
        mVideoSurface = nullptr;
    }
    if (mViewSpace != XR_NULL_HANDLE)
    {
        xrDestroySpace(mViewSpace);
        mViewSpace = XR_NULL_HANDLE;
    }
    if (mLocalSpace != XR_NULL_HANDLE)
    {
        xrDestroySpace(mLocalSpace);
        mLocalSpace = XR_NULL_HANDLE;
    }
    if (mSession != XR_NULL_HANDLE)
    {
        xrDestroySession(mSession);
        mSession = XR_NULL_HANDLE;
    }
    if (mEglDisplay != EGL_NO_DISPLAY)
    {
        eglMakeCurrent(mEglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (mEglSurface != EGL_NO_SURFACE) eglDestroySurface(mEglDisplay, mEglSurface);
        if (mEglContext != EGL_NO_CONTEXT) eglDestroyContext(mEglDisplay, mEglContext);
        eglTerminate(mEglDisplay);
        mEglSurface = EGL_NO_SURFACE;
        mEglContext = EGL_NO_CONTEXT;
        mEglDisplay = EGL_NO_DISPLAY;
    }
    if (mInstance != XR_NULL_HANDLE)
    {
        xrDestroyInstance(mInstance);
        mInstance = XR_NULL_HANDLE;
    }
}
