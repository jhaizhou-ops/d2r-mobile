package com.xiaoboluo.d2r;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import java.io.File;
import java.io.FileNotFoundException;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.util.List;

// 把诊断文件交给玩家在分享面板里选的那个 App（#216）。不导出、只读；对方只能凭分享意图上的一次性授权进来，只认缓存目录 sharelog/ 下
// 我们自己写的那一份（名字里不能有路径，必须是普通文件、不是链接）。App 没有 AndroidX，所以不用 FileProvider，照 S2Arm 安卓版的做法自带一个。
// Hands the diagnostics file to the app the player picks in the share sheet (#216). Not exported and read-only; the reader only gets in through
// the share intent's one-off grant, and only the one file we wrote in the cache directory sharelog/ is served (no path in the name, a regular
// file, not a link). The app has no AndroidX, so instead of a FileProvider it carries its own, as the S2Arm Android app does.
public final class ShareLogProvider extends ContentProvider {
    static final String DIR = "sharelog";

    static String authority(Context context) { return context.getPackageName() + ".sharelog"; }

    static File directory(Context context) { return new File(context.getCacheDir(), DIR); }

    static Uri uri(Context context, File file) {
        return new Uri.Builder().scheme("content").authority(authority(context)).appendPath(file.getName()).build();
    }

    private File resolve(Uri uri) {
        Context context = getContext();
        List<String> segments = uri.getPathSegments();
        if (context == null || segments.size() != 1) return null;
        String name = segments.get(0);
        if (name.isEmpty() || name.contains("/") || name.startsWith(".") || !name.endsWith(".txt")) return null;
        File f = new File(directory(context), name);
        if (!Files.isRegularFile(f.toPath(), LinkOption.NOFOLLOW_LINKS)) return null;
        return f;
    }

    @Override public boolean onCreate() { return true; }

    @Override public String getType(Uri uri) { return resolve(uri) != null ? "text/plain" : null; }

    @Override public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (!"r".equals(mode)) throw new SecurityException("read-only");
        File f = resolve(uri);
        if (f == null) throw new FileNotFoundException();
        return ParcelFileDescriptor.open(f, ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder) {
        File f = resolve(uri);
        if (f == null) return null;
        String[] columns = projection != null ? projection : new String[] { OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE };
        Object[] row = new Object[columns.length];
        for (int i = 0; i < columns.length; i++) {
            if (OpenableColumns.DISPLAY_NAME.equals(columns[i])) row[i] = f.getName();
            else if (OpenableColumns.SIZE.equals(columns[i])) row[i] = f.length();
        }
        MatrixCursor cursor = new MatrixCursor(columns, 1);
        cursor.addRow(row);
        return cursor;
    }

    @Override public Uri insert(Uri uri, ContentValues values) { throw new UnsupportedOperationException("read-only"); }
    @Override public int delete(Uri uri, String selection, String[] selectionArgs) { throw new UnsupportedOperationException("read-only"); }
    @Override public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) { throw new UnsupportedOperationException("read-only"); }
}
