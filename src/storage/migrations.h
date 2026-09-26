#ifndef GHM_STORAGE_MIGRATIONS_H
#define GHM_STORAGE_MIGRATIONS_H

#include <sqlite3.h>
#include <ghm/ghm.h>

int ghm_database_migrate(sqlite3 *db, GhmError *error);

#endif
