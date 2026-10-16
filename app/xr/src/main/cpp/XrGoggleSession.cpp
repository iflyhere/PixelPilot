#include "XrGoggleSession.h"

#include <android/log.h>
#include <stdlib.h>
#include <time.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

#define TAG "pixelpilot-xr"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace
{
// A hand gesture has to be held this long before it counts. Grabbing a drone or a radio
// squeezes the hand for a moment; deliberately holding a pinch does not happen by accident.
constexpr int64_t kHandHoldNs    = 700LL * 1000 * 1000;
constexpr float kMinDistance     = xri::kMinVideoDistance;
constexpr float kMaxDistance     = xri::kMaxVideoDistance;
constexpr float kMinWidth        = xri::kMinVideoWidth;
constexpr float kMaxWidth        = xri::kMaxVideoWidth;
constexpr float kMaxHeightOffset = xri::kMaxVideoHeight;
// One nudge, about 1.8 degrees at the default distance - small enough to fine-tune.
constexpr float kHeightStep      = 0.05f;

using xri::clampf;

float yawOf(const XrQuaternionf& q)
{
    return std::atan2(2.0f * (q.w * q.y + q.x * q.z), 1.0f - 2.0f * (q.y * q.y + q.z * q.z));
}

// The two vector libraries have the same layout; these only change the type.
xri::Vec3 toXri(const XrVector3f& v) { return {v.x, v.y, v.z}; }
xri::Quat toXri(const XrQuaternionf& q) { return {q.x, q.y, q.z, q.w}; }
xri::Pose toXri(const XrPosef& p) { return {toXri(p.orientation), toXri(p.position)}; }
XrVector3f toXr(const xri::Vec3& v) { return XrVector3f{v.x, v.y, v.z}; }
XrQuaternionf toXr(const xri::Quat& q) { return XrQuaternionf{q.x, q.y, q.z, q.w}; }
XrPosef toXr(const xri::Pose& p) { return XrPosef{toXr(p.orientation), toXr(p.position)}; }
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
    if (!resolveRuntime()) return false;
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

// ---------------------------------------------------------------------------------
// runtime discovery
// ---------------------------------------------------------------------------------

namespace
{
#if defined(__aarch64__)
constexpr const char* kAbi = "arm64-v8a";
#elif defined(__arm__)
constexpr const char* kAbi = "armeabi-v7a";
#elif defined(__x86_64__)
constexpr const char* kAbi = "x86_64";
#else
constexpr const char* kAbi = "x86";
#endif
}  // namespace

// True when a real runtime answered - a runtime-provided extension has to be in the list,
// not just "the call did not fail".
bool XrGoggleSession::runtimeAnswers()
{
    uint32_t count = 0;
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr)) || count == 0)
    {
        return false;
    }
    std::vector<XrExtensionProperties> props(count, {XR_TYPE_EXTENSION_PROPERTIES});
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, props.data())))
    {
        return false;
    }
    for (const auto& p : props)
    {
        if (std::strcmp(p.extensionName, XR_KHR_ANDROID_SURFACE_SWAPCHAIN_EXTENSION_NAME) == 0)
        {
            return true;
        }
    }
    return false;
}

// Drops a runtime manifest next to our files and points the loader at it.
bool XrGoggleSession::pointLoaderAt(const char* libraryPath)
{
    if (mManifestDir.empty()) return false;
    const std::string manifest = mManifestDir + "/openxr_runtime.json";
    {
        std::ofstream out(manifest, std::ios::trunc);
        if (!out) return false;
        out << "{\n"
            << "  \"file_format_version\": \"1.0.0\",\n"
            << "  \"runtime\": {\n"
            << "    \"name\": \"Android system OpenXR runtime\",\n"
            << "    \"library_path\": \"" << libraryPath << "\"\n"
            << "  }\n"
            << "}\n";
    }
    setenv("XR_RUNTIME_JSON", manifest.c_str(), 1);
    return true;
}

/*
 * Fallback for a headset where the loader cannot find a runtime by itself.
 *
 * On a Quest 3 this is not needed: the Khronos loader resolves the Horizon runtime through
 * the org.khronos.openxr.runtime_broker content provider and the first candidate ("change
 * nothing") wins. Querying that provider from `adb shell` returns no rows, but only because
 * the shell does not hold org.khronos.openxr.permission.OPENXR - the app does.
 *
 * It is kept because the loader has exactly three discovery paths - the broker,
 * active_runtime.json on disk, and XR_RUNTIME_JSON - and a device that offers neither of the
 * first two would otherwise be dead in the water. Where that happens the runtime is still
 * reachable as libopenxr_forwardloader.so from the VrDriver APEX: that library sits on every
 * app's linker search path and exports xrNegotiateLoaderRuntimeInterface, so as far as a
 * loader is concerned it *is* the runtime.
 */
bool XrGoggleSession::resolveRuntime()
{
    if (runtimeAnswers())
    {
        LOGI("runtime found without an override");
        mRuntimeLibrary = "<system>";
        return true;
    }

    const std::string apkPath = std::string("/apex/com.meta.xr/priv-app/VrDriver/VrDriver.apk!/lib/") +
                                kAbi + "/libopenxr_forwardloader.so";
    const char* candidates[] = {
        // A bare file name means "use the system library search path", which is where the
        // APEX library actually lives for us.
        "libopenxr_forwardloader.so",
        // Some loaders insist on an absolute path; Android's linker understands apk!/entry.
        apkPath.c_str(),
    };

    for (const char* candidate : candidates)
    {
        if (!pointLoaderAt(candidate))
        {
            LOGW("could not write a runtime manifest into %s", mManifestDir.c_str());
            break;
        }
        LOGI("trying runtime library_path=%s", candidate);
        if (runtimeAnswers())
        {
            LOGI("runtime resolved via %s", candidate);
            mRuntimeLibrary = candidate;
            return true;
        }
    }

    unsetenv("XR_RUNTIME_JSON");
    setError(
        "No OpenXR runtime answered. Tried the system default, then "
        "libopenxr_forwardloader.so by name and by absolute APEX path.");
    return false;
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
    // Worth the log lines: it is the only way to see from a bug report what the runtime on
    // a given headset actually offers.
    LOGI("runtime exposes %zu instance extensions:", mAvailableExtensions.size());
    for (const auto& name : mAvailableExtensions)
    {
        LOGI("  %s", name.c_str());
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
    // Lets the same actions be driven by a pinch when flying without controllers.
    mHasHandInteraction = extensionSupported(XR_EXT_HAND_INTERACTION_EXTENSION_NAME);
    // Needed to correct the origin of an Android-produced image, see renderFrame().
    mHasImageLayout = extensionSupported(XR_FB_COMPOSITION_LAYER_IMAGE_LAYOUT_EXTENSION_NAME);
    // Lets the dashboard wrap around the pilot instead of being a flat card. The curve is
    // itself a depth cue - it gives parallax across the width - and without the extension the
    // same layer is submitted as a plain quad, which only looks flatter.
    mHasCylinder = extensionSupported(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
    // Thumb swipes along the index finger: a discrete control that does not occupy the
    // hand, so pinch and grasp stay free for the flight actions.
    mHasMicrogestures = mHasHandInteraction &&
                        extensionSupported(XR_META_HAND_TRACKING_MICROGESTURES_EXTENSION_NAME);
    // Without this only one interaction profile is live at a time: with hands tracked the
    // controller bindings never fire, which looks exactly like "the stick is broken".
    mHasSimultaneous =
        extensionSupported(XR_META_SIMULTANEOUS_HANDS_AND_CONTROLLERS_EXTENSION_NAME);
    if (mHasPassthrough) enabled.push_back(XR_FB_PASSTHROUGH_EXTENSION_NAME);
    if (mHasLayerSettings) enabled.push_back(XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME);
    if (mHasRefreshRate) enabled.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    if (mHasHandInteraction) enabled.push_back(XR_EXT_HAND_INTERACTION_EXTENSION_NAME);
    if (mHasImageLayout) enabled.push_back(XR_FB_COMPOSITION_LAYER_IMAGE_LAYOUT_EXTENSION_NAME);
    if (mHasCylinder) enabled.push_back(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
    if (mHasMicrogestures) enabled.push_back(XR_META_HAND_TRACKING_MICROGESTURES_EXTENSION_NAME);
    if (mHasSimultaneous)
        enabled.push_back(XR_META_SIMULTANEOUS_HANDS_AND_CONTROLLERS_EXTENSION_NAME);
    LOGI(
        "optional extensions: passthrough=%d layerSettings=%d refreshRate=%d handInteraction=%d "
        "imageLayout=%d microgestures=%d simultaneousHandsControllers=%d cylinder=%d",
        (int) mHasPassthrough,
        (int) mHasLayerSettings,
        (int) mHasRefreshRate,
        (int) mHasHandInteraction,
        (int) mHasImageLayout,
        (int) mHasMicrogestures,
        (int) mHasSimultaneous,
        (int) mHasCylinder);

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
    if (mHasSimultaneous)
    {
        xrGetInstanceProcAddr(mInstance,
                              "xrResumeSimultaneousHandsAndControllersTrackingMETA",
                              (PFN_xrVoidFunction*) &mXrResumeSimultaneous);
        mHasSimultaneous = mXrResumeSimultaneous != nullptr;
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

/*
 * MediaCodec is the producer for an Android surface swapchain, so the image description
 * fields are not really ours to choose. The extension documents them as ignored, but Meta's
 * runtime validates them and wants them zeroed: passing the "obvious" 1s gets
 * XR_ERROR_VALIDATION_FAILURE on a Quest 3 (Horizon OS, Android 14), while all-zero is
 * accepted. The zeroed shape is therefore first; the rest stay as a fallback for a runtime
 * that wants something else, and the accepted one is logged.
 */
bool XrGoggleSession::createSurfaceSwapchain(
    JNIEnv* env, int width, int height, const char* what, XrSwapchain* outSwapchain, jobject* outSurface)
{
    // GL_RGBA8, for the variant where the runtime wants a real format.
    constexpr int64_t kGlRgba8 = 0x8058;

    struct Variant
    {
        const char*          what;
        XrSwapchainUsageFlags usage;
        int64_t              format;
        uint32_t             counts;  // sampleCount / faceCount / arraySize / mipCount
    };
    const Variant variants[] = {
        {"ignored fields zeroed", XR_SWAPCHAIN_USAGE_SAMPLED_BIT, 0, 0},
        {"ignored fields set to 1", XR_SWAPCHAIN_USAGE_SAMPLED_BIT, 0, 1},
        {"zeroed + color attachment",
         XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT,
         0,
         0},
        {"zeroed + explicit RGBA8", XR_SWAPCHAIN_USAGE_SAMPLED_BIT, kGlRgba8, 0},
        {"1s + explicit RGBA8", XR_SWAPCHAIN_USAGE_SAMPLED_BIT, kGlRgba8, 1},
    };

    jobject  surface = nullptr;
    XrResult last    = XR_ERROR_VALIDATION_FAILURE;
    for (const auto& variant : variants)
    {
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags  = variant.usage;
        info.format      = variant.format;
        info.sampleCount = variant.counts;
        info.width       = (uint32_t) width;
        info.height      = (uint32_t) height;
        info.faceCount   = variant.counts;
        info.arraySize   = variant.counts;
        info.mipCount    = variant.counts;

        surface = nullptr;
        last    = mXrCreateSwapchainAndroidSurfaceKHR(mSession, &info, outSwapchain, &surface);
        if (XR_SUCCEEDED(last) && surface != nullptr)
        {
            LOGI("%s surface swapchain %dx%d created (%s)", what, width, height, variant.what);
            *outSurface = env->NewGlobalRef(surface);
            return true;
        }
        char name[XR_MAX_RESULT_STRING_SIZE] = {0};
        xrResultToString(mInstance, last, name);
        LOGW("%s swapchain variant %s rejected: %s", what, variant.what, name);
        if (XR_SUCCEEDED(last) && *outSwapchain != XR_NULL_HANDLE)
        {
            // Succeeded but handed back no Surface - not usable, do not leak it.
            xrDestroySwapchain(*outSwapchain);
            *outSwapchain = XR_NULL_HANDLE;
        }
    }

    // Nothing worked. Dump what the runtime does offer so the next attempt is informed.
    uint32_t formatCount = 0;
    if (XR_SUCCEEDED(xrEnumerateSwapchainFormats(mSession, 0, &formatCount, nullptr)) &&
        formatCount > 0)
    {
        std::vector<int64_t> formats(formatCount);
        if (XR_SUCCEEDED(
                xrEnumerateSwapchainFormats(mSession, formatCount, &formatCount, formats.data())))
        {
            for (int64_t f : formats)
            {
                LOGI("runtime swapchain format 0x%llx", (unsigned long long) f);
            }
        }
    }
    return check(last, "xrCreateSwapchainAndroidSurfaceKHR");
}

/*
 * Where each overlay sits: xri::defaultLayout(), the same table the host tests read.
 *
 * The pixel sizes are chosen for the angle each layer subtends rather than for their own sake:
 * a layer that covers twice the angle needs twice the pixels to read equally sharp.
 */
void XrGoggleSession::describeOverlays()
{
    xri::Placement layout[OVERLAY_COUNT];
    xri::defaultLayout(layout);
    for (int i = 0; i < OVERLAY_COUNT; ++i)
    {
        OverlayLayer& o = mOverlays[i];
        o.place         = layout[i];
        o.name          = xri::layerName(i);
        o.width         = layout[i].pixelWidth;
        o.height        = layout[i].pixelHeight;
    }

    // Printed in full because the off-headset preview test carries its own copy of these
    // numbers - it cannot ask for them without bringing a session up, and a preview that
    // quietly disagrees with the real arrangement is worse than no preview. One grep for
    // "layout:" says whether the two still match.
    for (int i = 0; i < OVERLAY_COUNT; ++i)
    {
        const xri::Placement& p = mOverlays[i].place;
        char arc[32] = "";
        if (p.cylinder) snprintf(arc, sizeof(arc), " cylinder arc %.0f", p.centralAngleDeg);
        LOGI("layout: %-10s %4dx%-4d yaw %6.1f pitch %6.1f tilt %5.1f dist %5.2f width %5.2f "
             "aspect %4.2f%s",
             mOverlays[i].name, p.pixelWidth, p.pixelHeight, p.yawDeg, p.pitchDeg, p.tiltDeg,
             p.distance, p.widthM, p.aspect, arc);
    }
}

void XrGoggleSession::currentPlacements(xri::Placement out[OVERLAY_COUNT]) const
{
    for (int i = 0; i < OVERLAY_COUNT; ++i)
    {
        out[i]         = mOverlays[i].place;
        out[i].present = mOverlays[i].swapchain != XR_NULL_HANDLE && mOverlays[i].visible.load();
    }
}

void XrGoggleSession::applyHapticTo(int hand, float amplitude, int ms)
{
    if (mActionHaptic == XR_NULL_HANDLE || hand < 0 || hand > 1 || ms <= 0) return;
    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
    vibration.amplitude = clampf(amplitude, 0.0f, 1.0f);
    vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
    vibration.duration  = (XrDuration) ms * 1000000LL;

    // On the hand that did it, not on both: a tick in the wrong hand reads as something else.
    XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
    info.action        = mActionHaptic;
    info.subactionPath = mHandPath[hand];
    xrApplyHapticFeedback(mSession, &info, (const XrHapticBaseHeader*) &vibration);
}

void XrGoggleSession::updatePointer(JNIEnv* env, jobject listener, const xri::Anchor& anchor,
                                    xri::Video* video, XrSpace layoutSpace, XrTime time,
                                    float dt)
{
    // The head, in a space that knows which way is down - the hand checks need that.
    xri::HeadPose head;
    head.forward = xri::Vec3{0.0f, 0.0f, -1.0f};
    static constexpr XrSpaceLocationFlags kValid =
        XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    static constexpr XrSpaceLocationFlags kTracked =
        kValid | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    {
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(xrLocateSpace(mViewSpace, mLocalSpace, time, &loc)) &&
            (loc.locationFlags & kValid) == kValid)
        {
            head.position = toXri(loc.pose.position);
            head.forward  = xri::rotate(toXri(loc.pose.orientation), xri::Vec3{0.0f, 0.0f, -1.0f});
        }
    }
    // The eye in layout space, which the beams and cursors are turned towards. Head-locked,
    // the layout space is the view itself.
    const xri::Vec3 eye = layoutSpace == mViewSpace ? xri::Vec3{} : head.position;

    xri::HandInput hands[2];
    xri::HandPose  free[2];
    for (int i = 0; i < 2; ++i)
    {
        if (mAimSpace[i] == XR_NULL_HANDLE) continue;
        // A controller may coast on inertia for a moment and still point; a hand that is only
        // being guessed at may not - that is exactly when a recogniser invents gestures.
        const XrSpaceLocationFlags needed = mSideIsHand[i] ? kTracked : kValid;
        XrSpaceLocation            loc{XR_TYPE_SPACE_LOCATION};
        if (XR_FAILED(xrLocateSpace(mAimSpace[i], layoutSpace, time, &loc)) ||
            (loc.locationFlags & needed) != needed)
        {
            continue;
        }
        hands[i].present      = true;
        hands[i].isHand       = mSideIsHand[i];
        hands[i].aim          = toXri(loc.pose);
        hands[i].select       = mSelect[i];
        hands[i].touching     = mHasTouchSensors ? mTouch[i] : true;
        hands[i].holdProgress = mHoldProgress[i];
        if (mSideIsHand[i])
        {
            XrSpaceLocation inLocal{XR_TYPE_SPACE_LOCATION};
            if (XR_SUCCEEDED(xrLocateSpace(mAimSpace[i], mLocalSpace, time, &inLocal)) &&
                (inLocal.locationFlags & kTracked) == kTracked)
            {
                free[i].tracked   = true;
                free[i].position  = toXri(inLocal.pose.position);
                free[i].direction =
                    xri::rotate(toXri(inLocal.pose.orientation), xri::Vec3{0.0f, 0.0f, -1.0f});
            }
        }
    }

    // Which hands may act. A controller always may; a hand only once it is up and away from
    // the other one, which is not how anyone holds a transmitter. Logged on change, so a test
    // flight says why a gesture did or did not count.
    for (int i = 0; i < 2; ++i)
    {
        xri::HandBlock block = hands[i].present ? xri::HAND_FREE : xri::HAND_NOT_TRACKED;
        if (hands[i].present && hands[i].isHand)
        {
            block             = xri::handBlock(free[i], free[1 - i], head);
            hands[i].eligible = block == xri::HAND_FREE;
            if (block != mHandBlock[i])
            {
                const float below = head.position.y - free[i].position.y;
                const float dx    = free[i].position.x - head.position.x;
                const float dz    = free[i].position.z - head.position.z;
                const float apart = free[1 - i].tracked
                                        ? xri::length(free[i].position - free[1 - i].position)
                                        : -1.0f;
                LOGI("%s hand %s (%.2f m below the eyes, %.2f m out from the head, %.2f m from "
                     "the other hand)",
                     i == 0 ? "left" : "right", xri::handBlockName(block), below,
                     std::sqrt(dx * dx + dz * dz), apart);
            }
        }
        mHandBlock[i] = block;
    }

    xri::Settings settings;
    settings.pointing = mDragEnabled.load();
    settings.handDrag = mHandDragEnabled.load() && mHandDragBound;

    xri::Placement layers[OVERLAY_COUNT];
    currentPlacements(layers);

    bool selectWas[2] = {mPointerSelectWas[0], mPointerSelectWas[1]};
    xri::Events events;
    mInteraction.update(hands, mStickX, mStickY, dt, settings, anchor, eye, layers, OVERLAY_COUNT,
                        video, mPointers, &events);

    // Every press, and what it was aimed at - the one thing a "the drag does not work" report
    // needs, and the old code could not say.
    for (int i = 0; i < 2; ++i)
    {
        if (hands[i].present && hands[i].select && !selectWas[i])
        {
            const bool took = events.grabbed && events.grabHand == i;
            LOGI("%s %s pressed, aimed at %s%s", i == 0 ? "left" : "right",
                 hands[i].isHand ? "hand" : "controller", xri::layerName(mPointers[i].target),
                 took ? " - took hold" : "");
        }
        mPointerSelectWas[i] = hands[i].present && hands[i].select;
    }

    // Copy back what moved. Only the held layer: the others may be written by a restore on
    // another thread, and writing the whole table back every frame would undo it.
    const auto copyBack = [&](int id) {
        if (id < 0 || id >= OVERLAY_COUNT) return;
        xri::Placement& to = mOverlays[id].place;
        to.yawDeg          = layers[id].yawDeg;
        to.pitchDeg        = layers[id].pitchDeg;
        to.tiltDeg         = layers[id].tiltDeg;
        to.distance        = layers[id].distance;
        to.widthM          = layers[id].widthM;
        to.centralAngleDeg = layers[id].centralAngleDeg;
    };
    copyBack(mInteraction.held());
    if (events.released) copyBack(events.releaseTarget);
    if (events.videoChanged)
    {
        setQuadDistance(video->distance);
        setQuadWidth(video->width);
        setQuadHeightOffset(video->height);
    }

    for (int i = 0; i < 2; ++i)
    {
        if (events.haptic[i].ms > 0) applyHapticTo(i, events.haptic[i].amplitude, events.haptic[i].ms);
    }

    if (listener == nullptr) return;
    if (events.released)
    {
        const int id = events.releaseTarget;
        if (id >= 0 && id < OVERLAY_COUNT)
        {
            const xri::Placement& p = mOverlays[id].place;
            // A cylinder's size is its arc; saved as the arc's length, so it restores through
            // the same width field as a flat panel.
            const float width =
                p.cylinder ? (p.distance > 0.0f ? p.distance : video->distance) *
                                 p.centralAngleDeg * xri::kDeg2Rad
                           : p.widthM;
            if (mOnOverlayGrab != nullptr)
            {
                env->CallVoidMethod(listener, mOnOverlayGrab, (jint) id, (jboolean) JNI_FALSE);
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
            // Reported on release rather than every frame: this ends up in SharedPreferences,
            // and a drag is a hundred frames of writes if it is not.
            if (mOnOverlayMoved != nullptr)
            {
                env->CallVoidMethod(listener, mOnOverlayMoved, (jint) id, (jfloat) p.yawDeg,
                                    (jfloat) p.pitchDeg, (jfloat) p.tiltDeg, (jfloat) p.distance,
                                    (jfloat) width);
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
            LOGI("%s placed at yaw %.1f pitch %.1f dist %.2f width %.2f", mOverlays[id].name,
                 p.yawDeg, p.pitchDeg, p.distance, width);
        }
        else if (id == xri::TARGET_VIDEO)
        {
            if (mOnButton != nullptr)
            {
                env->CallVoidMethod(listener, mOnButton, (jint) BUTTON_VIDEO_MOVED);
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
            LOGI("video placed %.2f m away, %.2f m wide, %.2f m up", video->distance,
                 video->width, video->height);
        }
    }
    if (events.grabbed)
    {
        const int id = events.grabTarget;
        if (id >= 0 && id < OVERLAY_COUNT && mOnOverlayGrab != nullptr)
        {
            env->CallVoidMethod(listener, mOnOverlayGrab, (jint) id, (jboolean) JNI_TRUE);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        LOGI("took hold of the %s with the %s %s", xri::layerName(id),
             events.grabHand == 0 ? "left" : "right",
             hands[events.grabHand].isHand ? "hand" : "controller");
    }
    if (events.hoverChanged && mOnOverlayHover != nullptr)
    {
        env->CallVoidMethod(listener, mOnOverlayHover, (jint) events.hovered);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
}

/*
 * Called from whatever thread restores a saved layout, while the frame loop reads the same
 * fields. Left unsynchronised deliberately: these are aligned 32-bit floats, so a reader sees
 * one value or the other and never half of each, and the worst case is one frame of the old
 * arrangement at session start. A lock here would sit in the frame path to protect a write
 * that happens once.
 */
void XrGoggleSession::setOverlayPose(int id, float yawDeg, float pitchDeg, float tiltDeg,
                                     float distance, float widthM)
{
    if (id < 0 || id >= OVERLAY_COUNT || id == OVERLAY_SYMBOLOGY) return;
    xri::Placement& p = mOverlays[id].place;
    if (std::isfinite(yawDeg)) p.yawDeg = clampf(yawDeg, xri::kMinYawDeg, xri::kMaxYawDeg);
    if (std::isfinite(pitchDeg)) p.pitchDeg = clampf(pitchDeg, xri::kMinPitchDeg, xri::kMaxPitchDeg);
    if (std::isfinite(tiltDeg)) p.tiltDeg = clampf(tiltDeg, xri::kMinTiltDeg, xri::kMaxTiltDeg);
    // A non-positive value keeps the built-in one, which is how a layout saved before a
    // field existed still restores cleanly.
    if (std::isfinite(distance) && distance > 0.0f)
    {
        p.distance = clampf(distance, xri::kMinPanelDistance, xri::kMaxPanelDistance);
    }
    if (std::isfinite(widthM) && widthM > 0.0f)
    {
        if (p.cylinder)
        {
            // Saved as the arc's length; the cylinder wants the angle back.
            const float radius = p.distance > 0.0f ? p.distance : mQuadDistance.load();
            p.centralAngleDeg  = clampf(widthM / radius * xri::kRad2Deg, xri::kMinCentralAngleDeg,
                                        xri::kMaxCentralAngleDeg);
        }
        else
        {
            p.widthM = clampf(widthM, xri::kMinPanelWidth, xri::kMaxPanelWidth);
        }
    }
}

bool XrGoggleSession::overlayPose(int id, float* yawDeg, float* pitchDeg, float* tiltDeg,
                                  float* distance, float* widthM) const
{
    if (id < 0 || id >= OVERLAY_COUNT) return false;
    const xri::Placement& p = mOverlays[id].place;
    if (yawDeg != nullptr) *yawDeg = p.yawDeg;
    if (pitchDeg != nullptr) *pitchDeg = p.pitchDeg;
    if (tiltDeg != nullptr) *tiltDeg = p.tiltDeg;
    if (distance != nullptr) *distance = p.distance;
    if (widthM != nullptr) *widthM = p.widthM;
    return true;
}

void XrGoggleSession::resetOverlayLayout()
{
    // describeOverlays() owns the arrangement, so resetting is re-running it. It rewrites
    // the pixel sizes and names as well, which are the values the swapchains were made with
    // anyway.
    describeOverlays();
    mInteraction.reset();
    LOGI("instrument layout reset to the built-in arrangement");
}

bool XrGoggleSession::createSwapchain(JNIEnv* env)
{
    if (!createSurfaceSwapchain(env, mSwapchainWidth, mSwapchainHeight, "video", &mSwapchain, &mVideoSurface))
    {
        return false;
    }

    describeOverlays();
    for (int i = 0; i < OVERLAY_COUNT; ++i)
    {
        OverlayLayer& o = mOverlays[i];
        // Every overlay is a bonus, not a requirement: a runtime that runs out of surface
        // swapchains still flies, just with fewer instruments.
        if (!createSurfaceSwapchain(env, o.width, o.height, o.name, &o.swapchain, &o.surface))
        {
            LOGW("no swapchain for the %s layer - carrying on without it", o.name);
            o.swapchain = XR_NULL_HANDLE;
            o.surface   = nullptr;
        }
    }

    // The pointer's two pictures. Optional like the overlays: without them nothing is drawn
    // where the beam goes, and everything else still works.
    const struct
    {
        int         width;
        int         height;
        const char* name;
    } art[] = {
        {kBeamArtWidth, kBeamArtHeight, "pointer beam"},
        {kCursorArtCell * xri::CURSOR_FRAME_COUNT, kCursorArtCell, "pointer cursor"},
    };
    for (int k = 0; k < SURFACE_COUNT - POINTER_BEAM; ++k)
    {
        PointerArtLayer& a = mPointerArt[k];
        a.width            = art[k].width;
        a.height           = art[k].height;
        if (!createSurfaceSwapchain(env, a.width, a.height, art[k].name, &a.swapchain, &a.surface))
        {
            LOGW("no swapchain for the %s - the pointer will not be drawn", art[k].name);
            a.swapchain = XR_NULL_HANDLE;
            a.surface   = nullptr;
        }
    }
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
    mActionRaise       = boolAction("raise", "Move the screen up");
    mActionLower       = boolAction("lower", "Move the screen down");
    // Separate actions so the hold requirement applies to the hand only - a controller
    // button should still act the moment it is pressed.
    mActionHandRecenter    = boolAction("hand_recenter", "Recenter view (hold)");
    mActionHandPassthrough = boolAction("hand_passthrough", "Toggle passthrough (hold)");
    mActionExit            = boolAction("exit", "Leave VR");

    XrActionCreateInfo stick{XR_TYPE_ACTION_CREATE_INFO};
    stick.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
    std::strncpy(stick.actionName, "place", XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(stick.localizedActionName, "Move the screen", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    if (!check(xrCreateAction(mActionSet, &stick, &mActionStick), "xrCreateAction(place)"))
    {
        return false;
    }

    // Where a controller is pointing, so an instrument can be picked out of the arrangement.
    // Aim rather than grip: aim is the runtime's own pointing ray, already corrected for how
    // a Touch controller sits in the hand, and it is what every other Quest UI points with.
    XrActionCreateInfo aim{XR_TYPE_ACTION_CREATE_INFO};
    aim.actionType = XR_ACTION_TYPE_POSE_INPUT;
    std::strncpy(aim.actionName, "aim", XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(aim.localizedActionName, "Pointing direction", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    mHandPath[0] = path("/user/hand/left");
    mHandPath[1] = path("/user/hand/right");
    const XrPath handPaths[2] = {mHandPath[0], mHandPath[1]};
    aim.countSubactionPaths = 2;
    aim.subactionPaths      = handPaths;
    if (!check(xrCreateAction(mActionSet, &aim, &mActionAim), "xrCreateAction(aim)"))
    {
        return false;
    }

    // The trigger, and nothing else: it takes hold of whatever the beam is on, and aimed at
    // nothing it does nothing. It used to pull the video nearer when it missed, so a near
    // miss on a panel moved the screen instead. Per hand, so the drag follows whichever
    // controller started it.
    XrActionCreateInfo grab{XR_TYPE_ACTION_CREATE_INFO};
    grab.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    std::strncpy(grab.actionName, "grab", XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(grab.localizedActionName, "Grab an instrument", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    grab.countSubactionPaths = 2;
    grab.subactionPaths      = handPaths;
    if (!check(xrCreateAction(mActionSet, &grab, &mActionGrab), "xrCreateAction(grab)"))
    {
        return false;
    }

    // A finger resting on the controller - capacitive, so it needs no press. It is what
    // shows the beam: a controller put down on the table during a flight draws nothing.
    XrActionCreateInfo touchInfo{XR_TYPE_ACTION_CREATE_INFO};
    touchInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    std::strncpy(touchInfo.actionName, "touch", XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(touchInfo.localizedActionName, "Finger on the controller",
                 XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    touchInfo.countSubactionPaths = 2;
    touchInfo.subactionPaths      = handPaths;
    if (!check(xrCreateAction(mActionSet, &touchInfo, &mActionTouch), "xrCreateAction(touch)"))
    {
        return false;
    }

    // Per hand, so a tick lands in the hand that did something.
    XrActionCreateInfo haptic{XR_TYPE_ACTION_CREATE_INFO};
    haptic.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
    std::strncpy(haptic.actionName, "haptic", XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(haptic.localizedActionName, "Haptic feedback", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    haptic.countSubactionPaths = 2;
    haptic.subactionPaths      = handPaths;
    if (!check(xrCreateAction(mActionSet, &haptic, &mActionHaptic), "xrCreateAction(haptic)"))
    {
        return false;
    }

    // Mirrored left and right, so a single controller in either hand is enough. A/B and
    // X/Y sit in the same places on their respective controllers, so binding both to the
    // same action gives the same thumb positions whichever one you picked up.
    //
    //   trigger                take hold of what the beam is on (a panel or the video)
    //   thumbstick, holding    push it away / pull it in, make it bigger / smaller
    //   thumbstick, not        screen up/down, smaller/bigger
    //   A / X (lower button)   recenter
    //   B / Y (upper button)   toggle passthrough
    //   thumbstick click       toggle recording
    //   left menu button       leave VR
    //
    // The grip is deliberately unbound. Holding a Touch controller squeezes it, and it used to
    // push the video away while the pilot was only holding on.
    //
    // Head lock stays on a hand gesture and in the flat settings menu - it is set once,
    // not adjusted in flight, and there is no button left that exists on both controllers.
    std::vector<XrActionSuggestedBinding> touchBindings = {
        {mActionRecenter, path("/user/hand/right/input/a/click")},
        {mActionRecenter, path("/user/hand/left/input/x/click")},
        {mActionPassthrough, path("/user/hand/right/input/b/click")},
        {mActionPassthrough, path("/user/hand/left/input/y/click")},
        {mActionRecord, path("/user/hand/right/input/thumbstick/click")},
        {mActionRecord, path("/user/hand/left/input/thumbstick/click")},
        {mActionStick, path("/user/hand/right/input/thumbstick")},
        {mActionStick, path("/user/hand/left/input/thumbstick")},
        {mActionExit, path("/user/hand/left/input/menu/click")},
        {mActionHaptic, path("/user/hand/left/output/haptic")},
        {mActionHaptic, path("/user/hand/right/output/haptic")},
        {mActionAim, path("/user/hand/left/input/aim/pose")},
        {mActionAim, path("/user/hand/right/input/aim/pose")},
        {mActionGrab, path("/user/hand/left/input/trigger/value")},
        {mActionGrab, path("/user/hand/right/input/trigger/value")},
    };
    const size_t touchBase = touchBindings.size();
    // The capacitive sensors, any of which means a finger is on the controller. Standard
    // paths of this profile - but a suggestion is atomic, so they go in a tier of their own
    // rather than risk the buttons with them.
    for (const char* side : {"left", "right"})
    {
        for (const char* sensor : {"trigger/touch", "thumbstick/touch", "thumbrest/touch"})
        {
            const std::string where = std::string("/user/hand/") + side + "/input/" + sensor;
            touchBindings.push_back({mActionTouch, path(where.c_str())});
        }
    }
    touchBindings.push_back({mActionTouch, path("/user/hand/right/input/a/touch")});
    touchBindings.push_back({mActionTouch, path("/user/hand/right/input/b/touch")});
    touchBindings.push_back({mActionTouch, path("/user/hand/left/input/x/touch")});
    touchBindings.push_back({mActionTouch, path("/user/hand/left/input/y/touch")});

    XrInteractionProfileSuggestedBinding touch{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    touch.interactionProfile     = path("/interaction_profiles/oculus/touch_controller");
    touch.suggestedBindings      = touchBindings.data();
    touch.countSuggestedBindings = (uint32_t) touchBindings.size();
    if (XR_SUCCEEDED(xrSuggestInteractionProfileBindings(mInstance, &touch)))
    {
        LOGI("touch controller bindings suggested, with the touch sensors");
        mHasTouchSensors = true;
    }
    else
    {
        touch.countSuggestedBindings = (uint32_t) touchBase;
        mHasTouchSensors             = false;
        if (XR_SUCCEEDED(xrSuggestInteractionProfileBindings(mInstance, &touch)))
        {
            // Without the sensors the beam shows whenever a controller is tracked.
            LOGW("touch sensors rejected - the beam will show whenever a controller is tracked");
        }
        else
        {
            LOGW("touch controller bindings rejected, controls may be unavailable");
        }
    }

    // Bare minimum so something still works on a runtime without Touch controllers.
    const XrActionSuggestedBinding simpleBindings[] = {
        {mActionRecenter, path("/user/hand/right/input/select/click")},
        {mActionPassthrough, path("/user/hand/left/input/select/click")},
        {mActionHaptic, path("/user/hand/left/output/haptic")},
        {mActionHaptic, path("/user/hand/right/output/haptic")},
        {mActionAim, path("/user/hand/left/input/aim/pose")},
        {mActionAim, path("/user/hand/right/input/aim/pose")},
    };
    XrInteractionProfileSuggestedBinding simple{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    simple.interactionProfile = path("/interaction_profiles/khr/simple_controller");
    simple.suggestedBindings  = simpleBindings;
    simple.countSuggestedBindings = (uint32_t) (sizeof(simpleBindings) / sizeof(simpleBindings[0]));
    if (XR_FAILED(xrSuggestInteractionProfileBindings(mInstance, &simple)))
    {
        LOGW("simple controller bindings rejected");
    }

    // Hand tracking, for flying without controllers. A boolean action may be bound to a
    // float source - the runtime does the thresholding - so the same four actions work.
    // Pinch is index-to-thumb, grasp is a whole-hand squeeze, which are far enough apart
    // not to trigger each other.
    if (mHasHandInteraction)
    {
        // No grasp bindings: a whole-hand squeeze is what you do to pick up a drone, and
        // having that flip passthrough mid-flight is worse than not having the shortcut.
        // Pinch survives, but only when held - see heldActionFired().
        // Built in tiers, largest first, because a suggestion is atomic per profile: one
        // path this runtime does not know costs every binding submitted with it. Measured
        // the hard way - a wrong pose path took the working pinch bindings down with it, so
        // additions are peeled back one tier at a time until a set is accepted rather than
        // all-or-nothing.
        std::vector<XrActionSuggestedBinding> handBindings = {
            {mActionHandRecenter, path("/user/hand/right/input/pinch_ext/value")},
            {mActionHandPassthrough, path("/user/hand/left/input/pinch_ext/value")},
        };
        const size_t baseOnly = handBindings.size();

        // Pointing at an instrument to move it. aim_activate is the extension's own "act on
        // what I am pointing at", which is narrower than the grasp this file otherwise
        // refuses to bind: it does nothing until the ray lands on a panel, so it cannot go
        // off in flight the way a global toggle would. The pose is the plain aim/pose - the
        // extension adds aim_activate_ext, pinch_ext and grasp_ext, but reuses the standard
        // pose paths, and asking for an aim_ext/pose gets XR_ERROR_PATH_UNSUPPORTED.
        handBindings.push_back({mActionAim, path("/user/hand/left/input/aim/pose")});
        handBindings.push_back({mActionAim, path("/user/hand/right/input/aim/pose")});
        handBindings.push_back(
            {mActionGrab, path("/user/hand/left/input/aim_activate_ext/value")});
        handBindings.push_back(
            {mActionGrab, path("/user/hand/right/input/aim_activate_ext/value")});
        const size_t withDrag = handBindings.size();

        // Thumb swipes along the index finger, for nudging the panel up and down without
        // spending pinch or grasp on it.
        if (mHasMicrogestures)
        {
            handBindings.push_back(
                {mActionRaise, path("/user/hand/right/input/swipe_forward_meta")});
            handBindings.push_back(
                {mActionLower, path("/user/hand/right/input/swipe_backward_meta")});
            // Deliberately no gesture for leaving immersive mode. A microgesture fires on one
            // thumb movement with nothing to hold, and a false exit in flight costs the video
            // feed - the menu button and the flat-mode menu are enough for something done once.
        }

        XrInteractionProfileSuggestedBinding hands{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        mHandProfilePath         = path("/interaction_profiles/ext/hand_interaction_ext");
        hands.interactionProfile = mHandProfilePath;
        hands.suggestedBindings  = handBindings.data();

        const struct
        {
            size_t      count;
            const char* what;
        } tiers[] = {
            {handBindings.size(), "held pinch + drag + microgesture height"},
            {withDrag, "held pinch + drag"},
            {baseOnly, "held pinch"},
        };

        XrResult r = XR_ERROR_PATH_UNSUPPORTED;
        for (const auto& tier : tiers)
        {
            // Skip a tier that is not smaller than one already tried.
            if (tier.count > handBindings.size()) continue;
            hands.countSuggestedBindings = (uint32_t) tier.count;
            r                            = xrSuggestInteractionProfileBindings(mInstance, &hands);
            if (XR_SUCCEEDED(r))
            {
                // Record what actually took, so the frame loop does not wait on input that
                // was never bound.
                mHasMicrogestures = tier.count > withDrag;
                mHandDragBound    = tier.count > baseOnly;
                LOGI("hand interaction bindings suggested (%s)", tier.what);
                break;
            }
            char name[XR_MAX_RESULT_STRING_SIZE] = {0};
            xrResultToString(mInstance, r, name);
            LOGW("hand bindings for \"%s\" rejected (%s), trying a smaller set", tier.what,
                 name);
        }
        if (XR_FAILED(r))
        {
            mHasMicrogestures = false;
            mHandDragBound    = false;
            LOGW("no hand interaction bindings accepted at all - hands do nothing");
        }
    }

    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets      = &mActionSet;
    if (!check(xrAttachSessionActionSets(mSession, &attach), "xrAttachSessionActionSets"))
    {
        return false;
    }

    // One space per hand for the pointing ray. A failure here is not fatal: it costs the
    // ability to rearrange the instruments and nothing else.
    for (int i = 0; i < 2; ++i)
    {
        XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        spaceInfo.action            = mActionAim;
        spaceInfo.subactionPath     = handPaths[i];
        spaceInfo.poseInActionSpace = XrPosef{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
        if (XR_FAILED(xrCreateActionSpace(mSession, &spaceInfo, &mAimSpace[i])))
        {
            LOGW("no aim space for the %s hand - instruments cannot be dragged with it",
                 i == 0 ? "left" : "right");
            mAimSpace[i] = XR_NULL_HANDLE;
        }
    }
    return true;
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

void XrGoggleSession::setQuadHeightOffset(float meters)
{
    mQuadHeightOffset = clampf(meters, -kMaxHeightOffset, kMaxHeightOffset);
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

void XrGoggleSession::resolveListenerMethods(JNIEnv* env, jobject listener)
{
    if (listener == nullptr) return;
    jclass cls = env->GetObjectClass(listener);
    if (cls == nullptr) return;
    mOnButton       = env->GetMethodID(cls, "onXrButton", "(I)V");
    mOnOverlayGrab  = env->GetMethodID(cls, "onXrOverlayGrab", "(IZ)V");
    mOnOverlayMoved = env->GetMethodID(cls, "onXrOverlayMoved", "(IFFFFF)V");
    mOnOverlayHover = env->GetMethodID(cls, "onXrOverlayHover", "(I)V");
    env->DeleteLocalRef(cls);
    // GetMethodID leaves a pending exception on a miss, which clearing hides - and a listener
    // method that silently never fires is the kind of thing that gets debugged from the wrong
    // end for an hour. Say so instead.
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (mOnButton == nullptr || mOnOverlayGrab == nullptr || mOnOverlayMoved == nullptr ||
        mOnOverlayHover == nullptr)
    {
        LOGW("listener is missing a method (button %d, grab %d, moved %d, hover %d) - those "
             "callbacks will not fire",
             mOnButton != nullptr, mOnOverlayGrab != nullptr, mOnOverlayMoved != nullptr,
             mOnOverlayHover != nullptr);
    }
}

void XrGoggleSession::runLoop(JNIEnv* env, jobject listener)
{
    resolveListenerMethods(env, listener);
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
                if (mSessionState == XR_SESSION_STATE_FOCUSED)
                {
                    mSelectArmed[0] = false;
                    mSelectArmed[1] = false;
                }
                LOGI("session state -> %d", (int) mSessionState);
                if (mSessionState == XR_SESSION_STATE_READY)
                {
                    XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                    begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    if (check(xrBeginSession(mSession, &begin), "xrBeginSession"))
                    {
                        mSessionRunning = true;
                        if (mHasSimultaneous)
                        {
                            XrSimultaneousHandsAndControllersTrackingResumeInfoMETA resume{
                                XR_TYPE_SIMULTANEOUS_HANDS_AND_CONTROLLERS_TRACKING_RESUME_INFO_META};
                            if (XR_SUCCEEDED(mXrResumeSimultaneous(mSession, &resume)))
                            {
                                LOGI("hands and controllers tracked simultaneously");
                            }
                            else
                            {
                                LOGW("xrResumeSimultaneousHandsAndControllersTrackingMETA failed");
                            }
                        }
                        logInteractionProfiles();
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
            case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
            {
                mSelectArmed[0] = false;
                mSelectArmed[1] = false;
                // The only way to see which of the suggested profiles is actually live.
                logInteractionProfiles();
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

// A hand action fires once, after being held for kHandHoldNs. Anything shorter is treated
// as an accident - and so is a pinch that began while the hand was not free to give input:
// it has to be let go and made again, or lifting a hand off the transmitter mid-pinch would
// fire it.
bool XrGoggleSession::heldActionFired(XrAction action, int slot, XrTime now, bool mayAct)
{
    if (action == XR_NULL_HANDLE) return false;
    HeldButton& held = mHeld[slot];

    XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
    get.action = action;
    XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
    if (XR_FAILED(xrGetActionStateBoolean(mSession, &get, &state)) || !state.isActive ||
        !state.currentState)
    {
        held = HeldButton{};  // released, which also clears a blocked pinch
        return false;
    }
    if (!mayAct || held.blocked)
    {
        held         = HeldButton{};
        held.blocked = true;
        return false;
    }
    if (!held.down)
    {
        held.down     = true;
        held.fired    = false;
        held.downTime = now;
        return false;
    }
    if (!held.fired && (now - held.downTime) >= kHandHoldNs)
    {
        held.fired = true;
        return true;
    }
    return false;
}

void XrGoggleSession::logInteractionProfiles()
{
    for (int i = 0; i < 2; ++i)
    {
        const char*               hand = i == 0 ? "/user/hand/left" : "/user/hand/right";
        XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
        if (XR_FAILED(xrGetCurrentInteractionProfile(mSession, mHandPath[i], &state)))
        {
            continue;
        }
        // Kept from the event rather than asked for every frame: the frame loop needs to know
        // whether each side is a hand, and this is the only moment it changes.
        mSideIsHand[i] =
            mHandProfilePath != XR_NULL_PATH && state.interactionProfile == mHandProfilePath;
        if (state.interactionProfile == XR_NULL_PATH)
        {
            LOGI("%s: no interaction profile active", hand);
            continue;
        }
        char     name[XR_MAX_PATH_LENGTH] = {0};
        uint32_t written                   = 0;
        if (XR_SUCCEEDED(
                xrPathToString(mInstance, state.interactionProfile, sizeof(name), &written, name)))
        {
            LOGI("%s: %s", hand, name);
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

    jmethodID onButton = mOnButton;

    struct
    {
        XrAction action;
        int      button;
        bool     handOnly;  // bound only to a hand gesture, so it obeys setHandInputEnabled
    } buttons[] = {
        {mActionRecenter, BUTTON_RECENTER, false},
        {mActionPassthrough, BUTTON_PASSTHROUGH, false},
        {mActionRecord, BUTTON_RECORD, false},
        {mActionLockMode, BUTTON_LOCK_MODE, false},
        // Raise and lower have no controller binding - the thumbstick vector does the panel
        // height - so these two are microgestures and nothing else.
        {mActionRaise, BUTTON_RAISE, true},
        {mActionLower, BUTTON_LOWER, true},
        {mActionExit, BUTTON_EXIT, false},
    };

    const bool handsMayAct = mHandInputEnabled.load();
    // The microgestures are bound on the right hand, and only count from it while it is up and
    // free: a thumb sliding along the index finger is what working a gimbal looks like.
    const bool rightHandFree = mSideIsHand[1] && mHandBlock[1] == xri::HAND_FREE;
    for (const auto& b : buttons)
    {
        if (b.action == XR_NULL_HANDLE) continue;
        XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
        get.action = b.action;
        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        if (XR_FAILED(xrGetActionStateBoolean(mSession, &get, &state))) continue;
        // Rising edge only.
        if (!(state.isActive && state.changedSinceLastSync && state.currentState)) continue;
        if (b.handOnly && !handsMayAct) continue;
        if (b.handOnly && !rightHandFree)
        {
            // Worth a line: this is the recogniser seeing a gesture nobody made.
            LOGI("microgesture %s ignored: right hand %s", b.button == BUTTON_RAISE ? "up" : "down",
                 xri::handBlockName(mHandBlock[1]));
            continue;
        }
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
            case BUTTON_RAISE:
                setQuadHeightOffset(mQuadHeightOffset.load() + kHeightStep);
                break;
            case BUTTON_LOWER:
                setQuadHeightOffset(mQuadHeightOffset.load() - kHeightStep);
                break;
            case BUTTON_EXIT:
                // Handled on the Java side; just confirm it was registered.
                requestHaptic(0.6f, 80);
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

    // Hand gestures, which need a deliberate hold before they count - and only when hand
    // input is switched on at all (see setHandInputEnabled()), from a hand that is up and
    // free, and not from one that is pointing at something: a pinch on a panel is a grab.
    const XrTime nowNs = mLastPredictedDisplayTime;
    const struct
    {
        XrAction action;
        int      slot;
        int      hand;
        int      button;
    } heldGestures[] = {
        {mActionHandRecenter, 0, 1, BUTTON_RECENTER},
        {mActionHandPassthrough, 1, 0, BUTTON_PASSTHROUGH},
    };
    for (const auto& g : heldGestures)
    {
        if (!handsMayAct)
        {
            mHeld[g.slot]         = HeldButton{};
            mHoldProgress[g.hand] = 0.0f;
            continue;
        }
        const bool free = mSideIsHand[g.hand] && mHandBlock[g.hand] == xri::HAND_FREE &&
                          mInteraction.heldBy() != g.hand &&
                          mInteraction.targetOf(g.hand) == xri::TARGET_NONE;
        const bool fired = heldActionFired(g.action, g.slot, nowNs, free);
        const HeldButton& held = mHeld[g.slot];
        // Shown as a ring filling up in front of the hand, so a pilot sees the gesture was
        // recognised - and can let go - before it does anything.
        mHoldProgress[g.hand] =
            held.down && !held.fired
                ? clampf((float) (nowNs - held.downTime) / (float) kHandHoldNs, 0.0f, 1.0f)
                : 0.0f;
        if (!fired) continue;
        if (g.button == BUTTON_RECENTER)
        {
            mRecenterRequested = true;
        }
        else
        {
            mPassthroughWanted = !mPassthroughWanted.load();
        }
        LOGI("%s hand pinch held: %s", g.hand == 0 ? "left" : "right",
             g.button == BUTTON_RECENTER ? "recenter" : "passthrough");
        requestHaptic(0.5f, 60);
        if (onButton != nullptr)
        {
            env->CallVoidMethod(listener, onButton, (jint) g.button);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
    }

    // The stick and, per hand, the trigger and the touch sensors - for the pointer, in the
    // frame that follows. What the stick acts on depends on what is held, which the pointer
    // knows and this does not.
    mStickX = 0.0f;
    mStickY = 0.0f;
    if (mActionStick != XR_NULL_HANDLE)
    {
        XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
        get.action = mActionStick;
        XrActionStateVector2f stickState{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(xrGetActionStateVector2f(mSession, &get, &stickState)) &&
            stickState.isActive)
        {
            mStickX = stickState.currentState.x;
            mStickY = stickState.currentState.y;
        }
    }
    const auto down = [&](XrAction action, XrPath side) {
        if (action == XR_NULL_HANDLE) return false;
        XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
        get.action        = action;
        get.subactionPath = side;
        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        return XR_SUCCEEDED(xrGetActionStateBoolean(mSession, &get, &state)) && state.isActive &&
               state.currentState == XR_TRUE;
    };
    for (int i = 0; i < 2; ++i)
    {
        // A trigger already held when the session comes into focus, or when a controller is
        // picked up, is not a press: it took hold of the video while the headset was being
        // put on. It only counts once it has been let go of.
        const bool pressed = down(mActionGrab, mHandPath[i]);
        if (!pressed) mSelectArmed[i] = true;
        mSelect[i] = pressed && mSelectArmed[i];
        mTouch[i]  = down(mActionTouch, mHandPath[i]);
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
    // The arrangement hangs from the head, turned to where it looks - yaw only, so nothing
    // ends up pitched or rolled with it. The video's distance is applied every frame from
    // here, so changing it takes effect without a recenter.
    mAnchorEye    = toXri(location.pose.position);
    mAnchorFacing = xri::aboutY(yawOf(location.pose.orientation));
    mAnchorValid  = true;
}

void XrGoggleSession::renderFrame(JNIEnv* env, jobject listener)
{
    XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState    frameState{XR_TYPE_FRAME_STATE};
    if (XR_FAILED(xrWaitFrame(mSession, &waitInfo, &frameState))) return;

    XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    if (XR_FAILED(xrBeginFrame(mSession, &beginInfo))) return;

    mLastPredictedDisplayTime = frameState.predictedDisplayTime;

    applyPendingHaptic();

    if (mRecenterRequested.exchange(false) || (!mHeadLocked.load() && !mAnchorValid))
    {
        recenterAt(frameState.predictedDisplayTime);
    }

    const bool headLocked = mHeadLocked.load();
    const bool wantPassthrough =
        mHasPassthrough && mPassthroughRunning && mPassthroughWanted.load();

    const int vw = mVideoWidth.load();
    const int vh = mVideoHeight.load();

    // What everything hangs from this frame: head-locked, the view itself; world-locked, the
    // head as it was at the last recenter. The video and every instrument are placed from it
    // by the same functions the pointer hits against.
    xri::Anchor anchor;
    if (!headLocked && mAnchorValid)
    {
        anchor.eye    = mAnchorEye;
        anchor.facing = mAnchorFacing;
    }
    xri::Video video;
    video.distance = mQuadDistance.load();
    video.width    = mQuadWidth.load();
    // Height follows the stream's aspect ratio so nothing is stretched.
    video.aspect = (vw > 0 && vh > 0) ? (float) vh / (float) vw : 9.0f / 16.0f;
    video.height = mQuadHeightOffset.load();
    const XrSpace layoutSpace = headLocked ? mViewSpace : mLocalSpace;

    // Before composing, so a panel taken hold of this frame is drawn where it was moved to
    // rather than a frame behind the beam.
    const float dt = frameState.predictedDisplayPeriod > 0
                         ? (float) ((double) frameState.predictedDisplayPeriod * 1e-9)
                         : 1.0f / 72.0f;
    updatePointer(env, listener, anchor, &video, layoutSpace, frameState.predictedDisplayTime,
                  dt);

    XrCompositionLayerSettingsFB layerSettings{XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB};
    layerSettings.layerFlags = XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SHARPENING_BIT_FB;

    // Everything that produces into an Android Surface - MediaCodec, and Canvas for the
    // status screen - has its origin at the top left, while the compositor samples a
    // swapchain image from the bottom left. Without this the picture is upside down.
    XrCompositionLayerImageLayoutFB imageLayout{XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB};
    imageLayout.flags = XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB;

    // Chain whatever applies onto the quad.
    const void* layerChain = nullptr;
    if (mHasLayerSettings && mSharpening.load())
    {
        layerSettings.next = layerChain;
        layerChain         = &layerSettings;
    }
    if (mHasImageLayout)
    {
        imageLayout.next = const_cast<void*>(layerChain);
        layerChain       = &imageLayout;
    }

    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad.next         = layerChain;
    quad.layerFlags   = 0;  // opaque: the video has to cover passthrough behind it
    quad.space        = layoutSpace;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain       = mSwapchain;
    quad.subImage.imageArrayIndex = 0;
    // Clamp to the swapchain the runtime handed us; MediaCodec resizes the buffers to the
    // stream resolution, which is normally the size this was created with.
    quad.subImage.imageRect.offset = XrOffset2Di{0, 0};
    quad.subImage.imageRect.extent =
        XrExtent2Di{vw > 0 && vw <= mSwapchainWidth ? vw : mSwapchainWidth,
                    vh > 0 && vh <= mSwapchainHeight ? vh : mSwapchainHeight};
    const xri::Surface screen = xri::videoSurface(anchor, video);
    quad.pose = toXr(screen.pose);
    quad.size = XrExtent2Df{screen.width, screen.height};

    // Straight alpha, because Canvas hands out straight alpha: whatever an overlay leaves
    // transparent has to show what is behind it rather than blacken it.
    static constexpr XrCompositionLayerFlags kOverlayFlags =
        XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT |
        XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;

    XrCompositionLayerImageLayoutFB overlayLayout{XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB};
    overlayLayout.flags = XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB;
    void* const overlayNext = mHasImageLayout ? &overlayLayout : nullptr;

    XrCompositionLayerQuad          overlayQuads[OVERLAY_COUNT];
    XrCompositionLayerCylinderKHR   overlayCyls[OVERLAY_COUNT];
    bool                            overlayIsCylinder[OVERLAY_COUNT] = {false};
    bool                            overlayReady[OVERLAY_COUNT]      = {false};

    xri::Placement placements[OVERLAY_COUNT];
    currentPlacements(placements);
    for (int i = 0; i < OVERLAY_COUNT; ++i)
    {
        const OverlayLayer& o = mOverlays[i];
        if (!placements[i].present)
        {
            continue;
        }
        const xri::Surface        s = xri::panelSurface(placements[i], anchor, video);
        const XrSwapchainSubImage sub{o.swapchain, {{0, 0}, {o.width, o.height}}, 0};

        if (s.cylinder && mHasCylinder)
        {
            XrCompositionLayerCylinderKHR& c = overlayCyls[i];
            c              = XrCompositionLayerCylinderKHR{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
            c.next         = overlayNext;
            c.layerFlags   = kOverlayFlags;
            c.space        = layoutSpace;
            c.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            c.subImage     = sub;
            // The cylinder's centre, with the visible arc one radius out along its -Z - not
            // the middle of the arc, which is what this used to pass and which put the
            // dashboard twice as far away as it was meant to be.
            c.pose         = toXr(s.axisPose);
            c.radius       = s.radius;
            c.centralAngle = s.centralAngle;
            // aspectRatio is width over height of the visible rectangle.
            c.aspectRatio  = s.height > 0.0f ? s.width / s.height : 1.0f;
            overlayIsCylinder[i] = true;
        }
        else
        {
            // A curved layer asked for on a runtime without the extension falls back to flat,
            // which only looks less dimensional.
            XrCompositionLayerQuad& q = overlayQuads[i];
            q               = XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
            q.next          = overlayNext;
            q.layerFlags    = kOverlayFlags;
            q.space         = layoutSpace;
            q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            q.subImage      = sub;
            q.pose          = toXr(s.pose);
            q.size          = XrExtent2Df{s.width, s.height};
            // The symbology sits exactly on the video, so nudge it a centimetre towards the
            // pilot. Layers composite in submission order anyway; this is for a runtime that
            // sorts by depth instead.
            if (i == OVERLAY_SYMBOLOGY)
            {
                q.pose.position = toXr(s.pose.position +
                                       xri::rotate(s.pose.orientation, xri::Vec3{0.0f, 0.0f, 0.01f}));
            }
        }
        overlayReady[i] = true;
    }

    // The pointers go last, on top of everything: the beams, then the cursors at their ends.
    const PointerArtLayer& beamArt   = mPointerArt[POINTER_BEAM - POINTER_BEAM];
    const PointerArtLayer& cursorArt = mPointerArt[POINTER_CURSOR - POINTER_BEAM];
    XrCompositionLayerQuad pointerQuads[4];
    int                    pointerCount = 0;
    for (int pass = 0; pass < 2; ++pass)
    {
        for (int i = 0; i < 2; ++i)
        {
            const xri::PointerOut& p = mPointers[i];
            XrCompositionLayerQuad q{XR_TYPE_COMPOSITION_LAYER_QUAD};
            q.next          = overlayNext;
            q.layerFlags    = kOverlayFlags;
            q.space         = layoutSpace;
            q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            if (pass == 0)
            {
                if (!p.beam || p.beamLength <= 0.0f || beamArt.swapchain == XR_NULL_HANDLE) continue;
                q.subImage = XrSwapchainSubImage{beamArt.swapchain,
                                                 {{0, 0}, {beamArt.width, beamArt.height}}, 0};
                q.pose     = toXr(p.beamPose);
                q.size     = XrExtent2Df{xri::kBeamWidth, p.beamLength};
            }
            else
            {
                if (!p.cursor || cursorArt.swapchain == XR_NULL_HANDLE) continue;
                // One frame of the strip, picked by the state the cursor is in.
                q.subImage = XrSwapchainSubImage{
                    cursorArt.swapchain,
                    {{p.cursorFrame * kCursorArtCell, 0}, {kCursorArtCell, kCursorArtCell}},
                    0};
                q.pose = toXr(p.cursorPose);
                q.size = XrExtent2Df{p.cursorSize, p.cursorSize};
            }
            pointerQuads[pointerCount++] = q;
        }
    }

    XrCompositionLayerPassthroughFB passthroughLayer{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
    passthroughLayer.flags       = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    passthroughLayer.space       = XR_NULL_HANDLE;
    passthroughLayer.layerHandle = mPassthroughLayer;

    const XrCompositionLayerBaseHeader* layers[2 + OVERLAY_COUNT + 4];
    uint32_t                            layerCount = 0;
    if (wantPassthrough)
    {
        layers[layerCount++] = (const XrCompositionLayerBaseHeader*) &passthroughLayer;
    }
    layers[layerCount++] = (const XrCompositionLayerBaseHeader*) &quad;
    for (int i = 0; i < OVERLAY_COUNT; ++i)
    {
        if (!overlayReady[i])
        {
            continue;
        }
        layers[layerCount++] = overlayIsCylinder[i]
                                   ? (const XrCompositionLayerBaseHeader*) &overlayCyls[i]
                                   : (const XrCompositionLayerBaseHeader*) &overlayQuads[i];
    }
    for (int i = 0; i < pointerCount; ++i)
    {
        layers[layerCount++] = (const XrCompositionLayerBaseHeader*) &pointerQuads[i];
    }

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
    for (int i = 0; i < OVERLAY_COUNT; ++i)
    {
        OverlayLayer& o = mOverlays[i];
        if (o.swapchain != XR_NULL_HANDLE)
        {
            xrDestroySwapchain(o.swapchain);
            o.swapchain = XR_NULL_HANDLE;
        }
        if (o.surface != nullptr)
        {
            env->DeleteGlobalRef(o.surface);
            o.surface = nullptr;
        }
    }
    for (PointerArtLayer& a : mPointerArt)
    {
        if (a.swapchain != XR_NULL_HANDLE)
        {
            xrDestroySwapchain(a.swapchain);
            a.swapchain = XR_NULL_HANDLE;
        }
        if (a.surface != nullptr)
        {
            env->DeleteGlobalRef(a.surface);
            a.surface = nullptr;
        }
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
