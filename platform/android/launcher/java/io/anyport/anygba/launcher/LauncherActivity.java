package io.anyport.anygba.launcher;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.text.format.DateUtils;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.Menu;
import android.view.MenuItem;
import android.view.View;
import android.view.ViewGroup;
import android.widget.BaseAdapter;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.TextView;
import android.widget.Toast;

import io.anyport.anygba.GameActivity;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Properties;
import java.util.regex.Pattern;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Lists imported game packages (.agp from `anygba build --target android-package`) with
 * cover, title and last played, and starts them in GameActivity. Packages are unpacked to
 * getFilesDir()/games/(sha256 prefix)/. Packaging decision: docs/dev/ANDROID.md.
 */
public class LauncherActivity extends Activity {
    private static final String TAG = "anygba";
    private static final int REQUEST_IMPORT = 0x47;
    private static final int[] SPEEDS = {100, 200, 300, 400};
    // Everything a package may contain; anything else (or any "..") rejects the package.
    private static final Pattern ALLOWED = Pattern.compile(
            "package\\.properties|game\\.gba|cover\\.png|cheats/[A-Za-z0-9_.-]+\\.toml|lib/(arm64-v8a|armeabi-v7a|x86_64|x86)/libmain\\.so");
    private static final long MAX_ENTRY = 256L << 20;

    static final class Game {
        File dir;
        String name;
        String sha256;
        String stem;
        Bitmap cover;
    }

    private final List<Game> games = new ArrayList<>();
    private SharedPreferences prefs;
    private Adapter adapter;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        prefs = getSharedPreferences("games", MODE_PRIVATE);
        ListView list = new ListView(this);
        adapter = new Adapter();
        list.setAdapter(adapter);
        list.setOnItemClickListener((parent, view, position, id) -> play(games.get(position)));
        list.setOnItemLongClickListener((parent, view, position, id) -> {
            settings(games.get(position));
            return true;
        });
        TextView empty = new TextView(this);
        empty.setText("No games yet.\n\nBuild a package on your PC with\nanygba build game.gba --target android-package\nthen import the .agp file from the menu.");
        empty.setGravity(Gravity.CENTER);
        empty.setPadding(dp(24), dp(24), dp(24), dp(24));
        android.widget.FrameLayout frame = new android.widget.FrameLayout(this);
        frame.addView(list);
        frame.addView(empty);
        list.setEmptyView(empty);
        setContentView(frame);
        handleView(getIntent());
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        handleView(intent);
    }

    @Override
    protected void onResume() {
        super.onResume();
        reload();
    }

    private void handleView(Intent intent) {
        if (intent != null && Intent.ACTION_VIEW.equals(intent.getAction()) && intent.getData() != null) {
            importPackage(intent.getData());
        }
    }

    @Override
    public boolean onCreateOptionsMenu(Menu menu) {
        menu.add(0, 1, 0, "Import game package").setShowAsAction(MenuItem.SHOW_AS_ACTION_ALWAYS);
        return true;
    }

    @Override
    public boolean onOptionsItemSelected(MenuItem item) {
        if (item.getItemId() != 1) return super.onOptionsItemSelected(item);
        Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        pick.addCategory(Intent.CATEGORY_OPENABLE);
        pick.setType("*/*");
        startActivityForResult(pick, REQUEST_IMPORT);
        return true;
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request == REQUEST_IMPORT && result == RESULT_OK && data != null && data.getData() != null) {
            importPackage(data.getData());
        }
    }

    private File gamesDir() {
        return new File(getFilesDir(), "games");
    }

    private void reload() {
        games.clear();
        File[] dirs = gamesDir().listFiles();
        if (dirs != null) {
            for (File dir : dirs) {
                Game g = load(dir);
                if (g != null) games.add(g);
            }
        }
        Collections.sort(games, (a, b) -> {
            long la = prefs.getLong("played." + a.sha256, 0), lb = prefs.getLong("played." + b.sha256, 0);
            return la != lb ? Long.compare(lb, la) : a.name.compareToIgnoreCase(b.name);
        });
        adapter.notifyDataSetChanged();
    }

    private static Game load(File dir) {
        File props = new File(dir, "package.properties");
        if (!props.isFile()) return null;
        Properties p = new Properties();
        try (InputStream in = new FileInputStream(props)) {
            p.load(in);
        } catch (IOException e) {
            Log.w(TAG, "cannot read " + props, e);
            return null;
        }
        Game g = new Game();
        g.dir = dir;
        g.name = p.getProperty("name", dir.getName());
        g.sha256 = p.getProperty("sha256", dir.getName());
        g.stem = p.getProperty("stem");
        g.cover = BitmapFactory.decodeFile(new File(dir, "cover.png").getPath());
        return g;
    }

    private void play(Game g) {
        if (GameActivity.gameLibraryFor(g.dir) == null) {
            Toast.makeText(this, g.name + " has no library for this device", Toast.LENGTH_LONG).show();
            return;
        }
        prefs.edit().putLong("played." + g.sha256, System.currentTimeMillis()).apply();
        Intent i = new Intent(this, GameActivity.class);
        i.putExtra(GameActivity.EXTRA_GAME_DIR, g.dir.getAbsolutePath());
        if (g.stem != null) i.putExtra(GameActivity.EXTRA_SAVE_STEM, g.stem);
        int speed = prefs.getInt("speed." + g.sha256, 100);
        if (speed != 100) i.putExtra(GameActivity.EXTRA_ARGS, new String[] {"--speed", Integer.toString(speed)});
        startActivity(i);
    }

    private void settings(final Game g) {
        final String[] labels = new String[SPEEDS.length];
        int current = 0;
        int speed = prefs.getInt("speed." + g.sha256, 100);
        for (int i = 0; i < SPEEDS.length; ++i) {
            labels[i] = "Start at " + SPEEDS[i] + "% speed";
            if (SPEEDS[i] == speed) current = i;
        }
        new AlertDialog.Builder(this)
                .setTitle(g.name)
                .setSingleChoiceItems(labels, current, (dialog, which) -> {
                    prefs.edit().putInt("speed." + g.sha256, SPEEDS[which]).apply();
                    dialog.dismiss();
                })
                .setNegativeButton("Remove game", (dialog, which) -> confirmRemove(g))
                .setPositiveButton("Close", null)
                .show();
    }

    private void confirmRemove(final Game g) {
        new AlertDialog.Builder(this)
                .setTitle("Remove " + g.name + "?")
                .setMessage("The game package is deleted. Saves stay in the app and come back if you import it again.")
                .setPositiveButton("Remove", (dialog, which) -> {
                    deleteTree(g.dir);
                    reload();
                })
                .setNegativeButton("Cancel", null)
                .show();
    }

    // ---- import ----

    private void importPackage(final Uri uri) {
        Toast.makeText(this, "Importing...", Toast.LENGTH_SHORT).show();
        new Thread(() -> {
            String error;
            try {
                error = unpack(uri);
            } catch (IOException | SecurityException e) {
                Log.e(TAG, "import " + uri, e);
                error = e.getMessage();
            }
            final String message = error == null ? "Game imported" : "Import failed: " + error;
            runOnUiThread(() -> {
                Toast.makeText(this, message, Toast.LENGTH_LONG).show();
                reload();
            });
        }, "anygba-import").start();
    }

    /** Unpacks into a temporary folder, validates, then swaps it in. Returns an error or null. */
    private String unpack(Uri uri) throws IOException {
        File tmp = new File(getFilesDir(), "import.tmp");
        deleteTree(tmp);
        if (!tmp.mkdirs()) return "cannot create " + tmp;
        try {
            try (ZipInputStream zip = new ZipInputStream(getContentResolver().openInputStream(uri))) {
                byte[] buf = new byte[1 << 16];
                for (ZipEntry e; (e = zip.getNextEntry()) != null; ) {
                    if (e.isDirectory()) continue;
                    String name = e.getName();
                    if (name.contains("..") || !ALLOWED.matcher(name).matches()) return "unexpected file " + name + " (not an AnyGBA game package?)";
                    File out = new File(tmp, name);
                    File parent = out.getParentFile();
                    if (parent != null && !parent.isDirectory() && !parent.mkdirs()) return "cannot create " + parent;
                    long total = 0;
                    try (OutputStream o = new FileOutputStream(out)) {
                        for (int n; (n = zip.read(buf)) > 0; ) {
                            total += n;
                            if (total > MAX_ENTRY) return name + " is too large";
                            o.write(buf, 0, n);
                        }
                    }
                }
            }
            Game g = load(tmp);
            if (g == null || g.sha256 == null || !g.sha256.matches("[0-9a-f]{64}")) return "missing or damaged package.properties";
            if (!new File(tmp, "game.gba").isFile()) return "the package has no game.gba";
            if (GameActivity.gameLibraryFor(tmp) == null) {
                return "no library for this device (" + String.join(", ", Build.SUPPORTED_ABIS) + ")";
            }
            File cheats = new File(tmp, "cheats");
            File[] dbs = cheats.listFiles();
            if (dbs != null) {
                for (File db : dbs) copyFile(db, new File(getFilesDir(), "cheats/" + db.getName()));
            }
            File dest = new File(gamesDir(), g.sha256.substring(0, 16));
            deleteTree(dest);
            File root = gamesDir();
            if (!root.isDirectory() && !root.mkdirs()) return "cannot create " + root;
            if (!tmp.renameTo(dest)) return "cannot move the game into place";
            // Android only maps code from read-only files (W^X).
            for (String abi : Build.SUPPORTED_ABIS) {
                File lib = new File(dest, "lib/" + abi + "/libmain.so");
                if (lib.isFile() && !lib.setReadOnly()) Log.w(TAG, "cannot make " + lib + " read-only");
            }
            return null;
        } finally {
            deleteTree(tmp);
        }
    }

    private static void copyFile(File from, File to) throws IOException {
        File parent = to.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("cannot create " + parent);
        try (InputStream in = new FileInputStream(from); OutputStream out = new FileOutputStream(to)) {
            byte[] buf = new byte[1 << 16];
            for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
        }
    }

    private static void deleteTree(File f) {
        File[] children = f.listFiles();
        if (children != null) for (File c : children) deleteTree(c);
        if (f.exists() && !f.delete()) Log.w(TAG, "cannot delete " + f);
    }

    private int dp(int v) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics()));
    }

    private final class Adapter extends BaseAdapter {
        @Override
        public int getCount() {
            return games.size();
        }

        @Override
        public Object getItem(int position) {
            return games.get(position);
        }

        @Override
        public long getItemId(int position) {
            return position;
        }

        @Override
        public View getView(int position, View convert, ViewGroup parent) {
            Game g = games.get(position);
            LinearLayout row = new LinearLayout(LauncherActivity.this);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setGravity(Gravity.CENTER_VERTICAL);
            row.setPadding(dp(12), dp(8), dp(12), dp(8));
            ImageView cover = new ImageView(LauncherActivity.this);
            if (g.cover != null) cover.setImageBitmap(g.cover);
            row.addView(cover, new LinearLayout.LayoutParams(dp(64), dp(64)));
            LinearLayout text = new LinearLayout(LauncherActivity.this);
            text.setOrientation(LinearLayout.VERTICAL);
            text.setPadding(dp(16), 0, 0, 0);
            TextView title = new TextView(LauncherActivity.this);
            title.setText(g.name);
            title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18);
            TextView played = new TextView(LauncherActivity.this);
            long when = prefs.getLong("played." + g.sha256, 0);
            played.setText(when == 0 ? "Never played" : "Last played " + DateUtils.getRelativeTimeSpanString(when));
            text.addView(title);
            text.addView(played);
            row.addView(text);
            return row;
        }
    }
}
