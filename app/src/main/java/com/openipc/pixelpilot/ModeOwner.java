package com.openipc.pixelpilot;

import android.app.Activity;
import android.util.Log;

import java.lang.ref.WeakReference;

/**
 * Guarantees that only one of the two presentation modes is alive at a time.
 *
 * <p>The flat and the immersive activity each construct their own {@code WfbNgLink}, and the
 * wifi adapter can only belong to one of them. Horizon OS keeps the immersive activity in its
 * own task, so switching modes left both of them resumed in separate tasks with the adapter
 * owned by neither, and no video in either mode.
 *
 * <p>Rather than trying to make two owners cooperate, whoever starts last evicts the other
 * and its {@code onDestroy()} releases the adapter. The proper answer is a single owner that
 * outlives both - a foreground service - but this removes the failure without that rework.
 */
final class ModeOwner {
    private static final String TAG = "pixelpilot";

    private static WeakReference<Activity> current = new WeakReference<>(null);

    private ModeOwner() {
    }

    /** Registers {@code activity} as the mode owner and finishes whoever held it before. */
    static synchronized void claim(Activity activity) {
        Activity previous = current.get();
        if (previous != null && previous != activity && !previous.isFinishing()) {
            Log.i(TAG, "mode switch: finishing " + previous.getClass().getSimpleName()
                    + " so " + activity.getClass().getSimpleName() + " can own the adapter");
            previous.finish();
        }
        current = new WeakReference<>(activity);
    }

    /** Clears the registration if {@code activity} still holds it. */
    static synchronized void release(Activity activity) {
        if (current.get() == activity) {
            current = new WeakReference<>(null);
        }
    }
}
