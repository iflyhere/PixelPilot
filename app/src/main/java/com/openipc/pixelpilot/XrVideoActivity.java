package com.openipc.pixelpilot;

import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.hardware.usb.UsbManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.util.Log;
import android.view.Gravity;
import android.view.Surface;
import android.view.View;
import android.view.WindowManager;
import android.widget.TextView;
import android.widget.Toast;

import androidx.annotation.Nullable;

import com.openipc.mavlink.MavlinkData;
import com.openipc.wfbngrtl8812.WfbNGStats;
import com.openipc.xr.XrGoggleSession;

import java.io.IOException;

/**
 * Immersive ground station.
 *
 * <p>Instead of a 2D panel that the headset compositor resamples onto a quad, the decoder
 * writes straight into a compositor swapchain
 * ({@code xrCreateSwapchainAndroidSurfaceKHR}) which is presented as a head locked quad
 * layer. That removes the view hierarchy, SurfaceFlinger and the panel's fixed resolution
 * from the video path.
 *
 * <p>The adapter, the wfb-ng link and the decoder belong to {@link LinkService}, not to this
 * activity. Entering and leaving immersive mode therefore only swaps the decoder's output
 * surface - the adapter is never reopened, so there is no reconnect delay and no second USB
 * permission prompt.
 *
 * <p>There is no in-headset menu yet: channel, bandwidth and the DVR folder are still
 * configured in {@link VideoActivity} before putting the headset on. Controllers cover
 * what matters in flight.
 */
public class XrVideoActivity extends LinkClientActivity implements XrGoggleSession.Listener {

    private static final String TAG = "pixelpilot-xr";

    /**
     * Enables or disables this activity's launcher entry to match the device.
     *
     * <p>The entry is what lets Horizon OS start the app straight into VR, but the manifest
     * cannot express "only on a device with an XR runtime", so it ships disabled and is
     * switched on here. On a phone it stays off and nothing about the flat mode changes -
     * no second icon, no way to land on an activity that cannot create a session.
     *
     * <p>Enabling the component is also what makes {@code startActivity} on it work at all,
     * so this has to run before VideoActivity's menu can offer it - hence {@code onCreate}
     * rather than when the menu is opened. Both are gated on the same predicate, so a device
     * {@link XrGoggleSession#isSupportedDevice} does not recognise sees neither the entry nor
     * the menu; if that ever turns out to be a false negative, widening that check is the fix.
     */
    static void syncLauncherEntry(Context context) {
        boolean wantEnabled = XrGoggleSession.isSupportedDevice(context);
        ComponentName component = new ComponentName(context, XrVideoActivity.class);
        PackageManager pm = context.getPackageManager();
        try {
            // COMPONENT_ENABLED_STATE_DEFAULT means "whatever the manifest said", which here
            // is disabled - so anything other than an explicit ENABLED counts as off.
            boolean isEnabled = pm.getComponentEnabledSetting(component)
                    == PackageManager.COMPONENT_ENABLED_STATE_ENABLED;
            if (isEnabled == wantEnabled) {
                return;
            }
            pm.setComponentEnabledSetting(
                    component,
                    wantEnabled
                            ? PackageManager.COMPONENT_ENABLED_STATE_ENABLED
                            // Back to the manifest value rather than an explicit disable, so
                            // an unrecognised device is left exactly as it shipped.
                            : PackageManager.COMPONENT_ENABLED_STATE_DEFAULT,
                    PackageManager.DONT_KILL_APP);
            Log.i(TAG, "immersive launcher entry " + (wantEnabled ? "enabled" : "disabled"));
        } catch (Exception e) {
            // A launcher entry is not worth taking the flat mode down over.
            Log.w(TAG, "could not update the immersive launcher entry", e);
        }
    }

    // Preference keys, all in the shared "general" store so the flat activity's settings
    // menu can write them.
    static final String PREF_HEAD_LOCKED = "xr_head_locked";
    static final String PREF_PASSTHROUGH = "xr_passthrough";
    static final String PREF_SHARPENING = "xr_sharpening";
    static final String PREF_REFRESH_HZ = "xr_refresh_hz";
    static final String PREF_QUAD_WIDTH = "xr_quad_width";
    static final String PREF_QUAD_DISTANCE = "xr_quad_distance";
    static final String PREF_QUAD_HEIGHT = "xr_quad_height";
    static final String PREF_LAST_VIDEO_W = "xr_last_video_width";
    static final String PREF_LAST_VIDEO_H = "xr_last_video_height";

    /**
     * Whether an immersive session owns the link right now. The USB_DEVICE_ATTACHED filter
     * lives on VideoActivity, so replugging the adapter would otherwise start the flat
     * activity alongside a running immersive session.
     */
    private static volatile boolean active;

    static boolean isActive() {
        return active;
    }

    private final Handler handler = new Handler(Looper.getMainLooper());

    private XrGoggleSession xr;
    @Nullable
    private XrHud hud;
    private TextView statusView;

    // The flat activity may still be releasing the USB interface when we get here, so the
    // adapter is retried for a while rather than given up on after one attempt.
    private static final int LINK_RETRY_INTERVAL_MS = 700;
    private static final int LINK_RETRY_LIMIT = 20;

    private int linkRetries;
    private boolean xrStarting;
    private boolean linkStarted;
    private Surface xrSurface;
    private volatile String lastStatus = "";
    private boolean videoSeen;
    private boolean statusScreenUsable = true;
    private ParcelFileDescriptor dvrFd;

    // Link quality haptics
    private long lastHapticAt;
    private boolean linkWasBad;
    private volatile WfbNGStats lastStats;
    private long lastStatsLogAt;

    private final BroadcastReceiver usbReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            // Permission granted / adapter (re)appeared - re-evaluate whether we can go
            // immersive now.
            handler.post(XrVideoActivity.this::tryBringUpLink);
        }
    };

    // ------------------------------------------------------------------------------
    // lifecycle
    // ------------------------------------------------------------------------------

    @Override
    protected void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        active = true;
        // Evict the flat activity: it owns a WfbNgLink too, and the adapter can only belong
        // to one of them.
        ModeOwner.claim(this);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        // Only shown while still flat: USB permission, errors, "waiting for adapter".
        statusView = new TextView(this);
        statusView.setTextColor(Color.WHITE);
        statusView.setBackgroundColor(Color.BLACK);
        statusView.setTextSize(22f);
        statusView.setGravity(Gravity.CENTER);
        statusView.setText(R.string.xr_starting);
        setContentView(statusView);

        if (!XrGoggleSession.isLibraryLoaded()) {
            failToFlat("This build has no OpenXR support");
            return;
        }

        xr = new XrGoggleSession(this, this);
        SharedPreferences prefs = prefs();
        // Match the swapchain to the stream so the compositor never resamples twice. The
        // resolution from the last session is a good guess; 1080p until we have seen one.
        xr.setSwapchainSize(prefs.getInt(PREF_LAST_VIDEO_W, 1920), prefs.getInt(PREF_LAST_VIDEO_H, 1080));
        xr.setHeadLocked(prefs.getBoolean(PREF_HEAD_LOCKED, true));
        xr.setPassthrough(prefs.getBoolean(PREF_PASSTHROUGH, false));
        xr.setSharpening(prefs.getBoolean(PREF_SHARPENING, true));
        xr.setQuadWidth(prefs.getFloat(PREF_QUAD_WIDTH, 2.2f));
        xr.setQuadDistance(prefs.getFloat(PREF_QUAD_DISTANCE, 1.6f));
        xr.setQuadHeightOffset(prefs.getFloat(PREF_QUAD_HEIGHT, 0f));
    }

    @Override
    protected void onLinkServiceConnected(LinkService service) {
        if (xr == null) {
            return;
        }
        registerUsbReceiver();
        tryBringUpLink();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (xr != null && link != null) {
            tryBringUpLink();
        }
    }

    // Deliberately no teardown in onPause/onStop. In an immersive session the runtime
    // pauses and resumes us for its own reasons (system menu, proximity sensor), and
    // dropping the USB link there costs seconds of black screen on the way back.

    @Override
    protected void onDestroy() {
        active = false;
        ModeOwner.release(this);
        persistGeometry();
        try {
            unregisterReceiver(usbReceiver);
        } catch (IllegalArgumentException ignored) {
            // never registered
        }
        stopDvr();
        // Hand the decoder's surface back but leave the link running: the flat activity is
        // about to take over, and the whole point of the service is that the adapter stays
        // open across the switch.
        if (link != null) {
            link.detachSurface(0);
        }
        if (hud != null) {
            hud.stop();
            hud = null;
        }
        if (xr != null) {
            xr.release();
            xr = null;
        }
        super.onDestroy();
    }

    private SharedPreferences prefs() {
        return getSharedPreferences("general", Context.MODE_PRIVATE);
    }

    private void registerUsbReceiver() {
        IntentFilter filter = new IntentFilter();
        filter.addAction(WfbLinkManager.ACTION_USB_PERMISSION);
        filter.addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED);
        filter.addAction(UsbManager.ACTION_USB_DEVICE_DETACHED);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(usbReceiver, filter, Context.RECEIVER_NOT_EXPORTED);
        } else {
            registerReceiver(usbReceiver, filter);
        }
    }

    // ------------------------------------------------------------------------------
    // bring-up
    // ------------------------------------------------------------------------------

    /**
     * Resolves USB permission while still on a flat panel - a permission dialog is far
     * easier to deal with before the compositor takes over - and only then starts the
     * immersive session.
     */
    private void tryBringUpLink() {
        if (xr == null || link == null) {
            return;
        }
        link.ensureAdapters();

        // Going immersive does not depend on the link, and the pilot is better off seeing the
        // status screen with a reason than a flat activity that says nothing.
        if (!xrStarting) {
            xrStarting = true;
            statusView.setText(R.string.xr_entering);
            xr.start();
        }

        // The adapter may still be waiting for a USB permission the service asked for, so
        // keep checking back rather than giving up on the first look.
        if (!link.hasActiveAdapter() && linkRetries < LINK_RETRY_LIMIT) {
            linkRetries++;
            Log.i(TAG, "adapter not up yet, retry " + linkRetries + "/" + LINK_RETRY_LIMIT);
            handler.postDelayed(this::tryBringUpLink, LINK_RETRY_INTERVAL_MS);
        } else if (link.hasActiveAdapter()) {
            linkRetries = 0;
        }
    }

    @Override
    public void onXrReady(Surface videoSurface) {
        Log.i(TAG, "immersive session up, handing the swapchain surface to the decoder");
        xrSurface = videoSurface;
        // Put something on the layer before the decoder owns it. Without this the panel is
        // an opaque black rectangle until the first frame arrives, which is
        // indistinguishable from "nothing is being composited at all".
        drawStatusScreen();
        // The decoder writes into the compositor swapchain from here on. Only the surface
        // changes - the receivers and the adapter keep running in the service.
        if (link != null) {
            link.attachSurface(videoSurface, 0);
            if (link.videoWidth() > 0) {
                xr.setVideoResolution(link.videoWidth(), link.videoHeight());
            }
        }
        applyRefreshRatePreference();
        statusView.setVisibility(View.GONE);
        linkStarted = true;
    }

    private void applyRefreshRatePreference() {
        float wanted = prefs().getFloat(PREF_REFRESH_HZ, 0f);
        if (wanted <= 0f) {
            // "Auto" means the highest the headset offers: at 60 fps video a higher
            // display rate only makes the cadence more even.
            for (float hz : xr.refreshRates()) {
                wanted = Math.max(wanted, hz);
            }
        }
        if (wanted > 0f) {
            xr.requestRefreshRate(wanted);
        }
    }

    @Override
    public void onXrButton(int button) {
        switch (button) {
            case XrGoggleSession.BUTTON_RECENTER:
                // Applied natively already, nothing to persist.
                break;
            case XrGoggleSession.BUTTON_PASSTHROUGH:
                prefs().edit().putBoolean(PREF_PASSTHROUGH, xr.isPassthroughEnabled()).apply();
                break;
            case XrGoggleSession.BUTTON_LOCK_MODE:
                prefs().edit().putBoolean(PREF_HEAD_LOCKED, xr.isHeadLocked()).apply();
                break;
            case XrGoggleSession.BUTTON_RECORD:
                toggleDvr();
                break;
            case XrGoggleSession.BUTTON_EXIT:
                leaveVr();
                break;
            case XrGoggleSession.BUTTON_RAISE:
            case XrGoggleSession.BUTTON_LOWER:
                prefs().edit().putFloat(PREF_QUAD_HEIGHT, xr.quadHeightOffset()).apply();
                break;
            default:
                break;
        }
    }

    @Override
    public void onXrHudReady(Surface hudSurface) {
        Log.i(TAG, "hud layer up");
        hud = new XrHud(hudSurface, XrGoggleSession.HUD_WIDTH, XrGoggleSession.HUD_HEIGHT);
        hud.setNote(dvrFd != null ? "REC" : "");
        hud.start();
        if (link != null) {
            // Replay whatever the service already has, so the instruments are populated
            // before the next telemetry packet rather than a second later.
            WfbNGStats stats = lastStats;
            if (stats != null) {
                hud.onLink(stats);
            }
        }
    }

    @Override
    public void onMavlink(MavlinkData data) {
        XrHud h = hud;
        if (h != null) {
            h.onTelemetry(data);
        }
    }

    @Override
    public void onXrStopped(@Nullable String error) {
        if (error != null) {
            Log.e(TAG, "immersive session failed: " + error);
            failToFlat(error);
        } else {
            finish();
        }
    }

    /**
     * Back to the flat activity. Cheap now that the link lives in the service: the adapter
     * stays open and video keeps running, only the presentation changes.
     */
    private void leaveVr() {
        Log.i(TAG, "leaving immersive mode on request");
        startActivity(new Intent(this, VideoActivity.class));
        finish();
    }

    /** Immersive mode is not usable here - say why and go back to the flat activity. */
    private void failToFlat(String reason) {
        Toast.makeText(this, "VR mode unavailable: " + reason, Toast.LENGTH_LONG).show();
        if (statusView != null) {
            statusView.setVisibility(View.VISIBLE);
            statusView.setText(getString(R.string.xr_failed, reason));
        }
        handler.postDelayed(() -> {
            startActivity(new Intent(this, VideoActivity.class));
            finish();
        }, 2500);
    }

    private void persistGeometry() {
        if (xr == null) {
            return;
        }
        prefs().edit()
                .putFloat(PREF_QUAD_WIDTH, xr.quadWidth())
                .putFloat(PREF_QUAD_DISTANCE, xr.quadDistance())
                .putFloat(PREF_QUAD_HEIGHT, xr.quadHeightOffset())
                .putBoolean(PREF_HEAD_LOCKED, xr.isHeadLocked())
                .putBoolean(PREF_PASSTHROUGH, xr.isPassthroughEnabled())
                .apply();
    }

    // ------------------------------------------------------------------------------
    // "no signal" screen
    // ------------------------------------------------------------------------------

    /**
     * Paints status straight into the compositor swapchain's Surface.
     *
     * <p>Only valid until MediaCodec becomes the producer for that Surface - a buffer queue
     * has one producer - so this stops as soon as a frame has been decoded, and gives up
     * quietly if the canvas is refused.
     */
    private void drawStatusScreen() {
        if (videoSeen || !statusScreenUsable || xrSurface == null || !xrSurface.isValid()) {
            return;
        }
        Canvas canvas = null;
        try {
            canvas = xrSurface.lockHardwareCanvas();
            if (canvas == null) {
                statusScreenUsable = false;
                return;
            }
            paintStatus(canvas);
            xrSurface.unlockCanvasAndPost(canvas);
        } catch (Throwable t) {
            // Most likely the decoder already connected as the producer.
            Log.w(TAG, "status screen unavailable: " + t);
            statusScreenUsable = false;
            return;
        }
        handler.postDelayed(this::drawStatusScreen, 1000);
    }

    private void paintStatus(Canvas canvas) {
        final int w = canvas.getWidth();
        final int h = canvas.getHeight();

        canvas.drawColor(Color.rgb(12, 12, 16));

        Paint line = new Paint();
        line.setColor(Color.rgb(70, 80, 100));
        line.setStrokeWidth(Math.max(2f, h / 300f));
        line.setStyle(Paint.Style.STROKE);
        // A border and a centre cross: makes the panel's real extent and centre visible, so
        // size and distance can be judged with nothing else on screen.
        float inset = h / 40f;
        canvas.drawRect(inset, inset, w - inset, h - inset, line);
        float cx = w / 2f, cy = h / 2f, arm = h / 20f;
        canvas.drawLine(cx - arm, cy, cx + arm, cy, line);
        canvas.drawLine(cx, cy - arm, cx, cy + arm, line);

        Paint title = new Paint();
        title.setAntiAlias(true);
        title.setColor(Color.rgb(235, 235, 240));
        title.setTextSize(h / 14f);
        title.setTextAlign(Paint.Align.CENTER);
        canvas.drawText("No video", cx, cy - h / 8f, title);

        Paint body = new Paint();
        body.setAntiAlias(true);
        body.setColor(Color.rgb(170, 180, 195));
        body.setTextSize(h / 28f);
        body.setTextAlign(Paint.Align.CENTER);

        String status = lastStatus;
        float y = cy + h / 7f;
        if (!status.isEmpty()) {
            canvas.drawText(status, cx, y, body);
            y += h / 20f;
        }
        // The link counters answer the first question a pilot has when the screen stays
        // empty: is anything arriving at all?
        WfbNGStats stats = lastStats;
        if (stats != null) {
            canvas.drawText("wfb packets " + stats.count_p_all
                            + "   ok " + stats.count_p_dec_ok
                            + "   lost " + stats.count_p_lost
                            + "   err " + stats.count_p_dec_err,
                    cx, y, body);
            y += h / 20f;
        } else {
            canvas.drawText("no wfb statistics yet", cx, y, body);
            y += h / 20f;
        }
        canvas.drawText("channel " + VideoActivity.getChannel(this)
                        + " @ " + VideoActivity.getBandwidth(this) + " MHz",
                cx, y, body);
        y += h / 20f;
        String wifi = VideoActivity.wirelessInfo(this);
        if (wifi != null) {
            canvas.drawText("or push a stream to udp://" + wifi + ":5600", cx, y, body);
            y += h / 20f;
        }
        canvas.drawText(xr != null && xr.isHeadLocked() ? "head locked" : "world locked", cx, y, body);
        y += h / 20f;

        Paint hint = new Paint(body);
        hint.setTextSize(h / 36f);
        hint.setColor(Color.rgb(130, 140, 155));
        canvas.drawText("A recenter    B passthrough    stick click record", cx, y, hint);
        y += h / 26f;
        canvas.drawText("menu button or thumb swipe left: back to flat mode", cx, y, hint);
    }

    // ------------------------------------------------------------------------------
    // DVR
    // ------------------------------------------------------------------------------

    private void toggleDvr() {
        if (dvrFd != null) {
            stopDvr();
            xr.haptic(0.7f, 60);
            Toast.makeText(this, "Recording stopped", Toast.LENGTH_SHORT).show();
            return;
        }
        if (!DvrFiles.hasRecordingFolder(this)) {
            // Picking a folder needs the flat activity; refuse rather than drop out of VR.
            xr.haptic(1.0f, 250);
            Toast.makeText(this, "Pick a DVR folder in flat mode first", Toast.LENGTH_LONG).show();
            return;
        }
        Uri target = DvrFiles.createRecording(this);
        if (target == null) {
            xr.haptic(1.0f, 250);
            Toast.makeText(this, "Could not create the recording file", Toast.LENGTH_LONG).show();
            return;
        }
        if (link == null) {
            return;
        }
        try {
            dvrFd = getContentResolver().openFileDescriptor(target, "rw");
            link.startDvr(dvrFd.getFd(), DvrFiles.fragmentedMp4(this));
            xr.haptic(0.7f, 120);
            Toast.makeText(this, "Recording", Toast.LENGTH_SHORT).show();
        } catch (IOException e) {
            Log.e(TAG, "Failed to open dvr file", e);
            dvrFd = null;
            xr.haptic(1.0f, 250);
        }
    }

    private void stopDvr() {
        if (dvrFd == null) {
            return;
        }
        if (link != null) {
            link.stopDvr();
        }
        try {
            dvrFd.close();
        } catch (IOException e) {
            Log.e(TAG, "Failed to close dvr file", e);
        }
        dvrFd = null;
    }

    // ------------------------------------------------------------------------------
    // stream + link callbacks
    // ------------------------------------------------------------------------------

    @Override
    public void onVideoResolution(int videoW, int videoH) {
        if (videoW <= 0 || videoH <= 0) {
            return;
        }
        Log.i(TAG, "stream resolution " + videoW + "x" + videoH);
        videoSeen = true;
        if (xr != null) {
            xr.setVideoResolution(videoW, videoH);
        }
        // So the next session can size its swapchain exactly.
        prefs().edit().putInt(PREF_LAST_VIDEO_W, videoW).putInt(PREF_LAST_VIDEO_H, videoH).apply();
    }

    @Override
    public void onWfbStats(WfbNGStats data) {
        lastStats = data;
        XrHud h = hud;
        if (h != null) {
            h.onLink(data);
        }
        // Without this there is no way to tell "the adapter is dead" from "the link is fine
        // but nothing decodes" while wearing the headset.
        final long nowMs = System.currentTimeMillis();
        if (nowMs - lastStatsLogAt > 2000) {
            lastStatsLogAt = nowMs;
            Log.i(TAG, "wfb: all=" + data.count_p_all
                    + " decOk=" + data.count_p_dec_ok
                    + " decErr=" + data.count_p_dec_err
                    + " fecRec=" + data.count_p_fec_recovered
                    + " lost=" + data.count_p_lost
                    + " bad=" + data.count_p_bad
                    + " rssi=" + data.avg_rssi
                    + " videoSeen=" + videoSeen);
        }
        if (xr == null || !linkStarted || data.count_p_all <= 0) {
            return;
        }
        // A headset can do something goggles cannot: warn you through the controllers, so
        // a degrading link does not depend on you reading an OSD number mid-manoeuvre.
        final int lost = data.count_p_lost;
        final int recovered = data.count_p_fec_recovered;
        final boolean bad = lost > 0 || recovered > data.count_p_all / 8;

        final long now = System.currentTimeMillis();
        if (bad && (!linkWasBad || now - lastHapticAt > 2000)) {
            lastHapticAt = now;
            xr.haptic(lost > 0 ? 1.0f : 0.5f, lost > 0 ? 180 : 70);
        }
        linkWasBad = bad;
    }

    // ------------------------------------------------------------------------------
    // link service callbacks
    // ------------------------------------------------------------------------------

    @Override
    public void onLinkStatus(String message) {
        lastStatus = message;
        runOnUiThread(() -> {
            if (statusView != null && !linkStarted) {
                statusView.setText(message);
            }
        });
    }

    @Override
    public void onLocalStreamHint(String url) {
        Log.i(TAG, "listening on " + url);
    }
}
