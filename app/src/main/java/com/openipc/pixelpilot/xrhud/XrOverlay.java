package com.openipc.pixelpilot.xrhud;

import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.PorterDuff;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.os.Handler;
import android.os.HandlerThread;
import android.view.Surface;

/**
 * One compositor overlay layer and the thread that draws it.
 *
 * <p>Each layer is redrawn on its own thread, so a slow instrument delays that instrument and
 * nothing else - the compositor reprojects each layer independently, including the video.
 *
 * <p>Subclasses get the shared look from here rather than choosing their own. Two rules shape
 * it, and they pull in opposite directions:
 *
 * <ul>
 *   <li><b>Over the video</b> - line art only, no fills, every glyph outlined. A panel behind
 *       the symbology would hide the thing the pilot is actually looking at.
 *   <li><b>Beside the video</b> - real cards with a translucent dark fill. There is no video
 *       behind them, so a card reads far better than floating outlined text, and it is what
 *       makes the instruments look like instruments.
 * </ul>
 */
public abstract class XrOverlay {

    // A restrained palette: one accent, two warning levels, everything else neutral. Colour
    // means something here, so it is spent sparingly.
    protected static final int INK = Color.rgb(240, 246, 252);
    protected static final int INK_DIM = Color.rgb(150, 163, 180);
    protected static final int ACCENT = Color.rgb(70, 225, 205);
    protected static final int WARN = Color.rgb(255, 196, 84);
    protected static final int ALERT = Color.rgb(255, 92, 104);
    protected static final int PANEL = Color.argb(158, 10, 14, 20);
    protected static final int PANEL_EDGE = Color.argb(58, 190, 214, 240);

    protected final Surface surface;
    protected final int width;
    protected final int height;

    /** One layout unit: everything is expressed in these so a layer scales with its canvas. */
    protected final float u;

    private final HandlerThread thread;
    private final Handler handler;
    private final long frameMs;
    private volatile boolean running;

    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint halo = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF rect = new RectF();

    protected XrOverlay(String name, Surface surface, int width, int height, int unitDivisor,
                        long frameMs) {
        this.surface = surface;
        this.width = width;
        this.height = height;
        this.u = height / (float) unitDivisor;
        this.frameMs = frameMs;
        thread = new HandlerThread(name);
        thread.start();
        handler = new Handler(thread.getLooper());

        stroke.setStyle(Paint.Style.STROKE);
        stroke.setStrokeCap(Paint.Cap.ROUND);
        halo.setStyle(Paint.Style.STROKE);
        halo.setStrokeCap(Paint.Cap.ROUND);
        halo.setColor(Color.argb(168, 0, 0, 0));
        fill.setTypeface(Typeface.create("sans-serif", Typeface.NORMAL));
    }

    public final void start() {
        if (!running) {
            running = true;
            handler.post(this::frame);
        }
    }

    public final void stop() {
        running = false;
        handler.removeCallbacksAndMessages(null);
        thread.quitSafely();
    }

    /** Draw one frame. The canvas is already cleared to fully transparent. */
    protected abstract void draw(Canvas canvas);

    private void frame() {
        if (!running) {
            return;
        }
        Canvas canvas = null;
        try {
            canvas = surface.lockHardwareCanvas();
            if (canvas != null) {
                canvas.drawColor(0, PorterDuff.Mode.CLEAR);
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
            handler.postDelayed(this::frame, frameMs);
        }
    }

    // ------------------------------------------------------------------ drawing kit

    /** A card: translucent fill, hairline edge, generous corner. */
    protected final void card(Canvas canvas, float left, float top, float right, float bottom) {
        rect.set(left, top, right, bottom);
        final float r = u * 0.55f;
        fill.setStyle(Paint.Style.FILL);
        fill.setColor(PANEL);
        canvas.drawRoundRect(rect, r, r, fill);
        stroke.setStrokeWidth(Math.max(1.5f, u * 0.035f));
        stroke.setColor(PANEL_EDGE);
        canvas.drawRoundRect(rect, r, r, stroke);
    }

    /** A big number with a small label above it - the unit of a modern instrument panel. */
    protected final void readout(Canvas canvas, float cx, float baseline, String label,
                                 String value, String suffix, int colour) {
        label(canvas, label, cx, baseline - u * 1.35f, Paint.Align.CENTER);
        text(canvas, value, cx, baseline, u * 1.25f, colour, Paint.Align.CENTER, false);
        if (suffix != null && !suffix.isEmpty()) {
            label(canvas, suffix, cx, baseline + u * 0.62f, Paint.Align.CENTER);
        }
    }

    /** Small, wide-tracked, dim: a label should be findable but never compete with a value. */
    protected final void label(Canvas canvas, String s, float x, float y, Paint.Align align) {
        fill.setStyle(Paint.Style.FILL);
        fill.setColor(INK_DIM);
        fill.setTextSize(u * 0.40f);
        fill.setTextAlign(align);
        fill.setLetterSpacing(0.20f);
        fill.setFakeBoldText(false);
        canvas.drawText(s.toUpperCase(), x, y, fill);
        fill.setLetterSpacing(0f);
    }

    protected final void text(Canvas canvas, String s, float x, float y, float size, int colour,
                              Paint.Align align, boolean outlined) {
        if (outlined) {
            halo.setTextSize(size);
            halo.setTextAlign(align);
            halo.setStrokeWidth(Math.max(2.5f, size * 0.17f));
            halo.setFakeBoldText(true);
            canvas.drawText(s, x, y, halo);
        }
        fill.setStyle(Paint.Style.FILL);
        fill.setTextSize(size);
        fill.setTextAlign(align);
        fill.setColor(colour);
        fill.setFakeBoldText(outlined);
        canvas.drawText(s, x, y, fill);
    }

    /** A line with a dark underlay, so it reads over a bright sky and a dark treeline alike. */
    protected final void hairline(Canvas canvas, float x1, float y1, float x2, float y2,
                                 float w, int colour, boolean outlined) {
        if (outlined) {
            halo.setStrokeWidth(w + Math.max(2f, w * 0.9f));
            canvas.drawLine(x1, y1, x2, y2, halo);
        }
        stroke.setStrokeWidth(w);
        stroke.setColor(colour);
        canvas.drawLine(x1, y1, x2, y2, stroke);
    }

    /**
     * A thin progress arc. Reads faster than a number for anything with a range, which is why
     * the battery and the throttle use one and the altimeter does not.
     */
    protected final void arc(Canvas canvas, float cx, float cy, float radius, float startDeg,
                             float sweepDeg, float fraction, int colour) {
        rect.set(cx - radius, cy - radius, cx + radius, cy + radius);
        final float w = u * 0.22f;
        stroke.setStrokeWidth(w);
        stroke.setColor(Color.argb(70, 190, 214, 240));
        canvas.drawArc(rect, startDeg, sweepDeg, false, stroke);
        stroke.setColor(colour);
        canvas.drawArc(rect, startDeg, sweepDeg * Math.max(0f, Math.min(1f, fraction)), false,
                stroke);
    }

    /** A rounded bar, for a value with a natural full scale. */
    protected final void bar(Canvas canvas, float left, float top, float right, float bottom,
                             float fraction, int colour) {
        final float r = (bottom - top) * 0.5f;
        rect.set(left, top, right, bottom);
        fill.setStyle(Paint.Style.FILL);
        fill.setColor(Color.argb(70, 190, 214, 240));
        canvas.drawRoundRect(rect, r, r, fill);
        final float f = Math.max(0f, Math.min(1f, fraction));
        if (f > 0.001f) {
            rect.set(left, top, left + (right - left) * f, bottom);
            fill.setColor(colour);
            canvas.drawRoundRect(rect, r, r, fill);
        }
    }

    protected static int batteryColour(float perCell) {
        return perCell < 3.5f ? ALERT : perCell < 3.7f ? WARN : INK;
    }
}
