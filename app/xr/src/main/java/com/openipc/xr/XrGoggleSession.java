package com.openipc.xr;

import android.app.Activity;
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Surface;

import androidx.annotation.Keep;
import androidx.annotation.Nullable;

/**
 * Immersive OpenXR presentation of the decoded video stream.
 *
 * <p>The video is not rendered by this class. {@code xrCreateSwapchainAndroidSurfaceKHR}
 * hands out a {@link Surface} that is the producer side of a compositor swapchain;
 * MediaCodec decodes straight into it and the runtime composites it as a head locked
 * quad layer. No GL blit, no readback, no view hierarchy.
 *
 * <p>All native calls happen on a dedicated thread owned by this class. Listener
 * callbacks are posted to the main looper.
 */
@Keep
public final class XrGoggleSession {
    // Keep in sync with XrGoggleSession.h
    public static final int BUTTON_RECENTER = 0;
    public static final int BUTTON_PASSTHROUGH = 1;
    public static final int BUTTON_RECORD = 2;
    public static final int BUTTON_LOCK_MODE = 3;
    public static final int BUTTON_RAISE = 4;
    public static final int BUTTON_LOWER = 5;
    public static final int BUTTON_EXIT = 6;

    private static final String TAG = "pixelpilot-xr";

    /**
     * The overlay layers, each its own compositor layer at its own distance and angle - which
     * is what makes them read as having depth. Keep in sync with the enum in the header.
     *
     * <p>{@link #OVERLAY_SYMBOLOGY} is flat and sits exactly on the video; the rest are placed
     * around it and their canvas sizes come from the runtime, so nothing here has to guess.
     */
    public static final int OVERLAY_SYMBOLOGY = 0;
    public static final int OVERLAY_DASHBOARD = 1;
    public static final int OVERLAY_MINIMAP = 2;
    public static final int OVERLAY_CHART = 3;
    public static final int OVERLAY_COUNT = 4;

    private static boolean libraryLoaded;

    static {
        try {
            System.loadLibrary("XrGoggle");
            libraryLoaded = true;
        } catch (Throwable t) {
            // A device without the OpenXR loader still has to be able to run the flat app.
            Log.w(TAG, "libXrGoggle not available: " + t.getMessage());
            libraryLoaded = false;
        }
    }

    public interface Listener {
        /** The compositor swapchain is ready; hand this Surface to the decoder. */
        void onXrReady(Surface videoSurface);

        /**
         * One of the transparent overlay layers is ready to be drawn into. Called once per
         * layer the runtime granted, on the main thread, with the canvas size to draw at.
         *
         * <p>A layer the runtime refused is simply never announced - video still works, there
         * are just fewer instruments.
         *
         * @param id one of the {@code OVERLAY_*} constants
         */
        void onXrOverlayReady(int id, Surface surface, int width, int height);

        /** One of the BUTTON_* constants was pressed on a controller. */
        void onXrButton(int button);

        /** Session ended. {@code error} is null for a normal stop. */
        void onXrStopped(@Nullable String error);
    }

    private final Activity activity;
    private final Listener listener;
    private final Handler main = new Handler(Looper.getMainLooper());

    private long handle;
    private Thread thread;
    private volatile boolean stopping;

    public XrGoggleSession(Activity activity, Listener listener) {
        this.activity = activity;
        this.listener = listener;
    }

    /**
     * Cheap pre-flight check used to decide whether to offer the mode at all. It says
     * "this looks like a headset", not "a session will succeed" — only {@link #start()}
     * can tell you that.
     */
    public static boolean isSupportedDevice(Context context) {
        if (!libraryLoaded) {
            return false;
        }
        PackageManager pm = context.getPackageManager();
        if (pm.hasSystemFeature("android.hardware.vr.headtracking")
                || pm.hasSystemFeature("android.software.xr.immersive")
                || pm.hasSystemFeature("oculus.software.handtracking")) {
            return true;
        }
        // Horizon OS shell, present on every Quest.
        try {
            pm.getPackageInfo("com.oculus.vrshell", 0);
            return true;
        } catch (PackageManager.NameNotFoundException ignored) {
            return false;
        }
    }

    public static boolean isLibraryLoaded() {
        return libraryLoaded;
    }

    /** Seeds the swapchain geometry. Must be called before {@link #start()}. */
    public void setSwapchainSize(int width, int height) {
        ensureHandle();
        if (handle != 0) {
            nativeSetSwapchainSize(handle, width, height);
        }
    }

    /**
     * Creates the session on a background thread and starts the frame loop. The listener
     * gets either {@link Listener#onXrReady} or {@link Listener#onXrStopped} with an
     * error.
     */
    public void start() {
        if (thread != null) {
            return;
        }
        ensureHandle();
        if (handle == 0) {
            main.post(() -> listener.onXrStopped("OpenXR native library is not available"));
            return;
        }
        thread = new Thread(this::threadBody, "xr-goggle");
        // The frame loop drives the compositor; it must not lose to background work.
        thread.setPriority(Thread.MAX_PRIORITY);
        thread.start();
    }

    /** Asks the frame loop to exit. Returns immediately. */
    public void stop() {
        stopping = true;
        if (handle != 0) {
            nativeRequestStop(handle);
        }
    }

    /** Stops and joins, then releases the native object. Safe to call twice. */
    public void release() {
        stop();
        Thread t = thread;
        if (t != null) {
            try {
                t.join(2000);
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
            }
            thread = null;
        }
        if (handle != 0) {
            nativeFree(handle);
            handle = 0;
        }
    }

    private void ensureHandle() {
        if (libraryLoaded && handle == 0) {
            handle = nativeAlloc();
            // Somewhere writable for the runtime manifest the loader may need.
            nativeSetManifestDir(handle, activity.getFilesDir().getAbsolutePath());
        }
    }

    private void threadBody() {
        final long h = handle;
        if (!nativeCreate(h, activity)) {
            final String error = errorOr("OpenXR session could not be created");
            main.post(() -> listener.onXrStopped(error));
            return;
        }
        final Surface surface = nativeGetVideoSurface(h);
        if (surface == null) {
            main.post(() -> listener.onXrStopped("OpenXR gave us no video surface"));
            nativeDestroy(h);
            return;
        }
        main.post(() -> listener.onXrReady(surface));

        for (int id = 0; id < OVERLAY_COUNT; id++) {
            final Surface overlay = nativeGetOverlaySurface(h, id);
            if (overlay == null) {
                Log.w(TAG, "overlay layer " + id + " not available from this runtime");
                continue;
            }
            final int oid = id;
            final int ow = nativeGetOverlayWidth(h, id);
            final int oh = nativeGetOverlayHeight(h, id);
            main.post(() -> listener.onXrOverlayReady(oid, overlay, ow, oh));
        }

        // Blocks until stop() or an unrecoverable runtime error. Button presses arrive
        // through onXrButton() below, called from this thread.
        nativeRunLoop(h, this);

        final String error = stopping ? null : errorOr("OpenXR session ended unexpectedly");
        nativeDestroy(h);
        main.post(() -> listener.onXrStopped(error));
    }

    private String errorOr(String fallback) {
        String error = handle != 0 ? nativeLastError(handle) : null;
        return error != null ? error : fallback;
    }

    /** Called from the XR frame loop thread. */
    @Keep
    void onXrButton(int button) {
        main.post(() -> listener.onXrButton(button));
    }

    // --- live settings ---------------------------------------------------------------

    public void setVideoResolution(int width, int height) {
        if (handle != 0) nativeSetVideoResolution(handle, width, height);
    }

    public void setQuadDistance(float meters) {
        if (handle != 0) nativeSetQuadDistance(handle, meters);
    }

    public void setQuadWidth(float meters) {
        if (handle != 0) nativeSetQuadWidth(handle, meters);
    }

    public void setQuadHeightOffset(float meters) {
        if (handle != 0) nativeSetQuadHeightOffset(handle, meters);
    }

    public float quadHeightOffset() {
        return handle != 0 ? nativeGetQuadHeightOffset(handle) : 0f;
    }

    public void setPassthrough(boolean enabled) {
        if (handle != 0) nativeSetPassthrough(handle, enabled);
    }

    public void setSharpening(boolean enabled) {
        if (handle != 0) nativeSetSharpening(handle, enabled);
    }

    /**
     * Whether hand tracking may act at all. Off by default.
     *
     * <p>A pilot's hands are on a transmitter, and Meta's thumb microgestures are a thumb
     * sliding along the index finger - close enough to working a gimbal that the recogniser
     * fires on it. So hand input is something to switch on when the hands are free, not a
     * channel that is always live.
     */
    public void setHandInputEnabled(boolean enabled) {
        if (handle != 0) {
            nativeSetHandInputEnabled(handle, enabled);
        }
    }

    /** Submits or drops one overlay layer. Takes effect on the next frame. */
    public void setOverlayVisible(int id, boolean visible) {
        if (handle != 0) {
            nativeSetOverlayVisible(handle, id, visible);
        }
    }

    public void setHeadLocked(boolean enabled) {
        if (handle != 0) nativeSetHeadLocked(handle, enabled);
    }

    /** 0 leaves the runtime default alone. */
    public void requestRefreshRate(float hz) {
        if (handle != 0) nativeRequestRefreshRate(handle, hz);
    }

    public void recenter() {
        if (handle != 0) nativeRequestRecenter(handle);
    }

    public void haptic(float amplitude, int durationMs) {
        if (handle != 0) nativeHaptic(handle, amplitude, durationMs);
    }

    public float[] refreshRates() {
        return handle != 0 ? nativeGetRefreshRates(handle) : new float[0];
    }

    public float quadDistance() {
        return handle != 0 ? nativeGetQuadDistance(handle) : 0f;
    }

    public float quadWidth() {
        return handle != 0 ? nativeGetQuadWidth(handle) : 0f;
    }

    public boolean isHeadLocked() {
        return handle != 0 && nativeIsHeadLocked(handle);
    }

    public boolean isPassthroughEnabled() {
        return handle != 0 && nativeIsPassthroughEnabled(handle);
    }

    // --- native ---------------------------------------------------------------------

    private static native long nativeAlloc();

    private static native void nativeFree(long handle);

    private static native void nativeSetSwapchainSize(long handle, int width, int height);

    private static native void nativeSetManifestDir(long handle, String dir);

    private static native boolean nativeCreate(long handle, Activity activity);

    private static native Surface nativeGetVideoSurface(long handle);

    private static native Surface nativeGetOverlaySurface(long handle, int id);

    private static native int nativeGetOverlayWidth(long handle, int id);

    private static native int nativeGetOverlayHeight(long handle, int id);

    private static native void nativeSetOverlayVisible(long handle, int id, boolean visible);

    private static native void nativeSetHandInputEnabled(long handle, boolean enabled);

    private static native void nativeRunLoop(long handle, XrGoggleSession listener);

    private static native void nativeRequestStop(long handle);

    private static native void nativeDestroy(long handle);

    private static native String nativeLastError(long handle);

    private static native float[] nativeGetRefreshRates(long handle);

    private static native void nativeSetVideoResolution(long handle, int width, int height);

    private static native void nativeSetQuadDistance(long handle, float meters);

    private static native void nativeSetQuadWidth(long handle, float meters);

    private static native void nativeSetQuadHeightOffset(long handle, float meters);

    private static native float nativeGetQuadHeightOffset(long handle);

    private static native void nativeSetPassthrough(long handle, boolean enabled);

    private static native void nativeSetSharpening(long handle, boolean enabled);

    private static native void nativeSetHeadLocked(long handle, boolean enabled);

    private static native void nativeRequestRefreshRate(long handle, float hz);

    private static native void nativeRequestRecenter(long handle);

    private static native void nativeHaptic(long handle, float amplitude, int durationMs);

    private static native float nativeGetQuadDistance(long handle);

    private static native float nativeGetQuadWidth(long handle);

    private static native boolean nativeIsHeadLocked(long handle);

    private static native boolean nativeIsPassthroughEnabled(long handle);
}
