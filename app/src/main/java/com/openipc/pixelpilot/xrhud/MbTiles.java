package com.openipc.pixelpilot.xrhud;

import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;
import android.database.sqlite.SQLiteException;
import android.util.Log;

import androidx.annotation.Nullable;

import java.io.File;
import java.util.HashMap;
import java.util.Map;

/**
 * Read side of an MBTiles file.
 *
 * <p>MBTiles is a SQLite database with a {@code tiles(zoom_level, tile_column, tile_row,
 * tile_data)} table, so Android can open it with nothing added - the blobs are ordinary PNG or
 * JPEG for a raster set, or elevation packed into the channels for a height model. That is
 * hand-rolled rather than delegated to a map engine: a 640px minimap redrawn twice a second
 * needs a handful of tiles decoded onto a Canvas, and a vector renderer would be ten to
 * fifteen megabytes per ABI and a second GL context for the privilege.
 *
 * <p>The one trap is the row index. MBTiles stores {@code tile_row} bottom-up (TMS) while
 * every slippy-map formula produces it top-down (XYZ), so one of the two has to be flipped and
 * getting it wrong yields a map that is plausible and mirrored.
 */
public final class MbTiles implements AutoCloseable {

    private static final String TAG = "pixelpilot";

    private final SQLiteDatabase db;
    private final Map<String, String> metadata = new HashMap<>();
    private final int minZoom;
    private final int maxZoom;

    private MbTiles(SQLiteDatabase db) {
        this.db = db;
        try (Cursor c = db.rawQuery("SELECT name, value FROM metadata", null)) {
            while (c.moveToNext()) {
                metadata.put(c.getString(0), c.getString(1));
            }
        } catch (SQLiteException e) {
            // metadata is optional in practice; the zoom range can be derived from the tiles.
            Log.w(TAG, "mbtiles has no readable metadata table", e);
        }
        int lo = parse(metadata.get("minzoom"), -1);
        int hi = parse(metadata.get("maxzoom"), -1);
        if (lo < 0 || hi < 0) {
            try (Cursor c = db.rawQuery("SELECT MIN(zoom_level), MAX(zoom_level) FROM tiles",
                    null)) {
                if (c.moveToFirst()) {
                    lo = c.getInt(0);
                    hi = c.getInt(1);
                }
            } catch (SQLiteException e) {
                Log.w(TAG, "cannot determine the zoom range", e);
            }
        }
        minZoom = Math.max(0, lo);
        maxZoom = Math.max(minZoom, hi);
    }

    /** Opens a file for reading, or null when it is not a usable MBTiles database. */
    @Nullable
    public static MbTiles open(File file) {
        if (file == null || !file.isFile() || file.length() == 0) {
            return null;
        }
        try {
            final SQLiteDatabase db = SQLiteDatabase.openDatabase(
                    file.getAbsolutePath(), null, SQLiteDatabase.OPEN_READONLY);
            final MbTiles t = new MbTiles(db);
            if (t.maxZoom < t.minZoom) {
                t.close();
                return null;
            }
            Log.i(TAG, "mbtiles " + file.getName() + " format=" + t.format()
                    + " zoom " + t.minZoom + ".." + t.maxZoom);
            return t;
        } catch (SQLiteException e) {
            Log.e(TAG, "cannot open " + file.getName() + " as mbtiles", e);
            return null;
        }
    }

    /** A metadata value, or {@code fallback} when the file does not carry the key. */
    public String meta(String key, String fallback) {
        final String v = metadata.get(key);
        return (v == null || v.isEmpty()) ? fallback : v;
    }

    public String format() {
        final String f = metadata.get("format");
        return f == null ? "" : f;
    }

    public int minZoom() {
        return minZoom;
    }

    public int maxZoom() {
        return maxZoom;
    }

    /** Raw tile bytes at XYZ coordinates, or null when that tile is not in the file. */
    @Nullable
    public byte[] tile(int z, int x, int y) {
        if (z < minZoom || z > maxZoom) {
            return null;
        }
        final int n = 1 << z;
        if (x < 0 || y < 0 || x >= n || y >= n) {
            return null;
        }
        // XYZ is top-down, MBTiles stores bottom-up.
        final int tmsY = n - 1 - y;
        try (Cursor c = db.rawQuery(
                "SELECT tile_data FROM tiles WHERE zoom_level=? AND tile_column=? AND tile_row=?",
                new String[]{String.valueOf(z), String.valueOf(x), String.valueOf(tmsY)})) {
            return c.moveToFirst() ? c.getBlob(0) : null;
        } catch (SQLiteException e) {
            Log.w(TAG, "tile read failed at " + z + "/" + x + "/" + y, e);
            return null;
        }
    }

    @Override
    public void close() {
        try {
            db.close();
        } catch (SQLiteException ignored) {
        }
    }

    private static int parse(@Nullable String s, int fallback) {
        if (s == null) {
            return fallback;
        }
        try {
            return Integer.parseInt(s.trim());
        } catch (NumberFormatException e) {
            return fallback;
        }
    }

    // ------------------------------------------------------------------ web mercator

    /** Fractional tile x for a longitude at a zoom level. */
    public static double tileX(double lon, int z) {
        return (lon + 180.0) / 360.0 * (1 << z);
    }

    /** Fractional tile y for a latitude at a zoom level. */
    public static double tileY(double lat, int z) {
        final double r = Math.toRadians(Math.max(-85.05112878, Math.min(85.05112878, lat)));
        return (1.0 - Math.log(Math.tan(r) + 1.0 / Math.cos(r)) / Math.PI) / 2.0 * (1 << z);
    }

    /** Metres per pixel at a latitude, for 256px tiles. */
    public static double metresPerPixel(double lat, int z) {
        return 156543.03392804097 * Math.cos(Math.toRadians(lat)) / (1 << z);
    }

    /** The zoom whose pixel scale is closest to the wanted metres per pixel. */
    public static int zoomForScale(double lat, double wantedMetresPerPixel, int lo, int hi) {
        int best = lo;
        double bestErr = Double.MAX_VALUE;
        for (int z = lo; z <= hi; z++) {
            final double err = Math.abs(Math.log(metresPerPixel(lat, z) / wantedMetresPerPixel));
            if (err < bestErr) {
                bestErr = err;
                best = z;
            }
        }
        return best;
    }
}
