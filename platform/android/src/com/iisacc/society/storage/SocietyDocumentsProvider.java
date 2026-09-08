package com.iisacc.society.storage;

import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.*;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import android.provider.DocumentsContract.Root;
import android.provider.DocumentsProvider;
import android.webkit.MimeTypeMap;
import java.io.*;
import java.util.ArrayList;

/** The sole public drive root is the CONTENTS of Society/Files/. */
public final class SocietyDocumentsProvider extends DocumentsProvider {
    public static final String AUTHORITY = "com.iisacc.society.documents";
    private static final String[] DOCUMENT_COLUMNS = {Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME,
        Document.COLUMN_MIME_TYPE, Document.COLUMN_FLAGS, Document.COLUMN_SIZE, Document.COLUMN_LAST_MODIFIED};
    private static final String[] ROOT_COLUMNS = {Root.COLUMN_ROOT_ID, Root.COLUMN_DOCUMENT_ID, Root.COLUMN_TITLE,
        Root.COLUMN_FLAGS, Root.COLUMN_MIME_TYPES, Root.COLUMN_AVAILABLE_BYTES};
    private final ArrayList<FileObserver> observers = new ArrayList<>();
    private DriveStore store() throws FileNotFoundException {
        try { return DriveStore.get(getContext()); } catch (IOException error) { throw missing(error); }
    }
    private static FileNotFoundException missing(Exception error) {
        return new FileNotFoundException(error.getMessage());
    }
    @Override public boolean onCreate() { return true; }
    public static Uri rootUri(DriveStore store) {
        return DocumentsContract.buildDocumentUri(AUTHORITY, store.rootId());
    }
    private void changed() {
        getContext().getContentResolver().notifyChange(DocumentsContract.buildRootsUri(AUTHORITY), null);
        getContext().getContentResolver().notifyChange(Uri.parse("content://" + AUTHORITY + "/document"), null);
    }
    private synchronized void watch(File directory) {
        for (FileObserver observer : observers) observer.stopWatching();
        observers.clear();
        watchTree(directory);
    }
    @SuppressWarnings("deprecation") private void watchTree(File directory) {
        FileObserver observer = new FileObserver(directory.getAbsolutePath(), FileObserver.CREATE | FileObserver.DELETE
                | FileObserver.MOVED_FROM | FileObserver.MOVED_TO | FileObserver.CLOSE_WRITE | FileObserver.ATTRIB) {
            @Override public void onEvent(int event, String path) { changed(); }
        };
        observer.startWatching(); observers.add(observer);
        File[] children = directory.listFiles();
        if (children != null) for (File child : children) {
            try { DriveStore.checkFile(child); if (child.isDirectory()) watchTree(child); } catch (IOException ignored) { }
        }
    }
    @Override public Cursor queryRoots(String[] projection) throws FileNotFoundException {
        DriveStore store = store();
        MatrixCursor result = new MatrixCursor(projection == null ? ROOT_COLUMNS : projection);
        MatrixCursor.RowBuilder row = result.newRow();
        row.add(Root.COLUMN_ROOT_ID, store.identifier()).add(Root.COLUMN_DOCUMENT_ID, store.rootId())
            .add(Root.COLUMN_TITLE, "Society Container").add(Root.COLUMN_FLAGS, Root.FLAG_SUPPORTS_CREATE | Root.FLAG_SUPPORTS_IS_CHILD)
            .add(Root.COLUMN_MIME_TYPES, "*/*").add(Root.COLUMN_AVAILABLE_BYTES, store.files().getUsableSpace());
        result.setNotificationUri(getContext().getContentResolver(), DocumentsContract.buildRootsUri(AUTHORITY));
        return result;
    }
    static String mime(File file) {
        if (file.isDirectory()) return Document.MIME_TYPE_DIR;
        String name = file.getName();
        String type = MimeTypeMap.getSingleton().getMimeTypeFromExtension(name.substring(name.lastIndexOf('.') + 1).toLowerCase(java.util.Locale.ROOT));
        return type == null ? "application/octet-stream" : type;
    }
    private void row(MatrixCursor result, DriveStore store, String id) throws IOException {
        File file = store.document(id);
        int flags = file.isDirectory() ? Document.FLAG_DIR_SUPPORTS_CREATE : Document.FLAG_SUPPORTS_WRITE;
        if (!id.equals(store.rootId())) flags |= Document.FLAG_SUPPORTS_DELETE | Document.FLAG_SUPPORTS_RENAME | Document.FLAG_SUPPORTS_MOVE;
        result.newRow().add(Document.COLUMN_DOCUMENT_ID, id)
            .add(Document.COLUMN_DISPLAY_NAME, id.equals(store.rootId()) ? "Society Container" : file.getName())
            .add(Document.COLUMN_MIME_TYPE, mime(file)).add(Document.COLUMN_FLAGS, flags)
            .add(Document.COLUMN_SIZE, file.isDirectory() ? null : file.length()).add(Document.COLUMN_LAST_MODIFIED, file.lastModified());
    }
    @Override public Cursor queryDocument(String id, String[] projection) throws FileNotFoundException {
        MatrixCursor result = new MatrixCursor(projection == null ? DOCUMENT_COLUMNS : projection);
        try { row(result, store(), id); } catch (IOException error) { throw missing(error); }
        result.setNotificationUri(getContext().getContentResolver(), DocumentsContract.buildDocumentUri(AUTHORITY, id));
        return result;
    }
    @Override public Cursor queryChildDocuments(String parent, String[] projection, String sortOrder) throws FileNotFoundException {
        DriveStore store = store();
        MatrixCursor result = new MatrixCursor(projection == null ? DOCUMENT_COLUMNS : projection);
        synchronized (store) {
            try { for (File file : store.children(parent)) row(result, store, store.idFor(file)); }
            catch (IOException error) { throw missing(error); }
        }
        watch(store.files());
        result.setNotificationUri(getContext().getContentResolver(), DocumentsContract.buildChildDocumentsUri(AUTHORITY, parent));
        return result;
    }
    @Override public ParcelFileDescriptor openDocument(String id, String mode, CancellationSignal signal) throws FileNotFoundException {
        if (signal != null) signal.throwIfCanceled();
        DriveStore store = store();
        synchronized (store) {
            try {
                File file = store.document(id);
                if (!file.isFile()) throw new IOException("Cannot open a Society directory as a file");
                int flags = ParcelFileDescriptor.parseMode(mode);
                return ParcelFileDescriptor.open(file, flags, new Handler(Looper.getMainLooper()), error -> changed());
            } catch (IOException error) { throw missing(error); }
        }
    }
    @Override public String createDocument(String parent, String mime, String name) throws FileNotFoundException {
        try { String id = store().create(parent, mime, name); changed(); return id; }
        catch (IOException error) { throw missing(error); }
    }
    @Override public void deleteDocument(String id) throws FileNotFoundException {
        try { store().delete(id); changed(); } catch (IOException error) { throw missing(error); }
    }
    @Override public String renameDocument(String id, String name) throws FileNotFoundException {
        DriveStore store = store();
        synchronized (store) {
            try { store.move(id, store.parentId(id), name); changed(); return id; }
            catch (IOException error) { throw missing(error); }
        }
    }
    @Override public String moveDocument(String id, String sourceParent, String targetParent) throws FileNotFoundException {
        DriveStore store = store();
        synchronized (store) {
            try {
                if (!store.parentId(id).equals(sourceParent)) throw new IOException("Source parent changed");
                store.move(id, targetParent, store.document(id).getName()); changed(); return id;
            } catch (IOException error) { throw missing(error); }
        }
    }
    @Override public boolean isChildDocument(String parent, String child) {
        try { return store().isChild(parent, child); } catch (IOException error) { return false; }
    }
    @Override public DocumentsContract.Path findDocumentPath(String parent, String child) throws FileNotFoundException {
        DriveStore store = store();
        try { return new DocumentsContract.Path(parent == null ? store.identifier() : null,
            store.documentPath(parent == null ? store.rootId() : parent, child)); }
        catch (IOException error) { throw missing(error); }
    }
}
