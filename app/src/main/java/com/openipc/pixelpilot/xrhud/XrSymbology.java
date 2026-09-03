package com.openipc.pixelpilot.xrhud;

import android.graphics.Canvas;
import android.graphics.Paint;
import android.view.Surface;

import java.util.Locale;

/**
 * The layer that sits on the video: horizon, pitch ladder, reticle, roll.
 *
 * <p>Flat and exactly aligned with the video quad, because these marks only mean anything
 * where the image is. Everything with a background lives on the dashboard instead - a card
 * here would cover the one thing the pilot is looking at.
 *
 * <p>The ladder is head-locked and aircraft-relative on purpose. An artificial horizon shows
 * how the craft is lying, not how the pilot's head is; hanging it in the room would be a
 * different instrument and a worse one.
 */
public final class XrSymbology extends XrOverlay {

    private final FlightData data;

    public XrSymbology(Surface surface, int width, int height, FlightData data) {
        // 40ms: the air side sends attitude at about 20Hz, so more would redraw the same angle.
        super("XrSymbology", surface, width, height, 20, 40);
        this.data = data;
    }

    @Override
    protected void draw(Canvas canvas) {
        final float cx = width / 2f;
        final float cy = height / 2f;
        final FlightData.Snapshot s = data.snapshot();

        if (s.fresh) {
            ladder(canvas, cx, cy, s);
            rollScale(canvas, cx, cy, s);
        } else {
            text(canvas, "NO TELEMETRY", cx, cy - u * 3.6f, u * 0.72f, ALERT, Paint.Align.CENTER,
                    true);
        }
        // Always drawn, even with no flight controller at all: it is what the pilot aims with.
        reticle(canvas, cx, cy);
    }

    /** Pitch ladder, clipped to a band. A full-screen rolling grid over real video is noise. */
    private void ladder(Canvas canvas, float cx, float cy, FlightData.Snapshot s) {
        final float perDeg = u * 0.34f;
        final float halfW = u * 4.6f;
        final float halfH = u * 3.5f;

        canvas.save();
        canvas.clipRect(cx - halfW, cy - halfH, cx + halfW, cy + halfH);
        canvas.rotate(-s.roll, cx, cy);
        canvas.translate(0, s.pitch * perDeg);

        // The horizon, open in the middle so the reticle stays readable.
        final float gap = u * 0.85f;
        hairline(canvas, cx - halfW, cy, cx - gap, cy, u * 0.055f, ACCENT, true);
        hairline(canvas, cx + gap, cy, cx + halfW, cy, u * 0.055f, ACCENT, true);

        for (int deg = -40; deg <= 40; deg += 10) {
            if (deg == 0) {
                continue;
            }
            final float y = cy - deg * perDeg;
            final boolean major = deg % 20 == 0;
            final float w = major ? u * 2.0f : u * 1.15f;
            // Ticks turn towards the horizon: the usual way of showing which side is up.
            final float tick = deg > 0 ? u * 0.26f : -u * 0.26f;
            hairline(canvas, cx - w, y, cx - u * 0.45f, y, u * 0.04f, INK, true);
            hairline(canvas, cx - w, y, cx - w, y + tick, u * 0.04f, INK, true);
            hairline(canvas, cx + u * 0.45f, y, cx + w, y, u * 0.04f, INK, true);
            hairline(canvas, cx + w, y, cx + w, y + tick, u * 0.04f, INK, true);
            if (major) {
                text(canvas, String.valueOf(Math.abs(deg)), cx - w - u * 0.22f, y + u * 0.13f,
                        u * 0.34f, INK, Paint.Align.RIGHT, true);
            }
        }
        canvas.restore();
    }

    /** Roll arc above the ladder, with a fixed pointer - reads at a glance in a turn. */
    private void rollScale(Canvas canvas, float cx, float cy, FlightData.Snapshot s) {
        final float r = u * 3.9f;
        final float top = cy - r;
        for (int deg = -60; deg <= 60; deg += 15) {
            final double a = Math.toRadians(deg - 90);
            final float len = (deg % 30 == 0) ? u * 0.34f : u * 0.20f;
            final float x1 = cx + (float) Math.cos(a) * r;
            final float y1 = cy + (float) Math.sin(a) * r;
            final float x2 = cx + (float) Math.cos(a) * (r - len);
            final float y2 = cy + (float) Math.sin(a) * (r - len);
            hairline(canvas, x1, y1, x2, y2, u * 0.04f, INK_DIM, true);
        }
        // The pointer moves with the craft, the scale stays put.
        canvas.save();
        canvas.rotate(-s.roll, cx, cy);
        final float t = u * 0.30f;
        hairline(canvas, cx, top + u * 0.06f, cx - t * 0.6f, top + t, u * 0.05f, ACCENT, true);
        hairline(canvas, cx, top + u * 0.06f, cx + t * 0.6f, top + t, u * 0.05f, ACCENT, true);
        canvas.restore();

        text(canvas, String.format(Locale.US, "%+.0f", s.roll), cx, top - u * 0.28f, u * 0.42f,
                INK, Paint.Align.CENTER, true);
    }

    private void reticle(Canvas canvas, float cx, float cy) {
        final float a = u * 0.62f;
        final float b = u * 0.20f;
        hairline(canvas, cx - a, cy, cx - b, cy, u * 0.07f, ACCENT, true);
        hairline(canvas, cx + b, cy, cx + a, cy, u * 0.07f, ACCENT, true);
        hairline(canvas, cx, cy - b * 0.55f, cx, cy + b * 0.55f, u * 0.07f, ACCENT, true);
    }
}
