#ifndef FLOW_KEY_H
#define FLOW_KEY_H 1

#include "sketch_util.h"
#define FIVE_TUPLE
struct flow_key {
    uint32_t srcip;
    uint32_t dstip;
#ifdef FIVE_TUPLE
    union {
        struct {
            uint16_t srcport;
            uint16_t dstport;
        };
        uint32_t port;
    };
    uint16_t protocol;
#endif
};

static struct flow_key empty_key;

static inline struct flow_key rand_flow_key(void) {
    struct flow_key key;
    key.srcip = rand_uint32();
    key.dstip = rand_uint32();
#ifdef FIVE_TUPLE
    key.srcport = rand_uint16();
    key.dstport = rand_uint16();
    key.protocol = rand_uint16();
#endif
    return key;
}


static inline uint32_t flow_key_hash_old(struct flow_key* key) {
    int hashCode = (int)key->srcip;
    hashCode = (hashCode * 397) ^ (int)key->dstip;
#ifdef FIVE_TUPLE
    hashCode = (hashCode * 397) ^ (int)key->port;
    //hashCode = (hashCode * 397) ^ (int)key->srcport;
    //hashCode = (hashCode * 397) ^ (int)key->dstport;
    //hashCode = (hashCode * 397) ^ (int)key->protocol;
#endif
    return (uint32_t)hashCode;
}

static inline uint32_t flow_key_hash(struct flow_key* key, uint32_t bits) {
    uint32_t hash = sketch_hash_32(key->srcip, bits);
    hash ^= sketch_hash_32(key->dstip, bits);
#ifdef FIVE_TUPLE
    hash ^= sketch_hash_32(key->port, bits);
    //hash ^= sketch_hash_32(key->srcport, bits);
    //hash ^= sketch_hash_32(key->dstport, bits);
    //hash ^= sketch_hash_32(key->protocol, bits);
#endif
    return hash;
}

static inline int flow_key_equal(struct flow_key* lhs, struct flow_key* rhs) {
#ifdef FIVE_TUPLE
    return lhs->srcip == rhs->srcip && lhs->dstip == rhs->dstip && lhs->srcport == rhs->srcport &&
        lhs->dstport == rhs->dstport && lhs->protocol == rhs->protocol;
#else
    return lhs->srcip == rhs->srcip && lhs->dstip == rhs->dstip;
#endif
}

#endif