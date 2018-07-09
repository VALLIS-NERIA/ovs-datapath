#ifndef MY_HASH_HEAP
#define MY_HASH_HEAP

#include "hashtable.h"
//#include <math.h>
#ifndef SUCCESS
#define SUCCESS 0
#endif

#define HEAP_EXCCED -1
#define HEAP_UNINTAILIZED -2

#define LCHILD(x) 2 * x + 1
#define RCHILD(x) 2 * x + 2
#define PARENT(x) (x - 1) / 2

typedef elemtype heap_data;

struct node {
    struct flow_key* key;
    heap_data data;
};

struct hash_heap {
    struct flow_key* keys;
    struct hash_table* indexes;
    int size;
    int max_size;
    struct node* elem;
};


/*
Function to initialize the min heap with size = 0
*/
struct hash_heap* new_hash_heap(int max_size);

int hash_heap_insert(struct hash_heap* this, struct flow_key* key, heap_data data);

heap_data inline hash_heap_peek(struct hash_heap* this) {
    if (this->size) {
        int last = (this->size) - 1;
        return this->elem[last].data;
    }
    return 0;
}

inline struct flow_key* hash_heap_peek_key(struct hash_heap* this) {
    if (this->size) {
        int last = (this->size) - 1;
        return this->elem[last].key;
    }
    return NULL;
}

void hash_heap_extract(struct hash_heap* this);
int hash_heap_inc(struct hash_heap* this, struct flow_key* key, heap_data value);
void delete_hash_heap(struct hash_heap* this);

#endif
