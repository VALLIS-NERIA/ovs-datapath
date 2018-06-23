#include "countmin.h"
#include "sketch_util.h"
//typedef long long elemtype;

struct countmin_sketch* new_countmin_sketch(int w, int d) {
    struct countmin_sketch* this = new (struct countmin_sketch);
    this->w = w;
    this->d = d;
    this->masks = newarr(uint32_t, d);
    int i;
    for (i = 0; i < this->d; ++i) {
        this->masks[i] = rand_uint32;
    }
    this->data = newarr(elemtype, w * d);
    return this;
}

elemtype countmin_sketch_update(struct countmin_sketch* this, struct flow_key* key, elemtype value) {
    if (!this) {
        printk("this is NULL!\n");
        return -1;
    }
    // I don't think you'll make a sketch longer than 65536
    uint32_t hash = flow_key_hash(key, 16);
    int i = 0;
    elemtype ret = -1;
    for (i = 0; i < this->d; ++i) {
        uint32_t index = (hash ^ this->masks[i]) % this->w;
        uint32_t long_index = this->w * i + index;
        if (long_index >= this->w * this->d) {
            printk("overflow! index = %u, i = %u\n", index, i);
            return -1;
        }
        elemtype candidate = this->data[long_index];
        if (candidate < ret || ret < 0) {
            ret = candidate;
        }
        this->data[long_index] += value;
    }
    return ret + value;
}

elemtype countmin_sketch_query(struct countmin_sketch* this, struct flow_key* key) {
    elemtype ret = -1;
    uint32_t hash = flow_key_hash(key, 16);
    int i = 0;
    for (i = 0; i < this->d; ++i) {
        uint32_t index = (hash ^ this->masks[i]) % this->w;
        uint32_t long_index = this->w * i + index;
        elemtype candidate = this->data[long_index];
        if (candidate < ret || ret < 0) {
            ret = candidate;
        }
    }
    return ret;
}

void delete_countmin_sketch(struct countmin_sketch* this) {
    kfree(this->masks);
    kfree(this->data);
    kfree(this);
}