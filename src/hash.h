#include <stdint.h>

#define HASH_NOTFOUND (SIZE_MAX-1)

extern uint32_t hash_salt; /* Set this first */

typedef struct {
	size_t count;
	size_t keylen;
	char *keys;
} hash_t;

int hash_init(hash_t *const hash, size_t const count, size_t const keylen);
void hash_destroy(hash_t *const hash);

/* Returns index of key (bring your own payload storage) */
size_t hash_get(hash_t *const hash, char const *const key);
size_t hash_set(hash_t *const hash, char const *const key);

/* Updates external data array for you (elements must be fixed size) */
size_t hash_del(hash_t *const hash, char const *const key, void *const values, size_t const len);
void hash_del_offset(hash_t *const hash, size_t const x, void *const values, size_t const len);

/* Low level functions */
size_t hash_func(hash_t *const hash, char const *const key);
int hash_bucket_empty(hash_t *const hash, size_t const x);
int hash_bucket_match(hash_t *const hash, size_t const x, char const *const key);
void hash_set_raw(hash_t *const hash, size_t const x, char const *const key);
size_t hash_del_keyonly(hash_t *const hash, size_t const x);

