#ifndef SKETCH_MANAGE_H
#define SKETCH_MANAGE_H

#include "flow.h"
#include "flow_key.h"


int init_filter_sketch(int w, int d, int filter_threshold);
void clean_filter_sketch(void) ;
void my_label_sketch(char* dev_name, struct sk_buff* skb, struct sw_flow_key* key);

#endif