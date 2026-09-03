package com.openipc.pixelpilot.xrhud;

import android.util.Log;

import androidx.annotation.Nullable;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.SocketTimeoutException;

/**
 * Camera health, pushed by the air unit over the wfb-ng tunnel.
 *
 * <p>None of this arrives as telemetry. Betaflight's MAVLink carries no temperatures at all -
 * verified against its source, where {@code BATTERY_STATUS.temperature} is hardcoded to
 * unknown - so the camera has to report its own. It is also the part of an OpenIPC setup that
 * thermally throttles and eventually dies on a hot day, which makes its temperature the one of
 * the three worth the effort.
 *
 * <p><b>Why this listens rather than fetching.</b> The obvious design is an HTTP GET to the
 * camera at 10.5.0.10 over the tunnel, and it cannot work: <b>Android excludes a
 * {@code VpnService} owner from its own tunnel</b>, so this app cannot route to 10.5.0.10 even
 * though a shell on the same headset reaches it in 13ms. Measured both ways - the shell gets
 * HTTP 200, the app times out on connect, at 1500ms and at 4000ms alike. Pushing inverts the
 * direction, and an inbound packet delivered to a bound socket is not route filtered, so this
 * works where fetching cannot.
 *
 * <p>The camera end is {@code /usr/bin/pixelpilot-status}, started from {@code /etc/rc.local},
 * which sends a short key=value block every two seconds with a two second connect timeout so
 * it never blocks while nothing is listening.
 */
public final class CameraStats {

    private static final String TAG = "pixelpilot";

    /** Where the camera pushes to. Must match the nc line in pixelpilot-status. */
    private static final int PORT = 9099;

    /** Older than this and the reading is not shown as if it were current. */
    public static final long STALE_MS = 12000;

    /** Bounded so a stopped listener shuts down promptly instead of blocking in accept(). */
    private static final int ACCEPT_TIMEOUT_MS = 1000;

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
    private volatile ServerSocket server;
    @Nullable
    private Thread thread;

    public void start() {
        if (running) {
            return;
        }
        running = true;
        thread = new Thread(this::listen, "CameraStats");
        thread.setDaemon(true);
        thread.start();
    }

    public void stop() {
        running = false;
        final ServerSocket s = server;
        if (s != null) {
            try {
                s.close();  // unblocks a waiting accept() at once
            } catch (Exception ignored) {
            }
        }
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

    private void listen() {
        try (ServerSocket ss = new ServerSocket(PORT)) {
            ss.setSoTimeout(ACCEPT_TIMEOUT_MS);
            server = ss;
            Log.i(TAG, "camera stats listening on " + PORT);
            while (running) {
                try (Socket client = ss.accept()) {
                    client.setSoTimeout(3000);
                    final Snapshot s = read(client);
                    if (s != null) {
                        latest = s;
                        latestAt = System.currentTimeMillis();
                    }
                } catch (SocketTimeoutException e) {
                    // Nothing pushed in the last second, which is normal with the link down.
                } catch (Exception e) {
                    if (running) {
                        Log.i(TAG, "camera stats read failed: " + e);
                    }
                }
            }
        } catch (Exception e) {
            // A port already in use is worth saying out loud: it would mean no camera
            // readings at all, silently, for the whole session.
            Log.e(TAG, "camera stats listener could not start on " + PORT, e);
        } finally {
            server = null;
        }
    }

    @Nullable
    private Snapshot read(Socket client) throws Exception {
        final Snapshot s = new Snapshot();
        boolean any = false;
        try (BufferedReader r = new BufferedReader(new InputStreamReader(client.getInputStream()))) {
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
                        continue;
                }
                any = true;
            }
        }
        return any ? s : null;
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
