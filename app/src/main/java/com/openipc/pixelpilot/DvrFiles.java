package com.openipc.pixelpilot;

import android.content.Context;
import android.net.Uri;
import android.util.Log;

import androidx.documentfile.provider.DocumentFile;

import java.time.LocalDateTime;
import java.time.format.DateTimeFormatter;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * DVR file naming and creation, shared by the flat and the immersive activity.
 *
 * <p>Everything here works off the already granted {@code dvr_folder_} tree URI. Picking a
 * folder needs an Activity result and stays with the flat activity.
 */
public final class DvrFiles {
    private static final String TAG = "pixelpilot";
    private static final String PREF_DVR_FILENAME = "dvr_filename";
    private static final String DEFAULT_TEMPLATE = "pixelpilot_[yyyyMMdd-HHmmss]";
    private static final Pattern TEMPLATE_PATTERN = Pattern.compile("\\[([^\\]]*)\\]");

    private DvrFiles() {
    }

    public static String template(Context context) {
        return context.getSharedPreferences("general", Context.MODE_PRIVATE)
                .getString(PREF_DVR_FILENAME, DEFAULT_TEMPLATE);
    }

    public static void setTemplate(Context context, String template) {
        context.getSharedPreferences("general", Context.MODE_PRIVATE)
                .edit().putString(PREF_DVR_FILENAME, template).apply();
    }

    public static boolean fragmentedMp4(Context context) {
        return context.getSharedPreferences("general", Context.MODE_PRIVATE)
                .getBoolean("dvr_fmp4", true);
    }

    /** Expands the {@code [pattern]} placeholder in a file name template. */
    public static String fileName(String template, LocalDateTime time) {
        Matcher matcher = TEMPLATE_PATTERN.matcher(template);
        String fallbackTime = time.format(DateTimeFormatter.ofPattern("yyyyMMdd-HHmmss"));

        if (matcher.find()) {
            String prefix = template.substring(0, matcher.start());
            String pattern = matcher.group(1);
            String suffix = template.substring(matcher.end());

            try {
                String timePart = time.format(DateTimeFormatter.ofPattern(pattern));
                return prefix + timePart + suffix;
            } catch (IllegalArgumentException e) {
                return prefix + fallbackTime + suffix;
            }
        }
        return "pixelpilot_" + fallbackTime;
    }

    public static boolean hasRecordingFolder(Context context) {
        return !context.getSharedPreferences("general", Context.MODE_PRIVATE)
                .getString("dvr_folder_", "").isEmpty();
    }

    /**
     * Creates the next recording file in the configured folder.
     *
     * @return its URI, or null if no folder is configured or it is not writable
     */
    public static Uri createRecording(Context context) {
        String dvrFolder = context.getSharedPreferences("general", Context.MODE_PRIVATE)
                .getString("dvr_folder_", "");
        if (dvrFolder.isEmpty()) {
            Log.e(TAG, "dvrFolder is empty");
            return null;
        }
        DocumentFile pickedDir = DocumentFile.fromTreeUri(context, Uri.parse(dvrFolder));
        if (pickedDir == null || !pickedDir.canWrite()) {
            return null;
        }
        String name = fileName(template(context), LocalDateTime.now()) + ".mp4";
        DocumentFile newFile = pickedDir.createFile("video/mp4", name);
        if (newFile == null) {
            Log.e(TAG, "dvr newFile null");
            return null;
        }
        return newFile.getUri();
    }

    /**
     * A new file in Movies/PixelPilot, for when no folder was picked: recording then works
     * without one, and the file shows up in the headset's file manager and over USB. It stays
     * pending - invisible to other apps - until {@link #finishMovieRecording}, so a player is
     * never offered a file that is still being written.
     *
     * @return its URI, or null before Android 10 or if the media store refused
     */
    @androidx.annotation.Nullable
    public static Uri createMovieRecording(Context context) {
        if (android.os.Build.VERSION.SDK_INT < android.os.Build.VERSION_CODES.Q) {
            return null;
        }
        android.content.ContentValues values = new android.content.ContentValues();
        values.put(android.provider.MediaStore.Video.Media.DISPLAY_NAME, nextFileName(context));
        values.put(android.provider.MediaStore.Video.Media.MIME_TYPE, "video/mp4");
        values.put(android.provider.MediaStore.Video.Media.RELATIVE_PATH,
                android.os.Environment.DIRECTORY_MOVIES + "/PixelPilot");
        values.put(android.provider.MediaStore.Video.Media.IS_PENDING, 1);
        try {
            return context.getContentResolver().insert(
                    android.provider.MediaStore.Video.Media.getContentUri(
                            android.provider.MediaStore.VOLUME_EXTERNAL_PRIMARY), values);
        } catch (RuntimeException e) {
            Log.e(TAG, "could not create a recording in Movies", e);
            return null;
        }
    }

    /** Publishes a recording made with {@link #createMovieRecording}. */
    public static void finishMovieRecording(Context context, @androidx.annotation.Nullable Uri uri) {
        if (uri == null || android.os.Build.VERSION.SDK_INT < android.os.Build.VERSION_CODES.Q) {
            return;
        }
        android.content.ContentValues values = new android.content.ContentValues();
        values.put(android.provider.MediaStore.Video.Media.IS_PENDING, 0);
        try {
            context.getContentResolver().update(uri, values, null, null);
        } catch (RuntimeException e) {
            Log.e(TAG, "could not publish the recording " + uri, e);
        }
    }

    /** File name the next {@link #createRecording} would use, for UI previews. */
    public static String nextFileName(Context context) {
        return fileName(template(context), LocalDateTime.now()) + ".mp4";
    }
}
