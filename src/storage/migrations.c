#include "storage/migrations.h"

#include <stdio.h>

int ghm_database_migrate(sqlite3 *db, GhmError *error)
{
    sqlite3_stmt *version_statement = NULL;
    int version;
    static const char initial_sql[] =
        "BEGIN;"
        "CREATE TABLE IF NOT EXISTS users ("
        "id INTEGER PRIMARY KEY, github_id INTEGER UNIQUE, login TEXT NOT NULL UNIQUE, display_name TEXT);"
        "CREATE TABLE IF NOT EXISTS repositories ("
        "id INTEGER PRIMARY KEY, name TEXT NOT NULL, path TEXT NOT NULL UNIQUE, "
        "github_id INTEGER UNIQUE, remote_url TEXT, last_opened_at INTEGER);"
        "CREATE TABLE IF NOT EXISTS branches ("
        "id INTEGER PRIMARY KEY, repository_id INTEGER NOT NULL REFERENCES repositories(id) ON DELETE CASCADE, "
        "name TEXT NOT NULL, last_seen_at INTEGER, UNIQUE(repository_id, name));"
        "CREATE TABLE IF NOT EXISTS scheduled_jobs ("
        "id INTEGER PRIMARY KEY, repository_id INTEGER NOT NULL REFERENCES repositories(id) ON DELETE CASCADE, "
        "branch TEXT NOT NULL, message TEXT NOT NULL, author_timestamp TEXT NOT NULL, "
        "committer_timestamp TEXT NOT NULL, execute_at INTEGER NOT NULL, "
        "push_after_commit INTEGER NOT NULL DEFAULT 0, status TEXT NOT NULL DEFAULT 'PENDING' "
        "CHECK(status IN ('PENDING','RUNNING','COMPLETED','FAILED','CANCELLED')), "
        "created_at INTEGER NOT NULL DEFAULT (unixepoch()), completed_at INTEGER, error_message TEXT);"
        "CREATE INDEX IF NOT EXISTS scheduled_jobs_due ON scheduled_jobs(status, execute_at);"
        "CREATE TABLE IF NOT EXISTS application_settings (key TEXT PRIMARY KEY, value TEXT NOT NULL);"
        "PRAGMA user_version = 1;"
        "COMMIT;";
    static const char scheduler_sql[] =
        "BEGIN;"
        "ALTER TABLE scheduled_jobs ADD COLUMN snapshot_ref TEXT;"
        "ALTER TABLE scheduled_jobs ADD COLUMN snapshot_tree_oid TEXT;"
        "ALTER TABLE scheduled_jobs ADD COLUMN base_parent_oid TEXT;"
        "ALTER TABLE scheduled_jobs ADD COLUMN predecessor_job_id INTEGER REFERENCES scheduled_jobs(id);"
        "ALTER TABLE scheduled_jobs ADD COLUMN result_oid TEXT;"
        "ALTER TABLE scheduled_jobs ADD COLUMN author_name TEXT;"
        "ALTER TABLE scheduled_jobs ADD COLUMN author_email TEXT;"
        "ALTER TABLE scheduled_jobs ADD COLUMN author_offset INTEGER;"
        "ALTER TABLE scheduled_jobs ADD COLUMN committer_name TEXT;"
        "ALTER TABLE scheduled_jobs ADD COLUMN committer_email TEXT;"
        "ALTER TABLE scheduled_jobs ADD COLUMN committer_offset INTEGER;"
        "CREATE INDEX scheduled_jobs_branch_order ON "
        "scheduled_jobs(repository_id,branch,status,execute_at);"
        "UPDATE scheduled_jobs SET status='FAILED', error_message='Legacy job has no frozen snapshot' "
        "WHERE status IN ('PENDING','RUNNING') AND snapshot_ref IS NULL;"
        "PRAGMA user_version = 2;"
        "COMMIT;";
    static const char push_sql[] =
        "BEGIN;"
        "ALTER TABLE scheduled_jobs ADD COLUMN push_status TEXT NOT NULL DEFAULT 'NONE' "
        "CHECK(push_status IN ('NONE','PENDING','RUNNING','COMPLETED','FAILED'));"
        "CREATE INDEX scheduled_jobs_push_due ON scheduled_jobs(push_status,id);"
        "PRAGMA user_version = 3;"
        "COMMIT;";
    static const char retry_sql[] =
        "BEGIN;"
        "ALTER TABLE scheduled_jobs ADD COLUMN push_retry_at INTEGER NOT NULL DEFAULT 0;"
        "ALTER TABLE scheduled_jobs ADD COLUMN push_attempts INTEGER NOT NULL DEFAULT 0;"
        "ALTER TABLE scheduled_jobs ADD COLUMN push_error_code INTEGER NOT NULL DEFAULT 0;"
        "CREATE INDEX scheduled_jobs_push_retry ON scheduled_jobs(push_status,push_retry_at);"
        "PRAGMA user_version = 4;"
        "COMMIT;";
    char *detail = NULL;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &version_statement, NULL) != SQLITE_OK ||
        sqlite3_step(version_statement) != SQLITE_ROW) {
        if (error != NULL) {
            error->code = GHM_ERROR_DATABASE;
            (void)snprintf(error->message, sizeof(error->message), "Read database version: %s", sqlite3_errmsg(db));
        }
        sqlite3_finalize(version_statement);
        return -1;
    }
    version = sqlite3_column_int(version_statement, 0);
    sqlite3_finalize(version_statement);
    if (version > 4 || version < 0) {
        if (error != NULL) {
            error->code = GHM_ERROR_DATABASE;
            (void)snprintf(error->message, sizeof(error->message), "Unsupported database version: %d", version);
        }
        return -1;
    }
    if (version == 0 && sqlite3_exec(db, initial_sql, NULL, NULL, &detail) != SQLITE_OK) {
        if (error != NULL) {
            error->code = GHM_ERROR_DATABASE;
            (void)snprintf(error->message, sizeof(error->message), "Database migration: %s",
                           detail != NULL ? detail : sqlite3_errmsg(db));
        }
        sqlite3_free(detail);
        (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    if (version == 0) version = 1;
    if (version == 1 && sqlite3_exec(db, scheduler_sql, NULL, NULL, &detail) != SQLITE_OK) {
        if (error != NULL) {
            error->code = GHM_ERROR_DATABASE;
            (void)snprintf(error->message, sizeof(error->message), "Scheduler migration: %s",
                           detail != NULL ? detail : sqlite3_errmsg(db));
        }
        sqlite3_free(detail);
        (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    if (version <= 2 && sqlite3_exec(db, push_sql, NULL, NULL, &detail) != SQLITE_OK) {
        if (error != NULL) {
            error->code = GHM_ERROR_DATABASE;
            (void)snprintf(error->message, sizeof(error->message), "Push migration: %s",
                           detail != NULL ? detail : sqlite3_errmsg(db));
        }
        sqlite3_free(detail);
        (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    if (version <= 3 && sqlite3_exec(db, retry_sql, NULL, NULL, &detail) != SQLITE_OK) {
        if (error != NULL) {
            error->code = GHM_ERROR_DATABASE;
            (void)snprintf(error->message, sizeof(error->message), "Retry migration: %s", detail != NULL ? detail : sqlite3_errmsg(db));
        }
        sqlite3_free(detail);
        (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    return 0;
}
