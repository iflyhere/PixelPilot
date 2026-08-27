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

    /** File name the next {@link #createRecording} would use, for UI previews. */
    public static String nextFileName(Context context) {
        return fileName(template(context), LocalDateTime.now()) + ".mp4";
    }
}
