// android.database.sqlite.SQLiteOpenHelper -- subclassed by apps to
// manage a versioned SQLite schema. As of v1.0.102, opens a real
// file under Context.getDatabasePath(name) and runs the version-
// migration callbacks (onCreate / onUpgrade / onDowngrade /
// onConfigure / onOpen) just like AOSP does.
//
// AOSP mirror: frameworks/base/core/java/android/database/sqlite/
//   SQLiteOpenHelper.java. Their version dance is:
//     1. onConfigure(db)
//     2. if version == 0: onCreate(db); setVersion(targetVersion)
//        else if version < target: onUpgrade(db, old, new); setVersion
//        else if version > target: onDowngrade(db, old, new); setVersion
//     3. onOpen(db)
//   We mirror exactly, with the version stored in PRAGMA user_version.
//
// The whole point of this class for our scenario: WorkManager's
// WorkDatabase extends RoomDatabase -> SupportSQLiteOpenHelper which
// extends our SQLiteOpenHelper. Its onCreate creates the WorkSpec,
// Configuration, Preference, Dependency, etc. tables. Without those,
// every Room query against "WorkSpec" fails with "no such table"
// (or worse, "column 'id' does not exist" because the query joins
// nothing). Real onCreate behavior unblocks Room end-to-end.
package android.database.sqlite;

import android.content.Context;
import java.io.File;

// Implements AutoCloseable so try-with-resources (and AOSP's
// API 16+ contract) resolves -- some androidx helpers close the open
// helper via the AutoCloseable interface.
public abstract class SQLiteOpenHelper implements AutoCloseable {
    private final Context context;
    private final String  name;
    // NOT final since API 28: setOpenParams() may replace the cursor factory
    // and error handler before the database is first opened.
    private SQLiteDatabase.CursorFactory factory;
    private final int     newVersion;
    private android.database.DatabaseErrorHandler errorHandler;
    /** The OpenParams last supplied, applied when the database is opened. */
    private SQLiteDatabase.OpenParams openParams;

    /** Open db, lazily, the first time getWritableDatabase or
     *  getReadableDatabase is called. */
    private SQLiteDatabase database;
    private boolean writeAheadLoggingEnabled;
    private boolean initializing;

    public SQLiteOpenHelper(Context context, String name,
                             SQLiteDatabase.CursorFactory factory, int version) {
        this(context, name, factory, version, null);
    }
    public SQLiteOpenHelper(Context context, String name,
                             SQLiteDatabase.CursorFactory factory, int version,
                             android.database.DatabaseErrorHandler errorHandler) {
        if (version < 1) throw new IllegalArgumentException("version must be >= 1, was " + version);
        this.context      = context;
        this.name         = name;
        this.factory      = factory;
        this.newVersion   = version;
        this.errorHandler = errorHandler;
    }

    /** AOSP API 28 ctor taking an OpenParams bag. We pull the cursor
     *  factory, error handler, and WAL preference out of it; the other
     *  open options (lookaside, idle timeout) are no-ops on our backing
     *  but are carried so the surface resolves. */
    public SQLiteOpenHelper(Context context, String name, int version,
                             SQLiteDatabase.OpenParams openParams) {
        if (version < 1) throw new IllegalArgumentException("version must be >= 1, was " + version);
        this.context      = context;
        this.name         = name;
        this.factory      = (openParams != null) ? openParams.getCursorFactory() : null;
        this.newVersion   = version;
        this.errorHandler = (openParams != null) ? openParams.getErrorHandler() : null;
        this.openParams   = openParams;
    }

    /**
     * AOSP (API 28): "Sets configuration parameters that are used for opening
     * SQLiteDatabase. Please note that SQLiteDatabase.OpenParams.Builder#setOpenFlags(int)
     * is not supported by SQLiteOpenHelper."
     * @throws IllegalStateException if the database is already open.
     *
     * The throw is load-bearing rather than defensive: silently accepting
     * params after the open would leave the caller believing a journal mode or
     * cursor factory took effect when the live connection is still using the
     * old one.
     */
    public void setOpenParams(SQLiteDatabase.OpenParams openParams) {
        java.util.Objects.requireNonNull(openParams);
        synchronized (this) {
            if (database != null && database.isOpen()) {
                throw new IllegalStateException(
                        "OpenParams cannot be set after opening the database");
            }
            this.openParams = openParams;
            // AOSP's SQLiteOpenHelper reads the factory + error handler out of
            // the params at open time; carrying them into the fields now is the
            // same thing one step earlier, and keeps the two constructors and
            // this setter converging on one code path.
            if (openParams.getCursorFactory() != null) {
                this.factory = openParams.getCursorFactory();
            }
            if (openParams.getErrorHandler() != null) {
                this.errorHandler = openParams.getErrorHandler();
            }
        }
    }

    public synchronized SQLiteDatabase getWritableDatabase() {
        if (database != null && database.isOpen() && !database.isReadOnly()) return database;
        if (initializing) {
            throw new IllegalStateException("getWritableDatabase called recursively");
        }
        initializing = true;
        try {
            File path = (name == null || ":memory:".equals(name))
                ? null
                : (context != null ? context.getDatabasePath(name) : new File(name));
            String dbPath = path != null ? path.getAbsolutePath() : ":memory:";
            if (path != null && path.getParentFile() != null) {
                path.getParentFile().mkdirs();
            }
            database = SQLiteDatabase.openDatabase(dbPath, factory,
                SQLiteDatabase.OPEN_READWRITE | SQLiteDatabase.CREATE_IF_NECESSARY);
            if (openParams != null) {
                // Journal / synchronous mode from the params bag. AOSP applies
                // these through SQLiteDatabase's own open path; ours are
                // PRAGMAs on the freshly-opened connection, which is where
                // SQLite itself applies them either way.
                database.macapkApplyOpenParams(openParams);
            }
            if (writeAheadLoggingEnabled) {
                database.enableWriteAheadLogging();
            } else {
                database.disableWriteAheadLogging();
            }
            // ----- migration dance ------------------------------------
            onConfigure(database);
            int currentVersion = database.getVersion();
            if (currentVersion != newVersion) {
                if (database.isReadOnly()) {
                    throw new SQLiteException("Can't upgrade read-only db from v"
                        + currentVersion + " to v" + newVersion);
                }
                database.beginTransaction();
                try {
                    if (currentVersion == 0) {
                        onCreate(database);
                    } else if (currentVersion > newVersion) {
                        onDowngrade(database, currentVersion, newVersion);
                    } else {
                        onUpgrade(database, currentVersion, newVersion);
                    }
                    database.setVersion(newVersion);
                    database.setTransactionSuccessful();
                } finally {
                    database.endTransaction();
                }
            }
            onOpen(database);
            return database;
        } finally {
            initializing = false;
        }
    }
    public synchronized SQLiteDatabase getReadableDatabase() {
        // AOSP: if writable is already open, use it; else try readonly.
        // Simplest: always use writable. Room never asks for read-only.
        return getWritableDatabase();
    }

    public synchronized void close() {
        if (database != null) {
            database.close();
            database = null;
        }
    }
    public String getDatabaseName() { return name; }

    public synchronized void setWriteAheadLoggingEnabled(boolean enabled) {
        this.writeAheadLoggingEnabled = enabled;
        if (database != null && database.isOpen()) {
            if (enabled) database.enableWriteAheadLogging();
            else         database.disableWriteAheadLogging();
        }
    }
    public synchronized boolean isWriteAheadLoggingEnabled() {
        return writeAheadLoggingEnabled;
    }
    public synchronized void setIdleConnectionTimeout(long ms) {}
    public synchronized void setLookasideConfig(int slotSize, int slotCount) {}

    // ---- callbacks the subclass implements ------------------------
    public abstract void onCreate(SQLiteDatabase db);
    public abstract void onUpgrade(SQLiteDatabase db, int oldVersion, int newVersion);
    public void onDowngrade(SQLiteDatabase db, int oldVersion, int newVersion) {
        throw new SQLiteException("Can't downgrade database from v"
            + oldVersion + " to v" + newVersion);
    }
    public void onOpen(SQLiteDatabase db) {}
    public void onConfigure(SQLiteDatabase db) {}
}
