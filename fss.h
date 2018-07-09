#ifndef FSS
#define FSS

#include "hashheap.h"


struct fss_sketch {
    size_t w;
    uint32_t mask;
    //struct flow_key* keys;
    elemtype* counters;
    int* hash_counters;
    struct hash_heap* heap;
};

struct fss_manager {
    size_t w;
    size_t d;
    size_t sw_count;
    struct fss_sketch** sketches;
};


struct fss_sketch* new_fss_sketch(int w);

void delete_fss_sketch(struct fss_sketch* this);

elemtype fss_sketch_query(struct fss_sketch* this, struct flow_key* key);

void fss_sketch_update(struct fss_sketch* this, struct flow_key* key, elemtype value);

///*              */



#ifndef NULL
//
//struct fss_manager* new_fss_manager(int w, int d, int sw_count) {
//    struct fss_manager* manager = new(struct fss_manager);
//    manager->w = w;
//    manager->d = d;
//    manager->sw_count = sw_count;
//    manager->sketches = newarr(struct fss_sketch, sw_count);
//    //kzalloc(sw_count * sizeof(struct fss_sketch*), GFP_ATOMIC);
//    int i = 0;
//    for (i = 0; i < sw_count; i++) {
//        manager->sketches[i] = new_fss_sketch(w);
//    }
//    return manager;
//}
//
//static void delete_fss_manager(struct fss_manager* this) {
//    int i = 0;
//    for (i = 0; i < this->sw_count; i++) {
//        delete_fss_sketch(this->sketches[i]);
//    }
//    kfree(this->sketches);
//}
//
//void fss_manager_update(struct fss_manager* this, int sw_id, struct flow_key* key, elemtype value) {
//    if (sw_id < 0 || sw_id >= this->sw_count) {
//        return;
//    }
//    fss_sketch_update(this->sketches[sw_id], key, value);
//}
//
//elemtype fss_manager_query(struct fss_manager* this, struct flow_key* key) {
//    elemtype max = 0;
//    int i = 0;
//    for (i = 0; i < this->sw_count; i++) {
//        elemtype q = fss_sketch_query(this->sketches[i], key);
//        if (q > max) {
//            max = q;
//        }
//    }
//    return max;
//}
static struct fss_line* new_fss_line(int w);
static void fss_line_update(struct fss_line* this, struct flow_key* key, elemtype value);
static elemtype fss_line_query(struct fss_line* this, struct flow_key* key);
static void delete_fss_line(struct fss_line* this);

static struct fss_sketch* new_fss_sketch(int w, int d);
static void fss_sketch_update(struct fss_sketch* this, struct flow_key* key, elemtype value);
static elemtype fss_sketch_query(struct fss_sketch* this, struct flow_key* key);
static void delete_fss_sketch(struct fss_sketch* this);

static struct fss_manager* new_fss_manager(int w, int d, int sw_count);
static void fss_manager_update(struct fss_manager* this, int sw_id, struct flow_key* key,
    elemtype value);
static elemtype fss_manager_query(struct fss_manager* this, struct flow_key* key);
static void delete_fss_manager(struct fss_manager* this);
#endif

#endif
