package com.iisacc.society.storage.tests;

import android.app.Instrumentation;
import android.content.ContentResolver;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import android.provider.DocumentsContract.Root;
import com.iisacc.society.storage.DriveStore;
import com.iisacc.society.storage.SocietyDocumentsProvider;
import com.iisacc.society.storage.SocietyStorage;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

/** Runs inside the installed Society app against its real registered provider. */
public final class ProviderInstrumentation extends Instrumentation {
    private int checks;
    private String phase;
    @Override public void onCreate(Bundle arguments) { super.onCreate(arguments); phase = arguments.getString("phase", "operations"); start(); }
    private void check(boolean value, String message) { ++checks; if (!value) throw new AssertionError(message); }
    private static String string(Cursor row, String column) { return row.getString(row.getColumnIndexOrThrow(column)); }
    private Uri document(String id) { return DocumentsContract.buildDocumentUri(SocietyDocumentsProvider.AUTHORITY, id); }
    private List<String> children(ContentResolver resolver, String parent) {
        ArrayList<String> result = new ArrayList<>();
        try (Cursor rows = resolver.query(DocumentsContract.buildChildDocumentsUri(SocietyDocumentsProvider.AUTHORITY, parent), null, null, null, null)) {
            while (rows.moveToNext()) result.add(string(rows, Document.COLUMN_DISPLAY_NAME));
        }
        return result;
    }
    private void write(ContentResolver resolver, Uri uri, String content, String mode) throws IOException {
        try (OutputStream output = resolver.openOutputStream(uri, mode)) { output.write(content.getBytes(StandardCharsets.UTF_8)); }
    }
    private String read(ContentResolver resolver, Uri uri) throws IOException {
        ByteArrayOutputStream result = new ByteArrayOutputStream();
        try (InputStream input = resolver.openInputStream(uri)) {
            byte[] buffer = new byte[1024]; int count;
            while ((count = input.read(buffer)) != -1) result.write(buffer, 0, count);
        }
        return result.toString("UTF-8");
    }
    private interface Checked { void run() throws Exception; }
    private void rejected(Checked operation) throws Exception {
        boolean denied = false;
        try { operation.run(); } catch (IOException | IllegalArgumentException | SecurityException expected) { denied = true; }
        check(denied, "An invalid provider operation was accepted");
    }
    @Override public void onStart() {
        Bundle result = new Bundle();
        try {
            ContentResolver resolver = getTargetContext().getContentResolver();
            DriveStore store = DriveStore.get(getTargetContext());
            if (phase.equals("restart")) {
                String id = getTargetContext().getSharedPreferences("society-provider-tests", 0).getString("persistent", "");
                check(read(resolver, document(id)).equals("survives restart"), "Document ID did not survive process restart");
                DocumentsContract.deleteDocument(resolver, document(id));
            } else {
                File source = new File(store.rootPath());
                for (String name : new String[]{"Asset Library", "Deleted", "Files", "Forked", "Generation History", "Models", "Published", "Thinking Space"})
                    check(new File(source, name).isDirectory(), "Missing logical area: " + name);
                try (OutputStream output = new FileOutputStream(new File(source, "Models/private.safetensor"))) { output.write(42); }
                String root;
                try (Cursor rows = resolver.query(DocumentsContract.buildRootsUri(SocietyDocumentsProvider.AUTHORITY), null, null, null, null)) {
                    check(rows.getCount() == 1 && rows.moveToFirst(), "Exactly one public drive is required");
                    root = string(rows, Root.COLUMN_DOCUMENT_ID);
                    check(string(rows, Root.COLUMN_TITLE).equals("Society"), "Drive label must be Society");
                }
                List<String> visible = children(resolver, root);
                for (String hidden : new String[]{"Models", "Asset Library", "Deleted", "Files", "Forked", "Generation History", "Published", "Thinking Space", ".society-drive.json"})
                    check(!visible.contains(hidden), "Private source layout leaked: " + hidden);
                Uri rootUri = document(root);
                try (Cursor rows = resolver.query(rootUri, null, null, null, null)) {
                    check(rows.moveToFirst() && string(rows, Document.COLUMN_DISPLAY_NAME).equals("Society"), "Root name must be Society");
                }
                File manifest = new File(source, ".society-drive.json");
                byte[] originalManifest;
                try (InputStream input = new FileInputStream(manifest)) {
                    ByteArrayOutputStream bytes = new ByteArrayOutputStream();
                    byte[] buffer = new byte[1024]; int count;
                    while ((count = input.read(buffer)) != -1) bytes.write(buffer, 0, count);
                    originalManifest = bytes.toByteArray();
                }
                try {
                    org.json.JSONObject legacy = new org.json.JSONObject(new String(originalManifest, StandardCharsets.UTF_8));
                    legacy.put("displayName", "Society Container");
                    try (OutputStream output = new FileOutputStream(manifest)) { output.write(legacy.toString().getBytes(StandardCharsets.UTF_8)); }
                    check(DriveStore.get(getTargetContext()).identifier().equals(store.identifier()), "Legacy labels must retain identity");
                    try (Cursor rows = resolver.query(rootUri, null, null, null, null)) {
                        check(rows.moveToFirst() && string(rows, Document.COLUMN_DISPLAY_NAME).equals("Society"), "Legacy roots must display Society");
                    }
                } finally {
                    try (OutputStream output = new FileOutputStream(manifest)) { output.write(originalManifest); }
                }
                try {
                    org.json.JSONObject mirror = new org.json.JSONObject(new String(originalManifest, StandardCharsets.UTF_8));
                    String hostID = UUID.randomUUID().toString();
                    mirror.put("identifier", hostID).put("localIdentifier", store.identifier()).put("replicaReady", false);
                    try (OutputStream output = new FileOutputStream(manifest)) { output.write(mirror.toString().getBytes(StandardCharsets.UTF_8)); }
                    check(DriveStore.get(getTargetContext()).identifier().equals(hostID), "Managed storage must reopen the host identity");
                    check(new org.json.JSONObject(SocietyStorage.request(getTargetContext(), "default", "", "")).has("sourcePath"), "Interrupted mirrors must retain their managed source path");
                    rejected(() -> DriveStore.get(getTargetContext()).requireReady());
                    mirror.put("replicaReady", true);
                    try (OutputStream output = new FileOutputStream(manifest)) { output.write(mirror.toString().getBytes(StandardCharsets.UTF_8)); }
                    DriveStore.get(getTargetContext()).requireReady();
                    rejected(() -> resolver.openFileDescriptor(document(store.identifier() + ":root"), "r"));
                    try (Cursor rows = resolver.query(DocumentsContract.buildRootsUri(SocietyDocumentsProvider.AUTHORITY), null, null, null, null)) {
                        check(rows.moveToFirst() && string(rows, Root.COLUMN_ROOT_ID).equals(hostID), "The OS must expose the adopted logical drive");
                    }
                } finally {
                    try (OutputStream output = new FileOutputStream(manifest)) { output.write(originalManifest); }
                    DriveStore.get(getTargetContext()).requireReady();
                }
                Uri folder = DocumentsContract.createDocument(resolver, rootUri, Document.MIME_TYPE_DIR, "테스트 folder");
                Uri file = DocumentsContract.createDocument(resolver, folder, "text/plain", "hello.txt");
                String fileId = DocumentsContract.getDocumentId(file);
                write(resolver, file, "Society Files", "w");
                check(read(resolver, file).equals("Society Files"), "Read/write failed");
                check(new File(source, "Files/테스트 folder/hello.txt").isFile(), "Drive writes did not reach Files");
                write(resolver, file, "ok", "wt");
                check(read(resolver, file).equals("ok"), "Truncation failed");
                folder = DocumentsContract.renameDocument(resolver, folder, "renamed");
                check(read(resolver, file).equals("ok"), "Child ID changed after parent rename");
                file = DocumentsContract.moveDocument(resolver, file, folder, rootUri);
                check(DocumentsContract.getDocumentId(file).equals(fileId), "Move changed document ID");
                check(DocumentsContract.isChildDocument(resolver, rootUri, file), "Child relationship is missing");
                Uri tree = DocumentsContract.buildTreeDocumentUri(SocietyDocumentsProvider.AUTHORITY, root);
                DocumentsContract.Path path = DocumentsContract.findDocumentPath(resolver, DocumentsContract.buildDocumentUriUsingTree(tree, fileId));
                check(path.getPath().equals(Arrays.asList(root, fileId)), "Public path exposed an internal parent");
                check(SocietyStorage.displayName(getTargetContext(), file.toString()).equals("hello.txt"), "Content URI display name failed");
                for (String name : new String[]{"..", "../Models/leak", "a/b", "a\\b", "C:secret"}) {
                    final String invalid = name;
                    rejected(() -> DocumentsContract.createDocument(resolver, rootUri, "text/plain", invalid));
                }
                rejected(() -> DocumentsContract.deleteDocument(resolver, rootUri));
                rejected(() -> resolver.openFileDescriptor(document(store.identifier() + ":../Models/private.safetensor"), "r"));
                rejected(() -> resolver.openFileDescriptor(document(store.identifier() + ":Models"), "r"));
                File link = new File(source, "Files/private-link");
                android.system.Os.symlink(new File(source, "Models").toString(), link.toString());
                check(!children(resolver, root).contains("private-link"), "Symbolic link escaped the Files boundary");
                link.delete();
                CancellationSignal cancel = new CancellationSignal(); cancel.cancel();
                boolean cancelled = false;
                try { resolver.openFileDescriptor(file, "r", cancel); } catch (android.os.OperationCanceledException expected) { cancelled = true; }
                check(cancelled, "Cancelled open was accepted");
                DocumentsContract.deleteDocument(resolver, file);
                DocumentsContract.deleteDocument(resolver, folder);
                final Uri deleted = file;
                rejected(() -> resolver.openFileDescriptor(deleted, "r"));
                check(new File(source, "Models/private.safetensor").length() == 1, "Public operations modified Models");
                Uri persistent = DocumentsContract.createDocument(resolver, rootUri, "text/plain", "restart.txt");
                write(resolver, persistent, "survives restart", "w");
                getTargetContext().getSharedPreferences("society-provider-tests", 0).edit().putString("persistent", DocumentsContract.getDocumentId(persistent)).commit();
            }
            result.putString("stream", "Society Android provider " + phase + ": " + checks + " checks passed\n");
            finish(-1, result);
        } catch (Throwable error) {
            StringWriter trace = new StringWriter(); error.printStackTrace(new PrintWriter(trace));
            result.putString("stream", trace.toString()); finish(1, result);
        }
    }
}
