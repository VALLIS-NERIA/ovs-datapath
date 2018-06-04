#ifndef SKETCH_REPORT_H
#define SKETCH_REPORT_H
#include "flow_key.h"
int sketch_report_init(void* sketch, elemtype(*query)(void* this, struct flow_key* key));
int sketch_report_clean(void);
#endif