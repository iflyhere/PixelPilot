package com.openipc.pixelpilot.xrhud;

import android.content.Context;
import android.net.Uri;
import android.util.Log;

import androidx.annotation.Nullable;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * Where the offline map files live, and how one gets there.
 *
 * <p>They have to be copied into the app rather than read where the user picked them: SQLite
 * needs a real path and a random-access file, and a SAF document URI gives neither. So an
 * import is a copy, which for a height model of a whole flying area can be a while - hence
 * {@link #importFrom} being something to call off the main thread.
 *
 * <p>Two files, because they answer different questions and are usually produced separately: a
 * raster basemap to look at, and a terrain-rgb height model to measure against. Either can be
 * absent; the minimap falls back to the track alone and the chart to height above the arming
 * point.
 */
public final class MapFiles {

    private static final String TAG = "pixelpilot";
    private static final String DIR = "maps";
    private static final String BASEMAP = "basemap.mbtiles";
    private static final String TERRAIN = "terrain.mbtiles";

    public enum Kind {BASEMAP, TERRAIN}

    private MapFiles() {
    }

    public static File file(Context context, Kind kind) {
        final File dir = new File(context.getFilesDir(), DIR);
        return new File(dir, kind == Kind.BASEMAP ? BASEMAP : TERRAIN);
    }

    public static boolean have(Context context, Kind kind) {
        final File f = file(context, kind);
        return f.isFile() && f.length() > 0;
    }

    /** Human-readable state for a menu entry. */
    public static String describe(Context context, Kind kind) {
        final File f = file(context, kind);
        if (!f.isFile() || f.length() == 0) {
            return "not installed";
        }
        return String.format("%.1f MB", f.length() / 1048576.0);
    }

    /**
     * Copies a picked document in, replacing whatever was there.
     *
     * @return null on success, or a message to show the user
     */
    @Nullable
    public static String importFrom(Context context, Kind kind, Uri source) {
        final File target = file(context, kind);
        final File dir = target.getParentFile();
        if (dir != null && !dir.isDirectory() && !dir.mkdirs()) {
            return "could not create the maps folder";
        }
        // Copy beside the target and rename, so an interrupted import cannot leave a
        // half-written file in place of a working one.
        final File tmp = new File(target.getPath() + ".part");
        long copied = 0;
        try (InputStream in = context.getContentResolver().openInputStream(source);
             OutputStream out = new FileOutputStream(tmp)) {
            if (in == null) {
                return "cannot read the selected file";
            }
            final byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
                copied += n;
            }
        } catch (IOException e) {
            Log.e(TAG, "map import failed", e);
            deleteQuietly(tmp);
            return "import failed: " + e.getMessage();
        }
        if (copied == 0) {
            deleteQuietly(tmp);
            return "the selected file is empty";
        }
        // Refuse a file that is not actually an MBTiles rather than fail later, in the air.
        final MbTiles probe = MbTiles.open(tmp);
        if (probe == null) {
            deleteQuietly(tmp);
            return "that is not a readable mbtiles file";
        }
        final String format = probe.format();
        final int minZ = probe.minZoom();
        final int maxZ = probe.maxZoom();
        probe.close();

        deleteQuietly(target);
        if (!tmp.renameTo(target)) {
            deleteQuietly(tmp);
            return "could not move the imported file into place";
        }
        Log.i(TAG, "imported " + kind + " " + format + " zoom " + minZ + ".." + maxZ
                + " (" + copied / 1048576 + " MB)");
        return null;
    }

    public static void remove(Context context, Kind kind) {
        deleteQuietly(file(context, kind));
    }

    private static void deleteQuietly(File f) {
        if (f.exists() && !f.delete()) {
            Log.w(TAG, "could not delete " + f);
        }
    }
}
