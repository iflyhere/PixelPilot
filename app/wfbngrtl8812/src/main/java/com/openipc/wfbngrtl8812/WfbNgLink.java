package com.openipc.wfbngrtl8812;

import android.content.Context;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbManager;
import android.util.Log;

import androidx.annotation.Keep;
import androidx.appcompat.app.AppCompatActivity;

import java.util.HashMap;
import java.util.Map;
import java.util.Timer;
import java.util.TimerTask;

@Keep
public class WfbNgLink implements WfbNGStatsChanged {

    // Set FEC thresholds for switching levels (Java wrapper and JNI)
    public static native void nativeSetFecThresholds(long nativeInstance, int lostTo5, int recTo4, int recTo3, int recTo2, int recTo1);

    public void setFecThresholds(int lostTo5, int recTo4, int recTo3, int recTo2, int recTo1) {
        nativeSetFecThresholds(nativeWfbngLink, lostTo5, recTo4, recTo3, recTo2, recTo1);
    }
    public static String TAG = "pixelpilot";

    // Load the native library on application startup.
    static {
        System.loadLibrary("WfbngRtl8812");
    }

    /**
     * Devices with a live RX loop anywhere in the process, keyed by device name.
     *
     * <p>Per-instance bookkeeping is not enough: each Activity owns its own WfbNgLink, so
     * two of them both see an empty map and both claim the same dongle. Keyed on the thread
     * so a claim left behind by a thread that died is simply ignored rather than locking the
     * adapter out for good.
     */
    private static final Map<String, Thread> claimedDevices = new HashMap<>();

    private static synchronized boolean claim(String deviceName, Thread t) {
        Thread holder = claimedDevices.get(deviceName);
        if (holder != null && holder.isAlive()) {
            return false;
        }
        claimedDevices.put(deviceName, t);
        return true;
    }

    private static synchronized void release(String deviceName, Thread t) {
        if (claimedDevices.get(deviceName) == t) {
            claimedDevices.remove(deviceName);
        }
    }

    private final long nativeWfbngLink;
    private final Timer timer;
    private final Context context;
    Map<UsbDevice, Thread> linkThreads = new HashMap<>();
    Map<UsbDevice, UsbDeviceConnection> linkConns = new HashMap<>();
    private WfbNGStatsChanged statsChanged;

    // Native method declarations.
    public static native long nativeInitialize(Context context);
    public static native void nativeRun(long nativeInstance, Context context, int wifiChannel, int bandWidth, int fd);
    public static native void nativeStop(long nativeInstance, Context context, int fd);
    public static native void nativeRefreshKey(long nativeInstance);
    public static native <T extends WfbNGStatsChanged> void nativeCallBack(T t, long nativeInstance);
    public static native void nativeStartAdaptivelink(long nativeInstance);
    public static native void nativeSetAdaptiveLinkEnabled(long nativeInstance, boolean enabled);
    public static native void nativeSetTxPower(long nativeInstance, int power);
    public static native void nativeSetUseFec(long nativeInstance, int use);
    public static native void nativeSetUseLdpc(long nativeInstance, int use);
    public static native void nativeSetUseStbc(long nativeInstance, int use);

    public WfbNgLink(final AppCompatActivity parent) {
        this.context = parent;
        nativeWfbngLink = nativeInitialize(context);
        timer = new Timer();
        timer.schedule(new TimerTask() {
            @Override
            public void run() {
                nativeCallBack(WfbNgLink.this, nativeWfbngLink);
            }
        }, 0, 300);
    }

    public boolean isRunning() {
        return !linkThreads.isEmpty();
    }

    public void refreshKey() {
        nativeRefreshKey(nativeWfbngLink);
    }

    // Instance wrapper for nativeSetAdaptiveLinkEnabled.
    public void nativeSetAdaptiveLinkEnabled(boolean state) {
        nativeSetAdaptiveLinkEnabled(nativeWfbngLink, state);
    }

    // Instance wrapper for nativeSetTxPower.
    public void nativeSetTxPower(int power) {
        nativeSetTxPower(nativeWfbngLink, power);
    }

    public void nativeSetUseFec(int use) {
        nativeSetUseFec(nativeWfbngLink, use);
    }

    public void nativeSetUseLdpc(int use) {
        nativeSetUseLdpc(nativeWfbngLink, use);
    }

    public void nativeSetUseStbc(int use) {
        nativeSetUseStbc(nativeWfbngLink, use);
    }

    public synchronized boolean start(int wifiChannel, int bandWidth, UsbDevice usbDevice) {
        // A second RX loop on the same adapter means two libusb handles on one interface:
        // neither gets usable video, the old thread is orphaned because linkThreads.put()
        // overwrites its entry, and stopAll() then joins only the last one - which is how
        // three live "wfb-001/002" threads and a hung main thread happen.
        Thread existing = linkThreads.get(usbDevice);
        if (existing != null && existing.isAlive()) {
            Log.w(TAG, "wfb-ng already running on " + usbDevice.getDeviceName() + ", not starting a second");
            return true;
        }
        Log.d(TAG, "wfb-ng monitoring on " + usbDevice.getDeviceName() + " using wifi channel " + wifiChannel);
        UsbManager usbManager = (UsbManager) context.getSystemService(Context.USB_SERVICE);
        // Returns null when the permission was revoked or the device disappeared between
        // the permission check and here, which is easy to hit on a re-enumerating hub.
        UsbDeviceConnection usbDeviceConnection = usbManager.openDevice(usbDevice);
        if (usbDeviceConnection == null) {
            Log.e(TAG, "Could not open " + usbDevice.getDeviceName() + " (no permission or already gone)");
            return false;
        }
        int fd = usbDeviceConnection.getFileDescriptor();
        if (fd < 0) {
            Log.e(TAG, "Invalid file descriptor for " + usbDevice.getDeviceName());
            usbDeviceConnection.close();
            return false;
        }
        final String deviceName = usbDevice.getDeviceName();
        final Thread[] self = new Thread[1];
        Thread t = new Thread(() -> {
            try {
                nativeRun(nativeWfbngLink, context, wifiChannel, bandWidth, fd);
            } finally {
                release(deviceName, self[0]);
            }
        });
        self[0] = t;
        t.setName(threadNameFor(usbDevice));
        if (!claim(deviceName, t)) {
            Log.w(TAG, "another wfb-ng RX loop already owns " + deviceName + ", not starting a second");
            usbDeviceConnection.close();
            return false;
        }
        linkThreads.put(usbDevice, t);
        linkConns.put(usbDevice, usbDeviceConnection);
        t.start();
        Log.d(TAG, "wfb-ng thread on " + deviceName + " started.");
        return true;
    }

    private static String threadNameFor(UsbDevice usbDevice) {
        String name = usbDevice.getDeviceName();
        String[] parts = name.split("/dev/bus/usb/");
        return "wfb-" + (parts.length > 1 ? parts[1] : name);
    }

    /**
     * The RX loop is joined so the USB interface is released before anything reopens it, but
     * these calls come from Activity lifecycle callbacks on the main thread. An unbounded
     * join there is a five second ANR waiting to happen, and it did: a driver thread that
     * does not come back froze the UI. Wait, but give up and say so.
     */
    private static final long JOIN_TIMEOUT_MS = 1500;

    private static void joinBounded(Thread t, String what) throws InterruptedException {
        if (t == null) {
            return;
        }
        t.join(JOIN_TIMEOUT_MS);
        if (t.isAlive()) {
            Log.e(TAG, "wfb-ng thread on " + what + " did not stop within " + JOIN_TIMEOUT_MS
                    + "ms, leaving it behind");
        } else {
            Log.d(TAG, "wfb-ng thread on " + what + " done.");
        }
    }

    public synchronized void stopAll() throws InterruptedException {
        for (Map.Entry<UsbDevice, UsbDeviceConnection> entry : linkConns.entrySet()) {
            nativeStop(nativeWfbngLink, context, entry.getValue().getFileDescriptor());
        }
        for (Map.Entry<UsbDevice, UsbDeviceConnection> entry : linkConns.entrySet()) {
            joinBounded(linkThreads.get(entry.getKey()), entry.getKey().getDeviceName());
            // The connection holds a dup of the usbfs fd. Without close() every
            // attach/detach cycle leaks one, until the process runs out.
            entry.getValue().close();
        }
        linkThreads.clear();
        linkConns.clear();
    }

    public synchronized void stop(UsbDevice dev) throws InterruptedException {
        UsbDeviceConnection conn = linkConns.get(dev);
        if (conn == null) {
            return;
        }
        int fd = conn.getFileDescriptor();
        nativeStop(nativeWfbngLink, context, fd);
        joinBounded(linkThreads.get(dev), dev.getDeviceName());
        linkThreads.remove(dev);
        linkConns.remove(dev);
        conn.close();
    }

    public void SetWfbNGStatsChanged(final WfbNGStatsChanged callback) {
        statsChanged = callback;
    }

    // Called by native code via NDK.
    @Override
    public void onWfbNgStatsChanged(WfbNGStats stats) {
        if (statsChanged != null) {
            statsChanged.onWfbNgStatsChanged(stats);
        }
    }
}
