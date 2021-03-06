#define DB_RDWR 0
#define DB_RDONLY 0x20000

#define DB_NOOVERWRITE 0x10 /* May be expensive for LSM-tree back-ends */

#define DB_SUCCESS 0
#define DB_NOTFOUND (-30798)
#define DB_KEYEXIST (-30799)
#define DB_EINVAL EINVAL
#define DB_EACCES EACCES

typedef struct {
	size_t size;
	void *data;
} DB_val;

typedef struct DB_env DB_env;
typedef struct DB_txn DB_txn;
typedef struct DB_cursor DB_cursor;

int db_env_create(DB_env **const out);
int db_env_set_mapsize(DB_env *const env, size_t const size);
int db_env_open(DB_env *const env, char const *const name, unsigned const flags, unsigned const mode);
void db_env_close(DB_env *const env);

int db_txn_begin(DB_env *const env, DB_txn *const parent, unsigned const flags, DB_txn **const out);
int db_txn_commit(DB_txn *const txn);
void db_txn_abort(DB_txn *const txn);
int db_txn_reset(DB_txn *const txn);
int db_txn_renew(DB_txn *const txn);
int db_txn_get_flags(DB_txn *const txn, unsigned *const flags);
int db_txn_cursor(DB_txn *const txn, DB_cursor **const out);

int db_cursor_open(DB_txn *const txn, DB_cursor **const out);
void db_cursor_close(DB_cursor *const cursor);
int db_cursor_reset(DB_cursor *const cursor);
int db_cursor_renew(DB_txn *const txn, DB_cursor **const out);

int db_cursor_current(DB_cursor *const cursor, DB_val *const key, DB_val *const data);
int db_cursor_seek(DB_cursor *const cursor, DB_val *const key, DB_val *const data, int const dir);
int db_cursor_first(DB_cursor *const cursor, DB_val *const key, DB_val *const data, int const dir);
int db_cursor_next(DB_cursor *const cursor, DB_val *const key, DB_val *const data, int const dir);

int db_cursor_put(DB_cursor *const cursor, DB_val const *const key, DB_val const *const data, unsigned const flags);
int db_cursor_del(DB_cursor *const cursor);

int db_cmp(DB_txn *const txn, DB_val const *const a, DB_val const *const b);

