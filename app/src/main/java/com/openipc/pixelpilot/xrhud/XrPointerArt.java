package com.openipc.pixelpilot.xrhud;

import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.PorterDuff;
import android.graphics.PorterDuffXfermode;
import android.graphics.RectF;
import android.graphics.Shader;
import android.view.Surface;

import com.openipc.xr.XrGoggleSession;

/**
 * The pointer's two pictures: the beam, and the cursor as a strip of square frames.
 *
 * <p>They are drawn here, once, and placed by the native pointer every frame - it picks which
 * frame of the strip to show, so a hover, a hold or a hand gesture filling up costs no redraw.
 * The frame order is xri::CursorFrame's, mirrored in {@link XrGoggleSession}.
 */
public final class XrPointerArt extends XrOverlay {

    private static final int HALO = Color.argb(110, 0, 0, 0);
    private static final int FAINT = Color.argb(80, 240, 246, 252);

    private final boolean isBeam;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF oval = new RectF();

    public static XrPointerArt beam(Surface surface, int width, int height) {
        return new XrPointerArt("xr-pointer-beam", surface, width, height, true);
    }

    public static XrPointerArt cursor(Surface surface, int width, int height) {
        return new XrPointerArt("xr-pointer-cursor", surface, width, height, false);
    }

    private XrPointerArt(String name, Surface surface, int width, int height, boolean isBeam) {
        // Redrawn once a second although nothing changes: two tiny bitmaps, and a compositor
        // that ever drops a buffer gets it back without anyone having to notice.
        super(name, surface, width, height, 8, 1000);
        this.isBeam = isBeam;
    }

    @Override
    protected void draw(Canvas canvas) {
        if (isBeam) {
            drawBeam(canvas);
        } else {
            drawCursor(canvas);
        }
    }

    /**
     * The top of the picture is the far end of the beam and the bottom is the hand: the quad
     * is stood along the beam with its +Y pointing away.
     */
    private void drawBeam(Canvas c) {
        final float w = width;
        final float h = height;
        // Across: a bright core that fades to nothing, so the edge never aliases into steps.
        paint.setShader(new LinearGradient(0f, 0f, w, 0f,
                new int[]{0x00FFFFFF, 0xC8FFFFFF, 0xFFFFFFFF, 0xC8FFFFFF, 0x00FFFFFF},
                new float[]{0f, 0.3f, 0.5f, 0.7f, 1f}, Shader.TileMode.CLAMP));
        c.drawRect(0f, 0f, w, h, paint);
        // Along: faint where it leaves the hand, solid through the middle, thinner towards the
        // far end - a miss should read as "pointing there", not as a laser to the horizon.
        paint.setShader(new LinearGradient(0f, h, 0f, 0f,
                new int[]{0x26000000, 0xE0000000, 0xC0000000, 0x60000000},
                new float[]{0f, 0.12f, 0.7f, 1f}, Shader.TileMode.CLAMP));
        paint.setXfermode(new PorterDuffXfermode(PorterDuff.Mode.DST_IN));
        c.drawRect(0f, 0f, w, h, paint);
        paint.setXfermode(null);
        paint.setShader(null);
    }

    private void drawCursor(Canvas c) {
        final int cell = height;
        final float r = cell / 2f;
        for (int frame = 0; frame < XrGoggleSession.CURSOR_FRAME_COUNT; frame++) {
            // Everything stays well inside its own square, so filtering never pulls in a
            // sliver of the neighbouring frame.
            final float cx = frame * cell + r;
            final float cy = r;
            if (frame == XrGoggleSession.CURSOR_IDLE) {
                disc(c, cx, cy, r * 0.40f, HALO);
                disc(c, cx, cy, r * 0.28f, INK);
            } else if (frame == XrGoggleSession.CURSOR_HOVER) {
                // On something the trigger would take hold of.
                disc(c, cx, cy, r * 0.64f, HALO);
                ring(c, cx, cy, r * 0.48f, r * 0.14f, ACCENT, 0f, 360f);
                disc(c, cx, cy, r * 0.20f, INK);
            } else if (frame == XrGoggleSession.CURSOR_HELD) {
                disc(c, cx, cy, r * 0.64f, HALO);
                disc(c, cx, cy, r * 0.54f, ACCENT);
                disc(c, cx, cy, r * 0.22f, INK);
            } else {
                // A held hand gesture, in eighths, filling clockwise from the top.
                final int step = frame - XrGoggleSession.CURSOR_PROGRESS_FIRST + 1;
                disc(c, cx, cy, r * 0.70f, HALO);
                ring(c, cx, cy, r * 0.56f, r * 0.16f, FAINT, 0f, 360f);
                ring(c, cx, cy, r * 0.56f, r * 0.16f, ACCENT, -90f,
                        360f * step / XrGoggleSession.CURSOR_PROGRESS_STEPS);
                disc(c, cx, cy, r * 0.16f, INK);
            }
        }
    }

    private void disc(Canvas c, float cx, float cy, float radius, int colour) {
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(colour);
        c.drawCircle(cx, cy, radius, paint);
    }

    private void ring(Canvas c, float cx, float cy, float radius, float stroke, int colour,
                      float startDeg, float sweepDeg) {
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(stroke);
        paint.setStrokeCap(sweepDeg >= 360f ? Paint.Cap.BUTT : Paint.Cap.ROUND);
        paint.setColor(colour);
        oval.set(cx - radius, cy - radius, cx + radius, cy + radius);
        c.drawArc(oval, startDeg, sweepDeg, false, paint);
    }
}
