#ifndef Q2_SQLITE_H
#define Q2_SQLITE_H
#include "q2/kernel.h"
/** @file sqlite.h Optional in-memory SQLite KV component. Persistent VFS is
 * intentionally separate. */
typedef struct q2_sqlite q2_sqlite_t;
/** Create an unstarted component; database is opened by its start hook. */
q2_result_t q2_sqlite_create(q2_sqlite_t **out_database);
/** Obtain descriptor; must outlive kernel registration. */
q2_result_t q2_sqlite_component(q2_sqlite_t *database, const q2_component_t **out_component);
/** Destroy after kernel stop; refuses while database is open. */
q2_result_t q2_sqlite_destroy(q2_sqlite_t **database);
#endif
