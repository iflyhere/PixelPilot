package com.openipc.pixelpilot.xrhud;

import android.content.Context;
import android.graphics.Bitmap;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;

import androidx.annotation.Nullable;

/**
 * Keeps the map and the height model fed, on a thread where a tile decode does no harm.
 *
 * <p>Both jobs are slow enough that they cannot sit anywhere else. A terrain lookup decodes a
 * 256px PNG on a miss, and a basemap render decodes several - on the telemetry thread that
 * would stall the instruments, and on the main thread it would stall everything.
 *
 * <p>Nothing here is required. With no files installed the minimap draws the track alone and
 * the chart shows height above the arming point, which is what a first flight in a new place
 * looks like anyway.
 */
public final class OfflineMaps {

    private static final String TAG = "pixelpilot";

    /** Terrain lookups per pass. Enough to fill a chart in a few seconds, not enough to hog. */
    private static final int LOOKUPS_PER_PASS = 24;

    private static final long PASS_MS = 250;
    private static final long BASEMAP_MS = 2000;

    private final FlightData data;
    private final HandlerThread thread;
    private final Handler handler;

    @Nullable
    private TerrainDem terrain;
    @Nullable
    private BasemapRenderer basemap;
    @Nullable
    private XrMinimap minimap;

    private volatile boolean running;
    private double lastMapLat;
    private double lastMapLon;
    private float lastMapSpan;
    private long lastMapAt;

    public OfflineMaps(FlightData data) {
        this.data = data;
        thread = new HandlerThread("OfflineMaps");
        thread.start();
        handler = new Handler(thread.getLooper());
    }

    /** Opens whatever is installed and starts working. Cheap when nothing is installed. */
    public void start(Context context, @Nullable XrMinimap minimap) {
        if (running) {
            return;
        }
        running = true;
        this.minimap = minimap;
        final Context app = context.getApplicationContext();
        handler.post(() -> {
            if (MapFiles.have(app, MapFiles.Kind.TERRAIN)) {
                terrain = TerrainDem.open(MapFiles.file(app, MapFiles.Kind.TERRAIN));
                Log.i(TAG, "terrain model " + (terrain != null ? "open" : "unusable"));
            }
            if (MapFiles.have(app, MapFiles.Kind.BASEMAP)) {
                basemap = BasemapRenderer.open(MapFiles.file(app, MapFiles.Kind.BASEMAP));
                Log.i(TAG, "basemap " + (basemap != null ? "open" : "unusable"));
            }
        });
        handler.postDelayed(this::pass, PASS_MS);
    }

    public void stop() {
        running = false;
        handler.removeCallbacksAndMessages(null);
        handler.post(() -> {
            if (terrain != null) {
                terrain.close();
                terrain = null;
            }
            if (basemap != null) {
                basemap.close();
                basemap = null;
            }
        });
        thread.quitSafely();
    }

    private void pass() {
        if (!running) {
            return;
        }
        fillTerrain();
        maybeRenderBasemap();
        if (running) {
            handler.postDelayed(this::pass, PASS_MS);
        }
    }

    private void fillTerrain() {
        final TerrainDem dem = terrain;
        if (dem == null) {
            return;
        }
        for (int i = 0; i < LOOKUPS_PER_PASS; i++) {
            final int index = data.pendingTerrain();
            if (index < 0) {
                return;
            }
            final float m = dem.elevationAt(data.latAt(index), data.lonAt(index));
            if (Float.isNaN(m)) {
                // Outside the model's coverage. Recorded as such, or it is retried forever and
                // starves the samples that could be filled.
                data.setTerrainMissing(index);
            } else {
                data.setTerrainAt(index, m);
            }
        }
    }

    private void maybeRenderBasemap() {
        final BasemapRenderer renderer = basemap;
        final XrMinimap map = minimap;
        if (renderer == null || map == null) {
            return;
        }
        final long now = System.currentTimeMillis();
        if (now - lastMapAt < BASEMAP_MS) {
            return;
        }
        final FlightData.Snapshot s = data.snapshot();
        if (!s.homeKnown || !s.gpsFix) {
            return;
        }
        // The minimap is centred on home and sized by how far the craft has gone, so a render
        // is only worth repeating when one of those has actually moved.
        final float span = Math.max(240f, s.homeDistance * 2.6f);
        final boolean moved = Math.abs(s.homeLat - lastMapLat) > 1e-5
                || Math.abs(s.homeLon - lastMapLon) > 1e-5
                || Math.abs(span - lastMapSpan) > span * 0.15f;
        if (!moved) {
            return;
        }
        lastMapAt = now;
        final Bitmap bmp = renderer.render(s.homeLat, s.homeLon, span, 512);
        if (bmp == null) {
            return;
        }
        lastMapLat = s.homeLat;
        lastMapLon = s.homeLon;
        lastMapSpan = span;
        map.setBasemap(bmp, span);
    }
}
