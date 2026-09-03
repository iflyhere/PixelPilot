package com.openipc.pixelpilot.xrhud;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.util.Log;
import android.util.LruCache;

import androidx.annotation.Nullable;

import java.io.File;

/**
 * Ground elevation from an offline height model, so the chart can show height above ground.
 *
 * <p>This exists because the rangefinder cannot do the job. Betaflight's MAVLink telemetry
 * does not carry it - measured over fifty seconds, the stream is msgids 0, 1, 2, 24, 30, 33,
 * 35, 42, 74 and 147, with no DISTANCE_SENSOR and no RANGEFINDER - and a sonar's few metres of
 * range would only ever cover takeoff and landing anyway. A height model covers the whole
 * flight and works with no connection, which is the situation this app is used in.
 *
 * <p>Elevation is packed into the colour channels, and there are two incompatible conventions
 * in the wild - a file in the wrong one decodes to plausible-looking nonsense rather than
 * failing, so the file has to say which it is, in an {@code encoding} metadata key:
 *
 * <ul>
 *   <li><b>terrarium</b>: {@code (R * 256 + G + B / 256) - 32768} metres. What the AWS open
 *       data terrain tiles carry, which is the source that needs no account.
 *   <li><b>mapbox</b> (terrain-rgb): {@code -10000 + (R * 65536 + G * 256 + B) * 0.1} metres.
 * </ul>
 *
 * <p>Terrarium is the default for a file that does not say, because that is what the tooling
 * in this repository produces.
 */
public final class TerrainDem implements AutoCloseable {

    private static final String TAG = "pixelpilot";

    /** Sixteen decoded tiles is a few square kilometres at working zoom - well under a MB. */
    private static final int CACHE_TILES = 16;

    private final MbTiles tiles;
    private final int zoom;
    private final boolean terrarium;
    private final LruCache<Long, Bitmap> cache = new LruCache<Long, Bitmap>(CACHE_TILES) {
        @Override
        protected void entryRemoved(boolean evicted, Long key, Bitmap old, Bitmap now) {
            if (old != null && old != now && !old.isRecycled()) {
                old.recycle();
            }
        }
    };
    private final BitmapFactory.Options opts = new BitmapFactory.Options();

    private TerrainDem(MbTiles tiles) {
        this.tiles = tiles;
        final String enc = tiles.meta("encoding", "terrarium").toLowerCase();
        // Anything but an explicit mapbox/terrain-rgb marker is read as terrarium.
        this.terrarium = !(enc.contains("mapbox") || enc.contains("terrain-rgb")
                || enc.contains("terrainrgb"));
        Log.i(TAG, "terrain: " + (terrarium ? "terrarium" : "mapbox") + " encoding, zoom "
                + tiles.maxZoom());
        // The highest zoom the file has: elevation is read pointwise, so there is nothing to
        // gain from a coarser tile and a lot to lose in a valley.
        this.zoom = tiles.maxZoom();
        // ARGB_8888 and no scaling: the channels are data, and any filtering corrupts them.
        opts.inPreferredConfig = Bitmap.Config.ARGB_8888;
        opts.inScaled = false;
        opts.inDither = false;
    }

    /** Opens a terrain MBTiles, or null when the file is unusable. */
    @Nullable
    public static TerrainDem open(File file) {
        final MbTiles t = MbTiles.open(file);
        if (t == null) {
            return null;
        }
        final String fmt = t.format().toLowerCase();
        // A vector file cannot be sampled this way, and saying so beats returning NaN forever.
        if (fmt.contains("pbf") || fmt.contains("mvt")) {
            Log.e(TAG, "terrain file is vector (" + fmt + "), which carries no elevation raster");
            t.close();
            return null;
        }
        return new TerrainDem(t);
    }

    /**
     * Ground elevation in metres above sea level, or NaN when the file does not cover the
     * position. Safe to call from any single thread; not thread safe across several.
     */
    public float elevationAt(double lat, double lon) {
        final double tx = MbTiles.tileX(lon, zoom);
        final double ty = MbTiles.tileY(lat, zoom);
        final int tileXi = (int) Math.floor(tx);
        final int tileYi = (int) Math.floor(ty);

        final Bitmap bmp = tileBitmap(tileXi, tileYi);
        if (bmp == null) {
            return Float.NaN;
        }
        final int px = clamp((int) ((tx - tileXi) * bmp.getWidth()), 0, bmp.getWidth() - 1);
        final int py = clamp((int) ((ty - tileYi) * bmp.getHeight()), 0, bmp.getHeight() - 1);
        final int c = bmp.getPixel(px, py);
        final int r = (c >> 16) & 0xFF;
        final int g = (c >> 8) & 0xFF;
        final int b = c & 0xFF;
        return terrarium
                ? (float) (r * 256.0 + g + b / 256.0 - 32768.0)
                : (float) (-10000.0 + (r * 65536.0 + g * 256.0 + b) * 0.1);
    }

    @Nullable
    private Bitmap tileBitmap(int x, int y) {
        final long key = ((long) x << 32) | (y & 0xFFFFFFFFL);
        Bitmap cached = cache.get(key);
        if (cached != null && !cached.isRecycled()) {
            return cached;
        }
        final byte[] blob = tiles.tile(zoom, x, y);
        if (blob == null) {
            return null;
        }
        final Bitmap bmp = BitmapFactory.decodeByteArray(blob, 0, blob.length, opts);
        if (bmp == null) {
            return null;
        }
        cache.put(key, bmp);
        return bmp;
    }

    @Override
    public void close() {
        cache.evictAll();
        tiles.close();
    }

    private static int clamp(int v, int lo, int hi) {
        return v < lo ? lo : Math.min(v, hi);
    }
}
