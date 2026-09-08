package com.iisacc.society.storage;

import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import org.json.JSONObject;

/** Qt bridge; the native app and DocumentsProvider open the same private source. */
public final class SocietyStorage {
    private SocietyStorage() { }
    public static String request(Context context, String action, String source, String identifier) {
        try {
            DriveStore store = DriveStore.get(context);
            if (!source.isEmpty() && !source.equals(store.rootPath()))
                throw new IllegalArgumentException("Android uses the managed Society container");
            if (!identifier.isEmpty() && !identifier.equals(store.identifier()))
                throw new IllegalArgumentException("The Society container identity changed");
            if (!action.equals("default") && !action.equals("register") && !action.equals("refresh") && !action.equals("path"))
                throw new IllegalArgumentException("Unknown Society drive action");
            Uri root = DocumentsContract.buildRootUri(SocietyDocumentsProvider.AUTHORITY, store.identifier());
            if (action.equals("path")) {
                Intent intent = new Intent(Intent.ACTION_VIEW).setDataAndType(root, DocumentsContract.Root.MIME_TYPE_ITEM)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                context.startActivity(intent);
            }
            context.getContentResolver().notifyChange(DocumentsContract.buildRootsUri(SocietyDocumentsProvider.AUTHORITY), null);
            return new JSONObject().put("identifier", store.identifier()).put("sourcePath", store.rootPath())
                .put("systemPath", root.toString()).put("enabled", true).toString();
        } catch (Exception error) {
            try { return new JSONObject().put("error", error.getMessage() == null ? error.toString() : error.getMessage()).toString(); }
            catch (Exception impossible) { return "{\"error\":\"Society storage is unavailable\"}"; }
        }
    }
    public static String displayName(Context context, String source) {
        Uri uri = Uri.parse(source);
        if (!"content".equals(uri.getScheme())) return "";
        try (Cursor rows = context.getContentResolver().query(uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            return rows != null && rows.moveToFirst() ? rows.getString(0) : "";
        } catch (Exception unavailable) { return ""; }
    }
}
