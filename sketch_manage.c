#include <linux/ip.h>
#include <linux/skbuff.h>
#include <linux/types.h>
#include "countmax.h"
#include "countmin.h"
#include "flow.h"
#include "flow_key.h"
#include "sketch_manage.h"
enum switch_type {
    filter = 1,
    egress = 2
};

static enum switch_type switch_types[] = {filter, filter, egress, egress, egress, egress};
#define SW_COUNT (sizeof(switch_types) / sizeof(enum switch_type))
static struct countmax_sketch* countmax[SW_COUNT];
static struct countmin_sketch* countmin[SW_COUNT];

static int threshold;

int init_filter_sketch(int w, int d, int filter_threshold) {
    int i;
    void* err;
    threshold = filter_threshold;
    for (i = 0; i < SW_COUNT; ++i) {
        switch (switch_types[i]) {
            case filter:
                countmin[i] = new_countmin_sketch(w, d);
                if (!countmin[i]) return -1;
                break;
            case egress:
                countmax[i] = new_countmax_sketch(w, d);
                if (!countmax[i]) return -1;
                break;
        }
    }
}

static void extract_5_tuple(struct sw_flow_key* key, struct flow_key* tuple) {
    tuple->srcport = key->tp.src;
    tuple->dstport = key->tp.dst;
    tuple->srcip = key->ipv4.addr.src;
    tuple->dstip = key->ipv4.addr.dst;
    tuple->protocol = key->ip.proto;
}

void my_label_sketch(char* dev_name, struct sk_buff* skb, struct sw_flow_key* key) {
    if (dev_name[0] != 's') return;
    int dev_id = dev_name[1] - '0';
    if (dev_id < 0 || dev_id > 9 || dev_id > SW_COUNT) return;

    struct flow_key tuple;
    extract_5_tuple(key, &tuple);
    struct iphdr* ip_header = (struct iphdr*)skb_network_header(skb);
    uint32_t tos = ip_header->tos;
    if (switch_types[dev_id] == filter) {
        if ((tos & 3) == 0) {
            elemtype res = countmin_sketch_update(countmin[dev_id], &tuple, skb->len);
            if (res + skb->len > threshold)
                ip_header->tos = ip_header->tos | 1;  // put tag
        }
    } else if (switch_types[dev_id] == egress) {
        if ((tos & 3) != 0) {
            countmax_sketch_update(countmax[dev_id], &tuple, skb->len);
        }
    }
}
#undef SW_COUNT