#ifndef MY_HASH_TABLE
#define MY_HASH_TABLE

#include "flow_key.h"

typedef uint32_t ht_value;
#define HT_ERR_KEY_NOT_FOUND -1
#define SUCCESS 0

struct htlist_node {
    struct flow_key key;
    ht_value value;
    struct htlist_node* next;
};

struct htlist_head {
    struct htlist_node* first;
};

struct hash_table {
    size_t slot_count;
    int bits;
    struct htlist_head** data;
};


struct hash_table* new_hash_table(size_t bits);

int ht_count = 0;

void hash_table_insert(struct hash_table* htable, struct flow_key* key, ht_value value);
int hash_table_get(struct hash_table* htable, struct flow_key* key, ht_value* value);
int hash_table_set(struct hash_table* htable, struct flow_key* key, ht_value value);
int hash_table_inc(struct hash_table* htable, struct flow_key* key, ht_value value);
int hash_table_remove(struct hash_table* htable, struct flow_key* key);

void delete_hash_table(struct hash_table* htable);

#endif
