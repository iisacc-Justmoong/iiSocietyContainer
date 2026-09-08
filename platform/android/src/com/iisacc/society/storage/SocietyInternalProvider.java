package com.iisacc.society.storage;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.*;
import android.provider.OpenableColumns;
import org.json.*;
import java.io.*;
import java.util.List;

/** Same-signer iisacc IPC. Never registered as a DocumentsProvider or SAF root. */
public final class SocietyInternalProvider extends ContentProvider {
    public static final String AUTHORITY = "com.iisacc.society.internal";
    private void authorize() {
        if (Binder.getCallingUid() != android.os.Process.myUid()
            && getContext().getPackageManager().checkSignatures(Binder.getCallingUid(), android.os.Process.myUid()) != PackageManager.SIGNATURE_MATCH)
            throw new SecurityException("Society internal storage requires the iisacc app signing identity");
    }
    @Override public boolean onCreate() { return true; }
    static Uri uri(DriveStore store, String key, String path) {
        Uri.Builder uri = new Uri.Builder().scheme("content").authority(AUTHORITY).appendPath("drive")
            .appendPath(store.identifier());
        if (!key.isEmpty()) uri.appendPath(key);
        if (!path.isEmpty()) for (String component : path.split("/", -1)) uri.appendPath(component);
        return uri.build();
    }
    private File resolve(Uri uri, boolean missing) throws IOException {
        authorize();
        DriveStore store = DriveStore.get(getContext());
        List<String> parts = uri.getPathSegments();
        if (!AUTHORITY.equals(uri.getAuthority()) || parts.size() < 3 || !parts.get(0).equals("drive")
            || !parts.get(1).equals(store.identifier()) || uri.getQuery() != null || uri.getFragment() != null)
            throw new FileNotFoundException("Invalid Society internal URI");
        for (String part : parts.subList(3, parts.size())) DriveStore.name(part);
        return store.internal(parts.get(2), String.join("/", parts.subList(3, parts.size())), missing);
    }
    @Override public Bundle call(String method, String argument, Bundle extras) {
        authorize(); // ContentProvider.call does not imply a read/write permission check.
        Bundle result = new Bundle();
        try {
            DriveStore store = DriveStore.get(getContext());
            String expected = extras == null ? "" : extras.getString("identifier", "");
            if (!expected.isEmpty() && !expected.equals(store.identifier())) throw new IOException("Society container identity changed");
            String key = extras == null ? "" : extras.getString("section", "");
            String path = extras == null ? "" : extras.getString("path", "");
            JSONObject data = new JSONObject().put("identifier", store.identifier()).put("rootPath", uri(store, "", "").toString());
            synchronized (store) {
                if (method.equals("open")) { /* Identity and availability only. */ }
                else if (method.equals("path") || method.equals("mkdir")) {
                    if (method.equals("mkdir")) store.ensureInternalDirectory(key, path);
                    else store.internal(key, path, true);
                    data.put("path", uri(store, key, path).toString());
                } else if (method.equals("list")) {
                    File directory = store.internal(key, path, false);
                    File[] entries = directory.listFiles();
                    if (entries == null) throw new IOException("Society directory is unavailable");
                    JSONArray rows = new JSONArray();
                    for (File entry : entries) {
                        String child = path.isEmpty() ? entry.getName() : path + "/" + entry.getName();
                        try {
                            store.internal(key, child, false);
                            rows.put(new JSONObject().put("name", entry.getName()).put("path", uri(store, key, child).toString())
                                .put("isDirectory", entry.isDirectory()).put("size", entry.length()));
                        } catch (IOException redirect) { }
                    }
                    data.put("entries", rows);
                } else throw new IOException("Unknown Society storage operation");
            }
            result.putString("json", data.toString());
        } catch (Exception error) {
            try { result.putString("json", new JSONObject().put("error", error.toString()).toString()); }
            catch (JSONException impossible) { throw new IllegalStateException(impossible); }
        }
        return result;
    }
    @Override public Cursor query(Uri uri, String[] projection, String selection, String[] args, String sort) {
        try {
            File file = resolve(uri, true);
            MatrixCursor result = new MatrixCursor(projection == null ? new String[]{OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE} : projection);
            if (!file.exists()) return result;
            result.newRow().add(OpenableColumns.DISPLAY_NAME, file.getName()).add(OpenableColumns.SIZE, file.length());
            return result;
        } catch (IOException error) { throw new IllegalArgumentException(error); }
    }
    @Override public String getType(Uri uri) {
        try { return SocietyDocumentsProvider.mime(resolve(uri, false)); }
        catch (IOException error) { return null; }
    }
    @Override public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        try {
            DriveStore store = DriveStore.get(getContext());
            synchronized (store) {
                File file = resolve(uri, !mode.equals("r"));
                if (file.isDirectory()) throw new IOException("Cannot open a Society area as a file");
                return ParcelFileDescriptor.open(file, ParcelFileDescriptor.parseMode(mode));
            }
        } catch (IOException error) { throw new FileNotFoundException(error.getMessage()); }
    }
    @Override public Uri insert(Uri uri, ContentValues values) { throw new UnsupportedOperationException("Use openFile or mkdir"); }
    @Override public int update(Uri uri, ContentValues values, String selection, String[] args) { throw new UnsupportedOperationException(); }
    @Override public int delete(Uri uri, String selection, String[] args) { throw new UnsupportedOperationException(); }
}
