package com.openipc.pixelpilot;

import android.content.Context;
import android.content.SharedPreferences;
import android.util.Base64;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * The wfb-ng ground station key, shared by the flat and the immersive activity.
 *
 * <p>The key lives in preferences (so a user-picked one survives) and has to be
 * materialised as {@code files/gs.key} before {@code WfbNgLink} is constructed: the native
 * aggregators open that path in their constructor and throw if it is missing.
 */
public final class GsKey {
    private static final String TAG = "pixelpilot";
    private static final String PREF_KEY = "gs.key";
    private static final String FILE_NAME = "gs.key";

    private GsKey() {
    }

    public static byte[] get(Context context) {
        String pref = context.getSharedPreferences("general", Context.MODE_PRIVATE)
                .getString(PREF_KEY, "");
        return Base64.decode(pref, Base64.DEFAULT);
    }

    public static void set(Context context, InputStream inputStream) throws IOException {
        ByteArrayOutputStream result = new ByteArrayOutputStream();
        byte[] buffer = new byte[1024];
        int length;
        while ((length = inputStream.read(buffer)) != -1) {
            result.write(buffer, 0, length);
        }
        SharedPreferences prefs = context.getSharedPreferences("general", Context.MODE_PRIVATE);
        prefs.edit().putString(PREF_KEY, Base64.encodeToString(result.toByteArray(), Base64.DEFAULT)).apply();
    }

    /** Seeds preferences from the bundled asset the first time round. */
    public static void importDefaultIfMissing(Context context) {
        if (get(context).length > 0) {
            Log.d(TAG, "gs.key already saved in preferences.");
            return;
        }
        try (InputStream inputStream = context.getAssets().open(FILE_NAME)) {
            Log.d(TAG, "Importing default gs.key...");
            set(context, inputStream);
        } catch (IOException e) {
            Log.e(TAG, "Failed to import default gs.key", e);
        }
    }

    /** Writes the key from preferences to {@code files/gs.key}. */
    public static boolean writeToFilesDir(Context context) {
        byte[] keyBytes = get(context);
        if (keyBytes.length == 0) {
            Log.e(TAG, "No gs.key available to write");
            return false;
        }
        File file = new File(context.getApplicationContext().getFilesDir(), FILE_NAME);
        try (OutputStream out = new FileOutputStream(file)) {
            out.write(keyBytes, 0, keyBytes.length);
            Log.d(TAG, "Wrote gs.key to " + file.getAbsolutePath());
            return true;
        } catch (IOException e) {
            Log.e(TAG, "Failed to write gs.key", e);
            return false;
        }
    }

    /**
     * Everything that has to happen before {@code new WfbNgLink(...)}. Call from any
     * activity that brings up the link.
     */
    public static boolean ensure(Context context) {
        importDefaultIfMissing(context);
        return writeToFilesDir(context);
    }
}
