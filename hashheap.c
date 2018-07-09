#include "hashheap.h"

struct hash_heap* new_hash_heap(int max_size) {
    struct hash_heap* this = new(struct hash_heap);
    this->keys = newarr(struct flow_key, max_size);
    //this->indexes = new_hash_table(3);
    this->indexes = new_hash_table(log2(max_size) + 0);
    this->size = 0;
    this->max_size = max_size - 1;
    this->elem = newarr(struct node, max_size);
    return this;
}

static void hash_heap_swap(struct hash_heap* this, int index1, int index2) {
    struct node tmp = this->elem[index1];
    this->elem[index1] = this->elem[index2];
    this->elem[index2] = tmp;
    //ht_value tmpv;
    hash_table_set(this->indexes, this->elem[index1].key, index1);
    hash_table_set(this->indexes, this->elem[index2].key, index2);
}

/*
Heapify function is used to make sure that the heap property is never violated
In case of deletion of a struct node, or creating a min heap from an array, heap property
may be violated. In such cases, hash_heap_heapify function can be called to make sure that
heap property is never violated
*/
static void hash_heap_heapify(struct hash_heap* this, int i) {
    int smallest = (LCHILD(i) < this->size && this->elem[LCHILD(i)].data < this->elem[i].data) ? LCHILD(i) : i;
    if (RCHILD(i) < this->size && this->elem[RCHILD(i)].data < this->elem[smallest].data) {
        smallest = RCHILD(i);
    }
    if (smallest != i) {
        hash_heap_swap(this, i, smallest);
        hash_heap_heapify(this, smallest);
    }
}


/*
Function to insert a struct node into the min heap, by allocating space for that struct node in the
heap and also making sure that the heap property and shape propety are never violated.
*/
int hash_heap_insert(struct hash_heap* this, struct flow_key* key, heap_data data) {

    if (!this->elem)return HEAP_UNINTAILIZED;
    if (this->size == this->max_size)return HEAP_EXCCED;

    // copy the key
    struct flow_key* t_key = new(struct flow_key);
    *t_key = *key;

    struct node nd;
    nd.key = t_key;
    nd.data = data;

    int i = (this->size)++;
    while (i && nd.data < this->elem[PARENT(i)].data) {
        this->elem[i] = this->elem[PARENT(i)];
        i = PARENT(i);
    }
    this->elem[i] = nd;

    // insert to hashtable
    hash_table_insert(this->indexes, t_key, i);
    return SUCCESS;
}





/*
Function to delete a struct node from the min heap
It shall remove the root struct node, and place the last struct node in its place
and then call hash_heap_heapify function to make sure that the heap property
is never violated
*/
void hash_heap_extract(struct hash_heap* this) {
    if (this->size) {
        //printf("Deleting struct node %d\n\n", this->elem[0].data);
        int last = --(this->size);
        this->elem[0] = this->elem[last];
        hash_table_remove(this->indexes, this->elem[last].key);
        hash_heap_heapify(this, 0);
    }
}

int hash_heap_update_or_insert(struct hash_heap* this, struct flow_key* key, heap_data value) {
    uint32_t index;
    int ret = hash_table_get(this->indexes, key, &index);
    if (ret == SUCCESS) {
        this->elem[index].data = value;
        hash_heap_heapify(this, 0);
        return SUCCESS;
    }
    else if (ret == HT_ERR_KEY_NOT_FOUND) {
        return hash_heap_insert(this, key, value);
    }
    return -255;
}

int hash_heap_inc(struct hash_heap* this, struct flow_key* key, heap_data value) {
    uint32_t index;
    int ret = hash_table_get(this->indexes, key, &index);
    if (ret == SUCCESS) {
        this->elem[index].data += value;
        hash_heap_heapify(this, 0);
        return SUCCESS;
    }
    else if (ret == HT_ERR_KEY_NOT_FOUND) {
        return hash_heap_insert(this, key, value);
    }
    return -255;
}


/*
Function to clear the memory allocated for the min heap
*/
void delete_hash_heap(struct hash_heap* this) {
    delete_hash_table(this->indexes);
    kfree(this->elem);
    kfree(this);
}
