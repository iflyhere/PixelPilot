package com.openipc.pixelpilot.xrhud;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Rect;
import android.graphics.RectF;
import android.view.Surface;

import androidx.annotation.Nullable;

import java.util.Locale;

/**
 * Where the craft is, relative to where it took off.
 *
 * <p>Laid back on its own layer like a nav screen on a dashboard, so it is glanceable without
 * sitting in the middle of the video.
 *
 * <p>The track is drawn from the position history and needs nothing downloaded, which is the
 * point: it works on the first flight in a new place with no connection at all. A basemap is
 * an improvement on top - {@link #setBasemap} takes a bitmap rendered elsewhere, so whatever
 * produces it stays out of here.
 *
 * <p>North-up rather than track-up. Track-up spins the whole map every time the craft yaws,
 * which is exactly when the pilot is trying to read it.
 */
public final class XrMinimap extends XrOverlay {

    /** How many past positions to keep. At 2Hz that is a few minutes of track. */
    private static final int TRACK = 512;

    private final FlightData data;
    private final Path path = new Path();
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Rect src = new Rect();
    private final RectF dst = new RectF();

    private final double[] trackLat = new double[TRACK];
    private final double[] trackLon = new double[TRACK];
    private int trackCount;
    private int trackHead;
    private long lastTrackAt;

    @Nullable
    private volatile Bitmap basemap;
    private volatile float basemapSpanM;

    /** Metres from the middle of the map to its edge. */
    private volatile float rangeM = 120f;

    public XrMinimap(Surface surface, int width, int height, FlightData data) {
        super("XrMinimap", surface, width, height, 12, 200);
        this.data = data;
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeCap(Paint.Cap.ROUND);
        paint.setStrokeJoin(Paint.Join.ROUND);
    }

    /**
     * A rendered basemap covering {@code spanMetres} across, centred on home, north up.
     * Null clears it and leaves the track on its own.
     */
    public void setBasemap(@Nullable Bitmap bitmap, float spanMetres) {
        basemap = bitmap;
        basemapSpanM = spanMetres;
    }

    public void setRange(float metres) {
        rangeM = Math.max(25f, metres);
    }

    @Override
    protected void draw(Canvas canvas) {
        final FlightData.Snapshot s = data.snapshot();
        final float cx = width / 2f;
        final float cy = height / 2f;
        final float r = Math.min(width, height) / 2f - u * 0.35f;

        sampleTrack(s);

        // The card is round here, because the map is a radius from home rather than a rectangle.
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(PANEL);
        canvas.drawCircle(cx, cy, r, paint);

        drawBasemap(canvas, cx, cy, r, s);
        rings(canvas, cx, cy, r);

        if (s.homeKnown && s.gpsFix) {
            track(canvas, cx, cy, r, s);
            home(canvas, cx, cy, r, s);
            craft(canvas, cx, cy, r, s);
        } else {
            label(canvas, s.gpsFix ? "no home yet" : "no gps fix", cx, cy, Paint.Align.CENTER);
        }

        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(Math.max(1.5f, u * 0.06f));
        paint.setColor(PANEL_EDGE);
        canvas.drawCircle(cx, cy, r, paint);

        north(canvas, cx, cy, r);
        label(canvas, String.format(Locale.US, "%.0f m", rangeM), cx, cy + r - u * 0.35f,
                Paint.Align.CENTER);
    }

    private void drawBasemap(Canvas canvas, float cx, float cy, float r,
                             FlightData.Snapshot s) {
        final Bitmap map = basemap;
        if (map == null || map.isRecycled() || basemapSpanM <= 0f) {
            return;
        }
        // The view covers 2*rangeM metres across 2*r pixels, so r/rangeM pixels per metre.
        // The bitmap covers basemapSpanM metres, so it is drawn that many metres wide.
        final float half = basemapSpanM * r / rangeM / 2f;
        canvas.save();
        path.reset();
        path.addCircle(cx, cy, r, Path.Direction.CW);
        canvas.clipPath(path);
        src.set(0, 0, map.getWidth(), map.getHeight());
        dst.set(cx - half, cy - half, cx + half, cy + half);
        canvas.drawBitmap(map, src, dst, null);
        canvas.restore();
    }

    private void rings(Canvas canvas, float cx, float cy, float r) {
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(Math.max(1f, u * 0.04f));
        paint.setColor(Color.argb(52, 190, 214, 240));
        canvas.drawCircle(cx, cy, r * 0.5f, paint);
        // Cross hairs, broken in the middle so they do not fight the craft marker.
        hairline(canvas, cx - r, cy, cx - r * 0.12f, cy, u * 0.04f, Color.argb(40, 190, 214, 240),
                false);
        hairline(canvas, cx + r * 0.12f, cy, cx + r, cy, u * 0.04f,
                Color.argb(40, 190, 214, 240), false);
        hairline(canvas, cx, cy - r, cx, cy - r * 0.12f, u * 0.04f,
                Color.argb(40, 190, 214, 240), false);
        hairline(canvas, cx, cy + r * 0.12f, cx, cy + r, u * 0.04f,
                Color.argb(40, 190, 214, 240), false);
    }

    private void track(Canvas canvas, float cx, float cy, float r, FlightData.Snapshot s) {
        if (trackCount < 2) {
            return;
        }
        path.reset();
        final int start = (trackHead - trackCount + TRACK) % TRACK;
        boolean first = true;
        for (int i = 0; i < trackCount; i++) {
            final int j = (start + i) % TRACK;
            final float[] p = project(trackLat[j], trackLon[j], s, cx, cy, r);
            if (first) {
                path.moveTo(p[0], p[1]);
                first = false;
            } else {
                path.lineTo(p[0], p[1]);
            }
        }
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(u * 0.16f);
        paint.setColor(Color.argb(210, 70, 225, 205));
        canvas.drawPath(path, paint);
    }

    private void home(Canvas canvas, float cx, float cy, float r, FlightData.Snapshot s) {
        final float[] p = project(s.homeLat, s.homeLon, s, cx, cy, r);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(u * 0.14f);
        paint.setColor(INK);
        final float k = u * 0.34f;
        canvas.drawCircle(p[0], p[1], k, paint);
        hairline(canvas, p[0], p[1] - k * 1.9f, p[0], p[1] - k, u * 0.12f, INK, false);
    }

    private void craft(Canvas canvas, float cx, float cy, float r, FlightData.Snapshot s) {
        final float[] p = project(s.lat, s.lon, s, cx, cy, r);
        canvas.save();
        canvas.translate(p[0], p[1]);
        canvas.rotate(s.yaw);
        path.reset();
        final float k = u * 0.46f;
        path.moveTo(0, -k);
        path.lineTo(k * 0.66f, k * 0.72f);
        path.lineTo(0, k * 0.30f);
        path.lineTo(-k * 0.66f, k * 0.72f);
        path.close();
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(ACCENT);
        canvas.drawPath(path, paint);
        canvas.restore();
    }

    private void north(Canvas canvas, float cx, float cy, float r) {
        text(canvas, "N", cx, cy - r + u * 0.72f, u * 0.52f, INK_DIM, Paint.Align.CENTER, false);
    }

    /**
     * Position to canvas, north up, centred on home.
     *
     * <p>An equirectangular projection: over the few hundred metres a minimap covers the error
     * is far below one pixel, and it costs one cosine.
     */
    private float[] project(double lat, double lon, FlightData.Snapshot s, float cx, float cy,
                            float r) {
        final double mPerDegLat = 111320.0;
        final double mPerDegLon = 111320.0 * Math.cos(Math.toRadians(s.homeLat));
        final double north = (lat - s.homeLat) * mPerDegLat;
        final double east = (lon - s.homeLon) * mPerDegLon;
        final float px = cx + (float) (east / rangeM) * r;
        final float py = cy - (float) (north / rangeM) * r;
        return new float[]{px, py};
    }

    private void sampleTrack(FlightData.Snapshot s) {
        if (!s.gpsFix || !s.homeKnown) {
            return;
        }
        final long now = System.currentTimeMillis();
        if (now - lastTrackAt < 500) {
            return;
        }
        lastTrackAt = now;
        trackLat[trackHead] = s.lat;
        trackLon[trackHead] = s.lon;
        trackHead = (trackHead + 1) % TRACK;
        if (trackCount < TRACK) {
            trackCount++;
        }
        // Keep the whole flight in view rather than making the pilot change range by hand.
        if (s.homeDistance > rangeM * 0.85f) {
            rangeM = s.homeDistance * 1.3f;
        }
    }
}
