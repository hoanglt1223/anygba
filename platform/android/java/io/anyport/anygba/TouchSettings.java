package io.anyport.anygba;

import android.app.AlertDialog;
import android.util.Log;
import android.util.TypedValue;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Properties;

/**
 * The touch controls dialog (long press on MENU): size, opacity, position and haptics,
 * written to getFilesDir()/anygba-touch.ini, which the game re-reads while running; plus
 * save export and import. Keys and ranges match runtime/src/player/touch.h.
 */
final class TouchSettings {
    private TouchSettings() {}

    static File file(GameActivity activity) {
        return new File(activity.getFilesDir(), "anygba-touch.ini");
    }

    static void show(final GameActivity activity) {
        final Properties p = load(activity);
        LinearLayout column = new LinearLayout(activity);
        column.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(activity, 16);
        column.setPadding(pad, pad, pad, pad);

        slider(activity, column, p, "Size", "Size", 50, 200, 100);
        slider(activity, column, p, "Opacity", "Opacity", 10, 100, 45);
        slider(activity, column, p, "Move in from the sides", "OffsetX", -100, 100, 0);
        slider(activity, column, p, "Move up", "OffsetY", -100, 100, 0);

        final CheckBox haptics = new CheckBox(activity);
        haptics.setText("Vibrate on press");
        haptics.setChecked(!"0".equals(p.getProperty("Haptics", "1").trim()));
        haptics.setOnCheckedChangeListener((view, checked) -> {
            p.setProperty("Haptics", checked ? "1" : "0");
            save(activity, p);
        });
        column.addView(haptics);

        Button export = new Button(activity);
        export.setText("Export saves");
        export.setOnClickListener(v -> activity.exportSaves());
        column.addView(export);
        Button importSaves = new Button(activity);
        importSaves.setText("Import saves");
        importSaves.setOnClickListener(v -> activity.importSaves());
        column.addView(importSaves);

        ScrollView scroll = new ScrollView(activity);
        scroll.addView(column);
        new AlertDialog.Builder(activity)
                .setTitle("Touch controls")
                .setView(scroll)
                .setPositiveButton("Done", null)
                .show();
    }

    private static void slider(final GameActivity activity, LinearLayout column, final Properties p, final String title,
                               final String key, final int min, int max, int fallback) {
        final TextView label = new TextView(activity);
        int value = fallback;
        try {
            value = Math.max(min, Math.min(max, Integer.parseInt(p.getProperty(key, "" + fallback).trim())));
        } catch (NumberFormatException e) {
            Log.w(GameActivity.TAG, "bad " + key + " in anygba-touch.ini");
        }
        label.setText(title + ": " + value);
        label.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        column.addView(label);
        SeekBar bar = new SeekBar(activity);
        bar.setMax(max - min);
        bar.setProgress(value - min);
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                label.setText(title + ": " + (progress + min));
                p.setProperty(key, Integer.toString(progress + min));
            }

            @Override
            public void onStartTrackingTouch(SeekBar seekBar) {}

            @Override
            public void onStopTrackingTouch(SeekBar seekBar) {
                save(activity, p);
            }
        });
        column.addView(bar, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
    }

    private static Properties load(GameActivity activity) {
        Properties p = new Properties();
        File f = file(activity);
        if (f.isFile()) {
            try (InputStream in = new FileInputStream(f)) {
                p.load(in);
            } catch (IOException e) {
                Log.w(GameActivity.TAG, "cannot read " + f, e);
            }
        }
        return p;
    }

    private static void save(GameActivity activity, Properties p) {
        StringBuilder text = new StringBuilder("# AnyGBA touch controls\n");
        for (String key : new String[] {"Size", "Opacity", "OffsetX", "OffsetY", "Haptics"}) {
            String value = p.getProperty(key);
            if (value != null) text.append(key).append(" = ").append(value.trim()).append('\n');
        }
        File f = file(activity);
        File tmp = new File(f.getPath() + ".tmp");
        try (OutputStream out = new FileOutputStream(tmp)) {
            out.write(text.toString().getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) {
            Log.e(GameActivity.TAG, "cannot write " + tmp, e);
            return;
        }
        if (!tmp.renameTo(f)) Log.e(GameActivity.TAG, "cannot replace " + f);
    }

    private static int dp(GameActivity activity, int value) {
        return Math.round(value * activity.getResources().getDisplayMetrics().density);
    }
}
