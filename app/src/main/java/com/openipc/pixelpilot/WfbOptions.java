package com.openipc.pixelpilot;

import android.content.Context;
import android.content.SharedPreferences;

import com.openipc.wfbngrtl8812.WfbNgLink;

/**
 * Pushes the persisted wfb-ng options into the native link.
 *
 * <p>These are not read by the native side on its own, so any activity that constructs a
 * {@link WfbNgLink} has to apply them explicitly or the user's adaptive link, TX power and
 * FEC settings are silently ignored.
 */
public final class WfbOptions {
    private WfbOptions() {
    }

    public static void applyDefaults(Context context, WfbNgLink wfbLink) {
        if (wfbLink == null) {
            return;
        }
        SharedPreferences prefs = context.getSharedPreferences("general", Context.MODE_PRIVATE);

        wfbLink.nativeSetAdaptiveLinkEnabled(prefs.getBoolean("adaptive_link_enabled", true));
        wfbLink.nativeSetTxPower(prefs.getInt("adaptive_tx_power", 20));
        wfbLink.nativeSetUseFec(prefs.getBoolean("custom_fec_enabled", true) ? 1 : 0);
        wfbLink.nativeSetUseLdpc(prefs.getBoolean("custom_ldpc_enabled", true) ? 1 : 0);
        wfbLink.nativeSetUseStbc(prefs.getBoolean("custom_stbc_enabled", true) ? 1 : 0);

        applyFecThresholds(context, wfbLink);
    }

    public static void applyFecThresholds(Context context, WfbNgLink wfbLink) {
        if (wfbLink == null) {
            return;
        }
        SharedPreferences prefs = context.getSharedPreferences("general", Context.MODE_PRIVATE);
        wfbLink.setFecThresholds(
                prefs.getInt("fec_lost_to_5", 2),
                prefs.getInt("fec_recovered_to_4", 30),
                prefs.getInt("fec_recovered_to_3", 24),
                prefs.getInt("fec_recovered_to_2", 14),
                prefs.getInt("fec_recovered_to_1", 8));
    }
}
