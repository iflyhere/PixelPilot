package com.openipc.pixelpilot.xrhud;

import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.LinearGradient;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.Shader;
import android.graphics.SurfaceTexture;
import android.util.Log;
import android.view.Surface;

import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;

import com.openipc.mavlink.MavlinkData;
import com.openipc.wfbngrtl8812.WfbNGStats;

import org.junit.Test;
import org.junit.runner.RunWith;

import java.io.File;
import java.io.FileOutputStream;
import java.io.OutputStream;
import java.io.PrintWriter;
import java.net.Socket;
import java.util.Locale;

/**
 * Renders the HUD off the headset, so a layout change can be looked at without wearing one.
 *
 * <p>This is not an OpenXR test and cannot be. What it does exercise is everything that is not
 * OpenXR, which turns out to be most of what one wants to see: the instruments are ordinary
 * Canvas drawing on a Surface, and their {@code draw()} is reachable from this package - so the
 * shipping classes can be pointed at a Bitmap instead of a compositor swapchain and the result
 * written out as a PNG. No fork of the drawing code, no mock instruments.
 *
 * <p>It runs in the app's own process, which is the useful part: {@code getFilesDir()} is the
 * same directory the real offline maps were installed into, so the minimap and the height
 * profile render against the actual basemap and terrain model rather than a stand-in.
 *
 * <p>What is still hardware only: swapchain creation, the quad and cylinder poses as the
 * runtime resolves them, passthrough blending, the vertical flip, action bindings and head
 * tracking. The last picture here composes the layers with the same arithmetic the compositor
 * uses, so it shows the arrangement, but it is a projection of the layout - not proof that the
 * runtime agrees with it.
 *
 * <p>Run it and collect the pictures with:
 *
 * <pre>
 * ./gradlew :app:connectedDebugAndroidTest
 * adb shell run-as com.openipc.pixelpilot ls files/hud-preview
 * </pre>
 */
@RunWith(AndroidJUnit4.class)
public class HudPreviewTest {

    private static final String TAG = "pixelpilot";

    /**
     * Where the simulated flight takes off. Override it to wherever the installed maps
     * actually cover, which is the whole point of being able to look at this:
     *
     * <pre>
     * adb shell am instrument -w -e class ...HudPreviewTest -e lat 48.5582 -e lon 9.2058      *   com.openipc.pixelpilot.test/androidx.test.runner.AndroidJUnitRunner
     * </pre>
     */
    private static final double DEFAULT_LAT = 48.5582;
    private static final double DEFAULT_LON = 9.2058;

    private double homeLat = DEFAULT_LAT;
    private double homeLon = DEFAULT_LON;

    /**
     * Two hertz for two minutes, which is what the position history is sized for.
     *
     * <p>Fed as fast as the loop runs rather than in real time, so the chart's own time axis
     * reads a fraction of a minute - it stamps samples with the wall clock. The shape of the
     * traces is right; the elapsed label is not, and is not meant to be.
     */
    private static final int STEPS = 240;

    private static final float BIAS = 100000f;

    // The layout describeOverlays() sets, copied. It cannot be read from the native side
    // without bringing a session up, which is the one thing this test exists to avoid - so
    // this is a second copy, and a second copy drifts. The native side logs the whole table
    // at session start for exactly this reason:
    //
    //     adb logcat -s pixelpilot-xr:I | grep "layout:"
    //
    // If those lines and these constants disagree, the pictures are lying and these are what
    // is wrong.
    private static final int SYM_W = 2048, SYM_H = 1152;
    private static final int DASH_W = 1536, DASH_H = 1152;
    private static final int MAP_W = 640, MAP_H = 640;
    private static final int CHART_W = 896, CHART_H = 560;

    private File outDir;

    @Test
    public void rendersEveryInstrumentAndTheArrangement() throws Exception {
        final Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        final android.os.Bundle args = InstrumentationRegistry.getArguments();
        homeLat = parseArg(args, "lat", DEFAULT_LAT);
        homeLon = parseArg(args, "lon", DEFAULT_LON);
        Log.i(TAG, String.format(Locale.US, "flying from %.5f, %.5f", homeLat, homeLon));
        outDir = new File(context.getFilesDir(), "hud-preview");
        assertTrue("could not make " + outDir, outDir.isDirectory() || outDir.mkdirs());

        final FlightData data = new FlightData();
        pushCameraHealth();
        // The link card reads these, and the first version of this test never fed them - so
        // the card came out empty and looked like a drawing bug rather than a missing input.
        data.onLink(new WfbNGStats(184200, 3, 183900, 271, 12, 0, 0, 0, -62));

        // A whole flight rather than one frozen sample: the track, the height profile and the
        // climb rate are all history, and a single snapshot leaves them empty.
        final Surface dummy = new Surface(new SurfaceTexture(0));
        final XrSymbology symbology = new XrSymbology(dummy, SYM_W, SYM_H, data);
        final CameraStats camera = new CameraStats();
        camera.start();
        final XrDashboard dashboard = new XrDashboard(dummy, DASH_W, DASH_H, data, camera);
        final XrMinimap minimap = new XrMinimap(dummy, MAP_W, MAP_H, data);
        final XrChart chart = new XrChart(dummy, CHART_W, CHART_H, data);

        // The real map worker, against the real installed files.
        final OfflineMaps maps = new OfflineMaps(data);
        maps.start(context, minimap);
        Log.i(TAG, "maps installed: basemap=" + MapFiles.have(context, MapFiles.Kind.BASEMAP)
                + " terrain=" + MapFiles.have(context, MapFiles.Kind.TERRAIN));

        for (int i = 0; i < STEPS; i++) {
            data.onTelemetry(sample(i));
            // The height profile is filled by the map worker off the telemetry thread, so give
            // it the chance it gets in flight.
            if (i % 20 == 0) {
                Thread.sleep(60);
            }
        }
        // Let the terrain lookups catch up. pendingTerrain() returns -1 for "nothing left",
        // not 0, so the first version of this loop exited immediately and only worked because
        // of the sleep below.
        for (int waited = 0; waited < 100 && data.pendingTerrain() >= 0; waited++) {
            Thread.sleep(100);
        }
        Thread.sleep(1500);  // the basemap render is on its own schedule
        Log.i(TAG, "terrain samples still pending: " + data.pendingTerrain());

        final Bitmap symBmp = render(symbology, SYM_W, SYM_H, "symbology");
        final Bitmap dashBmp = render(dashboard, DASH_W, DASH_H, "dashboard");
        final Bitmap mapBmp = render(minimap, MAP_W, MAP_H, "minimap");
        final Bitmap chartBmp = render(chart, CHART_W, CHART_H, "chart");

        // And the same four where the pilot actually sees them.
        cockpit(data, symBmp, dashBmp, mapBmp, chartBmp, false, "cockpit");
        // Once more with an instrument held, so the grab highlight is in a picture too.
        minimap.setGrabbed(true);
        final Bitmap grabbed = render(minimap, MAP_W, MAP_H, "minimap-grabbed");
        cockpit(data, symBmp, dashBmp, grabbed, chartBmp, true, "cockpit-grabbing-the-map");

        // The flight log gets the same treatment as the drawing: written by the real class
        // from the real data, and then read back. A log that turns out to be unparseable
        // after a flight is worth nothing, so the check is that it parses.
        checkFlightLog(context, data);

        // What the minimap's backdrop actually looks like at the ranges it draws. Rendered
        // straight from the file rather than through a simulated flight, because the question
        // "is this map sharp enough" is about the map and not about the telemetry.
        spanLadder(context);

        maps.stop();
        camera.stop();
        symbology.stop();
        dashboard.stop();
        minimap.stop();
        chart.stop();
        dummy.release();

        Log.i(TAG, "wrote previews to " + outDir);
        assertTrue("nothing was written", outDir.listFiles() != null
                && outDir.listFiles().length >= 6);
    }

    /**
     * One telemetry frame of a scripted flight: climb out, turn away, come back.
     *
     * <p>Units are the ones the native decoder emits, which are not SI and not guessable from
     * the field names - centimetres biased by 100000 for the three that are, millivolts,
     * centiamps, degrees times 1e7. Getting these wrong is how the HUD once read 19940 volts.
     */
    private MavlinkData sample(int step) {
        final float t = step / 2f;                    // seconds
        final float progress = step / (float) STEPS;

        // Climbing out and away, so the LAST frame - the one that gets rendered - is the
        // interesting one. The first version came back and landed, which meant every picture
        // showed nought metres and four metres a second.
        final float altitude = alt(progress);
        final float climb = step == 0 ? 0f : (altitude - alt((step - 1) / (float) STEPS)) * 2f;
        final float speed = 3f + 16f * (float) Math.min(1.0, progress * 2.2);
        // A curve rather than a straight line, so the track is worth looking at.
        final double bearing = Math.toRadians(35.0 + 120.0 * progress);
        final double range = 950.0 * (float) Math.pow(progress, 1.25);
        final double lat = homeLat + range * Math.cos(bearing) / 111320.0;
        final double lon = homeLon + range * Math.sin(bearing)
                / (111320.0 * Math.cos(Math.toRadians(homeLat)));

        final float volts = 16.8f - 2.6f * progress;   // 4S, draining
        final float amps = 8f + 14f * (float) Math.abs(Math.sin(Math.PI * progress * 2));

        return new MavlinkData(
                altitude * 100f + BIAS,
                6f * (float) Math.sin(t / 3.0),                     // pitch
                18f * (float) Math.sin(t / 4.0),                    // roll
                (float) Math.toDegrees(bearing) % 360f,             // yaw
                volts * 1000f,
                amps * 100f,
                1450f * progress,                                   // mAh consumed
                lat * 1e7, lon * 1e7,
                homeLat * 1e7, homeLon * 1e7,
                0.0,                                                // hdg, never assigned natively
                range * 100.0,
                14f,                                                // sats
                speed * 100f + BIAS,
                climb * 100f + BIAS,
                42f + 30f * (float) Math.sin(Math.PI * progress),   // throttle
                (byte) 1, (byte) 0, (byte) 3, (byte) 1, (byte) -62, (byte) 0,
                "PixelPilot HUD preview");
    }

    /**
     * The basemap at the spans the minimap uses, so its resolution can be judged directly.
     *
     * <p>The minimap draws max(240 m, 2.6x the distance from home) across 512 pixels. A file
     * built only to 25 m a pixel is an unreadable smear at the short end and perfectly legible
     * at the long one, which is impossible to argue about once it is two pictures.
     */
    private void spanLadder(Context context) throws Exception {
        final java.io.File file = MapFiles.file(context, MapFiles.Kind.BASEMAP);
        if (!file.isFile()) {
            Log.i(TAG, "no basemap installed, skipping the span ladder");
            return;
        }
        try (BasemapRenderer renderer = BasemapRenderer.open(file)) {
            if (renderer == null) {
                return;
            }
            Log.i(TAG, "basemap credit: " + renderer.attribution());
            for (float span : new float[]{240f, 600f, 1500f, 5000f}) {
                final Bitmap bmp = renderer.render(homeLat, homeLon, span, 512);
                if (bmp == null) {
                    Log.w(TAG, "no basemap render at " + span + " m");
                    continue;
                }
                write(bmp, String.format(Locale.US, "basemap-%04.0fm.png", span));
            }
        }
    }

    /**
     * Runs a log against the same simulated flight and checks what came out.
     *
     * <p>Not just that a file appeared: that the sample rows have the column count the header
     * promises and that the numbers parse as numbers. The device here is set to German, where
     * an unguarded {@code %.1f} writes a decimal comma and silently splits every value into
     * two columns - which is the kind of thing that is only discovered when the log is
     * finally needed.
     */
    private void checkFlightLog(Context context, FlightData data) throws Exception {
        final FlightLog log = FlightLog.open(context, data, null);
        assertNotNull("no flight log was opened", log);
        log.event("test", "checking the log writes what it says it does");
        // Long enough for several samples at 200 ms.
        Thread.sleep(1200);
        log.close();
        Thread.sleep(400);  // the close is posted to the log's own thread

        int header = 0;
        int samples = 0;
        int events = 0;
        int columns = -1;
        try (java.io.BufferedReader r = new java.io.BufferedReader(
                new java.io.InputStreamReader(new java.io.FileInputStream(log.file())))) {
            String line;
            while ((line = r.readLine()) != null) {
                if (line.startsWith("#")) {
                    header++;
                } else if (line.startsWith("S,t_ms")) {
                    columns = line.split(",", -1).length;
                } else if (line.startsWith("S,")) {
                    final String[] f = line.split(",", -1);
                    assertTrue("sample has " + f.length + " fields, header says " + columns,
                            f.length == columns);
                    // Every non-empty field after the tag must be a number.
                    for (int i = 1; i < f.length; i++) {
                        if (!f[i].isEmpty()) {
                            Double.parseDouble(f[i]);
                        }
                    }
                    samples++;
                } else if (line.startsWith("E,")) {
                    events++;
                }
            }
        }
        Log.i(TAG, "flight log: " + header + " header lines, " + columns + " columns, "
                + samples + " samples, " + events + " events, "
                + log.file().length() / 1024 + " kB");
        assertTrue("no samples were written", samples >= 3);
        assertTrue("the test event is missing", events >= 1);
    }

    private static double parseArg(android.os.Bundle args, String key, double fallback) {
        final String v = args == null ? null : args.getString(key);
        if (v == null) {
            return fallback;
        }
        try {
            return Double.parseDouble(v);
        } catch (NumberFormatException e) {
            Log.w(TAG, "ignoring -e " + key + " " + v);
            return fallback;
        }
    }

    /** Height above the arming point, easing off as it levels out. */
    private static float alt(float progress) {
        return (float) (135.0 * (1.0 - Math.exp(-3.2 * progress)));
    }

    /**
     * Pushes one camera-health block the way the air unit does, so the dashboard's camera row
     * has real numbers in it. Exercises the parser as a side effect.
     */
    private void pushCameraHealth() {
        new Thread(() -> {
            for (int attempt = 0; attempt < 20; attempt++) {
                try (Socket s = new Socket("127.0.0.1", 9099);
                     PrintWriter w = new PrintWriter(s.getOutputStream())) {
                    w.print("temp_c=61.5\ncpu_pct=31\nmem_used_mb=34\nmem_total_mb=90\n"
                            + "tx_kbit=29900\nuptime_s=1840\n");
                    w.flush();
                    return;
                } catch (Exception e) {
                    try {
                        Thread.sleep(250);  // the listener may not be up yet
                    } catch (InterruptedException ignored) {
                        return;
                    }
                }
            }
            Log.w(TAG, "camera health push never connected");
        }, "fake-camera").start();
    }

    /** Draws one instrument with its own shipping draw(), on the dark the compositor is not. */
    private Bitmap render(XrOverlay overlay, int w, int h, String name) throws Exception {
        final Bitmap bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
        final Canvas canvas = new Canvas(bmp);
        // renderOnce rather than draw: the grab outline is part of composing a frame, and
        // calling draw alone silently left it out of every picture.
        overlay.renderOnce(canvas);

        // Saved twice: the layer as it really is, transparent, and flattened onto a dark
        // ground so it can be looked at in a picture viewer without reading as empty.
        write(bmp, name + "-layer.png");
        final Bitmap onDark = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
        final Canvas c2 = new Canvas(onDark);
        c2.drawColor(Color.rgb(18, 20, 26));
        c2.drawBitmap(bmp, 0f, 0f, null);
        write(onDark, name + ".png");
        return bmp;
    }

    /**
     * The four layers where the layout puts them, projected through a pinhole camera.
     *
     * <p>Same arithmetic as the compositor: {@code facing = RotY(-yaw) * RotX(pitch)}, centre
     * at {@code facing * (0,0,-dist)}, the panel laid back by its own tilt. A four-point
     * perspective map per panel, which is what setPolyToPoly does.
     */
    private void cockpit(FlightData data, Bitmap sym, Bitmap dash, Bitmap map, Bitmap chart,
                         boolean grabbing, String name) throws Exception {
        // Sized to roughly a Quest 3's field of view, 104 by 96 degrees. The first version
        // was 1600x1000, which at that horizontal angle is only 73 degrees tall - and the
        // instruments sit thirty degrees down, so it cut all three of them in half.
        final int outW = 1600;
        final int outH = 1400;
        final float f = (float) (outW / 2 / Math.tan(Math.toRadians(104.0) / 2));
        final Bitmap out = Bitmap.createBitmap(outW, outH, Bitmap.Config.ARGB_8888);
        final Canvas c = new Canvas(out);

        final FlightData.Snapshot s = data.snapshot();
        videoStandIn(c, outW, outH);

        // The video quad, at its default placement, then the instruments on top.
        drawPanel(c, sym, 0f, 0f, 0f, 1.6f, 2.2f, sym.getHeight() / (float) sym.getWidth(), f,
                outW, outH);
        drawPanel(c, dash, 0f, -30f, 0f, 1.40f, 1.40f * 30f * (float) Math.PI / 180f, 0.75f, f,
                outW, outH);
        drawPanel(c, map, -25f, -30f, 26f, 1.25f, 0.42f, 1.0f, f, outW, outH);
        drawPanel(c, chart, 25f, -30f, 26f, 1.25f, 0.54f,
                chart.getHeight() / (float) chart.getWidth(), f, outW, outH);

        final Paint label = new Paint(Paint.ANTI_ALIAS_FLAG);
        label.setColor(Color.argb(150, 235, 240, 248));
        label.setTextSize(20f);
        c.drawText(String.format(Locale.US,
                        "layout preview - alt %.0f m, %.0f m out, %.1f V%s",
                        s.altitude, s.homeDistance, s.volts,
                        grabbing ? ", holding the minimap" : ""),
                24f, outH - 24f, label);
        write(out, name + ".png");
    }

    /**
     * Where the video goes, drawn as a plain graded ground rather than a horizon.
     *
     * <p>It was a sky and a horizon at first, banked to the telemetry attitude, and that was
     * a mistake: the symbology draws its own horizon on its own scale, the two did not line
     * up, and a picture where the instrument disagrees with the view behind it reads as a
     * bug in the instrument. Nothing here pretends to be a real frame now.
     */
    private void videoStandIn(Canvas c, int w, int h) {
        c.drawColor(Color.rgb(10, 11, 15));
        final Paint p = new Paint();
        p.setShader(new LinearGradient(0f, 0f, 0f, h,
                Color.rgb(46, 52, 64), Color.rgb(24, 27, 34), Shader.TileMode.CLAMP));
        c.drawRect(0f, 0f, w, h, p);
    }

    /** One panel, placed by the layout numbers and mapped through the projection. */
    private void drawPanel(Canvas c, Bitmap bmp, float yawDeg, float pitchDeg, float tiltDeg,
                           float dist, float widthM, float aspect, float f, int outW, int outH) {
        final float[] facing = qmul(aboutY((float) Math.toRadians(-yawDeg)),
                aboutX((float) Math.toRadians(pitchDeg)));
        final float[] centre = qrot(facing, new float[]{0f, 0f, -dist});
        final float[] orient = qmul(facing, aboutX((float) Math.toRadians(tiltDeg)));
        final float hw = widthM / 2f;
        final float hh = widthM * aspect / 2f;

        final float[][] corners = {{-hw, hh, 0f}, {hw, hh, 0f}, {hw, -hh, 0f}, {-hw, -hh, 0f}};
        final float[] dst = new float[8];
        for (int i = 0; i < 4; i++) {
            final float[] world = qrot(orient, corners[i]);
            final float x = centre[0] + world[0];
            final float y = centre[1] + world[1];
            final float z = centre[2] + world[2];
            if (z >= -0.05f) {
                return;  // behind the eye, nothing sensible to draw
            }
            dst[i * 2] = outW / 2f + f * x / -z;
            dst[i * 2 + 1] = outH / 2f - f * y / -z;
        }
        final float[] src = {0f, 0f, bmp.getWidth(), 0f, bmp.getWidth(), bmp.getHeight(),
                0f, bmp.getHeight()};
        final Matrix m = new Matrix();
        if (!m.setPolyToPoly(src, 0, dst, 0, 4)) {
            Log.w(TAG, "could not map a panel into the preview");
            return;
        }
        final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        p.setFilterBitmap(true);
        c.drawBitmap(bmp, m, p);
    }

    // --- just enough quaternion maths to mirror the compositor ------------------------

    private static float[] qmul(float[] a, float[] b) {
        return new float[]{
                a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
                a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
                a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
                a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
    }

    private static float[] aboutX(float a) {
        return new float[]{(float) Math.sin(a / 2), 0f, 0f, (float) Math.cos(a / 2)};
    }

    private static float[] aboutY(float a) {
        return new float[]{0f, (float) Math.sin(a / 2), 0f, (float) Math.cos(a / 2)};
    }

    private static float[] qrot(float[] q, float[] v) {
        final float tx = 2f * (q[1] * v[2] - q[2] * v[1]);
        final float ty = 2f * (q[2] * v[0] - q[0] * v[2]);
        final float tz = 2f * (q[0] * v[1] - q[1] * v[0]);
        return new float[]{
                v[0] + q[3] * tx + (q[1] * tz - q[2] * ty),
                v[1] + q[3] * ty + (q[2] * tx - q[0] * tz),
                v[2] + q[3] * tz + (q[0] * ty - q[1] * tx)};
    }

    private void write(Bitmap bmp, String name) throws Exception {
        final File file = new File(outDir, name);
        try (OutputStream out = new FileOutputStream(file)) {
            assertTrue("png encode failed for " + name,
                    bmp.compress(Bitmap.CompressFormat.PNG, 100, out));
        }
        assertNotNull(file);
        assertTrue(name + " is empty", file.length() > 0);
        Log.i(TAG, "preview: " + file.getName() + " " + file.length() / 1024 + " kB");
    }
}
