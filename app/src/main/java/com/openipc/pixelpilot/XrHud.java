package com.openipc.pixelpilot;

import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PorterDuff;
import android.os.Handler;
import android.os.HandlerThread;
import android.view.Surface;

import androidx.annotation.Nullable;

import com.openipc.mavlink.MavlinkData;
import com.openipc.wfbngrtl8812.WfbNGStats;

import java.util.Locale;

/**
 * Flight instruments drawn onto their own compositor layer.
 *
 * <p>This is not the flat mode's OSD. That one is a stack of Android views over a
 * {@code SurfaceView}; here the target is a transparent quad layer in front of the video, so
 * the instruments are composited at panel resolution instead of being drawn into a stream
 * that has already been through an H.265 encoder. Nothing the pilot reads goes through the
 * air link twice.
 *
 * <p>Everything is drawn with {@link Surface#lockHardwareCanvas()} on a dedicated thread, so
 * a slow frame delays the HUD and never the video - the two layers are independent and the
 * compositor reprojects whichever one is stale.
 *
 * <p>Design rules, because a HUD that is merely informative is a distraction while flying:
 * the middle of the view stays clear, every glyph carries a dark outline so it reads against
 * a bright sky and a dark treeline alike, and anything the flight controller has not sent is
 * left out rather than shown as a zero.
 */
final class XrHud {

    /** Redraw rate. The air side sends at about 20Hz, so more would draw the same numbers. */
    private static final long FRAME_MS = 40;

    /** Telemetry older than this is not shown as if it were current. */
    private static final long STALE_MS = 2000;

    private static final int COL_PRIMARY = Color.rgb(235, 245, 255);
    private static final int COL_ACCENT = Color.rgb(120, 235, 160);
    private static final int COL_WARN = Color.rgb(255, 190, 70);
    private static final int COL_ALERT = Color.rgb(255, 95, 95);
    private static final int COL_HORIZON = Color.rgb(150, 220, 255);

    private final Surface surface;
    private final int width;
    private final int height;
    private final HandlerThread thread;
    private final Handler handler;

    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint outline = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint line = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint lineOutline = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path path = new Path();

    private volatile boolean running;
    @Nullable
    private volatile MavlinkData telemetry;
    private volatile long telemetryAt;
    @Nullable
    private volatile WfbNGStats link;
    private volatile String note = "";

    XrHud(Surface surface, int width, int height) {
        this.surface = surface;
        this.width = width;
        this.height = height;
        thread = new HandlerThread("XrHud");
        thread.start();
        handler = new Handler(thread.getLooper());

        outline.setStyle(Paint.Style.STROKE);
        outline.setColor(Color.argb(190, 0, 0, 0));
        line.setStyle(Paint.Style.STROKE);
        line.setStrokeCap(Paint.Cap.ROUND);
        lineOutline.setStyle(Paint.Style.STROKE);
        lineOutline.setStrokeCap(Paint.Cap.ROUND);
        lineOutline.setColor(Color.argb(170, 0, 0, 0));
    }

    void start() {
        if (running) {
            return;
        }
        running = true;
        handler.post(this::frame);
    }

    void stop() {
        running = false;
        handler.removeCallbacksAndMessages(null);
        thread.quitSafely();
    }

    void onTelemetry(MavlinkData data) {
        telemetry = data;
        telemetryAt = System.currentTimeMillis();
    }

    void onLink(WfbNGStats stats) {
        link = stats;
    }

    /** A short line under the instruments, for recording state and the like. */
    void setNote(String text) {
        note = text == null ? "" : text;
    }

    // ------------------------------------------------------------------ drawing

    private void frame() {
        if (!running) {
            return;
        }
        Canvas canvas = null;
        try {
            canvas = surface.lockHardwareCanvas();
            if (canvas != null) {
                draw(canvas);
            }
        } catch (IllegalArgumentException | IllegalStateException e) {
            // The surface goes away when the session ends; stop rather than spin on it.
            running = false;
            return;
        } finally {
            if (canvas != null) {
                try {
                    surface.unlockCanvasAndPost(canvas);
                } catch (IllegalArgumentException | IllegalStateException ignored) {
                    running = false;
                }
            }
        }
        if (running) {
            handler.postDelayed(this::frame, FRAME_MS);
        }
    }

    private void draw(Canvas canvas) {
        // Transparent everywhere the HUD does not draw, so the video shows through.
        canvas.drawColor(0, PorterDuff.Mode.CLEAR);

        final float cx = width / 2f;
        final float cy = height / 2f;
        final float unit = height / 24f;  // one "line" of layout

        final MavlinkData t = telemetry;
        final boolean fresh = t != null && (System.currentTimeMillis() - telemetryAt) < STALE_MS;

        if (fresh) {
            drawHorizon(canvas, cx, cy, unit, t);
            drawHeading(canvas, cx, unit, t);
            drawTape(canvas, unit * 2.2f, cy, unit, t.telemetryAltitude, "ALT", "m", true);
            drawTape(canvas, width - unit * 2.2f, cy, unit, t.telemetryGSpeed, "SPD", "m/s", false);
            drawBattery(canvas, unit, height - unit * 1.2f, unit, t);
            drawSats(canvas, cx, height - unit * 0.8f, unit, t);
            drawHome(canvas, cx, cy, unit, t);
        } else {
            text(canvas, "NO TELEMETRY", cx, cy - unit * 4f, unit * 0.8f, COL_ALERT, Paint.Align.CENTER);
        }

        // The reticle is always drawn: it is the one thing that has to be there even with no
        // flight controller at all, because it is what the pilot aims with.
        drawReticle(canvas, cx, cy, unit);
        drawLink(canvas, width - unit, height - unit * 1.2f, unit);

        if (!note.isEmpty()) {
            text(canvas, note, cx, height - unit * 2.2f, unit * 0.6f, COL_WARN, Paint.Align.CENTER);
        }
    }

    /**
     * Pitch ladder that rolls about the centre, with a fixed reference at the middle.
     *
     * <p>The ladder is clipped to a band rather than filling the view: a full-screen rolling
     * grid is what makes simulator HUDs unreadable over real video.
     */
    private void drawHorizon(Canvas canvas, float cx, float cy, float unit, MavlinkData t) {
        final float pxPerDegree = unit * 0.42f;
        final float half = unit * 5.5f;

        canvas.save();
        canvas.clipRect(cx - half, cy - half * 0.8f, cx + half, cy + half * 0.8f);
        canvas.rotate(-t.telemetryRoll, cx, cy);
        canvas.translate(0, t.telemetryPitch * pxPerDegree);

        stroke(unit * 0.055f, COL_HORIZON);
        // The horizon itself, with a gap in the middle for the reference.
        final float gap = unit * 0.9f;
        seg(canvas, cx - half, cy, cx - gap, cy);
        seg(canvas, cx + gap, cy, cx + half, cy);

        stroke(unit * 0.045f, COL_HORIZON);
        fill.setTextSize(unit * 0.42f);
        for (int deg = -30; deg <= 30; deg += 10) {
            if (deg == 0) {
                continue;
            }
            final float y = cy - deg * pxPerDegree;
            final float w = (deg % 20 == 0) ? unit * 2.4f : unit * 1.4f;
            // Ticks point towards the horizon, the usual convention for which way is up.
            final float tick = deg > 0 ? unit * 0.3f : -unit * 0.3f;
            seg(canvas, cx - w, y, cx - unit * 0.5f, y);
            seg(canvas, cx - w, y, cx - w, y + tick);
            seg(canvas, cx + unit * 0.5f, y, cx + w, y);
            seg(canvas, cx + w, y, cx + w, y + tick);
            if (deg % 20 == 0) {
                text(canvas, String.valueOf(Math.abs(deg)), cx - w - unit * 0.25f, y + unit * 0.15f,
                        unit * 0.38f, COL_HORIZON, Paint.Align.RIGHT);
            }
        }
        canvas.restore();

        // Roll readout, outside the clip so it is never cut off.
        text(canvas, String.format(Locale.US, "%+.0f°", t.telemetryRoll),
                cx, cy - half * 0.8f - unit * 0.3f, unit * 0.45f, COL_PRIMARY, Paint.Align.CENTER);
    }

    /** Fixed aircraft reference: what the ladder rolls around. */
    private void drawReticle(Canvas canvas, float cx, float cy, float unit) {
        stroke(unit * 0.08f, COL_ACCENT);
        final float a = unit * 0.75f;
        final float b = unit * 0.25f;
        seg(canvas, cx - a, cy, cx - b, cy);
        seg(canvas, cx + b, cy, cx + a, cy);
        seg(canvas, cx, cy - b * 0.6f, cx, cy + b * 0.6f);
    }

    private void drawHeading(Canvas canvas, float cx, float unit, MavlinkData t) {
        final int hdg = ((int) t.telemetryYaw % 360 + 360) % 360;
        final float y = unit * 1.5f;
        text(canvas, String.format(Locale.US, "%03d°", hdg), cx, y, unit * 0.7f,
                COL_PRIMARY, Paint.Align.CENTER);

        // A short tape either side, so a turn is visible as movement and not only as a number.
        stroke(unit * 0.04f, COL_PRIMARY);
        final float span = unit * 4.5f;
        final float pxPerDeg = span / 45f;
        canvas.save();
        canvas.clipRect(cx - span, y - unit, cx + span, y + unit * 0.5f);
        for (int d = -45; d <= 45; d += 15) {
            final int mark = ((hdg + d) % 360 + 360) % 360;
            final float x = cx + d * pxPerDeg;
            seg(canvas, x, y + unit * 0.15f, x, y + unit * 0.35f);
            text(canvas, cardinal(mark), x, y + unit * 0.32f + unit * 0.35f, unit * 0.32f,
                    COL_PRIMARY, Paint.Align.CENTER);
        }
        canvas.restore();
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

    /** A boxed number with a label, on the left or right edge. */
    private void drawTape(Canvas canvas, float x, float cy, float unit, float value,
                          String label, String suffix, boolean leftSide) {
        final Paint.Align align = leftSide ? Paint.Align.LEFT : Paint.Align.RIGHT;
        text(canvas, label, x, cy - unit * 1.1f, unit * 0.36f, COL_PRIMARY, align);
        text(canvas, String.format(Locale.US, "%.0f", value), x, cy, unit * 0.95f,
                COL_PRIMARY, align);
        text(canvas, suffix, x, cy + unit * 0.6f, unit * 0.36f, COL_PRIMARY, align);
    }

    private void drawBattery(Canvas canvas, float x, float y, float unit, MavlinkData t) {
        final float volts = t.telemetryBattery;
        // Per-cell is the number that means something across pack sizes; 6S at 19.9V is fine,
        // 4S at 19.9V is not, and only the cell count tells them apart.
        final int cells = volts > 21f ? 6 : volts > 14f ? 4 : volts > 7f ? 2 : 1;
        final float perCell = cells > 0 ? volts / cells : volts;
        final int colour = perCell < 3.5f ? COL_ALERT : perCell < 3.7f ? COL_WARN : COL_PRIMARY;

        text(canvas, String.format(Locale.US, "%.2fV", volts), x, y, unit * 0.7f, colour,
                Paint.Align.LEFT);
        text(canvas, String.format(Locale.US, "%.2fV/cell  %.1fA  %.0fmAh", perCell,
                        t.telemetryCurrent, t.telemetryCurrentConsumed),
                x, y + unit * 0.55f, unit * 0.36f, colour, Paint.Align.LEFT);
    }

    private void drawSats(Canvas canvas, float cx, float y, float unit, MavlinkData t) {
        final int sats = (int) t.telemetrySat;
        final boolean fix = t.gps_fix_type >= 3;
        text(canvas, String.format(Locale.US, "%d sat%s", sats, fix ? "" : " no fix"),
                cx, y, unit * 0.36f, fix ? COL_PRIMARY : COL_WARN, Paint.Align.CENTER);
    }

    /** Direction and distance home, as an arrow relative to where the nose is pointing. */
    private void drawHome(Canvas canvas, float cx, float cy, float unit, MavlinkData t) {
        if (t.gps_fix_type < 3 || t.telemetryDistance < 1) {
            return;
        }
        final float y = cy + unit * 5.5f;
        final double rel = Math.toRadians(t.telemetryHdg - t.telemetryYaw);

        canvas.save();
        canvas.translate(cx, y);
        canvas.rotate((float) Math.toDegrees(rel));
        stroke(unit * 0.07f, COL_ACCENT);
        path.reset();
        path.moveTo(0, -unit * 0.55f);
        path.lineTo(unit * 0.38f, unit * 0.45f);
        path.lineTo(0, unit * 0.18f);
        path.lineTo(-unit * 0.38f, unit * 0.45f);
        path.close();
        canvas.drawPath(path, lineOutline);
        canvas.drawPath(path, line);
        canvas.restore();

        text(canvas, String.format(Locale.US, "%.0fm", t.telemetryDistance),
                cx, y + unit * 1.25f, unit * 0.4f, COL_ACCENT, Paint.Align.CENTER);
    }

    private void drawLink(Canvas canvas, float x, float y, float unit) {
        final WfbNGStats s = link;
        if (s == null) {
            return;
        }
        final int colour = s.count_p_lost > 0 ? COL_ALERT
                : s.count_p_fec_recovered > 0 ? COL_WARN : COL_PRIMARY;
        text(canvas, String.format(Locale.US, "%d dBm", s.avg_rssi), x, y,
                unit * 0.36f, colour, Paint.Align.RIGHT);
        text(canvas, String.format(Locale.US, "fec %d   lost %d", s.count_p_fec_recovered,
                s.count_p_lost), x, y + unit * 0.5f, unit * 0.36f, colour, Paint.Align.RIGHT);
    }

    // ------------------------------------------------------------------ helpers

    private void stroke(float w, int colour) {
        line.setStrokeWidth(w);
        line.setColor(colour);
        lineOutline.setStrokeWidth(w + Math.max(2f, w * 0.9f));
    }

    /** One segment, dark underlay first so it reads over anything. */
    private void seg(Canvas canvas, float x1, float y1, float x2, float y2) {
        canvas.drawLine(x1, y1, x2, y2, lineOutline);
        canvas.drawLine(x1, y1, x2, y2, line);
    }

    private void text(Canvas canvas, String s, float x, float y, float size, int colour,
                      Paint.Align align) {
        fill.setTextSize(size);
        fill.setTextAlign(align);
        fill.setColor(colour);
        fill.setFakeBoldText(true);
        outline.setTextSize(size);
        outline.setTextAlign(align);
        outline.setStrokeWidth(Math.max(2.5f, size * 0.16f));
        outline.setFakeBoldText(true);
        canvas.drawText(s, x, y, outline);
        canvas.drawText(s, x, y, fill);
    }
}
