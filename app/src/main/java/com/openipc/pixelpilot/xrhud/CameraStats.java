package com.openipc.pixelpilot.xrhud;

import android.util.Log;

import androidx.annotation.Nullable;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.net.HttpURLConnection;
import java.net.URL;

/**
 * Camera health, fetched from the air unit over the wfb-ng tunnel.
 *
 * <p>None of this arrives as telemetry. Betaflight's MAVLink carries no temperatures at all -
 * verified against its source, where {@code BATTERY_STATUS.temperature} is hardcoded to
 * unknown - so the camera has to be asked directly. It is also the part of an OpenIPC setup
 * that thermally throttles and eventually dies in summer, which makes its temperature the one
 * of the three worth the effort.
 *
 * <p>The transport is the tunnel that already exists: {@code wfb_tun} gives the air unit
 * 10.5.0.10 and the app's own VpnService takes 10.5.0.3, so a plain HTTP GET reaches the
 * camera over the air link - measured at about 18ms round trip.
 *
 * <p>A file rather than a CGI on the other end, because majestic is the web server there and
 * it answers 500 for every CGI, haserl included; static files it serves happily. See
 * {@code /usr/bin/pixelpilot-status} on the camera.
 */
public final class CameraStats {

    private static final String TAG = "pixelpilot";
    private static final String URL_TEXT = "http://10.5.0.10/pixelpilot.txt";

    /** The camera publishes every two seconds, so asking faster only costs air time. */
    private static final long POLL_MS = 3000;

    /** Short: this shares the air link with the video, and a stall must not queue up. */
    private static final int TIMEOUT_MS = 1500;

    /** Older than this and the reading is not shown as if it were current. */
    public static final long STALE_MS = 12000;

    public static final class Snapshot {
        public boolean fresh;
        public float tempC = Float.NaN;
        public int cpuPct = -1;
        public int memUsedMb = -1;
        public int memTotalMb = -1;
        public int txKbit = -1;
        public long uptimeS = -1;
    }

    private volatile Snapshot latest = new Snapshot();
    private volatile long latestAt;
    private volatile boolean running;
    @Nullable
    private Thread thread;

    public void start() {
        if (running) {
            return;
        }
        running = true;
        thread = new Thread(this::loop, "CameraStats");
        thread.setDaemon(true);
        thread.start();
    }

    public void stop() {
        running = false;
        final Thread t = thread;
        if (t != null) {
            t.interrupt();
        }
        thread = null;
    }

    public Snapshot snapshot() {
        final Snapshot s = latest;
        s.fresh = (System.currentTimeMillis() - latestAt) < STALE_MS;
        return s;
    }

    private void loop() {
        while (running) {
            final Snapshot s = fetch();
            if (s != null) {
                latest = s;
                latestAt = System.currentTimeMillis();
            }
            try {
                Thread.sleep(POLL_MS);
            } catch (InterruptedException e) {
                return;
            }
        }
    }

    @Nullable
    private Snapshot fetch() {
        HttpURLConnection conn = null;
        try {
            conn = (HttpURLConnection) new URL(URL_TEXT).openConnection();
            conn.setConnectTimeout(TIMEOUT_MS);
            conn.setReadTimeout(TIMEOUT_MS);
            conn.setUseCaches(false);
            if (conn.getResponseCode() != 200) {
                return null;
            }
            final Snapshot s = new Snapshot();
            try (BufferedReader r = new BufferedReader(new InputStreamReader(conn.getInputStream()))) {
                String line;
                while ((line = r.readLine()) != null) {
                    final int eq = line.indexOf('=');
                    if (eq <= 0) {
                        continue;
                    }
                    final String k = line.substring(0, eq);
                    final String v = line.substring(eq + 1).trim();
                    switch (k) {
                        case "temp_c":
                            s.tempC = parseFloat(v);
                            break;
                        case "cpu_pct":
                            s.cpuPct = parseInt(v);
                            break;
                        case "mem_used_mb":
                            s.memUsedMb = parseInt(v);
                            break;
                        case "mem_total_mb":
                            s.memTotalMb = parseInt(v);
                            break;
                        case "tx_kbit":
                            s.txKbit = parseInt(v);
                            break;
                        case "uptime_s":
                            s.uptimeS = parseInt(v);
                            break;
                        default:
                            // An unknown key is a newer camera script, not an error.
                            break;
                    }
                }
            }
            return s;
        } catch (Exception e) {
            // Expected whenever the link is down or the camera has no publisher installed;
            // logged once in a while rather than every three seconds.
            if (latestAt != 0) {
                Log.d(TAG, "camera stats unreachable: " + e.getMessage());
                latestAt = 0;
            }
            return null;
        } finally {
            if (conn != null) {
                conn.disconnect();
            }
        }
    }

    private static float parseFloat(String s) {
        try {
            return Float.parseFloat(s);
        } catch (NumberFormatException e) {
            return Float.NaN;
        }
    }

    private static int parseInt(String s) {
        try {
            return (int) Float.parseFloat(s);
        } catch (NumberFormatException e) {
            return -1;
        }
    }
}
