#include "sketch_manage.h"
#include <linux/ip.h>
#include <linux/skbuff.h>
#include <linux/types.h>
#include "countmax.h"
#include "countmin.h"
#include "flow.h"
#include "flow_key.h"
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
    printk("filter_sketch initing!\n");
    threshold = filter_threshold;
    for (i = 0; i < SW_COUNT; ++i) {
        switch (switch_types[i]) {
            case filter:
                countmin[i] = new_countmin_sketch(w, d);
                if (!countmin[i]) return -1;
                printk("s%d: countmin\n", i);
                break;
            case egress:
                countmax[i] = new_countmax_sketch(w, d);
                if (!countmax[i]) return -1;
                break;
        }
    }
    printk("filter_sketch inited!\n");
    return 0;
}

void clean_filter_sketch(void) {
    int i;
    for (i = 0; i < SW_COUNT; ++i) {
        switch (switch_types[i]) {
            case filter:
                if (countmin[i]) delete_countmin_sketch(countmin[i]);
                break;
            case egress:
                if (countmax[i]) delete_countmax_sketch(countmax[i]);
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
    int dev_id;
    struct flow_key tuple;
    struct iphdr* ip_header;
    uint32_t tos;

    printk("on dev %s ", dev_name);
    if (dev_name[0] != 's') return;
    dev_id = dev_name[1] - '0';
    if (dev_id < 0 || dev_id > 9 || dev_id > SW_COUNT) return;
    printk("(dev_id=%d): ", dev_id);

    extract_5_tuple(key, &tuple);
    ip_header = (struct iphdr*)skb_network_header(skb);
    tos = ip_header->tos;
    printk("tos = %u\n", tos);
    if (switch_types[dev_id] == filter) {
        if ((tos & 3) == 0) {
            elemtype res = countmin_sketch_update(countmin[dev_id], &tuple, skb->len);
            //if (res + skb->len > threshold)
                ip_header->tos = ip_header->tos | 0x80;  // put tag
                // REMEMBER TO CALCULATE THE CHECKSUM!!!!!
                ip_send_check(ip_header);
        }
    } else if (switch_types[dev_id] == egress) {
        if ((tos & 3) != 0) {
            //countmax_sketch_update(countmax[dev_id], &tuple, skb->len);
        }
    }
}
#undef SW_COUNT