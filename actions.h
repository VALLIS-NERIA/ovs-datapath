#ifndef ACTIONS_H
#define ACTIONS_H 1

#include <linux/gfp.h>

struct datapath;
struct sk_buff;
struct odp_flow_key;
union odp_action;

struct sk_buff *make_writable(struct sk_buff *, gfp_t gfp);
int dp_xmit_skb(struct sk_buff *);
int execute_actions(struct datapath *dp, struct sk_buff *skb,
		    struct odp_flow_key *key,
		    const union odp_action *, int n_actions,
		    gfp_t gfp);

#endif /* actions.h */
