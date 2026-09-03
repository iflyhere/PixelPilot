package com.openipc.pixelpilot;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.Bundle;
import android.os.IBinder;
import android.util.Log;

import androidx.annotation.Nullable;
import androidx.appcompat.app.AppCompatActivity;

import com.openipc.mavlink.MavlinkData;
import com.openipc.videonative.DecodingInfo;
import com.openipc.wfbngrtl8812.WfbNGStats;

/**
 * Binds to {@link LinkService} for the lifetime of the activity.
 *
 * <p>Bound in {@code onCreate} and unbound in {@code onDestroy} rather than around
 * start/stop: the point of the service is that the link survives the activity being paused,
 * so a system menu or taking the headset off no longer costs a reconnect.
 */
public abstract class LinkClientActivity extends AppCompatActivity implements LinkService.Client {

    private static final String TAG = "pixelpilot";

    @Nullable
    protected LinkService link;
    private boolean bound;

    private final ServiceConnection connection = new ServiceConnection() {
        @Override
        public void onServiceConnected(ComponentName name, IBinder service) {
            link = ((LinkService.LocalBinder) service).service();
            link.addClient(LinkClientActivity.this);
            Log.i(TAG, "attached to the link service");
            onLinkServiceConnected(link);
        }

        @Override
        public void onServiceDisconnected(ComponentName name) {
            // The process is going away; nothing useful to do but stop talking to it.
            link = null;
        }
    };

    /** Called on the main thread once the link is available. */
    protected abstract void onLinkServiceConnected(LinkService service);

    /**
     * Whether to attach to the service at all.
     *
     * <p>A subclass that decides in {@code onCreate} that it should not run - stepping aside
     * for the other mode, say - has to say so before the bind, not after. Binding and then
     * finishing still delivers {@link #onServiceConnected}, and the service replays its last
     * state to a new client, so the callbacks would land on an activity that never built its
     * views.
     */
    protected boolean shouldAttachToLink() {
        return true;
    }

    @Override
    protected void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Intent intent = new Intent(this, LinkService.class);
        // Started as well as bound, so it keeps running while no activity is attached - that
        // is what carries the link across a mode switch.
        startForegroundService(intent);
        if (!shouldAttachToLink()) {
            // Still started, so the link survives; deliberately not bound.
            Log.i(TAG, "not attaching to the link service, this activity is standing aside");
            return;
        }
        bound = bindService(intent, connection, Context.BIND_AUTO_CREATE);
        if (!bound) {
            Log.e(TAG, "could not bind the link service");
        }
    }

    @Override
    protected void onDestroy() {
        if (link != null) {
            link.removeClient(this);
        }
        if (bound) {
            unbindService(connection);
            bound = false;
        }
        link = null;
        super.onDestroy();
    }

    // Defaults so a subclass only overrides what it draws.

    @Override
    public void onLinkStatus(String message) {
    }

    @Override
    public void onLocalStreamHint(String url) {
    }

    @Override
    public void onVideoResolution(int width, int height) {
    }

    @Override
    public void onDecodingInfo(DecodingInfo info) {
    }

    @Override
    public void onWfbStats(WfbNGStats stats) {
    }

    @Override
    public void onMavlink(MavlinkData data) {
    }
}
