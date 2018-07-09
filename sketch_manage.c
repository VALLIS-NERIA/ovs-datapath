#include "sketch_manage.h"
#include <linux/in.h>
#include <linux/inet.h>
#include <linux/init.h>
#include <linux/ip.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/skbuff.h>
#include <linux/socket.h>
#include <linux/string.h>
#include <linux/types.h>
#include <net/sock.h>
#include "countmax.h"
#include "countmin.h"
#include "flow.h"
#include "flow_key.h"
enum switch_type {
    filter = 1,
    ingress = 2,
    egress = 3
};

static enum switch_type switch_types[] = {filter, filter, ingress, ingress, egress, egress};
#define SW_COUNT (sizeof(switch_types) / sizeof(enum switch_type))
static struct countmax_sketch* countmax[SW_COUNT];
static struct countmin_sketch* countmin[SW_COUNT];

static int threshold;

#define MAX_BATCH 1024
static const unsigned short listen_port = 0x8888;
static const char* magic_command = "lol233hhh";
static const char* magic_finish = "finish";
/*****sketch update*****/

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
    uint32_t hash;

    //printk("on dev %s ", dev_name);
    if (dev_name[0] != 's') return;
    dev_id = dev_name[1] - '0';
    if (dev_id < 0 || dev_id > 9 || dev_id > SW_COUNT) return;
    //printk("(dev_id=%d): ", dev_id);

    extract_5_tuple(key, &tuple);
    hash = flow_key_hash(&tuple, 16);
    if (switch_types[dev_id] == egress) {
        //printk("update!\n");        
        //countmax_sketch_query(countmax[dev_id], &tuple);
        if (hash & 1) {
            countmax_sketch_update(countmax[dev_id], &tuple, 1);
            //countmax_sketch_update(countmax[dev_id], &tuple, skb->len);
        }
    }
    else if(switch_types[dev_id] == ingress){
        if (!(hash & 1)) {
            countmax_sketch_update(countmax[dev_id], &tuple, 1);
            //countmax_sketch_update(countmax[dev_id], &tuple, skb->len);
        }
    }
}

/*****sketch query*****/
struct countmax_entry {
    struct flow_key key;
    elemtype value;
};
void print_ip(uint32_t ip) {
    printk("%d.%d.%d.%d", (ip) % 0x100, (ip / 0x100) % 0x100, (ip / 0x10000) % 0x100, ip / 0x1000000);
}
static int get_stat_and_send_back(struct socket* client_sock) {
    int i, j, k;
    int count;
    struct countmax_entry* sendbuf;
    int buf_p;

    struct kvec vec;
    struct msghdr msg;

    struct flow_key key;
    elemtype value;
    count = 0;
    for (i = 0; i < SW_COUNT; ++i) {
        if (switch_types[i] == egress) {
            count = max(countmax[i]->w * countmax[i]->d, count);
        }
    }
    sendbuf = kzalloc(count * sizeof(struct countmax_entry), GFP_KERNEL);
    printk("querying...\n");
    for (i = 0; i < SW_COUNT; ++i) {
        if (switch_types[i] != egress) continue;

        buf_p = 0;
        printk("countmax #%d: w = %u, d = %u\n", i, countmax[i]->w, countmax[i]->d);
        for (j = 0; j < countmax[i]->d; ++j) {
            for (k = 0; k < countmax[i]->w; ++k) {
                key = countmax[i]->lines[j]->keys[k];
                value = countmax[i]->lines[j]->counters[k];
                if (value == 0) continue;
                sendbuf[buf_p].key = key;
                sendbuf[buf_p].value = value;
                print_ip(key.srcip);
                printk("->");
                print_ip(key.dstip);
                printk(": %llu\n", value);
                ++buf_p;
            }
        }
        printk("\nswitch #%d: %d get.\n", i, buf_p);

        // send back
        //printk("query finished, sending... #%d\n", i);
        memset(&vec, 0, sizeof(vec));
        memset(&msg, 0, sizeof(msg));
        vec.iov_base = sendbuf;
        vec.iov_len = buf_p * sizeof(struct countmax_entry);

        kernel_sendmsg(client_sock, &msg, &vec, 1, buf_p * sizeof(struct countmax_entry));
    }

    vec.iov_base = magic_finish;
    vec.iov_len = strlen(magic_finish) + 1;

    kernel_sendmsg(client_sock, &msg, &vec, 1, strlen(magic_finish) + 1);

    kfree(sendbuf);
    return 0;
}

static int sketch_report_listen(void* arg) {
    const unsigned buf_size = MAX_BATCH * sizeof(struct flow_key);
    struct socket *sock, *client_sock;
    struct sockaddr_in s_addr;
    int ret = 0;

    char* recvbuf;

    struct kvec vec;
    struct msghdr msg;

    memset(&s_addr, 0, sizeof(s_addr));
    s_addr.sin_family = AF_INET;
    s_addr.sin_port = htons(listen_port);
    s_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    //sock = (struct socket *)kmalloc(sizeof(struct socket), GFP_KERNEL);
    //client_sock = (struct socket *)kmalloc(sizeof(struct socket), GFP_KERNEL);

    /*create a socket*/
    ret = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, 0, &sock);
    if (ret) {
        printk("server:socket_create error!\n");
    }
    printk("server:socket_create ok!\n");

    /*bind the socket*/
    ret = kernel_bind(sock, (struct sockaddr*)&s_addr, sizeof(struct sockaddr_in));
    if (ret < 0) {
        printk("server: bind error\n");
        return ret;
    }
    printk("server:bind ok!\n");

    /*listen*/
    ret = kernel_listen(sock, 10);
    if (ret < 0) {
        printk("server: listen error\n");
        return ret;
    }
    printk("server:listen ok!\n");
    /*kmalloc a receive buffer*/
    recvbuf = kzalloc(buf_size, GFP_KERNEL);
    if (recvbuf == NULL) {
        printk("server: recvbuf kmalloc error!\n");
        return -1;
    }
    memset(recvbuf, 0, sizeof(recvbuf));

    //set_current_state(TASK_INTERRUPTIBLE);
    while (!kthread_should_stop()) {
        ret = kernel_accept(sock, &client_sock, O_NONBLOCK);
        if (ret < 0) {
            if (ret == -EAGAIN) {
                usleep_range(5000, 10000);
                schedule();
                continue;
            } else {
                printk("server:accept error!\n");
                return ret;
            }
        }
        printk("server: accept ok, Connection Established\n");

        /*receive message from client*/

        memset(&vec, 0, sizeof(vec));
        memset(&msg, 0, sizeof(msg));
        vec.iov_base = recvbuf;
        vec.iov_len = buf_size;
        ret = kernel_recvmsg(client_sock, &msg, &vec, 1, buf_size, 0); /*receive message*/
        printk("receive message, length: %d\n", ret);
        if (strcmp(recvbuf, magic_command) == 0) {
            get_stat_and_send_back(client_sock);
        }
        /*release socket*/
        sock_release(client_sock);
    }
    printk("listen thread exiting.\n");
    sock_release(sock);
    kfree(recvbuf);
    return ret;
}

static struct task_struct* listen_thread;

int sketch_report_init(void) {
    listen_thread = kthread_create(sketch_report_listen, NULL, "listen thread");
    wake_up_process(listen_thread);
    return 0;
}

int sketch_report_clean(void) {
    kthread_stop(listen_thread);
    return 0;
}

// deprecated
#ifndef NULL 
void my_label_sketch(char* dev_name, struct sk_buff* skb, struct sw_flow_key* key) {
    int dev_id;
    struct flow_key tuple;
    struct iphdr* ip_header;
    uint32_t tos;

    //printk("on dev %s ", dev_name);
    if (dev_name[0] != 's') return;
    dev_id = dev_name[1] - '0';
    if (dev_id < 0 || dev_id > 9 || dev_id > SW_COUNT) return;
    //printk("(dev_id=%d): ", dev_id);

    extract_5_tuple(key, &tuple);
    ip_header = (struct iphdr*)skb_network_header(skb);
    tos = ip_header->tos;
    //printk("tos = %u\n", tos);
    if (switch_types[dev_id] == filter) {
        if ((tos & 0x80) == 0) {
            elemtype res = countmin_sketch_update(countmin[dev_id], &tuple, 1);
            if (res < threshold) {
                ip_header->tos = ip_header->tos | 0x80;  // put tag
                // REMEMBER TO CALCULATE THE CHECKSUM!!!!!
                ip_send_check(ip_header);
            }
        }
    } else if (switch_types[dev_id] == egress) {
        //printk("update!\n");        
        //countmax_sketch_query(countmax[dev_id], &tuple);
        if ((tos & 0x80) == 0) {
            countmax_sketch_update(countmax[dev_id], &tuple, 1);
            //countmax_sketch_update(countmax[dev_id], &tuple, skb->len);
        }
    }
}

#endif
#undef SW_COUNT