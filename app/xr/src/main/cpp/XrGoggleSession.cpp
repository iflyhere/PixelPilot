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
constexpr float kStickDeadzone   = 0.30f;
constexpr float kTriggerDeadzone = 0.15f;
constexpr float kDistancePerSec  = 0.9f;   // meters per second of full stick deflection
constexpr float kWidthPerSec     = 1.4f;
constexpr float kMinDistance     = 0.4f;
constexpr float kMaxDistance     = 8.0f;
constexpr float kMinWidth        = 0.4f;
constexpr float kMaxWidth        = 12.0f;
constexpr float kMaxHeightOffset = 1.5f;
// One nudge, about 1.8 degrees at the default distance - small enough to fine-tune.
constexpr float kHeightStep      = 0.05f;
constexpr float kHeightPerSec    = 0.7f;

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
    if (mHasMicrogestures) enabled.push_back(XR_META_HAND_TRACKING_MICROGESTURES_EXTENSION_NAME);
    if (mHasSimultaneous)
        enabled.push_back(XR_META_SIMULTANEOUS_HANDS_AND_CONTROLLERS_EXTENSION_NAME);
    LOGI(
        "optional extensions: passthrough=%d layerSettings=%d refreshRate=%d handInteraction=%d "
        "imageLayout=%d microgestures=%d simultaneousHandsControllers=%d",
        (int) mHasPassthrough,
        (int) mHasLayerSettings,
        (int) mHasRefreshRate,
        (int) mHasHandInteraction,
        (int) mHasImageLayout,
        (int) mHasMicrogestures,
        (int) mHasSimultaneous);

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
bool XrGoggleSession::createSwapchain(JNIEnv* env)
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
        info.width       = (uint32_t) mSwapchainWidth;
        info.height      = (uint32_t) mSwapchainHeight;
        info.faceCount   = variant.counts;
        info.arraySize   = variant.counts;
        info.mipCount    = variant.counts;

        surface = nullptr;
        last    = mXrCreateSwapchainAndroidSurfaceKHR(mSession, &info, &mSwapchain, &surface);
        if (XR_SUCCEEDED(last) && surface != nullptr)
        {
            LOGI("android surface swapchain %dx%d created (%s)",
                 mSwapchainWidth,
                 mSwapchainHeight,
                 variant.what);
            mVideoSurface = env->NewGlobalRef(surface);
            return true;
        }
        char name[XR_MAX_RESULT_STRING_SIZE] = {0};
        xrResultToString(mInstance, last, name);
        LOGW("swapchain variant \'%s\' rejected: %s", variant.what, name);
        if (XR_SUCCEEDED(last) && mSwapchain != XR_NULL_HANDLE)
        {
            // Succeeded but handed back no Surface - not usable, do not leak it.
            xrDestroySwapchain(mSwapchain);
            mSwapchain = XR_NULL_HANDLE;
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

    XrActionCreateInfo stick{XR_TYPE_ACTION_CREATE_INFO};
    stick.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
    std::strncpy(stick.actionName, "place", XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(stick.localizedActionName, "Move the screen", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    if (!check(xrCreateAction(mActionSet, &stick, &mActionStick), "xrCreateAction(place)"))
    {
        return false;
    }

    // Analog, so the trigger and grip pull the panel in and push it out at a rate.
    XrActionCreateInfo axis{XR_TYPE_ACTION_CREATE_INFO};
    axis.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
    std::strncpy(axis.actionName, "nearer", XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(axis.localizedActionName, "Pull the screen closer", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    if (!check(xrCreateAction(mActionSet, &axis, &mActionNearer), "xrCreateAction(nearer)"))
    {
        return false;
    }
    std::strncpy(axis.actionName, "farther", XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(axis.localizedActionName, "Push the screen away", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    if (!check(xrCreateAction(mActionSet, &axis, &mActionFarther), "xrCreateAction(farther)"))
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

    // Mirrored left and right, so a single controller in either hand is enough. A/B and
    // X/Y sit in the same places on their respective controllers, so binding both to the
    // same action gives the same thumb positions whichever one you picked up.
    //
    //   thumbstick up/down     screen up/down
    //   thumbstick left/right  screen smaller/bigger
    //   trigger                pull the screen closer
    //   grip                   push the screen away
    //   A / X (lower button)   recenter
    //   B / Y (upper button)   toggle passthrough
    //   thumbstick click       toggle recording
    //
    // Head lock stays on a hand gesture and in the flat settings menu - it is set once,
    // not adjusted in flight, and there is no button left that exists on both controllers.
    const XrActionSuggestedBinding touchBindings[] = {
        {mActionRecenter, path("/user/hand/right/input/a/click")},
        {mActionRecenter, path("/user/hand/left/input/x/click")},
        {mActionPassthrough, path("/user/hand/right/input/b/click")},
        {mActionPassthrough, path("/user/hand/left/input/y/click")},
        {mActionRecord, path("/user/hand/right/input/thumbstick/click")},
        {mActionRecord, path("/user/hand/left/input/thumbstick/click")},
        {mActionStick, path("/user/hand/right/input/thumbstick")},
        {mActionStick, path("/user/hand/left/input/thumbstick")},
        {mActionNearer, path("/user/hand/right/input/trigger/value")},
        {mActionNearer, path("/user/hand/left/input/trigger/value")},
        {mActionFarther, path("/user/hand/right/input/squeeze/value")},
        {mActionFarther, path("/user/hand/left/input/squeeze/value")},
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

    // Hand tracking, for flying without controllers. A boolean action may be bound to a
    // float source - the runtime does the thresholding - so the same four actions work.
    // Pinch is index-to-thumb, grasp is a whole-hand squeeze, which are far enough apart
    // not to trigger each other.
    if (mHasHandInteraction)
    {
        std::vector<XrActionSuggestedBinding> handBindings = {
            {mActionRecenter, path("/user/hand/right/input/pinch_ext/value")},
            {mActionPassthrough, path("/user/hand/right/input/grasp_ext/value")},
            {mActionRecord, path("/user/hand/left/input/pinch_ext/value")},
            {mActionLockMode, path("/user/hand/left/input/grasp_ext/value")},
        };
        // Thumb swipes along the index finger, for nudging the panel up and down without
        // spending pinch or grasp on it.
        const size_t withoutMicrogestures = handBindings.size();
        if (mHasMicrogestures)
        {
            handBindings.push_back(
                {mActionRaise, path("/user/hand/right/input/swipe_forward_meta")});
            handBindings.push_back(
                {mActionLower, path("/user/hand/right/input/swipe_backward_meta")});
        }

        XrInteractionProfileSuggestedBinding hands{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        hands.interactionProfile = path("/interaction_profiles/ext/hand_interaction_ext");
        hands.suggestedBindings  = handBindings.data();
        hands.countSuggestedBindings = (uint32_t) handBindings.size();
        XrResult r = xrSuggestInteractionProfileBindings(mInstance, &hands);
        if (XR_FAILED(r) && handBindings.size() != withoutMicrogestures)
        {
            // Suggestions are atomic per profile, so one bad path would cost the working
            // pinch/grasp bindings too. Drop the microgestures and keep those.
            char name[XR_MAX_RESULT_STRING_SIZE] = {0};
            xrResultToString(mInstance, r, name);
            LOGW("microgesture bindings rejected (%s), retrying without them", name);
            mHasMicrogestures            = false;
            hands.countSuggestedBindings = (uint32_t) withoutMicrogestures;
            r                            = xrSuggestInteractionProfileBindings(mInstance, &hands);
        }
        if (XR_FAILED(r))
        {
            char name[XR_MAX_RESULT_STRING_SIZE] = {0};
            xrResultToString(mInstance, r, name);
            LOGW("hand interaction bindings rejected: %s", name);
        }
        else
        {
            LOGI("hand interaction bindings suggested (pinch/grasp%s)",
                 mHasMicrogestures ? " + microgesture height" : "");
        }
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

void XrGoggleSession::logInteractionProfiles()
{
    for (const char* hand : {"/user/hand/left", "/user/hand/right"})
    {
        XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
        if (XR_FAILED(xrGetCurrentInteractionProfile(mSession, path(hand), &state)))
        {
            continue;
        }
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
        {mActionRaise, BUTTON_RAISE},
        {mActionLower, BUTTON_LOWER},
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
                case BUTTON_RAISE:
                    setQuadHeightOffset(mQuadHeightOffset.load() + kHeightStep);
                    requestHaptic(0.25f, 20);
                    break;
                case BUTTON_LOWER:
                    setQuadHeightOffset(mQuadHeightOffset.load() - kHeightStep);
                    requestHaptic(0.25f, 20);
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
    if (mActionStick != XR_NULL_HANDLE)
    {
        get.action = mActionStick;
        XrActionStateVector2f stickState{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(xrGetActionStateVector2f(mSession, &get, &stickState)) && stickState.isActive)
        {
            // Either thumbstick: up/down raises the panel, left/right resizes it.
            const float y = stickState.currentState.y;
            if (std::fabs(y) > kStickDeadzone)
            {
                setQuadHeightOffset(mQuadHeightOffset.load() + y * kHeightPerSec * dt);
            }
            const float x = stickState.currentState.x;
            if (std::fabs(x) > kStickDeadzone)
            {
                setQuadWidth(mQuadWidth.load() + x * kWidthPerSec * dt);
            }
        }
    }

    // Trigger pulls the panel in, grip pushes it out, both analog.
    float distanceRate = 0.0f;
    for (int i = 0; i < 2; ++i)
    {
        const XrAction action = (i == 0) ? mActionNearer : mActionFarther;
        if (action == XR_NULL_HANDLE) continue;
        get.action = action;
        XrActionStateFloat axisState{XR_TYPE_ACTION_STATE_FLOAT};
        if (XR_SUCCEEDED(xrGetActionStateFloat(mSession, &get, &axisState)) && axisState.isActive &&
            axisState.currentState > kTriggerDeadzone)
        {
            distanceRate += (i == 0 ? -1.0f : 1.0f) * axisState.currentState;
        }
    }
    if (std::fabs(distanceRate) > 0.0f)
    {
        setQuadDistance(mQuadDistance.load() + distanceRate * kDistancePerSec * dt);
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
    const float heightOffset = mQuadHeightOffset.load();
    if (headLocked)
    {
        quad.pose =
            XrPosef{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, heightOffset, -mQuadDistance.load()}};
    }
    else
    {
        quad.pose = mAnchorPose;
        quad.pose.position.y += heightOffset;
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
