/*
 * Copyright (c) 2007-2012 Nicira, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/in.h>
#include <linux/in_route.h>
#include <linux/inetdevice.h>
#include <linux/jhash.h>
#include <linux/list.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/workqueue.h>
#include <linux/rculist.h>
#include <net/route.h>
#include <net/xfrm.h>

#include "checksum.h"
#include "compat.h"
#include "datapath.h"
#include "tunnel.h"
#include "vlan.h"
#include "vport.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,36)
#define rt_dst(rt) (rt->dst)
#else
#define rt_dst(rt) (rt->u.dst)
#endif

/**
 *	ovs_tnl_rcv - ingress point for generic tunnel code
 *
 * @vport: port this packet was received on
 * @skb: received packet
 * @tos: ToS from encapsulating IP packet, used to copy ECN bits
 *
 * Must be called with rcu_read_lock.
 *
 * Packets received by this function are in the following state:
 * - skb->data points to the inner Ethernet header.
 * - The inner Ethernet header is in the linear data area.
 * - skb->csum does not include the inner Ethernet header.
 * - The layer pointers are undefined.
 */
void ovs_tnl_rcv(struct vport *vport, struct sk_buff *skb)
{
	struct ethhdr *eh;

	skb_reset_mac_header(skb);
	eh = eth_hdr(skb);

	if (likely(ntohs(eh->h_proto) >= 1536))
		skb->protocol = eh->h_proto;
	else
		skb->protocol = htons(ETH_P_802_2);

	skb_dst_drop(skb);
	nf_reset(skb);
	skb_clear_rxhash(skb);
	secpath_reset(skb);
	vlan_set_tci(skb, 0);

	if (unlikely(compute_ip_summed(skb, false))) {
		kfree_skb(skb);
		return;
	}

	ovs_vport_receive(vport, skb);
}

static struct rtable *find_route(struct net *net,
		__be32 *saddr, __be32 daddr, u8 ipproto,
		u8 tos, u32 skb_mark)
{
	struct rtable *rt;
	/* Tunnel configuration keeps DSCP part of TOS bits, But Linux
	 * router expect RT_TOS bits only. */

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,39)
	struct flowi fl = { .nl_u = { .ip4_u = {
					.daddr = daddr,
					.saddr = *saddr,
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,20)
					.fwmark = skb_mark,
#endif
					.tos   = RT_TOS(tos) } },
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,20)
					.mark = skb_mark,
#endif
					.proto = ipproto };

	if (unlikely(ip_route_output_key(net, &rt, &fl)))
		return ERR_PTR(-EADDRNOTAVAIL);
	*saddr = fl.nl_u.ip4_u.saddr;
	return rt;
#else
	struct flowi4 fl = { .daddr = daddr,
			     .saddr = *saddr,
			     .flowi4_tos = RT_TOS(tos),
			     .flowi4_mark = skb_mark,
			     .flowi4_proto = ipproto };

	rt = ip_route_output_key(net, &fl);
	*saddr = fl.saddr;
	return rt;
#endif
}

static bool need_linearize(const struct sk_buff *skb)
{
	int i;

	if (unlikely(skb_shinfo(skb)->frag_list))
		return true;

	/*
	 * Generally speaking we should linearize if there are paged frags.
	 * However, if all of the refcounts are 1 we know nobody else can
	 * change them from underneath us and we can skip the linearization.
	 */
	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++)
		if (unlikely(page_count(skb_frag_page(&skb_shinfo(skb)->frags[i])) > 1))
			return true;

	return false;
}

static struct sk_buff *handle_offloads(struct sk_buff *skb,
				       const struct rtable *rt,
				       int tunnel_hlen)
{
	int min_headroom;
	int err;

	min_headroom = LL_RESERVED_SPACE(rt_dst(rt).dev) + rt_dst(rt).header_len
			+ tunnel_hlen
			+ (vlan_tx_tag_present(skb) ? VLAN_HLEN : 0);

	if (skb_headroom(skb) < min_headroom || skb_header_cloned(skb)) {
		int head_delta = SKB_DATA_ALIGN(min_headroom -
						skb_headroom(skb) +
						16);
		err = pskb_expand_head(skb, max_t(int, head_delta, 0),
					0, GFP_ATOMIC);
		if (unlikely(err))
			goto error_free;
	}

	forward_ip_summed(skb, true);

	if (skb_is_gso(skb)) {
		struct sk_buff *nskb;

		nskb = __skb_gso_segment(skb, 0, false);
		if (IS_ERR(nskb)) {
			kfree_skb(skb);
			err = PTR_ERR(nskb);
			goto error;
		}

		consume_skb(skb);
		skb = nskb;
	} else if (get_ip_summed(skb) == OVS_CSUM_PARTIAL) {
		/* Pages aren't locked and could change at any time.
		 * If this happens after we compute the checksum, the
		 * checksum will be wrong.  We linearize now to avoid
		 * this problem.
		 */
		if (unlikely(need_linearize(skb))) {
			err = __skb_linearize(skb);
			if (unlikely(err))
				goto error_free;
		}

		err = skb_checksum_help(skb);
		if (unlikely(err))
			goto error_free;
	}

	set_ip_summed(skb, OVS_CSUM_NONE);

	return skb;

error_free:
	kfree_skb(skb);
error:
	return ERR_PTR(err);
}

/* Compute source UDP port for outgoing packet.
 * Currently we use the flow hash.
 */
u16 ovs_tnl_get_src_port(struct sk_buff *skb)
{
	int low;
	int high;
	unsigned int range;
	u32 hash = OVS_CB(skb)->flow->hash;

	inet_get_local_port_range(&low, &high);
	range = (high - low) + 1;
	return (((u64) hash * range) >> 32) + low;
}

int ovs_tnl_send(struct vport *vport, struct sk_buff *skb)
{
	struct tnl_vport *tnl_vport = tnl_vport_priv(vport);
	struct rtable *rt;
	__be32 saddr;
	int sent_len = 0;
	int tunnel_hlen;

	if (unlikely(!OVS_CB(skb)->tun_key))
		goto error_free;

	/* Route lookup */
	saddr = OVS_CB(skb)->tun_key->ipv4_src;
	rt = find_route(ovs_dp_get_net(vport->dp),
			&saddr,
			OVS_CB(skb)->tun_key->ipv4_dst,
			tnl_vport->tnl_ops->ipproto,
			OVS_CB(skb)->tun_key->ipv4_tos,
			skb_get_mark(skb));
	if (IS_ERR(rt))
		goto error_free;

	/* Offloading */
	tunnel_hlen = tnl_vport->tnl_ops->hdr_len(OVS_CB(skb)->tun_key);
	tunnel_hlen += sizeof(struct iphdr);

	skb = handle_offloads(skb, rt, tunnel_hlen);
	if (IS_ERR(skb)) {
		skb = NULL;
		goto err_free_rt;
	}

	/* Reset SKB */
	nf_reset(skb);
	secpath_reset(skb);
	skb_dst_drop(skb);
	skb_clear_rxhash(skb);

	while (skb) {
		struct sk_buff *next_skb = skb->next;
		struct iphdr *iph;
		int frag_len;
		int err;

		skb->next = NULL;

		if (unlikely(vlan_deaccel_tag(skb)))
			goto next;

		frag_len = skb->len;
		skb_push(skb, tunnel_hlen);
		skb_reset_network_header(skb);
		skb_set_transport_header(skb, sizeof(struct iphdr));

		if (next_skb)
			skb_dst_set(skb, dst_clone(&rt_dst(rt)));
		else
			skb_dst_set(skb, &rt_dst(rt));

		/* Push Tunnel header. */
		tnl_vport->tnl_ops->build_header(vport, skb, tunnel_hlen);

		/* Push IP header. */
		iph = ip_hdr(skb);
		iph->version	= 4;
		iph->ihl	= sizeof(struct iphdr) >> 2;
		iph->protocol	= tnl_vport->tnl_ops->ipproto;
		iph->daddr	= OVS_CB(skb)->tun_key->ipv4_dst;
		iph->saddr	= saddr;
		iph->tos	= OVS_CB(skb)->tun_key->ipv4_tos;
		iph->ttl	= OVS_CB(skb)->tun_key->ipv4_ttl;
		iph->frag_off	= OVS_CB(skb)->tun_key->tun_flags &
				  OVS_TNL_F_DONT_FRAGMENT ?  htons(IP_DF) : 0;
		ip_select_ident(iph, &rt_dst(rt), NULL);

		memset(IPCB(skb), 0, sizeof(*IPCB(skb)));

		err = ip_local_out(skb);
		if (unlikely(net_xmit_eval(err)))
			goto next;

		sent_len += frag_len;

next:
		skb = next_skb;
	}

	if (unlikely(sent_len == 0))
		ovs_vport_record_error(vport, VPORT_E_TX_DROPPED);

	return sent_len;

err_free_rt:
	ip_rt_put(rt);
error_free:
	kfree_skb(skb);
	ovs_vport_record_error(vport, VPORT_E_TX_ERROR);
	return sent_len;
}

struct vport *ovs_tnl_create(const struct vport_parms *parms,
			     const struct vport_ops *vport_ops,
			     const struct tnl_ops *tnl_ops)
{
	struct vport *vport;
	struct tnl_vport *tnl_vport;
	int err;

	vport = ovs_vport_alloc(sizeof(struct tnl_vport), vport_ops, parms);
	if (IS_ERR(vport)) {
		err = PTR_ERR(vport);
		goto error;
	}

	tnl_vport = tnl_vport_priv(vport);

	strcpy(tnl_vport->name, parms->name);
	tnl_vport->tnl_ops = tnl_ops;

	return vport;

error:
	return ERR_PTR(err);
}

static void free_port_rcu(struct rcu_head *rcu)
{
	struct tnl_vport *tnl_vport = container_of(rcu,
						   struct tnl_vport, rcu);

	ovs_vport_free(vport_from_priv(tnl_vport));
}

void ovs_tnl_destroy(struct vport *vport)
{
	struct tnl_vport *tnl_vport = tnl_vport_priv(vport);

	call_rcu(&tnl_vport->rcu, free_port_rcu);
}

const char *ovs_tnl_get_name(const struct vport *vport)
{
	const struct tnl_vport *tnl_vport = tnl_vport_priv(vport);
	return tnl_vport->name;
}
