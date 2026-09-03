package com.openipc.pixelpilot.xrhud;

import android.graphics.Canvas;
import android.graphics.Paint;
import android.view.Surface;

import java.util.Locale;

/**
 * The instrument strip below the video, on a curved layer.
 *
 * <p>Nothing here overlays the image, so this is the one place cards belong: a translucent
 * panel behind a value reads far better than outlined text floating over passthrough, and the
 * curve gives the strip parallax across its width, which is what makes it sit in space rather
 * than in front of the eye.
 *
 * <p>Four cards, two by two, in the order a pilot asks the questions: how much is left, how
 * fast and how high, how much am I asking of it, and is the link healthy. Two rows rather than
 * one because the video quad is already about +-34 degrees wide, so horizontal room is scarce
 * and the map and the chart beside this need it more.
 */
public final class XrDashboard extends XrOverlay {

    private final FlightData data;
    private String note = "";

    public XrDashboard(Surface surface, int width, int height, FlightData data) {
        // Slower than the symbology: none of these numbers rewards 25Hz.
        super("XrDashboard", surface, width, height, 9, 100);
        this.data = data;
    }

    /** A short line for transient state - recording, a warning, a mode change. */
    public void setNote(String text) {
        note = text == null ? "" : text;
    }

    @Override
    protected void draw(Canvas canvas) {
        final FlightData.Snapshot s = data.snapshot();
        final float pad = u * 0.42f;
        final float gap = u * 0.36f;
        // Two by two. Horizontal room is what the map and the chart beside this need, so the
        // dashboard takes the vertical instead.
        final float cardW = (width - pad * 2 - gap) / 2f;
        final float cardH = (height - pad * 2 - gap) / 2f;
        final float x0 = pad;
        final float x1 = pad + cardW + gap;
        final float y0 = pad;
        final float y1 = pad + cardH + gap;

        battery(canvas, x0, y0, x0 + cardW, y0 + cardH, s);
        flight(canvas, x1, y0, x1 + cardW, y0 + cardH, s);
        demand(canvas, x0, y1, x0 + cardW, y1 + cardH, s);
        link(canvas, x1, y1, x1 + cardW, y1 + cardH, s);
    }

    private void battery(Canvas canvas, float l, float t, float r, float b,
                         FlightData.Snapshot s) {
        card(canvas, l, t, r, b);
        if (!s.fresh) {
            label(canvas, "battery", (l + r) / 2f, (t + b) / 2f, Paint.Align.CENTER);
            return;
        }
        final int colour = batteryColour(s.perCell);
        // The arc is the state of charge from empty to full cell, which is the thing that
        // matters; the numbers underneath are the detail.
        final float frac = (s.perCell - 3.3f) / (4.2f - 3.3f);
        final float cy = t + (b - t) * 0.52f;
        final float cx = l + (r - l) * 0.26f;
        arc(canvas, cx, cy, u * 1.30f, 140f, 260f, frac, colour);
        text(canvas, String.format(Locale.US, "%.2f", s.perCell), cx, cy + u * 0.22f, u * 0.86f,
                colour, Paint.Align.CENTER, false);
        label(canvas, "v/cell", cx, cy + u * 0.86f, Paint.Align.CENTER);

        final float tx = l + (r - l) * 0.52f;
        label(canvas, s.cells + "s pack", tx, t + u * 1.05f, Paint.Align.LEFT);
        text(canvas, String.format(Locale.US, "%.2f V", s.volts), tx, t + u * 1.95f, u * 0.62f,
                INK, Paint.Align.LEFT, false);
        text(canvas, String.format(Locale.US, "%.1f A", s.amps), tx, t + u * 2.75f, u * 0.62f,
                INK, Paint.Align.LEFT, false);
        text(canvas, String.format(Locale.US, "%.0f mAh", s.consumedMah), tx, t + u * 3.55f,
                u * 0.62f, INK_DIM, Paint.Align.LEFT, false);
    }

    private void flight(Canvas canvas, float l, float t, float r, float b,
                        FlightData.Snapshot s) {
        card(canvas, l, t, r, b);
        if (!s.fresh) {
            label(canvas, "flight", (l + r) / 2f, (t + b) / 2f, Paint.Align.CENTER);
            return;
        }
        final float third = (r - l) / 3f;
        final float base = t + (b - t) * 0.60f;
        readout(canvas, l + third * 0.5f, base, "alt", String.format(Locale.US, "%.0f", s.altitude),
                "m", INK);
        readout(canvas, l + third * 1.5f, base, "speed",
                String.format(Locale.US, "%.0f", s.groundSpeed), "m/s", INK);
        final int hdg = ((int) s.yaw % 360 + 360) % 360;
        readout(canvas, l + third * 2.5f, base, "heading", String.format(Locale.US, "%03d", hdg),
                cardinal(hdg), INK);
    }

    private void demand(Canvas canvas, float l, float t, float r, float b,
                        FlightData.Snapshot s) {
        card(canvas, l, t, r, b);
        if (!s.fresh) {
            label(canvas, "throttle", (l + r) / 2f, (t + b) / 2f, Paint.Align.CENTER);
            return;
        }
        final float pad = u * 0.7f;
        label(canvas, "throttle", l + pad, t + u * 1.05f, Paint.Align.LEFT);
        text(canvas, String.format(Locale.US, "%.0f%%", s.throttlePct), r - pad, t + u * 1.05f,
                u * 0.62f, INK, Paint.Align.RIGHT, false);
        bar(canvas, l + pad, t + u * 1.45f, r - pad, t + u * 1.85f, s.throttlePct / 100f, ACCENT);

        label(canvas, "climb", l + pad, t + u * 2.85f, Paint.Align.LEFT);
        final int climbColour = s.climb < -3f ? WARN : INK;
        text(canvas, String.format(Locale.US, "%+.1f m/s", s.climb), r - pad, t + u * 2.85f,
                u * 0.62f, climbColour, Paint.Align.RIGHT, false);

        label(canvas, s.armed ? "armed" : "disarmed", l + pad, t + u * 3.75f, Paint.Align.LEFT);
        if (!note.isEmpty()) {
            text(canvas, note, r - pad, t + u * 3.75f, u * 0.55f, WARN, Paint.Align.RIGHT, false);
        }
    }

    private void link(Canvas canvas, float l, float t, float r, float b,
                      FlightData.Snapshot s) {
        card(canvas, l, t, r, b);
        final float pad = u * 0.7f;
        if (s.linkQuality < 0) {
            label(canvas, "link", (l + r) / 2f, (t + b) / 2f, Paint.Align.CENTER);
            return;
        }
        // avg_rssi is a 0..100 figure here, not dBm, which is why the flat OSD colours it at
        // 30 and 60. Same thresholds, so the two agree.
        final int colour = s.linkQuality < 30 ? ALERT : s.linkQuality < 60 ? WARN : INK;
        label(canvas, "link", l + pad, t + u * 1.05f, Paint.Align.LEFT);
        text(canvas, String.valueOf(s.linkQuality), r - pad, t + u * 1.25f, u * 0.90f, colour,
                Paint.Align.RIGHT, false);
        bar(canvas, l + pad, t + u * 1.60f, r - pad, t + u * 2.00f, s.linkQuality / 100f, colour);

        label(canvas, "fec fixed", l + pad, t + u * 2.85f, Paint.Align.LEFT);
        text(canvas, String.valueOf(s.fecRecovered), r - pad, t + u * 2.85f, u * 0.58f,
                s.fecRecovered > 0 ? WARN : INK_DIM, Paint.Align.RIGHT, false);
        label(canvas, "lost", l + pad, t + u * 3.75f, Paint.Align.LEFT);
        text(canvas, String.valueOf(s.lost), r - pad, t + u * 3.75f, u * 0.58f,
                s.lost > 0 ? ALERT : INK_DIM, Paint.Align.RIGHT, false);

        if (s.fresh) {
            final String sats = s.gpsFix ? s.sats + " sat" : s.sats + " sat no fix";
            label(canvas, sats, (l + r) / 2f, b - u * 0.42f, Paint.Align.CENTER);
        }
    }

    private static String cardinal(int deg) {
        if (deg > 337 || deg <= 22) return "N";
        if (deg <= 67) return "NE";
        if (deg <= 112) return "E";
        if (deg <= 157) return "SE";
        if (deg <= 202) return "S";
        if (deg <= 247) return "SW";
        if (deg <= 292) return "W";
        return "NW";
    }
}
