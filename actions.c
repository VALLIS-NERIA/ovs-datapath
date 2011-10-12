/*
 * Distributed under the terms of the GNU GPL version 2.
 * Copyright (c) 2007, 2008, 2009, 2010, 2011 Nicira Networks.
 *
 * Significant portions of this file may be copied from parts of the Linux
 * kernel, by Linus Torvalds and others.
 */

/* Functions for executing flow actions. */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/skbuff.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/openvswitch.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/in6.h>
#include <linux/if_arp.h>
#include <linux/if_vlan.h>
#include <net/inet_ecn.h>
#include <net/ip.h>
#include <net/checksum.h>

#include "actions.h"
#include "checksum.h"
#include "datapath.h"
#include "vlan.h"
#include "vport.h"

static int do_execute_actions(struct datapath *dp, struct sk_buff *skb,
			const struct nlattr *attr, int len, bool keep_skb);

static int make_writable(struct sk_buff *skb, int write_len)
{
	if (!skb_cloned(skb) || skb_clone_writable(skb, write_len))
		return 0;

	return pskb_expand_head(skb, 0, 0, GFP_ATOMIC);
}

/* remove VLAN header from packet and update csum accrodingly. */
static int __pop_vlan_tci(struct sk_buff *skb, __be16 *current_tci)
{
	struct ethhdr *eh;
	struct vlan_ethhdr *veth;
	int err;

	err = make_writable(skb, VLAN_ETH_HLEN);
	if (unlikely(err))
		return err;

	if (get_ip_summed(skb) == OVS_CSUM_COMPLETE)
		skb->csum = csum_sub(skb->csum, csum_partial(skb->data
					+ ETH_HLEN, VLAN_HLEN, 0));

	veth = (struct vlan_ethhdr *) skb->data;
	*current_tci = veth->h_vlan_TCI;

	memmove(skb->data + VLAN_HLEN, skb->data, 2 * ETH_ALEN);

	eh = (struct ethhdr *)__skb_pull(skb, VLAN_HLEN);

	skb->protocol = eh->h_proto;
	skb->mac_header += VLAN_HLEN;

	return 0;
}

static int pop_vlan(struct sk_buff *skb)
{
	__be16 tci;
	int err;

	if (likely(vlan_tx_tag_present(skb))) {
		vlan_set_tci(skb, 0);
	} else {
		if (unlikely(skb->protocol != htons(ETH_P_8021Q) ||
		    skb->len < VLAN_ETH_HLEN))
			return 0;

		err = __pop_vlan_tci(skb, &tci);
		if (err)
			return err;
	}
	/* move next vlan tag to hw accel tag */
	if (likely(skb->protocol != htons(ETH_P_8021Q) ||
	    skb->len < VLAN_ETH_HLEN))
		return 0;

	err = __pop_vlan_tci(skb, &tci);
	if (unlikely(err))
		return err;

	__vlan_hwaccel_put_tag(skb, ntohs(tci));
	return 0;
}

static int push_vlan(struct sk_buff *skb, __be16 new_tci)
{
	if (unlikely(vlan_tx_tag_present(skb))) {
		u16 current_tag;

		/* push down current VLAN tag */
		current_tag = vlan_tx_tag_get(skb);

		if (!__vlan_put_tag(skb, current_tag))
			return -ENOMEM;

		if (get_ip_summed(skb) == OVS_CSUM_COMPLETE)
			skb->csum = csum_add(skb->csum, csum_partial(skb->data
					+ ETH_HLEN, VLAN_HLEN, 0));

	}
	__vlan_hwaccel_put_tag(skb, ntohs(new_tci));
	return 0;
}

static bool is_ip(struct sk_buff *skb)
{
	return (OVS_CB(skb)->flow->key.eth.type == htons(ETH_P_IP) &&
		skb->transport_header > skb->network_header);
}

static __sum16 *get_l4_checksum(struct sk_buff *skb)
{
	u8 nw_proto = OVS_CB(skb)->flow->key.ip.proto;
	int transport_len = skb->len - skb_transport_offset(skb);
	if (nw_proto == IPPROTO_TCP) {
		if (likely(transport_len >= sizeof(struct tcphdr)))
			return &tcp_hdr(skb)->check;
	} else if (nw_proto == IPPROTO_UDP) {
		if (likely(transport_len >= sizeof(struct udphdr)))
			return &udp_hdr(skb)->check;
	}
	return NULL;
}

static int set_nw_addr(struct sk_buff *skb, const struct nlattr *a)
{
	__be32 new_nwaddr = nla_get_be32(a);
	struct iphdr *nh;
	__sum16 *check;
	__be32 *nwaddr;
	int err;

	if (unlikely(!is_ip(skb)))
		return 0;

	err = make_writable(skb, skb_network_offset(skb) +
				 sizeof(struct iphdr));
	if (unlikely(err))
		return err;

	nh = ip_hdr(skb);
	nwaddr = nla_type(a) == OVS_ACTION_ATTR_SET_NW_SRC ? &nh->saddr : &nh->daddr;

	check = get_l4_checksum(skb);
	if (likely(check))
		inet_proto_csum_replace4(check, skb, *nwaddr, new_nwaddr, 1);
	csum_replace4(&nh->check, *nwaddr, new_nwaddr);

	skb_clear_rxhash(skb);

	*nwaddr = new_nwaddr;

	return 0;
}

static int set_nw_tos(struct sk_buff *skb, u8 nw_tos)
{
	struct iphdr *nh = ip_hdr(skb);
	u8 old, new;
	int err;

	if (unlikely(!is_ip(skb)))
		return 0;

	err = make_writable(skb, skb_network_offset(skb) +
				 sizeof(struct iphdr));
	if (unlikely(err))
		return err;

	/* Set the DSCP bits and preserve the ECN bits. */
	old = nh->tos;
	new = nw_tos | (nh->tos & INET_ECN_MASK);
	csum_replace4(&nh->check, (__force __be32)old,
				  (__force __be32)new);
	nh->tos = new;

	return 0;
}

static int set_tp_port(struct sk_buff *skb, const struct nlattr *a)
{
	struct udphdr *th;
	__sum16 *check;
	__be16 *port;
	int err;

	if (unlikely(!is_ip(skb)))
		return 0;

	err = make_writable(skb, skb_transport_offset(skb) +
				 sizeof(struct tcphdr));
	if (unlikely(err))
		return err;

	/* Must follow make_writable() since that can move the skb data. */
	check = get_l4_checksum(skb);
	if (unlikely(!check))
		return 0;

	/*
	 * Update port and checksum.
	 *
	 * This is OK because source and destination port numbers are at the
	 * same offsets in both UDP and TCP headers, and get_l4_checksum() only
	 * supports those protocols.
	 */
	th = udp_hdr(skb);
	port = nla_type(a) == OVS_ACTION_ATTR_SET_TP_SRC ? &th->source : &th->dest;
	inet_proto_csum_replace2(check, skb, *port, nla_get_be16(a), 0);
	*port = nla_get_be16(a);
	skb_clear_rxhash(skb);

	return 0;
}

static int do_output(struct datapath *dp, struct sk_buff *skb, int out_port)
{
	struct vport *vport;

	if (unlikely(!skb))
		return -ENOMEM;

	vport = rcu_dereference(dp->ports[out_port]);
	if (unlikely(!vport)) {
		kfree_skb(skb);
		return -ENODEV;
	}

	vport_send(vport, skb);
	return 0;
}

static int output_userspace(struct datapath *dp, struct sk_buff *skb,
			    const struct nlattr *attr)
{
	struct dp_upcall_info upcall;
	const struct nlattr *a;
	int rem;

	upcall.cmd = OVS_PACKET_CMD_ACTION;
	upcall.key = &OVS_CB(skb)->flow->key;
	upcall.userdata = NULL;
	upcall.pid = 0;

	for (a = nla_data(attr), rem = nla_len(attr); rem > 0;
		 a = nla_next(a, &rem)) {
		switch (nla_type(a)) {
		case OVS_USERSPACE_ATTR_USERDATA:
			upcall.userdata = a;
			break;

		case OVS_USERSPACE_ATTR_PID:
			upcall.pid = nla_get_u32(a);
			break;
		}
	}

	return dp_upcall(dp, skb, &upcall);
}

static int sample(struct datapath *dp, struct sk_buff *skb,
		  const struct nlattr *attr)
{
	const struct nlattr *acts_list = NULL;
	const struct nlattr *a;
	int rem;

	for (a = nla_data(attr), rem = nla_len(attr); rem > 0;
		 a = nla_next(a, &rem)) {
		switch (nla_type(a)) {
		case OVS_SAMPLE_ATTR_PROBABILITY:
			if (net_random() >= nla_get_u32(a))
				return 0;
			break;

		case OVS_SAMPLE_ATTR_ACTIONS:
			acts_list = a;
			break;
		}
	}

	return do_execute_actions(dp, skb, nla_data(acts_list),
						 nla_len(acts_list), true);
}

/* Execute a list of actions against 'skb'. */
static int do_execute_actions(struct datapath *dp, struct sk_buff *skb,
			const struct nlattr *attr, int len, bool keep_skb)
{
	/* Every output action needs a separate clone of 'skb', but the common
	 * case is just a single output action, so that doing a clone and
	 * then freeing the original skbuff is wasteful.  So the following code
	 * is slightly obscure just to avoid that. */
	int prev_port = -1;
	u32 priority = skb->priority;
	const struct nlattr *a;
	int rem;

	for (a = attr, rem = len; rem > 0;
	     a = nla_next(a, &rem)) {
		int err = 0;

		if (prev_port != -1) {
			do_output(dp, skb_clone(skb, GFP_ATOMIC), prev_port);
			prev_port = -1;
		}

		switch (nla_type(a)) {
		case OVS_ACTION_ATTR_OUTPUT:
			prev_port = nla_get_u32(a);
			break;

		case OVS_ACTION_ATTR_USERSPACE:
			output_userspace(dp, skb, a);
			break;

		case OVS_ACTION_ATTR_SET_TUNNEL:
			OVS_CB(skb)->tun_id = nla_get_be64(a);
			break;

		case OVS_ACTION_ATTR_PUSH_VLAN:
			err = push_vlan(skb, nla_get_be16(a));
			if (unlikely(err)) /* skb already freed */
				return err;
			break;

		case OVS_ACTION_ATTR_POP_VLAN:
			err = pop_vlan(skb);
			break;

		case OVS_ACTION_ATTR_SET_DL_SRC:
			err = make_writable(skb, ETH_HLEN);
			if (likely(!err))
				memcpy(eth_hdr(skb)->h_source, nla_data(a), ETH_ALEN);
			break;

		case OVS_ACTION_ATTR_SET_DL_DST:
			err = make_writable(skb, ETH_HLEN);
			if (likely(!err))
				memcpy(eth_hdr(skb)->h_dest, nla_data(a), ETH_ALEN);
			break;

		case OVS_ACTION_ATTR_SET_NW_SRC:
		case OVS_ACTION_ATTR_SET_NW_DST:
			err = set_nw_addr(skb, a);
			break;

		case OVS_ACTION_ATTR_SET_NW_TOS:
			err = set_nw_tos(skb, nla_get_u8(a));
			break;

		case OVS_ACTION_ATTR_SET_TP_SRC:
		case OVS_ACTION_ATTR_SET_TP_DST:
			err = set_tp_port(skb, a);
			break;

		case OVS_ACTION_ATTR_SET_PRIORITY:
			skb->priority = nla_get_u32(a);
			break;

		case OVS_ACTION_ATTR_POP_PRIORITY:
			skb->priority = priority;
			break;

		case OVS_ACTION_ATTR_SAMPLE:
			err = sample(dp, skb, a);
			break;

		}
		if (unlikely(err)) {
			kfree_skb(skb);
			return err;
		}
	}

	if (prev_port != -1) {
		if (keep_skb)
			skb = skb_clone(skb, GFP_ATOMIC);

		do_output(dp, skb, prev_port);
	} else if (!keep_skb)
		consume_skb(skb);

	return 0;
}

/* We limit the number of times that we pass into execute_actions()
 * to avoid blowing out the stack in the event that we have a loop. */
#define MAX_LOOPS 5

struct loop_counter {
	u8 count;		/* Count. */
	bool looping;		/* Loop detected? */
};

static DEFINE_PER_CPU(struct loop_counter, loop_counters);

static int loop_suppress(struct datapath *dp, struct sw_flow_actions *actions)
{
	if (net_ratelimit())
		pr_warn("%s: flow looped %d times, dropping\n",
				dp_name(dp), MAX_LOOPS);
	actions->actions_len = 0;
	return -ELOOP;
}

/* Execute a list of actions against 'skb'. */
int execute_actions(struct datapath *dp, struct sk_buff *skb)
{
	struct sw_flow_actions *acts = rcu_dereference(OVS_CB(skb)->flow->sf_acts);
	struct loop_counter *loop;
	int error;

	/* Check whether we've looped too much. */
	loop = &__get_cpu_var(loop_counters);
	if (unlikely(++loop->count > MAX_LOOPS))
		loop->looping = true;
	if (unlikely(loop->looping)) {
		error = loop_suppress(dp, acts);
		kfree_skb(skb);
		goto out_loop;
	}

	OVS_CB(skb)->tun_id = 0;
	error = do_execute_actions(dp, skb, acts->actions,
					 acts->actions_len, false);

	/* Check whether sub-actions looped too much. */
	if (unlikely(loop->looping))
		error = loop_suppress(dp, acts);

out_loop:
	/* Decrement loop counter. */
	if (!--loop->count)
		loop->looping = false;

	return error;
}
