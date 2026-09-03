package com.openipc.pixelpilot.xrhud;

import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.view.Surface;

import java.util.Locale;

/**
 * Height and pack voltage over the last few minutes, laid back beside the video.
 *
 * <p>The trace is worth more than the instantaneous number for both of these. A voltage that
 * reads 3.7V per cell is fine; a voltage that read 3.9V a minute ago and 3.7V now, with the
 * sag deepening on every punch-out, is a pilot who should be heading home. The dashboard shows
 * the value, this shows the shape.
 *
 * <p>Where a terrain elevation is known the ground is filled in underneath, which turns the
 * altitude trace into a height-above-ground trace - the thing that actually matters over
 * anything but a flat field. Betaflight's rangefinder cannot help here: it is not in the
 * telemetry stream at all, and its few metres of range would only cover takeoff anyway.
 */
public final class XrChart extends XrOverlay {

    private final FlightData data;
    private final Path path = new Path();
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);

    private final long[] time;
    private final float[] alt;
    private final float[] cell;
    private final float[] terrain;

    public XrChart(Surface surface, int width, int height, FlightData data) {
        super("XrChart", surface, width, height, 9, 250);
        this.data = data;
        final int cap = data.historyCapacity();
        time = new long[cap];
        alt = new float[cap];
        cell = new float[cap];
        terrain = new float[cap];
        paint.setStrokeCap(Paint.Cap.ROUND);
        paint.setStrokeJoin(Paint.Join.ROUND);
    }

    @Override
    protected void draw(Canvas canvas) {
        final float pad = u * 0.5f;
        card(canvas, pad, pad, width - pad, height - pad);

        final int n = data.history(time, alt, cell, terrain);
        final float left = pad + u * 0.9f;
        final float right = width - pad - u * 1.5f;
        final float top = pad + u * 1.5f;
        final float bottom = height - pad - u * 1.2f;

        label(canvas, "height", left, top - u * 0.5f, Paint.Align.LEFT);
        label(canvas, "v/cell", right, top - u * 0.5f, Paint.Align.RIGHT);

        if (n < 2) {
            label(canvas, "waiting for telemetry", (left + right) / 2f, (top + bottom) / 2f,
                    Paint.Align.CENTER);
            return;
        }

        // One vertical scale for both the flight path and the ground, so the gap between them
        // is the height above ground and can be read directly.
        float lo = Float.MAX_VALUE;
        float hi = -Float.MAX_VALUE;
        boolean anyTerrain = false;
        for (int i = 0; i < n; i++) {
            lo = Math.min(lo, alt[i]);
            hi = Math.max(hi, alt[i]);
            if (known(terrain[i])) {
                anyTerrain = true;
                lo = Math.min(lo, terrain[i]);
                hi = Math.max(hi, terrain[i]);
            }
        }
        if (!anyTerrain) {
            lo = Math.min(lo, 0f);
        }
        // A flat trace should not be magnified into noise.
        if (hi - lo < 5f) {
            final float mid = (hi + lo) / 2f;
            lo = mid - 2.5f;
            hi = mid + 2.5f;
        }
        final float span = hi - lo;

        grid(canvas, left, top, right, bottom, lo, hi);

        if (anyTerrain) {
            ground(canvas, left, top, right, bottom, n, lo, span);
        }
        trace(canvas, left, top, right, bottom, n, alt, lo, span, ACCENT, true);
        voltage(canvas, left, top, right, bottom, n);

        // The newest sample, called out where the eye lands at the end of the trace.
        final float lastY = yFor(alt[n - 1], top, bottom, lo, span);
        text(canvas, String.format(Locale.US, "%.0f m", alt[n - 1]), right - u * 0.2f,
                lastY - u * 0.3f, u * 0.62f, ACCENT, Paint.Align.RIGHT, false);
        if (anyTerrain && known(terrain[n - 1])) {
            text(canvas, String.format(Locale.US, "%.0f agl", alt[n - 1] - terrain[n - 1]),
                    right - u * 0.2f, lastY + u * 0.55f, u * 0.5f, INK_DIM, Paint.Align.RIGHT,
                    false);
        }

        final float minutes = (time[n - 1] - time[0]) / 60000f;
        label(canvas, String.format(Locale.US, "%.1f min", minutes), left,
                bottom + u * 0.75f, Paint.Align.LEFT);
    }

    private void grid(Canvas canvas, float left, float top, float right, float bottom, float lo,
                      float hi) {
        for (int i = 0; i <= 2; i++) {
            final float y = bottom - (bottom - top) * i / 2f;
            hairline(canvas, left, y, right, y, u * 0.03f, Color.argb(38, 190, 214, 240), false);
            final float v = lo + (hi - lo) * i / 2f;
            label(canvas, String.format(Locale.US, "%.0f", v), left - u * 0.2f, y + u * 0.14f,
                  Paint.Align.RIGHT);
        }
    }

    private void ground(Canvas canvas, float left, float top, float right, float bottom, int n,
                        float lo, float span) {
        path.reset();
        path.moveTo(left, bottom);
        for (int i = 0; i < n; i++) {
            final float x = left + (right - left) * i / (n - 1f);
            final float v = known(terrain[i]) ? terrain[i] : lo;
            path.lineTo(x, yFor(v, top, bottom, lo, span));
        }
        path.lineTo(right, bottom);
        path.close();
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(Color.argb(120, 96, 74, 52));
        canvas.drawPath(path, paint);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(u * 0.07f);
        paint.setColor(Color.argb(190, 168, 128, 88));
        canvas.drawPath(path, paint);
    }

    private void trace(Canvas canvas, float left, float top, float right, float bottom, int n,
                       float[] values, float lo, float span, int colour, boolean filled) {
        path.reset();
        for (int i = 0; i < n; i++) {
            final float x = left + (right - left) * i / (n - 1f);
            final float y = yFor(values[i], top, bottom, lo, span);
            if (i == 0) {
                path.moveTo(x, y);
            } else {
                path.lineTo(x, y);
            }
        }
        if (filled) {
            path.lineTo(right, bottom);
            path.lineTo(left, bottom);
            path.close();
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(Color.argb(46, Color.red(colour), Color.green(colour),
                    Color.blue(colour)));
            canvas.drawPath(path, paint);
        }
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(u * 0.10f);
        paint.setColor(colour);
        canvas.drawPath(path, paint);
    }

    /** Voltage on its own scale, dim and thin: a shape to notice, not a number to read here. */
    private void voltage(Canvas canvas, float left, float top, float right, float bottom,
                         int n) {
        final float lo = 3.2f;
        final float hi = 4.25f;
        path.reset();
        for (int i = 0; i < n; i++) {
            final float x = left + (right - left) * i / (n - 1f);
            final float y = yFor(cell[i], top, bottom, lo, hi - lo);
            if (i == 0) {
                path.moveTo(x, y);
            } else {
                path.lineTo(x, y);
            }
        }
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(u * 0.07f);
        paint.setColor(Color.argb(170, 255, 196, 84));
        canvas.drawPath(path, paint);
        text(canvas, String.format(Locale.US, "%.2f", cell[n - 1]), right + u * 0.15f,
                yFor(cell[n - 1], top, bottom, lo, hi - lo) + u * 0.18f, u * 0.46f, WARN,
                Paint.Align.LEFT, false);
    }

    /**
     * NaN means not looked up yet, negative infinity means looked up and not covered by the
     * height model. Neither is a value, and neither may scale the axis.
     */
    private static boolean known(float terrain) {
        return !Float.isNaN(terrain) && !Float.isInfinite(terrain);
    }

    private static float yFor(float value, float top, float bottom, float lo, float span) {
        final float f = span <= 0f ? 0.5f : (value - lo) / span;
        return bottom - (bottom - top) * Math.max(0f, Math.min(1f, f));
    }
}
