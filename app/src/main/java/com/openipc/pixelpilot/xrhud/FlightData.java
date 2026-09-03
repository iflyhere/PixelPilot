package com.openipc.pixelpilot.xrhud;

import androidx.annotation.Nullable;

import com.openipc.mavlink.MavlinkData;
import com.openipc.wfbngrtl8812.WfbNGStats;

/**
 * What the instruments read, and the only place the telemetry units are undone.
 *
 * <p>{@link MavlinkData} carries whatever unit the message happened to arrive in, and the flat
 * OSD scales at display time - so every consumer has to, and getting it wrong is silent. Read
 * raw, the battery shows 19940.00V and the altimeter 99875m. The conversions therefore live
 * here once, named, and the drawing code never touches a raw field.
 *
 * <table>
 *   <tr><td>{@code telemetryBattery}</td><td>millivolts</td></tr>
 *   <tr><td>{@code telemetryCurrent}</td><td>centiamps</td></tr>
 *   <tr><td>{@code telemetryCurrentConsumed}</td><td>mAh, already</td></tr>
 *   <tr><td>{@code telemetryAltitude}</td><td>centimetres, biased by +100000</td></tr>
 *   <tr><td>{@code telemetryGSpeed}, {@code telemetryVSpeed}</td><td>cm/s, biased by +100000</td></tr>
 *   <tr><td>{@code telemetryDistance}</td><td>centimetres</td></tr>
 *   <tr><td>{@code telemetryLat}, {@code telemetryLon}</td><td>degrees x 1e7</td></tr>
 *   <tr><td>pitch / roll / yaw</td><td>degrees, already</td></tr>
 * </table>
 *
 * <p>The +100000 bias is how the flat OSD tells "zero" apart from "never received".
 *
 * <p>Written from the main thread, read from each overlay's own draw thread, so the fields are
 * volatile and a reader takes one consistent snapshot rather than several fields separately.
 */
public final class FlightData {

    /** Telemetry older than this is not shown as if it were current. */
    public static final long STALE_MS = 2000;

    private static final float BIAS = 100000f;

    /** How much history the chart can draw. At 10Hz that is a little over three minutes. */
    private static final int HISTORY = 2048;

    /** One consistent set of values, so an instrument never mixes two packets. */
    public static final class Snapshot {
        public boolean fresh;
        public float roll, pitch, yaw;
        public float altitude;      // metres above the arming point
        public float groundSpeed;   // m/s
        public float climb;         // m/s
        public float volts;
        public float perCell;
        public int cells;
        public float amps;
        public float consumedMah;
        public float throttlePct;
        public int sats;
        public boolean gpsFix;
        public double lat, lon;      // degrees
        public double homeLat, homeLon;
        public boolean homeKnown;
        public float homeDistance;  // metres
        public boolean armed;
        public int linkQuality = -1;  // 0..100, -1 when unknown
        public int fecRecovered;
        public int lost;
    }

    private volatile MavlinkData latest;
    private volatile long latestAt;
    @Nullable
    private volatile WfbNGStats link;

    // Ring buffer for the chart. Written only from the telemetry thread.
    private final long[] histTime = new long[HISTORY];
    private final float[] histAlt = new float[HISTORY];
    private final float[] histCell = new float[HISTORY];
    private final float[] histTerrain = new float[HISTORY];
    // Where each sample was taken, so the ground under it can be looked up afterwards. Doing
    // it inline would put a tile decode on the telemetry thread.
    private final double[] histLat = new double[HISTORY];
    private final double[] histLon = new double[HISTORY];
    private final boolean[] histHasPos = new boolean[HISTORY];
    private volatile int histCount;
    private int histHead;
    private long lastSampleAt;

    public void onTelemetry(MavlinkData data) {
        latest = data;
        latestAt = System.currentTimeMillis();
        sample(data);
    }

    public void onLink(WfbNGStats stats) {
        link = stats;
    }

    public Snapshot snapshot() {
        final Snapshot s = new Snapshot();
        final WfbNGStats l = link;
        if (l != null) {
            s.linkQuality = l.avg_rssi;
            s.fecRecovered = l.count_p_fec_recovered;
            s.lost = l.count_p_lost;
        }
        final MavlinkData t = latest;
        if (t == null) {
            return s;
        }
        s.fresh = (System.currentTimeMillis() - latestAt) < STALE_MS;
        s.roll = t.telemetryRoll;
        s.pitch = t.telemetryPitch;
        s.yaw = t.telemetryYaw;
        s.altitude = (t.telemetryAltitude - BIAS) / 100f;
        s.groundSpeed = (t.telemetryGSpeed - BIAS) / 100f;
        s.climb = (t.telemetryVSpeed - BIAS) / 100f;
        s.volts = t.telemetryBattery / 1000f;
        s.cells = cellCount(s.volts);
        s.perCell = s.volts / s.cells;
        s.amps = t.telemetryCurrent / 100f;
        s.consumedMah = t.telemetryCurrentConsumed;
        s.throttlePct = Math.max(0f, Math.min(100f, t.telemetryThrottle));
        s.sats = (int) t.telemetrySat;
        s.gpsFix = t.gps_fix_type >= 3;
        s.lat = t.telemetryLat / 1e7;
        s.lon = t.telemetryLon / 1e7;
        s.homeLat = t.telemetryLatBase / 1e7;
        s.homeLon = t.telemetryLonBase / 1e7;
        s.homeKnown = t.telemetryLatBase != 0 || t.telemetryLonBase != 0;
        s.homeDistance = (float) (t.telemetryDistance / 100.0);
        s.armed = t.telemetryArm != 0;
        return s;
    }

    /**
     * Bearing from the craft back to where it armed, in degrees.
     *
     * <p>Computed rather than read from {@code telemetryHdg}: the native decoder passes that
     * field through to Java and never assigns it, so it is always zero and an arrow built on
     * it would point one fixed way for a whole flight.
     */
    public static double bearingHome(Snapshot s) {
        final double lat = Math.toRadians(s.lat);
        final double lon = Math.toRadians(s.lon);
        final double latH = Math.toRadians(s.homeLat);
        final double lonH = Math.toRadians(s.homeLon);
        final double dLon = lonH - lon;
        final double y = Math.sin(dLon) * Math.cos(latH);
        final double x =
                Math.cos(lat) * Math.sin(latH) - Math.sin(lat) * Math.cos(latH) * Math.cos(dLon);
        return Math.toDegrees(Math.atan2(y, x));
    }

    /**
     * Smallest common pack whose full voltage covers the reading.
     *
     * <p>Per-cell is the figure worth showing because the pack ranges do not overlap - 19.9V is
     * a three-quarters 6S and an impossible 4S. 4.25V per cell is the ceiling, a little above a
     * full 4.2V cell so a freshly charged pack is not rounded up to the next size.
     */
    public static int cellCount(float volts) {
        for (int n : new int[]{1, 2, 3, 4, 6, 8, 12}) {
            if (volts <= n * 4.25f) {
                return n;
            }
        }
        return 12;
    }

    // ------------------------------------------------------------------ history

    private void sample(MavlinkData t) {
        final long now = System.currentTimeMillis();
        if (now - lastSampleAt < 100) {
            return;
        }
        lastSampleAt = now;
        final float volts = t.telemetryBattery / 1000f;
        histTime[histHead] = now;
        histAlt[histHead] = (t.telemetryAltitude - BIAS) / 100f;
        histCell[histHead] = volts / cellCount(volts);
        histTerrain[histHead] = Float.NaN;  // filled in by the terrain lookup, when there is one
        histLat[histHead] = t.telemetryLat / 1e7;
        histLon[histHead] = t.telemetryLon / 1e7;
        histHasPos[histHead] = t.gps_fix_type >= 3;
        histHead = (histHead + 1) % HISTORY;
        if (histCount < HISTORY) {
            histCount++;
        }
    }

    /** Ground elevation under a past sample, in metres, or NaN where it is not known. */
    public void setTerrainAt(int index, float metres) {
        if (index >= 0 && index < HISTORY) {
            histTerrain[index] = metres;
        }
    }

    /**
     * A sample that has a position but no ground elevation yet, or -1 when there is none.
     *
     * <p>Newest first: if the lookup cannot keep up, the part of the chart the pilot is looking
     * at is the part that gets filled.
     */
    public int pendingTerrain() {
        final int n = histCount;
        for (int i = 0; i < n; i++) {
            final int j = (histHead - 1 - i + HISTORY) % HISTORY;
            if (histHasPos[j] && Float.isNaN(histTerrain[j])) {
                return j;
            }
        }
        return -1;
    }

    public double latAt(int index) {
        return (index >= 0 && index < HISTORY) ? histLat[index] : 0;
    }

    public double lonAt(int index) {
        return (index >= 0 && index < HISTORY) ? histLon[index] : 0;
    }

    /** Marks a sample as looked up and not covered, so it is not retried forever. */
    public void setTerrainMissing(int index) {
        if (index >= 0 && index < HISTORY) {
            histTerrain[index] = Float.NEGATIVE_INFINITY;
        }
    }

    /** Copies the history oldest-first into the given arrays and returns how many were written. */
    public int history(long[] outTime, float[] outAlt, float[] outCell, float[] outTerrain) {
        final int n = Math.min(histCount, Math.min(outTime.length,
                Math.min(outAlt.length, Math.min(outCell.length, outTerrain.length))));
        final int start = (histHead - n + HISTORY) % HISTORY;
        for (int i = 0; i < n; i++) {
            final int j = (start + i) % HISTORY;
            outTime[i] = histTime[j];
            outAlt[i] = histAlt[j];
            outCell[i] = histCell[j];
            outTerrain[i] = histTerrain[j];
        }
        return n;
    }

    public int historyCapacity() {
        return HISTORY;
    }
}
