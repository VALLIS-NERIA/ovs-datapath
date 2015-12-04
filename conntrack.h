/*
 * Copyright (c) 2015 Nicira, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 */

#ifndef OVS_CONNTRACK_H
#define OVS_CONNTRACK_H 1

#include <linux/version.h>
#include "flow.h"

struct ovs_conntrack_info;
enum ovs_key_attr;

#if IS_ENABLED(CONFIG_NF_CONNTRACK) && LINUX_VERSION_CODE > KERNEL_VERSION(3,9,0)
bool ovs_ct_verify(enum ovs_key_attr attr);
int ovs_ct_copy_action(struct net *, const struct nlattr *,
		       const struct sw_flow_key *, struct sw_flow_actions **,
		       bool log);
int ovs_ct_action_to_attr(const struct ovs_conntrack_info *, struct sk_buff *);

int ovs_ct_execute(struct net *, struct sk_buff *, struct sw_flow_key *,
		   const struct ovs_conntrack_info *);

void ovs_ct_fill_key(const struct sk_buff *skb, struct sw_flow_key *key);
int ovs_ct_put_key(const struct sw_flow_key *key, struct sk_buff *skb);
void ovs_ct_free_action(const struct nlattr *a);
#else
#include <linux/errno.h>

static inline bool ovs_ct_verify(int attr)
{
	return false;
}

static inline int ovs_ct_copy_action(struct net *net, const struct nlattr *nla,
				     const struct sw_flow_key *key,
				     struct sw_flow_actions **acts, bool log)
{
	return -ENOTSUPP;
}

static inline int ovs_ct_action_to_attr(const struct ovs_conntrack_info *info,
					struct sk_buff *skb)
{
	return -ENOTSUPP;
}

static inline int ovs_ct_execute(struct net *net, struct sk_buff *skb,
				 struct sw_flow_key *key,
				 const struct ovs_conntrack_info *info)
{
	return -ENOTSUPP;
}

static inline void ovs_ct_fill_key(const struct sk_buff *skb,
				   struct sw_flow_key *key)
{
	key->ct.state = 0;
	key->ct.zone = 0;
}

static inline int ovs_ct_put_key(const struct sw_flow_key *key,
				 struct sk_buff *skb)
{
	return 0;
}

static inline void ovs_ct_free_action(const struct nlattr *a) { }
#endif
#endif /* ovs_conntrack.h */
