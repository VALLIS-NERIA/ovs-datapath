#include "countsketch.h"

struct countsketch_line* new_countsketch_line(int w) {
    struct countsketch_line* line = new(struct countsketch_line);
    line->counters = newarr(elemtype, w);
    int i = 0;
    for (i = 0; i < w; i++) {
        line->counters[i] = 0;
    }
    //line->keys = (struct flow_key*)kzalloc(w * sizeof(struct flow_key), GFP_ATOMIC);
    line->w = w;
    uint32_t ran = rand_uint32();
    line->mask = ran;
    return line;
}

void delete_countsketch_line(struct countsketch_line* this) {
    kfree(this->counters);
    //kfree(this->keys);
    kfree(this);
}

void countsketch_line_update(struct countsketch_line* this, struct flow_key* key, elemtype value) {
    size_t index = ((uint32_t)flow_key_hash_old(key) ^ this->mask) % this->w;
    //struct flow_key* current_key = &(this->keys[index]);
    int sign = (flow_key_hash(key, 1) ^ this->mask) == 0 ? 1 : -1;
    this->counters[index] += sign * value;
}

elemtype countsketch_line_query(struct countsketch_line* this, struct flow_key* key) {
    size_t index = ((uint32_t)flow_key_hash_old(key) ^ this->mask) % this->w;
    int sign = (flow_key_hash(key, 1) ^ this->mask) == 0 ? 1 : -1;
    return this->counters[index] * sign;
}



struct countsketch_sketch* new_countsketch_sketch(int w, int d) {
    struct countsketch_sketch* sketch = new(struct countsketch_sketch);
    sketch->w = w;
    sketch->d = d;
    sketch->lines = newarr(struct countsketch_line*, d);
    sketch->heap = new_hash_heap(w);
    int i = 0;
    for (i = 0; i < d; i++) {
        sketch->lines[i] = new_countsketch_line(w);
    }
    return sketch;
}

void delete_countsketch_sketch(struct countsketch_sketch* this) {
    int i = 0;
    for (i = 0; i < this->d; i++) {
        delete_countsketch_line(this->lines[i]);
    }
    kfree(this->lines);
}

elemtype countsketch_sketch_forcequery(struct countsketch_sketch* this, struct flow_key* key) {
    elemtype* results = newarr(elemtype, this->d);
    int i = 0;
    for (i = 0; i < this->d; i++) {
        elemtype q = countsketch_line_query(this->lines[i], key);
        results[i] = q;
    }
    my_sort(results, this->d, sizeof(elemtype), cmpelem);
    if (this->d % 2 == 0) {
        return (results[this->d / 2] + results[this->d / 2 - 1]) / 2;
    }
    else {
        return results[(this->d - 1) / 2];
    }
}

elemtype countsketch_sketch_query(struct countsketch_sketch* this, struct flow_key* key) {
    elemtype tvalue;
    if (hash_table_get(this->heap->indexes, key, &tvalue) == HT_ERR_KEY_NOT_FOUND) {
        return 0;
    }
    return countsketch_sketch_forcequery(this, key);
}

void countsketch_sketch_update(struct countsketch_sketch* this, struct flow_key* key, elemtype value) {
    int i = 0;
    for (i = 0; i < this->d; i++) {
        countsketch_line_update(this->lines[i], key, value);
    }
    ht_value v_;
    int ret = hash_table_get(this->heap->indexes, key, &v_);
    // exist
    if (ret == SUCCESS) {
        hash_heap_inc(this->heap, key, value);
    }
    else if (ret == HT_ERR_KEY_NOT_FOUND) {
        // TODO
        elemtype v = countsketch_sketch_forcequery(this, key);
        if (this->heap->size < this->w) {
            hash_heap_insert(this->heap, key, v);
        }
        else {
            elemtype min = hash_heap_peek(this->heap);
            if (min < v) {
                hash_heap_extract(this->heap);
                hash_heap_insert(this->heap, key, v);
            }
        }
    }
}
