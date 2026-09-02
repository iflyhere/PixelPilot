package com.openipc.pixelpilot;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.usb.UsbManager;
import android.os.Binder;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.util.Log;
import android.view.Surface;

import androidx.annotation.Nullable;

import com.openipc.videonative.DecodingInfo;
import com.openipc.videonative.IVideoParamsChanged;
import com.openipc.videonative.VideoPlayer;
import com.openipc.wfbngrtl8812.WfbNGStats;
import com.openipc.wfbngrtl8812.WfbNGStatsChanged;
import com.openipc.wfbngrtl8812.WfbNgLink;

import java.util.concurrent.CopyOnWriteArrayList;

/**
 * Sole owner of the wifi adapter, the wfb-ng link and the decoder.
 *
 * <p>These used to belong to whichever Activity was on screen, which does not survive contact
 * with a ground station: the flat activity tore the link down in {@code onPause()} (so
 * glancing at a system menu cost seconds of black screen), and with a second presentation
 * mode there were two owners for one adapter - both activities ended up alive in separate
 * tasks with the adapter owned by neither, and no video in either mode.
 *
 * <p>Now the link outlives the activities. They bind, register as a {@link Client}, and hand
 * over a Surface; switching modes only swaps the Surface, so the adapter is never reopened
 * and the USB permission dialog is not asked again. The service is a foreground service
 * because it must keep running while the pilot is looking at something else.
 */
public class LinkService extends Service implements LinkStatusView, IVideoParamsChanged, WfbNGStatsChanged {

    public static final String ACTION_STOP = "com.openipc.pixelpilot.LINK_STOP";

    private static final String TAG = "pixelpilot";
    private static final String CHANNEL_ID = "link";
    private static final int NOTIFICATION_ID = 1;

    /**
     * Grace period after the last client unbinds. A mode switch unbinds one activity and
     * binds the next, and tearing the link down in between is exactly what this service
     * exists to avoid.
     */
    private static final long IDLE_SHUTDOWN_MS = 20_000;

    /** What an attached activity gets told. All callbacks arrive on the main thread. */
    public interface Client {
        void onLinkStatus(String message);

        void onLocalStreamHint(String url);

        void onVideoResolution(int width, int height);

        void onDecodingInfo(DecodingInfo info);

        void onWfbStats(WfbNGStats stats);
    }

    public class LocalBinder extends Binder {
        public LinkService service() {
            return LinkService.this;
        }
    }

    private final LocalBinder binder = new LocalBinder();
    private final Handler main = new Handler(Looper.getMainLooper());
    private final CopyOnWriteArrayList<Client> clients = new CopyOnWriteArrayList<>();

    private WfbNgLink wfbLink;
    private WfbLinkManager wfbLinkManager;
    private VideoPlayer videoPlayer;

    private boolean ready;
    private boolean usbReceiverRegistered;
    private String lastStatus = "";
    private WfbNGStats lastStats;
    private int videoWidth;
    private int videoHeight;

    private final Runnable idleShutdown = () -> {
        Log.i(TAG, "no client came back, stopping the link service");
        stopSelf();
    };

    // ------------------------------------------------------------------------------
    // lifecycle
    // ------------------------------------------------------------------------------

    @Override
    public void onCreate() {
        super.onCreate();
        startForegroundNotification();

        // The native aggregators open files/gs.key in their constructor and throw if it is
        // missing, so this has to happen before WfbNgLink exists.
        if (!GsKey.ensure(this)) {
            Log.e(TAG, "no usable gs.key, the link cannot come up");
        }

        wfbLink = new WfbNgLink(this);
        wfbLink.SetWfbNGStatsChanged(this);
        // Not read by the native side on its own: adaptive link, TX power, FEC/LDPC/STBC.
        WfbOptions.applyDefaults(this, wfbLink);

        wfbLinkManager = new WfbLinkManager(this, this, wfbLink);

        videoPlayer = new VideoPlayer(this);
        videoPlayer.setIVideoParamsChanged(this);
        videoPlayer.setLowLatency(VideoActivity.getLowLatencySetting(this));
        // Receivers come up once and stay up; only the output surface changes with the mode.
        videoPlayer.start();
        videoPlayer.startAudio();

        registerUsbReceiver();

        ready = true;
        Log.i(TAG, "link service up");
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && ACTION_STOP.equals(intent.getAction())) {
            Log.i(TAG, "stop requested from the notification");
            stopSelf();
            return START_NOT_STICKY;
        }
        return START_STICKY;
    }

    @Nullable
    @Override
    public IBinder onBind(Intent intent) {
        main.removeCallbacks(idleShutdown);
        return binder;
    }

    @Override
    public void onRebind(Intent intent) {
        main.removeCallbacks(idleShutdown);
    }

    @Override
    public boolean onUnbind(Intent intent) {
        if (clients.isEmpty()) {
            main.postDelayed(idleShutdown, IDLE_SHUTDOWN_MS);
        }
        // Allow onRebind, which is how a mode switch keeps the link alive.
        return true;
    }

    @Override
    public void onDestroy() {
        Log.i(TAG, "link service going down");
        main.removeCallbacks(idleShutdown);
        ready = false;
        if (usbReceiverRegistered) {
            try {
                unregisterReceiver(wfbLinkManager);
            } catch (IllegalArgumentException ignored) {
            }
            usbReceiverRegistered = false;
        }
        if (wfbLinkManager != null) {
            wfbLinkManager.stopAdapters();
        }
        if (videoPlayer != null) {
            videoPlayer.stopAudio();
            videoPlayer.stop();
        }
        super.onDestroy();
    }

    /**
     * WfbLinkManager is itself the USB attach/detach receiver, and it has to be registered by
     * whoever owns the adapter - previously an Activity, which meant plug events were missed
     * whenever no activity was around.
     */
    private void registerUsbReceiver() {
        IntentFilter filter = new IntentFilter();
        filter.addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED);
        filter.addAction(UsbManager.ACTION_USB_DEVICE_DETACHED);
        filter.addAction(WfbLinkManager.ACTION_USB_PERMISSION);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(wfbLinkManager, filter, Context.RECEIVER_NOT_EXPORTED);
        } else {
            registerReceiver(wfbLinkManager, filter);
        }
        usbReceiverRegistered = true;
    }

    private void startForegroundNotification() {
        NotificationManager manager = getSystemService(NotificationManager.class);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O && manager != null
                && manager.getNotificationChannel(CHANNEL_ID) == null) {
            NotificationChannel channel = new NotificationChannel(
                    CHANNEL_ID, "Video link", NotificationManager.IMPORTANCE_LOW);
            channel.setShowBadge(false);
            manager.createNotificationChannel(channel);
        }

        Intent stop = new Intent(this, LinkService.class).setAction(ACTION_STOP);
        PendingIntent stopIntent = PendingIntent.getService(
                this, 0, stop, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);

        Notification notification = new Notification.Builder(this, CHANNEL_ID)
                .setContentTitle("PixelPilot link")
                .setContentText("Holding the wifi adapter and decoder")
                .setSmallIcon(R.drawable.baseline_settings_24)
                .setOngoing(true)
                .addAction(new Notification.Action.Builder(null, "Stop", stopIntent).build())
                .build();

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(NOTIFICATION_ID, notification,
                    android.content.pm.ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
        } else {
            startForeground(NOTIFICATION_ID, notification);
        }
    }

    // ------------------------------------------------------------------------------
    // client API
    // ------------------------------------------------------------------------------

    public boolean isReady() {
        return ready;
    }

    /** Registers a client and replays the state it missed, so it never starts out blank. */
    public void addClient(Client client) {
        clients.addIfAbsent(client);
        main.removeCallbacks(idleShutdown);
        if (!lastStatus.isEmpty()) {
            client.onLinkStatus(lastStatus);
        }
        if (videoWidth > 0 && videoHeight > 0) {
            client.onVideoResolution(videoWidth, videoHeight);
        }
        WfbNGStats stats = lastStats;
        if (stats != null) {
            client.onWfbStats(stats);
        }
    }

    public void removeClient(Client client) {
        clients.remove(client);
    }

    /** Points the decoder at a new output surface. The link keeps running throughout. */
    public void attachSurface(Surface surface, int index) {
        if (videoPlayer != null) {
            videoPlayer.attachSurface(surface, index);
        }
    }

    public void detachSurface(int index) {
        if (videoPlayer != null) {
            videoPlayer.detachSurface(index);
        }
    }

    /** Brings the adapter up if it is not already, using the persisted channel/bandwidth. */
    public void ensureAdapters() {
        if (wfbLinkManager == null) {
            return;
        }
        wfbLinkManager.setChannel(VideoActivity.getChannel(this));
        wfbLinkManager.setBandwidth(VideoActivity.getBandwidth(this));
        wfbLinkManager.refreshAdapters();
    }

    public boolean hasActiveAdapter() {
        return wfbLinkManager != null && wfbLinkManager.hasActiveAdapter();
    }

    public void setChannel(int channel) {
        if (wfbLinkManager == null) {
            return;
        }
        wfbLinkManager.stopAdapters();
        wfbLinkManager.setChannel(channel);
        wfbLinkManager.startAdapters();
    }

    public void setBandwidth(int bandwidth) {
        if (wfbLinkManager == null) {
            return;
        }
        wfbLinkManager.stopAdapters();
        wfbLinkManager.setBandwidth(bandwidth);
        wfbLinkManager.startAdapters();
    }

    public void refreshKey() {
        if (wfbLinkManager != null) {
            wfbLinkManager.refreshKey();
        }
    }

    /** For the settings menus that drive the native link options directly. */
    @Nullable
    public WfbNgLink link() {
        return wfbLink;
    }

    public void startDvr(int fd, boolean fragmentedMp4) {
        if (videoPlayer != null) {
            videoPlayer.startDvr(fd, fragmentedMp4);
        }
    }

    public void stopDvr() {
        if (videoPlayer != null) {
            videoPlayer.stopDvr();
        }
    }

    public void setUdpForwarding(String ip, int port, boolean enabled) {
        if (videoPlayer != null) {
            videoPlayer.setUdpForwarding(ip, port, enabled);
        }
    }

    public void setLowLatency(boolean enabled) {
        if (videoPlayer != null) {
            videoPlayer.setLowLatency(enabled);
        }
    }

    public int videoWidth() {
        return videoWidth;
    }

    public int videoHeight() {
        return videoHeight;
    }

    // ------------------------------------------------------------------------------
    // fan-out to the attached activity
    // ------------------------------------------------------------------------------

    @Override
    public void showLinkMessage(String message) {
        Log.i(TAG, "link: " + message);
        lastStatus = message;
        main.post(() -> {
            for (Client c : clients) {
                c.onLinkStatus(message);
            }
        });
    }

    @Override
    public void showLocalStreamHint(String url) {
        main.post(() -> {
            for (Client c : clients) {
                c.onLocalStreamHint(url);
            }
        });
    }

    @Override
    public void onVideoRatioChanged(int width, int height) {
        videoWidth = width;
        videoHeight = height;
        main.post(() -> {
            for (Client c : clients) {
                c.onVideoResolution(width, height);
            }
        });
    }

    @Override
    public void onDecodingInfoChanged(DecodingInfo decodingInfo) {
        main.post(() -> {
            for (Client c : clients) {
                c.onDecodingInfo(decodingInfo);
            }
        });
    }

    @Override
    public void onWfbNgStatsChanged(WfbNGStats stats) {
        lastStats = stats;
        main.post(() -> {
            for (Client c : clients) {
                c.onWfbStats(stats);
            }
        });
    }
}
