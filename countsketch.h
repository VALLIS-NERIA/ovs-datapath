#ifndef COUNTSKETCH
#define COUNTSKETCH
#include "hashheap.h"



struct countsketch_line {
    size_t w;
    uint32_t mask;
    //struct flow_key* keys;
    elemtype* counters;
};

struct countsketch_sketch {
    size_t w;
    size_t d;
    struct countsketch_line** lines;
    struct hash_heap* heap;
};


/*              */

struct countsketch_sketch* new_countsketch_sketch(int w, int d);

void delete_countsketch_sketch(struct countsketch_sketch* this);

elemtype countsketch_sketch_query(struct countsketch_sketch* this, struct flow_key* key);

void countsketch_sketch_update(struct countsketch_sketch* this, struct flow_key* key, elemtype value);



#ifndef NULL
///*              */

//struct countsketch_manager {
//    size_t w;
//    size_t d;
//    size_t sw_count;
//    struct countsketch_sketch** sketches;
//};
//
//struct countsketch_manager* new_countsketch_manager(int w, int d, int sw_count) {
//    struct countsketch_manager* manager = new(struct countsketch_manager);
//    manager->w = w;
//    manager->d = d;
//    manager->sw_count = sw_count;
//    manager->sketches = newarr(struct countsketch_sketch, sw_count);
//    //kzalloc(sw_count * sizeof(struct countsketch_sketch*), GFP_ATOMIC);
//    int i = 0;
//    for (i = 0; i < sw_count; i++) {
//        manager->sketches[i] = new_countsketch_sketch(w, d);
//    }
//    return manager;
//}
//
//static void delete_countsketch_manager(struct countsketch_manager* this) {
//    int i = 0;
//    for (i = 0; i < this->sw_count; i++) {
//        delete_countsketch_sketch(this->sketches[i]);
//    }
//    kfree(this->sketches);
//}
//
//void countsketch_manager_update(struct countsketch_manager* this, int sw_id, struct flow_key* key, elemtype value) {
//    if (sw_id < 0 || sw_id >= this->sw_count) {
//        return;
//    }
//    countsketch_sketch_update(this->sketches[sw_id], key, value);
//}
//
//elemtype countsketch_manager_query(struct countsketch_manager* this, struct flow_key* key) {
//    elemtype max = 0;
//    int i = 0;
//    for (i = 0; i < this->sw_count; i++) {
//        elemtype q = countsketch_sketch_query(this->sketches[i], key);
//        if (q > max) {
//            max = q;
//        }
//    }
//    return max;
//}


static struct countsketch_line* new_countsketch_line(int w);
static void countsketch_line_update(struct countsketch_line* this, struct flow_key* key, elemtype value);
static elemtype countsketch_line_query(struct countsketch_line* this, struct flow_key* key);
static void delete_countsketch_line(struct countsketch_line* this);

static struct countsketch_sketch* new_countsketch_sketch(int w, int d);
static void countsketch_sketch_update(struct countsketch_sketch* this, struct flow_key* key, elemtype value);
static elemtype countsketch_sketch_query(struct countsketch_sketch* this, struct flow_key* key);
static void delete_countsketch_sketch(struct countsketch_sketch* this);

static struct countsketch_manager* new_countsketch_manager(int w, int d, int sw_count);
static void countsketch_manager_update(struct countsketch_manager* this, int sw_id, struct flow_key* key,
    elemtype value);
static elemtype countsketch_manager_query(struct countsketch_manager* this, struct flow_key* key);
static void delete_countsketch_manager(struct countsketch_manager* this);
#endif
#endif
