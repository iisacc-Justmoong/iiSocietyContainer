package com.iisacc.society.storage;

import android.content.Context;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;
import android.system.Os;
import android.system.OsConstants;
import android.system.StructStat;
import android.util.AtomicFile;
import org.json.JSONArray;
import org.json.JSONObject;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

/** Private sectioned source. DocumentsProvider receives only Files-relative IDs. */
public final class DriveStore {
    public static final String DISPLAY_NAME = "Society";
    private static DriveStore instance;
    private final File root;
    private final String identity;
    private final JSONArray catalog;
    private final SQLiteDatabase index;

    public static synchronized DriveStore get(Context context) throws IOException {
        if (instance == null) instance = new DriveStore(context.getApplicationContext());
        synchronized (instance) {
            try {
                if (!instance.manifest().getString("identifier").equals(instance.identity)) {
                    // Construct and validate first. Existing operations retain
                    // their old store and fail its identity check safely.
                    instance = new DriveStore(context.getApplicationContext());
                }
            } catch (Exception error) { throw failure(error); }
            instance.validate();
        }
        return instance;
    }

    private DriveStore(Context context) throws IOException {
        root = new File(context.getFilesDir(), "Society").getCanonicalFile();
        try (InputStream input = context.getAssets().open("society/Sections.json")) {
            catalog = new JSONArray(new String(readAll(input), StandardCharsets.UTF_8));
            if (catalog.length() != 9) throw new IOException("Invalid Society section catalog");
            if (!root.isDirectory() && !root.mkdirs()) throw new IOException("Cannot create Society storage");
            File manifest = new File(root, ".society-drive.json");
            if (!manifest.exists()) {
                for (int i = 0; i < catalog.length(); ++i) {
                    File section = new File(root, catalog.getJSONObject(i).getString("path"));
                    if (!section.isDirectory() && !section.mkdir()) throw new IOException("Cannot create Society area");
                    checkFile(section);
                }
                JSONObject data = new JSONObject().put("type", "SocietyDrive").put("schemaVersion", 1).put("filesLayoutVersion", 1)
                    .put("identifier", UUID.randomUUID().toString()).put("displayName", DISPLAY_NAME)
                    .put("sections", catalog);
                AtomicFile atomic = new AtomicFile(manifest);
                FileOutputStream output = atomic.startWrite();
                try { output.write(data.toString().getBytes(StandardCharsets.UTF_8)); atomic.finishWrite(output); }
                catch (Exception error) { atomic.failWrite(output); throw error; }
            }
            identity = manifest().getString("identifier");
            UUID.fromString(identity);
            validate();
            index = context.openOrCreateDatabase("society-files-index.db", Context.MODE_PRIVATE, null);
            index.execSQL("CREATE TABLE IF NOT EXISTS documents (id TEXT PRIMARY KEY, drive TEXT NOT NULL, path TEXT NOT NULL, device INTEGER NOT NULL, inode INTEGER NOT NULL, UNIQUE(drive,path))");
            index.execSQL("CREATE INDEX IF NOT EXISTS document_identity ON documents(drive,device,inode)");
        } catch (Exception error) { throw failure(error); }
    }

    static byte[] readAll(InputStream input) throws IOException {
        ByteArrayOutputStream result = new ByteArrayOutputStream();
        byte[] buffer = new byte[8192];
        int count;
        while ((count = input.read(buffer)) != -1) {
            if (result.size() + count > 65536) throw new IOException("Society metadata is too large");
            result.write(buffer, 0, count);
        }
        return result.toByteArray();
    }
    static IOException failure(Exception error) {
        return error instanceof IOException ? (IOException) error : new IOException(error.getMessage(), error);
    }
    private JSONObject manifest() throws Exception {
        File file = new File(root, ".society-drive.json");
        checkFile(file);
        try (InputStream input = new FileInputStream(file)) {
            return new JSONObject(new String(readAll(input), StandardCharsets.UTF_8));
        }
    }
    synchronized void validate() throws IOException {
        try {
            checkFile(root);
            JSONObject data = manifest();
            if (!data.getString("identifier").equals(identity) || !data.getString("type").equals("SocietyDrive")
                || data.getInt("schemaVersion") != 1
                || !(data.getString("displayName").equals(DISPLAY_NAME) || data.getString("displayName").equals("Society Container")))
                throw new IOException("The original Society container was replaced");
            JSONArray sections = data.getJSONArray("sections");
            if (sections.length() != catalog.length()) throw new IOException("Society areas changed");
            for (int i = 0; i < catalog.length(); ++i) {
                JSONObject expected = catalog.getJSONObject(i), actual = sections.getJSONObject(i);
                for (String key : new String[]{"id", "name", "path"})
                    if (!actual.getString(key).equals(expected.getString(key))) throw new IOException("Society areas changed");
                File section = new File(root, expected.getString("path"));
                checkFile(section);
                if (!section.isDirectory()) throw new IOException("Society area is unavailable");
            }
        } catch (Exception error) { throw failure(error); }
    }
    synchronized boolean protectedDocument(String id) throws IOException {
        document(id); // Keep normal identity and confinement validation.
        return rootId().equals(id);
    }
    public synchronized void requireReady() throws IOException {
        validate();
        try {
            JSONObject data = manifest();
            if (data.has("localIdentifier") && !data.optBoolean("replicaReady", false))
                throw new IOException("The initial Society mirror is not ready");
        } catch (Exception error) { throw failure(error); }
    }
    public String rootPath() { return root.getAbsolutePath(); }
    public String identifier() { return identity; }
    public String rootId() { return identity + ":root"; }
    File files() { return new File(root, "Files"); }

    synchronized File internal(String key, String path, boolean missingLeaf) throws IOException {
        requireReady();
        String area = null;
        try {
            for (int i = 0; i < catalog.length(); ++i)
                if (catalog.getJSONObject(i).getString("id").equals(key)) area = catalog.getJSONObject(i).getString("path");
        } catch (Exception error) { throw failure(error); }
        if (area == null) throw new IOException("Unknown Society area");
        File current = new File(root, area);
        checkFile(current);
        if (path.isEmpty()) return current;
        String[] parts = path.split("/", -1);
        for (int i = 0; i < parts.length; ++i) {
            name(parts[i]); current = new File(current, parts[i]);
            if (missingLeaf && i == parts.length - 1 && !current.exists()) {
                try { Os.lstat(current.toString()); } catch (android.system.ErrnoException error) {
                    if (error.errno == OsConstants.ENOENT) return current;
                    throw failure(error);
                }
            }
            checkFile(current);
        }
        return current;
    }
    synchronized File ensureInternalDirectory(String key, String path) throws IOException {
        if (path.isEmpty()) return internal(key, "", false);
        String accumulated = "";
        for (String component : path.split("/", -1)) {
            name(component); accumulated += (accumulated.isEmpty() ? "" : "/") + component;
            File directory = internal(key, accumulated, true);
            if (!directory.exists() && !directory.mkdir()) throw new IOException("Cannot create Society directory");
            checkFile(directory);
            if (!directory.isDirectory()) throw new IOException("Society path is not a directory");
        }
        return internal(key, path, false);
    }

    static StructStat checkFile(File file) throws IOException {
        try {
            StructStat stat = Os.lstat(file.getAbsolutePath());
            if (OsConstants.S_ISLNK(stat.st_mode) || (!OsConstants.S_ISDIR(stat.st_mode) && !OsConstants.S_ISREG(stat.st_mode))
                || !file.getAbsolutePath().equals(file.getCanonicalPath()))
                throw new IOException("Redirected and special files are not available in Society Files");
            return stat;
        } catch (Exception error) { throw failure(error); }
    }
    static void name(String name) throws IOException {
        if (name == null || name.isEmpty() || name.equals(".") || name.equals("..") || name.indexOf('/') >= 0
            || name.indexOf('\\') >= 0 || name.indexOf(':') >= 0 || name.indexOf('\0') >= 0)
            throw new IOException("Invalid Society filename");
    }
    private File relative(String path) throws IOException {
        File current = files();
        checkFile(current);
        if (path.isEmpty()) return current;
        for (String component : path.split("/", -1)) {
            name(component);
            current = new File(current, component);
            checkFile(current);
        }
        return current;
    }
    private String relative(File file) throws IOException {
        String base = files().getAbsolutePath(), path = file.getAbsolutePath();
        if (path.equals(base)) return "";
        if (!path.startsWith(base + "/")) throw new IOException("File is outside Society Files");
        return path.substring(base.length() + 1);
    }
    synchronized File document(String id) throws IOException {
        requireReady();
        if (rootId().equals(id)) return files();
        if (id == null || !id.startsWith(identity + ":")) throw new FileNotFoundException("Unknown Society document");
        try (Cursor rows = index.query("documents", new String[]{"path", "device", "inode"}, "id=? AND drive=?", new String[]{id, identity}, null, null, null)) {
            if (!rows.moveToFirst()) throw new FileNotFoundException("Unknown Society document");
            File file = relative(rows.getString(0));
            StructStat stat = checkFile(file);
            if (stat.st_dev != rows.getLong(1) || stat.st_ino != rows.getLong(2))
                throw new FileNotFoundException("The Society document was replaced");
            return file;
        }
    }
    synchronized String idFor(File file) throws IOException {
        validate();
        String path = relative(file);
        StructStat stat = checkFile(relative(path));
        if (path.isEmpty()) return rootId();
        String id = null;
        try (Cursor rows = index.query("documents", new String[]{"id", "path"}, "drive=? AND device=? AND inode=?", new String[]{identity, Long.toString(stat.st_dev), Long.toString(stat.st_ino)}, null, null, null)) {
            if (rows.moveToFirst()) {
                id = rows.getString(0);
                String previous = rows.getString(1);
                // A separately renamed file retains its ID. A hard link receives its own ID.
                if (!previous.equals(path) && new File(files(), previous).exists()) id = null;
            }
        }
        if (id == null) id = identity + ":" + UUID.randomUUID();
        ContentValues row = new ContentValues();
        row.put("id", id); row.put("drive", identity); row.put("path", path); row.put("device", stat.st_dev); row.put("inode", stat.st_ino);
        index.insertWithOnConflict("documents", null, row, SQLiteDatabase.CONFLICT_REPLACE);
        return id;
    }
    synchronized File[] children(String id) throws IOException {
        File parent = document(id);
        if (!parent.isDirectory()) throw new FileNotFoundException("Not a Society directory");
        File[] entries = parent.listFiles();
        if (entries == null) throw new IOException("Cannot list Society directory");
        ArrayList<File> visible = new ArrayList<>();
        for (File entry : entries) {
            try { relative(relative(entry)); visible.add(entry); } catch (IOException redirected) { /* never expose a redirect */ }
        }
        visible.sort(Comparator.comparing(File::getName));
        return visible.toArray(new File[0]);
    }
    synchronized String create(String parentId, String mime, String displayName) throws IOException {
        name(displayName);
        File parent = document(parentId);
        if (!parent.isDirectory()) throw new IOException("Not a Society directory");
        File file = new File(parent, displayName);
        boolean directory = android.provider.DocumentsContract.Document.MIME_TYPE_DIR.equals(mime);
        if (directory ? !file.mkdir() : !file.createNewFile()) throw new IOException("A file with this name already exists");
        return idFor(file);
    }
    synchronized String move(String id, String parentId, String displayName) throws IOException {
        if (protectedDocument(id)) throw new IOException("Cannot move the Society Files root");
        name(displayName);
        File source = document(id), parent = document(parentId), destination = new File(parent, displayName);
        if (!parent.isDirectory() || destination.exists() || parent.getAbsolutePath().startsWith(source.getAbsolutePath() + "/") || parent.equals(source))
            throw new IOException("Invalid Society move destination");
        String oldPath = relative(source), newPath = relative(destination);
        if (!source.renameTo(destination)) throw new IOException("Cannot move Society document");
        // Preserve descendant IDs across folder moves without SQL LIKE wildcard interpretation.
        index.beginTransaction();
        try (Cursor rows = index.query("documents", new String[]{"id", "path"}, "drive=?", new String[]{identity}, null, null, null)) {
            while (rows.moveToNext()) {
                String path = rows.getString(1);
                if (path.equals(oldPath) || path.startsWith(oldPath + "/")) {
                    ContentValues change = new ContentValues(); change.put("path", newPath + path.substring(oldPath.length()));
                    index.update("documents", change, "id=?", new String[]{rows.getString(0)});
                }
            }
            index.setTransactionSuccessful();
        } finally { index.endTransaction(); }
        return id;
    }
    synchronized void delete(String id) throws IOException {
        if (protectedDocument(id)) throw new IOException("Cannot delete the Society Files root");
        File file = document(id);
        String path = relative(file);
        deleteTree(file);
        try (Cursor rows = index.query("documents", new String[]{"id", "path"}, "drive=?", new String[]{identity}, null, null, null)) {
            while (rows.moveToNext())
                if (rows.getString(1).equals(path) || rows.getString(1).startsWith(path + "/"))
                    index.delete("documents", "id=?", new String[]{rows.getString(0)});
        }
    }
    private void deleteTree(File file) throws IOException {
        checkFile(file);
        if (file.isDirectory()) {
            File[] children = file.listFiles();
            if (children == null) throw new IOException("Cannot list Society directory");
            for (File child : children) deleteTree(child);
        }
        if (!file.delete()) throw new IOException("Cannot delete Society document");
    }
    synchronized String parentId(String id) throws IOException {
        if (rootId().equals(id)) throw new IOException("Society root has no public parent");
        return idFor(document(id).getParentFile());
    }
    synchronized boolean isChild(String parent, String child) throws IOException {
        return document(child).getAbsolutePath().startsWith(document(parent).getAbsolutePath() + "/");
    }
    synchronized List<String> documentPath(String parent, String child) throws IOException {
        File base = document(parent), current = document(child);
        if (!base.equals(current) && !isChild(parent, child)) throw new IOException("Document is outside the requested tree");
        LinkedList<String> path = new LinkedList<>();
        while (true) { path.addFirst(idFor(current)); if (current.equals(base)) return path; current = current.getParentFile(); }
    }
}
