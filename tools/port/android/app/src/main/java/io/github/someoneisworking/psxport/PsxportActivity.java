package io.github.someoneisworking.psxport;

import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.os.Bundle;
import android.util.Log;

import io.github.someoneisworking.android.AndroidActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/** A psxport title: the framework's assets unpacked into the files directory, then SDL's main. */
public final class PsxportActivity extends AndroidActivity {
    private static final String TAG = "psxport";
    // The native side reads its assets/ from the files directory (psx::config::Config).
    private static final String ASSETS = "assets";
    // The framework's assets/ inside the APK, apart from what the platform lists at its root.
    private static final String PACKAGED = "psxport";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        unpackAssets();
        super.onCreate(savedInstanceState);
    }

    @Override
    protected String[] getLibraries() {
        return new String[]{"SDL3", "main"};
    }

    // Once per installed build: the stamp is the package's last update time.
    private void unpackAssets() {
        File root = new File(getFilesDir(), ASSETS);
        File stamp = new File(getFilesDir(), ASSETS + ".stamp");
        String installed;
        try {
            installed = Long.toString(getPackageManager().getPackageInfo(getPackageName(), 0).lastUpdateTime);
        } catch (PackageManager.NameNotFoundException error) {
            throw new IllegalStateException("the running package has no PackageInfo", error);
        }
        try {
            if (stamp.isFile() && installed.equals(new String(Files.readAllBytes(stamp.toPath()), StandardCharsets.UTF_8))) {
                return;
            }
            copyTree(getAssets(), PACKAGED, root);
            Files.write(stamp.toPath(), installed.getBytes(StandardCharsets.UTF_8));
        } catch (IOException error) {
            Log.e(TAG, "the framework assets could not be unpacked", error);
        }
    }

    private static void copyTree(AssetManager assets, String path, File destination) throws IOException {
        String[] children = assets.list(path);
        if (children != null && children.length > 0) {
            if (!destination.isDirectory() && !destination.mkdirs()) {
                throw new IOException("cannot create " + destination);
            }
            for (String child : children) {
                copyTree(assets, path + "/" + child, new File(destination, child));
            }
            return;
        }
        try (InputStream in = assets.open(path); OutputStream out = new FileOutputStream(destination)) {
            byte[] buffer = new byte[64 * 1024];
            for (int read; (read = in.read(buffer)) > 0; ) {
                out.write(buffer, 0, read);
            }
        }
    }
}
