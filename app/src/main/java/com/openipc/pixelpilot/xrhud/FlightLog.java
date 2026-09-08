package com.openipc.pixelpilot.xrhud;

import android.content.Context;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;

import androidx.annotation.Nullable;

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStreamWriter;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * Writes what happened during a session to a file, so a flight produces evidence.
 *
 * <p>There is a DVR for the video and nothing for the numbers, which means every question about
 * a flight has to be answered from memory - what the link was doing when the picture broke up,
 * whether the height above ground was sane, whether a panel stayed where it was put. This turns
 * that into a file that can be read afterwards.
 *
 * <p>Two kinds of line, told apart by the first field. {@code S} is a sample of everything at
 * once, five times a second, which makes a uniform series that is trivial to plot. {@code E} is
 * a discrete event written the moment it happens - a button, a grab, the recorder starting, an
 * exception - because those are what a series between samples would miss.
 *
 * <p>The header names the columns and their units. That is not decoration: nothing about the
 * units in this project is guessable from the field names, and a log nobody can interpret in
 * six months is not evidence.
 *
 * <p>Written from its own thread and flushed once a second, so a crash costs at most a second
 * of samples and the last event line is always on disk - see {@link #installCrashHook()}.
 */
public final class FlightLog implements AutoCloseable {

    private static final String TAG = "pixelpilot";

    /** Five hertz. Fast enough to see a link dropout, slow enough to be nothing per hour. */
    private static final long SAMPLE_MS = 200;

    /** Bounded so a session left running overnight cannot fill the device. */
    private static final long MAX_BYTES = 64L * 1024 * 1024;

    private final FlightData data;
    @Nullable
    private final CameraStats camera;
    private final File file;
    private final BufferedWriter out;
    private final HandlerThread thread;
    private final Handler handler;
    private final long startedAt = System.currentTimeMillis();

    private long written;
    private volatile boolean running = true;
    private boolean full;
    @Nullable
    private Thread.UncaughtExceptionHandler previousHandler;

    private FlightLog(FlightData data, @Nullable CameraStats camera, File file,
                      BufferedWriter out) {
        this.data = data;
        this.camera = camera;
        this.file = file;
        this.out = out;
        thread = new HandlerThread("FlightLog");
        thread.start();
        handler = new Handler(thread.getLooper());
    }

    /**
     * Starts a log in the app's external files directory, or returns null if it cannot.
     *
     * <p>Not beside the DVR recording: that only exists while the pilot is recording, while a
     * log wants to cover the whole session. The recording's name is written as an event
     * instead, which is what actually lets the two be lined up. External files rather than
     * private storage so it can be fetched without root or run-as:
     *
     * <pre>
     * adb pull /sdcard/Android/data/com.openipc.pixelpilot/files/logs/
     * </pre>
     */
    @Nullable
    public static FlightLog open(Context context, FlightData data,
                                 @Nullable CameraStats camera) {
        final File dir = new File(context.getExternalFilesDir(null), "logs");
        if (!dir.isDirectory() && !dir.mkdirs()) {
            Log.w(TAG, "could not create " + dir + ", no flight log this session");
            return null;
        }
        final String stamp =
                new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date());
        final File file = new File(dir, "pixelpilot-" + stamp + ".csv");
        try {
            final BufferedWriter out = new BufferedWriter(new OutputStreamWriter(
                    new FileOutputStream(file), StandardCharsets.UTF_8), 16384);
            final FlightLog log = new FlightLog(data, camera, file, out);
            log.writeHeader();
            log.handler.postDelayed(log::tick, SAMPLE_MS);
            log.installCrashHook();
            Log.i(TAG, "flight log: " + file);
            return log;
        } catch (IOException e) {
            Log.w(TAG, "could not open a flight log", e);
            return null;
        }
    }

    public File file() {
        return file;
    }

    /**
     * Records something that happened, now.
     *
     * <p>Safe from any thread. {@code kind} is one word so the file can be grepped by it;
     * {@code detail} is free text with commas stripped, because this is a CSV.
     */
    public void event(String kind, @Nullable String detail) {
        if (!running) {
            return;
        }
        final long at = System.currentTimeMillis() - startedAt;
        final String safe = detail == null ? "" : detail.replace(',', ';').replace('\n', ' ');
        handler.post(() -> {
            line("E," + at + "," + kind + "," + safe);
            flush();  // an event is usually the thing you are looking for after a crash
        });
    }

    /**
     * Writes an event and flushes when the process is about to die, then hands the throwable
     * on. Without this the one line that explains a crash is the one still in the buffer.
     */
    private void installCrashHook() {
        previousHandler = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((t, e) -> {
            try {
                // Straight to the file rather than through the handler thread, which is not
                // going to get scheduled again.
                line("E," + (System.currentTimeMillis() - startedAt) + ",crash,"
                        + t.getName() + ": " + e.getClass().getName() + " "
                        + String.valueOf(e.getMessage()).replace(',', ';'));
                flush();
            } catch (Throwable ignored) {
                // Nothing useful left to do; the default handler still has to run.
            }
            if (previousHandler != null) {
                previousHandler.uncaughtException(t, e);
            }
        });
    }

    private void writeHeader() {
        line("# pixelpilot flight log v1, " + new Date());
        line("# S = sample every " + SAMPLE_MS + " ms, E = event when it happened");
        line("# units are the ones the app works in, none of which are guessable:");
        line("#   alt/agl/dist metres, gspd/climb m/s, volts V, cell V, amps A, mah mAh,");
        line("#   thr percent, lat/lon degrees, roll/pitch/yaw degrees, rssi dBm,");
        line("#   cam_c celsius, cam_mem MB, cam_kbit kbit/s. Empty means not known.");
        line("# agl is altitude above the ground below the craft, from the height model,");
        line("#   and is a difference of two samples of it - a constant bias cancels out.");
        line("S,t_ms,armed,fresh,lat,lon,alt,agl,gspd,climb,roll,pitch,yaw,volts,cell,cells,"
                + "amps,mah,thr,sats,fix,dist,rssi,fec,lost,cam_c,cam_cpu,cam_mem_used,"
                + "cam_mem_total,cam_kbit");
    }

    private void tick() {
        if (!running) {
            return;
        }
        try {
            sample();
        } catch (Throwable t) {
            // A log is never worth taking the session down for.
            Log.w(TAG, "flight log sample failed", t);
        }
        if (running && !full) {
            handler.postDelayed(this::tick, SAMPLE_MS);
        }
    }

    private void sample() {
        final FlightData.Snapshot s = data.snapshot();
        final StringBuilder b = new StringBuilder(220);
        b.append("S,").append(System.currentTimeMillis() - startedAt).append(',');
        b.append(s.armed ? 1 : 0).append(',').append(s.fresh ? 1 : 0).append(',');
        // Six decimals is about a tenth of a metre, which is more than the fix is worth.
        num(b, s.lat, 6);
        num(b, s.lon, 6);
        num(b, s.altitude, 1);
        num(b, data.aglMetres(s), 1);
        num(b, s.groundSpeed, 1);
        num(b, s.climb, 1);
        num(b, s.roll, 1);
        num(b, s.pitch, 1);
        num(b, s.yaw, 1);
        num(b, s.volts, 2);
        num(b, s.perCell, 3);
        b.append(s.cells).append(',');
        num(b, s.amps, 1);
        num(b, s.consumedMah, 0);
        num(b, s.throttlePct, 0);
        b.append(s.sats).append(',').append(s.gpsFix ? 1 : 0).append(',');
        num(b, s.homeDistance, 1);
        b.append(s.linkQuality).append(',').append(s.fecRecovered).append(',')
                .append(s.lost).append(',');

        final CameraStats.Snapshot c = camera != null ? camera.snapshot() : null;
        if (c != null && c.fresh) {
            num(b, c.tempC, 1);
            b.append(c.cpuPct).append(',').append(c.memUsedMb).append(',')
                    .append(c.memTotalMb).append(',').append(c.txKbit);
        } else {
            b.append(",,,");  // four empty camera fields
        }
        line(b.toString());

        // Flushed on a whole second rather than every sample: five writes a second through a
        // buffer costs nothing, five flushes does.
        if ((System.currentTimeMillis() - startedAt) % 1000 < SAMPLE_MS) {
            flush();
        }
    }

    /** Appends a number and a comma, or just a comma when it is not a number. */
    private static void num(StringBuilder b, double v, int decimals) {
        if (Double.isNaN(v) || Double.isInfinite(v)) {
            b.append(',');
            return;
        }
        // Locale.US deliberately: this device is set to German, where %.2f writes a comma and
        // would put a column break in the middle of every value.
        b.append(String.format(Locale.US, "%." + decimals + "f", v)).append(',');
    }

    private void line(String s) {
        if (full) {
            return;
        }
        try {
            out.write(s);
            out.write('\n');
            written += s.length() + 1;
            if (written > MAX_BYTES) {
                full = true;
                out.write("# stopped, log reached " + MAX_BYTES + " bytes\n");
                flush();
                Log.w(TAG, "flight log full at " + written + " bytes");
            }
        } catch (IOException e) {
            Log.w(TAG, "flight log write failed, giving up on it", e);
            full = true;
        }
    }

    private void flush() {
        try {
            out.flush();
        } catch (IOException ignored) {
            // A failed flush is not worth a message every second.
        }
    }

    @Override
    public void close() {
        if (!running) {
            return;
        }
        running = false;
        // Restore whatever was handling crashes before, or the next session inherits a hook
        // holding a closed writer.
        if (previousHandler != null) {
            Thread.setDefaultUncaughtExceptionHandler(previousHandler);
            previousHandler = null;
        }
        handler.post(() -> {
            line("# closed after " + (System.currentTimeMillis() - startedAt) / 1000 + " s");
            flush();
            try {
                out.close();
            } catch (IOException ignored) {
            }
            thread.quitSafely();
        });
        Log.i(TAG, "flight log closed: " + file.getName() + ", " + written / 1024 + " kB");
    }
}
