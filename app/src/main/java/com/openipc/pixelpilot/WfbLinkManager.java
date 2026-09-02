package com.openipc.pixelpilot;

import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbManager;
import android.util.Log;

import com.openipc.wfbngrtl8812.WfbNgLink;

import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Map;

public class WfbLinkManager extends BroadcastReceiver {
    public static final String ACTION_USB_PERMISSION = "com.openipc.pixelpilot.USB_PERMISSION";
    private static final String TAG = "pixelpilot";
    // Not static: it used to be, and with two activities each owning its own WfbNgLink
    // that shared map let both of them decide the adapter still needed starting, so the
    // same dongle ended up with several RX loops.
    private final Map<String, UsbDevice> activeWifiAdapters = new HashMap<>();
    private final WfbNgLink wfbLink;
    private final LinkStatusView status;
    private final Context context;
    private int wifiChannel;
    private Bandwidth bandWidth;

    public enum Bandwidth {
        BANDWIDTH_20(20),
        BANDWIDTH_40(40);

        private final int value;

        Bandwidth(int value) {
            this.value = value;
        }

        public int getValue() {
            return value;
        }
    }

    public WfbLinkManager(Context context, LinkStatusView status, WfbNgLink wfbNgLink) {
        this.status = status;
        this.context = context;
        this.wfbLink = wfbNgLink;
    }

    public void refreshKey() {
        wfbLink.refreshKey();
    }

    public void setChannel(int channel) {
        wifiChannel = channel;
    }
    public void setBandwidth(int bw) {
        switch(bw)
        {
            case 20:
                bandWidth = Bandwidth.BANDWIDTH_20;
                break;
            case 40:
                bandWidth = Bandwidth.BANDWIDTH_40;
                break;
            default:
                break;
        }
    }

    @Override
    public synchronized void onReceive(Context context, Intent intent) {
        UsbDevice dev = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
        if (android.hardware.usb.UsbManager.ACTION_USB_DEVICE_DETACHED.equals(intent.getAction())) {
            if (dev == null) {
                return;
            }
            Log.d(TAG, "usb device detached: " + dev.getVendorId() + "/" + dev.getProductId());
            refreshAdapters();
        } else if (android.hardware.usb.UsbManager.ACTION_USB_DEVICE_ATTACHED.equals(intent.getAction())) {
            if (dev == null) {
                return;
            }
            Log.d(TAG, "usb device attached: " + dev.getVendorId() + "/" + dev.getProductId());
            // No need to refresh since this should trigger a call to VideoActivity.onReceive();
        } else if (ACTION_USB_PERMISSION.equals(intent.getAction())) {
            Log.d(TAG, "Permission handled");
        }
    }

    public Map<String, UsbDevice> getAttachedAdapters() {
        android.hardware.usb.UsbManager manager =
                (android.hardware.usb.UsbManager) context.getSystemService(Context.USB_SERVICE);

        List<UsbDeviceFilter> filters;
        try {
            filters = UsbDeviceFilter.parseXml(context, R.xml.usb_device_filter);
        } catch (Exception e) {
            e.printStackTrace();
            return null;
        }

        Map<String, UsbDevice> res = new HashMap<>();
        Map<String, UsbDevice> attached = manager.getDeviceList();
        // Without this there is no way to tell "nothing is plugged in" from "the dongle is
        // there but its id is not in usb_device_filter.xml", and the id is what a bug report
        // needs in order to add it.
        Log.i(TAG, "usb devices attached: " + attached.size());
        for (UsbDevice dev : attached.values()) {
            boolean allowed = false;
            for (UsbDeviceFilter filter : filters) {
                if (filter.productId == dev.getProductId() && filter.vendorId == dev.getVendorId()) {
                    allowed = true;
                    break;
                }
            }
            Log.i(TAG, String.format("  %s  %04X:%04X  %s %s  -> %s",
                    dev.getDeviceName(),
                    dev.getVendorId(),
                    dev.getProductId(),
                    String.valueOf(dev.getManufacturerName()),
                    String.valueOf(dev.getProductName()),
                    allowed ? "supported" : "NOT in usb_device_filter.xml"));
            if (!allowed) {
                continue;
            }
            res.put(dev.getDeviceName(), dev);
        }
        return res;
    }

    public synchronized void refreshAdapters() {
        Map<String, UsbDevice> attachedAdapters = getAttachedAdapters();
        if (attachedAdapters == null) {
            Log.e(TAG, "Could not read the usb device filter, skipping adapter refresh.");
            return;
        }

        boolean missingPermissions = false;
        android.hardware.usb.UsbManager usbManager =
                (android.hardware.usb.UsbManager) context.getSystemService(Context.USB_SERVICE);
        for (Map.Entry<String, UsbDevice> entry : attachedAdapters.entrySet()) {
            if (!usbManager.hasPermission(entry.getValue())) {
                status.showLinkMessage("No permission for wifi adapter(s) " + entry.getValue().getDeviceName());
                // Android 14 refuses to deliver a PendingIntent built from an implicit
                // intent to a runtime registered receiver, so the permission result never
                // arrives unless the package is set explicitly.
                Intent permissionIntent = new Intent(WfbLinkManager.ACTION_USB_PERMISSION);
                permissionIntent.setPackage(context.getPackageName());
                PendingIntent pendingIntent = PendingIntent.getBroadcast(context, 0,
                        permissionIntent, PendingIntent.FLAG_IMMUTABLE);
                usbManager.requestPermission(entry.getValue(), pendingIntent);
                missingPermissions = true;
            }
        }

        if (missingPermissions) {
            return;
        }

        // Stops newly detached adapters.
        Iterator<Map.Entry<String, UsbDevice>> iterator = activeWifiAdapters.entrySet().iterator();
        while (iterator.hasNext()) {
            Map.Entry<String, UsbDevice> entry = iterator.next();
            if (attachedAdapters.containsKey(entry.getKey())) {
                continue;
            }
            stopAdapter(entry.getValue());
            iterator.remove();
        }

        // Starts newly attached adapters.
        boolean startFailed = false;
        for (Map.Entry<String, UsbDevice> entry : attachedAdapters.entrySet()) {
            if (activeWifiAdapters.containsKey(entry.getKey())) {
                continue;
            }
            // Only track it as active if it actually came up, otherwise a failed adapter
            // is never retried on the next refresh.
            if (startAdapter(entry.getValue())) {
                activeWifiAdapters.put(entry.getKey(), entry.getValue());
            } else {
                startFailed = true;
            }
        }

        if (activeWifiAdapters.isEmpty()) {
            // These are three different problems and telling the pilot the wrong one costs
            // a lot of pointless searching.
            if (startFailed) {
                status.showLinkMessage("Wifi adapter found but could not be started - see the log.");
            } else if (attachedAdapters.isEmpty()) {
                status.showLinkMessage("No compatible wifi adapter found.");
            } else {
                status.showLinkMessage("Waiting for wifi adapter permission…");
            }

            String wifi = VideoActivity.wirelessInfo(context);
            if (wifi != null) {
                status.showLocalStreamHint("udp://" + wifi + ":5600");
            }
        }
    }

    public synchronized void stopAdapters() {
        try {
            wfbLink.stopAll();
        } catch (InterruptedException ignored) {
        }
    }

    public synchronized void stopAdapter(UsbDevice dev) {
        try {
            wfbLink.stop(dev);
        } catch (InterruptedException e) {
            e.printStackTrace();
        }
    }

    public synchronized void startAdapters() {
        if (wfbLink.isRunning()) {
            return;
        }
        for (Map.Entry<String, UsbDevice> entry : activeWifiAdapters.entrySet()) {
            if (!startAdapter(entry.getValue())) {
                break;
            }
        }
    }

    public synchronized boolean startAdapter(UsbDevice dev) {
        status.showLinkMessage("Starting wfb-ng channel " + wifiChannel + " with " + String.format(
                "[%04X", dev.getVendorId()) + ":" + String.format("%04X]", dev.getProductId()));
        if (!wfbLink.start(wifiChannel, bandWidth.getValue(), dev)) {
            status.showLinkMessage("Could not open wifi adapter " + dev.getDeviceName());
            return false;
        }
        return true;
    }
}
