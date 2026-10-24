package io.anyport.anygba;

import android.app.Activity;
import android.content.ClipData;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.provider.OpenableColumns;
import android.util.Log;
import android.widget.Toast;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * Runs one recompiled game. In a per-game APK the game is this APK's libmain.so and its ROM
 * is the asset game.gba; started by the launcher, EXTRA_GAME_DIR names an imported game
 * package holding lib/(abi)/libmain.so and game.gba. Saves live in getFilesDir()/saves.
 */
public class GameActivity extends SDLActivity {
    static final String TAG = "anygba";
    /** Folder of an unpacked game package (launcher only). */
    public static final String EXTRA_GAME_DIR = "io.anyport.anygba.GAME_DIR";
    /** File stem of this game's saves, "(title)-(sha8)". */
    public static final String EXTRA_SAVE_STEM = "io.anyport.anygba.SAVE_STEM";
    /** Extra player arguments, e.g. adb shell am start ... --esa args --headless,--frames,600 */
    public static final String EXTRA_ARGS = "args";

    /** SDL_SendAndroidMessage() command from the touch overlay: hold MENU, or Setup in the quick menu. */
    static final int MSG_TOUCH_SETTINGS = 0x8001;
    /** SDL_SendAndroidMessage(): a new screenshot is in getFilesDir()/screenshots. */
    static final int MSG_SCREENSHOT = 0x8002;
    private static final int REQUEST_IMPORT = 0x41;

    private File gameDir() {
        String dir = getIntent().getStringExtra(EXTRA_GAME_DIR);
        return dir == null ? null : new File(dir);
    }

    public static File gameLibraryFor(File dir) {
        for (String abi : Build.SUPPORTED_ABIS) {
            File lib = new File(dir, "lib/" + abi + "/libmain.so");
            if (lib.isFile()) return lib;
        }
        return null;
    }

    @Override
    protected String[] getLibraries() {
        return gameDir() == null ? new String[] {"main"} : new String[0];
    }

    @Override
    public void loadLibraries() {
        File dir = gameDir();
        if (dir == null) {
            super.loadLibraries();
            return;
        }
        File lib = gameLibraryFor(dir);
        if (lib == null) throw new UnsatisfiedLinkError("the game package has no library for " + Arrays.toString(Build.SUPPORTED_ABIS));
        System.load(lib.getAbsolutePath());
    }

    @Override
    protected String getMainSharedObject() {
        File dir = gameDir();
        if (dir == null) return super.getMainSharedObject();
        File lib = gameLibraryFor(dir);
        return lib == null ? "" : lib.getAbsolutePath();
    }

    // Runs on the SDL thread, so unpacking the ROM here does not block the UI.
    @Override
    protected String[] getArguments() {
        List<String> args = new ArrayList<>();
        File dir = gameDir();
        try {
            args.add(dir == null ? unpackAssets().getAbsolutePath() : new File(dir, "game.gba").getAbsolutePath());
        } catch (IOException e) {
            Log.e(TAG, "cannot unpack the ROM", e);
        }
        args.add("--miss-log");
        args.add(new File(getFilesDir(), "anygba-miss.log").getAbsolutePath());
        String[] extra = getIntent().getStringArrayExtra(EXTRA_ARGS);
        if (extra != null) args.addAll(Arrays.asList(extra));
        return args.toArray(new String[0]);
    }

    /** Copies game.gba and cheats/ out of the APK when it was installed or updated. */
    private File unpackAssets() throws IOException {
        File rom = new File(getNoBackupFilesDir(), "game.gba");
        long installed;
        try {
            installed = getPackageManager().getPackageInfo(getPackageName(), 0).lastUpdateTime;
        } catch (PackageManager.NameNotFoundException e) {
            installed = Long.MAX_VALUE;
        }
        if (rom.isFile() && rom.lastModified() >= installed) return rom;
        copy(getAssets().open("game.gba"), rom);
        String[] cheats = getAssets().list("cheats");
        if (cheats != null) {
            for (String name : cheats) copy(getAssets().open("cheats/" + name), new File(getFilesDir(), "cheats/" + name));
        }
        return rom;
    }

    static void copy(InputStream in, File to) throws IOException {
        File parent = to.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("cannot create " + parent);
        File tmp = new File(to.getPath() + ".tmp");
        try (InputStream src = in; OutputStream out = new FileOutputStream(tmp)) {
            byte[] buf = new byte[1 << 16];
            for (int n; (n = src.read(buf)) > 0; ) out.write(buf, 0, n);
        }
        if (!tmp.renameTo(to)) throw new IOException("cannot replace " + to);
    }

    @Override
    protected boolean onUnhandledMessage(int command, Object param) {
        if (command == MSG_TOUCH_SETTINGS) {
            TouchSettings.show(this);
            return true;
        }
        if (command == MSG_SCREENSHOT) {
            final File shot = newestScreenshot();
            if (shot != null) new Thread(() -> publishScreenshot(shot), "anygba-screenshot").start();
            return true;
        }
        return super.onUnhandledMessage(command, param);
    }

    private File newestScreenshot() {
        File[] files = new File(getFilesDir(), "screenshots").listFiles((dir, name) -> name.endsWith(".png"));
        File newest = null;
        if (files != null) {
            for (File f : files) {
                if (newest == null || f.lastModified() > newest.lastModified()) newest = f;
            }
        }
        return newest;
    }

    /**
     * Copies a screenshot to Pictures/AnyGBA in the gallery (Android 10+), else to the app's
     * external Pictures folder, and removes the private copy once it is published.
     */
    private void publishScreenshot(File shot) {
        String where;
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                where = publishToMediaStore(shot);
            } else {
                where = copyToExternalPictures(shot);
            }
        } catch (IOException | RuntimeException e) {
            Log.w(TAG, "screenshot to the gallery", e);
            try {
                where = copyToExternalPictures(shot);
            } catch (IOException | RuntimeException e2) {
                Log.e(TAG, "screenshot " + shot, e2);
                where = null;
            }
        }
        if (where != null && !shot.delete()) Log.w(TAG, "cannot delete " + shot);
        final String message = where == null ? "Screenshot kept in app storage" : "Screenshot saved to " + where;
        runOnUiThread(() -> Toast.makeText(this, message, Toast.LENGTH_SHORT).show());
    }

    private String publishToMediaStore(File shot) throws IOException {
        android.content.ContentValues values = new android.content.ContentValues();
        values.put(android.provider.MediaStore.MediaColumns.DISPLAY_NAME, shot.getName());
        values.put(android.provider.MediaStore.MediaColumns.MIME_TYPE, "image/png");
        values.put(android.provider.MediaStore.MediaColumns.RELATIVE_PATH, "Pictures/AnyGBA");
        values.put(android.provider.MediaStore.MediaColumns.IS_PENDING, 1);
        android.content.ContentResolver resolver = getContentResolver();
        Uri uri = resolver.insert(android.provider.MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values);
        if (uri == null) throw new IOException("the gallery refused the screenshot");
        try (InputStream in = new java.io.FileInputStream(shot); OutputStream out = resolver.openOutputStream(uri)) {
            if (out == null) throw new IOException("cannot open " + uri);
            byte[] buf = new byte[1 << 16];
            for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
        } catch (IOException e) {
            resolver.delete(uri, null, null);
            throw e;
        }
        values.clear();
        values.put(android.provider.MediaStore.MediaColumns.IS_PENDING, 0);
        resolver.update(uri, values, null, null);
        return "Pictures/AnyGBA";
    }

    private String copyToExternalPictures(File shot) throws IOException {
        File dir = getExternalFilesDir(android.os.Environment.DIRECTORY_PICTURES);
        if (dir == null) throw new IOException("no external storage");
        copy(new java.io.FileInputStream(shot), new File(dir, shot.getName()));
        return dir.getPath();
    }

    @Override
    protected void onNewIntent(Intent intent) {
        String running = getIntent().getStringExtra(EXTRA_GAME_DIR);
        String wanted = intent.getStringExtra(EXTRA_GAME_DIR);
        if (wanted != null && !wanted.equals(running)) {
            Toast.makeText(this, "Close the running game first", Toast.LENGTH_LONG).show();
        }
        super.onNewIntent(intent);
    }

    @Override
    protected void onDestroy() {
        boolean finishing = isFinishing();
        super.onDestroy();
        // SDL cannot start twice in one process, and the launcher may start another game's
        // library next; the saves were written when the activity paused.
        if (finishing) android.os.Process.killProcess(android.os.Process.myPid());
    }

    // ---- saves: export through the share sheet, import through the document picker ----

    String saveStem() {
        String stem = getIntent().getStringExtra(EXTRA_SAVE_STEM);
        if (stem != null) return stem;
        try {
            ApplicationInfo info = getPackageManager().getApplicationInfo(getPackageName(), PackageManager.GET_META_DATA);
            if (info.metaData != null) return info.metaData.getString(EXTRA_SAVE_STEM);
        } catch (PackageManager.NameNotFoundException e) {
            Log.w(TAG, "no save stem", e);
        }
        return null;
    }

    static File savesDir(Activity activity) {
        return new File(activity.getFilesDir(), "saves");
    }

    void exportSaves() {
        String stem = saveStem();
        File[] files = savesDir(this).listFiles();
        ArrayList<Uri> uris = new ArrayList<>();
        if (files != null && stem != null) {
            for (File f : files) {
                if (f.isFile() && f.getName().startsWith(stem + ".") && !f.getName().endsWith(".tmp")) {
                    uris.add(SaveProvider.uriFor(this, f.getName()));
                }
            }
        }
        if (uris.isEmpty()) {
            Toast.makeText(this, "No saves yet", Toast.LENGTH_SHORT).show();
            return;
        }
        Intent send = new Intent(uris.size() == 1 ? Intent.ACTION_SEND : Intent.ACTION_SEND_MULTIPLE);
        send.setType("application/octet-stream");
        if (uris.size() == 1) {
            send.putExtra(Intent.EXTRA_STREAM, uris.get(0));
        } else {
            send.putParcelableArrayListExtra(Intent.EXTRA_STREAM, uris);
        }
        ClipData clip = ClipData.newRawUri("saves", uris.get(0));
        for (int i = 1; i < uris.size(); ++i) clip.addItem(new ClipData.Item(uris.get(i)));
        send.setClipData(clip);
        send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        startActivity(Intent.createChooser(send, "Export saves"));
    }

    void importSaves() {
        Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        pick.addCategory(Intent.CATEGORY_OPENABLE);
        pick.setType("*/*");
        pick.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        startActivityForResult(pick, REQUEST_IMPORT);
    }

    /** "(stem).sav", "(stem).ss3", "(stem).cheats.toml" for a picked file name, else null. */
    static String importName(String picked, String stem) {
        if (picked == null) return null;
        String lower = picked.toLowerCase(java.util.Locale.ROOT);
        if (lower.endsWith(".sav")) return stem + ".sav";
        if (lower.endsWith(".cheats.toml")) return stem + ".cheats.toml";
        if (lower.length() > 4 && lower.matches(".*\\.ss[1-9]")) return stem + lower.substring(lower.length() - 4);
        return null;
    }

    // Called before onResume: the game is still paused and its saves were written in
    // onPause, so the imported files are not overwritten as long as the game never resumes.
    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != REQUEST_IMPORT || result != RESULT_OK || data == null) return;
        String stem = saveStem();
        List<Uri> picked = new ArrayList<>();
        if (data.getClipData() != null) {
            for (int i = 0; i < data.getClipData().getItemCount(); ++i) picked.add(data.getClipData().getItemAt(i).getUri());
        } else if (data.getData() != null) {
            picked.add(data.getData());
        }
        int imported = 0;
        List<String> skipped = new ArrayList<>();
        for (Uri uri : picked) {
            String name = displayName(uri);
            String target = stem == null ? null : importName(name, stem);
            if (target == null) {
                skipped.add(name);
                continue;
            }
            try {
                copy(getContentResolver().openInputStream(uri), new File(savesDir(this), target));
                ++imported;
            } catch (IOException | SecurityException e) {
                Log.e(TAG, "import " + name, e);
                skipped.add(name);
            }
        }
        if (imported == 0) {
            Toast.makeText(this, "Nothing imported (use .sav, .ss1-.ss9 or .cheats.toml files)", Toast.LENGTH_LONG).show();
            return;
        }
        String message = "Imported " + imported + " file(s)" + (skipped.isEmpty() ? "" : ", skipped " + skipped)
                + ". Open the game again to use them.";
        Toast.makeText(getApplicationContext(), message, Toast.LENGTH_LONG).show();
        finishAndRemoveTask();
        android.os.Process.killProcess(android.os.Process.myPid());
    }

    private String displayName(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) return c.getString(0);
        } catch (RuntimeException e) {
            Log.w(TAG, "no name for " + uri, e);
        }
        return uri.getLastPathSegment();
    }
}
