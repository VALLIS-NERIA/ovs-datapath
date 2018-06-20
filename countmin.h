#ifndef COUNTMIN_H
#define COUNTMIN_H
#include "flow_key.h"
struct countmin_sketch {
    size_t w;
    size_t d;
    uint32_t* masks;
    elemtype* data;
};


struct countmin_sketch* new_countmin_sketch(int w, int d);

elemtype countmin_sketch_update(struct countmin_sketch* this, struct flow_key* key, elemtype value);

elemtype countmin_sketch_query(struct countmin_sketch* this, struct flow_key* key);

void delete_countmin_sketch(struct countmin_sketch* this);

#endif
