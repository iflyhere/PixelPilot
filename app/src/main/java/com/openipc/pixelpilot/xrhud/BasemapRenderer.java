package com.openipc.pixelpilot.xrhud;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.RectF;
import android.util.Log;

import androidx.annotation.Nullable;

import java.io.File;

/**
 * Stitches raster tiles from an MBTiles file into one bitmap for the minimap.
 *
 * <p>Deliberately not a map engine. The minimap is 640 pixels across and redrawn about twice a
 * second, which is a handful of 256px tiles decoded onto a Canvas - a few hundred lines of
 * arithmetic against ten to fifteen megabytes per ABI and a second GL context for a vector
 * renderer. A vector engine earns its keep when the map is interactive, styled, rotated and
 * zoomed continuously; here it is a north-up square that changes when the craft moves.
 *
 * <p>North up and centred on home, so the caller only has to say how many metres across.
 */
public final class BasemapRenderer implements AutoCloseable {

    private static final String TAG = "pixelpilot";

    private final MbTiles tiles;
    private final Paint paint = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG);
    private final Rect src = new Rect();
    private final RectF dst = new RectF();
    private final BitmapFactory.Options opts = new BitmapFactory.Options();

    private BasemapRenderer(MbTiles tiles) {
        this.tiles = tiles;
        opts.inPreferredConfig = Bitmap.Config.RGB_565;  // a basemap needs no alpha
    }

    /** Opens a raster MBTiles, or null when the file is unusable or is vector. */
    @Nullable
    public static BasemapRenderer open(File file) {
        final MbTiles t = MbTiles.open(file);
        if (t == null) {
            return null;
        }
        final String fmt = t.format().toLowerCase();
        if (fmt.contains("pbf") || fmt.contains("mvt")) {
            // Vector tiles need a style and a renderer; this draws pixels.
            Log.e(TAG, "basemap is vector (" + fmt + "); a raster mbtiles is needed here");
            t.close();
            return null;
        }
        return new BasemapRenderer(t);
    }

    /**
     * Renders a north-up square of {@code spanMetres} centred on the given position.
     *
     * <p>Call from a worker thread: it decodes several tiles. Returns null when the file covers
     * none of the area, so the caller can leave the track on its own rather than draw a blank.
     */
    @Nullable
    public Bitmap render(double lat, double lon, float spanMetres, int sizePx) {
        if (spanMetres <= 0f || sizePx <= 0) {
            return null;
        }
        final double wantedMpp = spanMetres / sizePx;
        final int z = MbTiles.zoomForScale(lat, wantedMpp, tiles.minZoom(), tiles.maxZoom());

        // How many tiles the wanted span covers at this zoom, in fractional tile units.
        final double mpp = MbTiles.metresPerPixel(lat, z);
        final double spanTiles = spanMetres / (mpp * 256.0);
        final double cx = MbTiles.tileX(lon, z);
        final double cy = MbTiles.tileY(lat, z);
        final double left = cx - spanTiles / 2.0;
        final double top = cy - spanTiles / 2.0;

        final int x0 = (int) Math.floor(left);
        final int y0 = (int) Math.floor(top);
        final int x1 = (int) Math.floor(left + spanTiles);
        final int y1 = (int) Math.floor(top + spanTiles);

        // Guard against a silly span asking for hundreds of tiles.
        if ((long) (x1 - x0 + 1) * (y1 - y0 + 1) > 64) {
            Log.w(TAG, "basemap span would need too many tiles at zoom " + z);
            return null;
        }

        Bitmap out = null;
        Canvas canvas = null;
        boolean anyDrawn = false;
        final float pxPerTile = (float) (sizePx / spanTiles);

        for (int tx = x0; tx <= x1; tx++) {
            for (int ty = y0; ty <= y1; ty++) {
                final byte[] blob = tiles.tile(z, tx, ty);
                if (blob == null) {
                    continue;
                }
                final Bitmap tile = BitmapFactory.decodeByteArray(blob, 0, blob.length, opts);
                if (tile == null) {
                    continue;
                }
                if (out == null) {
                    out = Bitmap.createBitmap(sizePx, sizePx, Bitmap.Config.RGB_565);
                    canvas = new Canvas(out);
                }
                src.set(0, 0, tile.getWidth(), tile.getHeight());
                final float dx = (float) ((tx - left) * pxPerTile);
                final float dy = (float) ((ty - top) * pxPerTile);
                dst.set(dx, dy, dx + pxPerTile, dy + pxPerTile);
                canvas.drawBitmap(tile, src, dst, paint);
                tile.recycle();
                anyDrawn = true;
            }
        }
        if (!anyDrawn && out != null) {
            out.recycle();
            return null;
        }
        return out;
    }

    @Override
    public void close() {
        tiles.close();
    }
}
