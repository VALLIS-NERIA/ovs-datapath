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

#include <linux/if_arp.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/if_vlan.h>
#include <linux/igmp.h>
#include <linux/in.h>
#include <linux/in_route.h>
#include <linux/inetdevice.h>
#include <linux/jhash.h>
#include <linux/list.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/workqueue.h>
#include <linux/rculist.h>

#include <net/dsfield.h>
#include <net/dst.h>
#include <net/icmp.h>
#include <net/inet_ecn.h>
#include <net/ip.h>
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
#include <net/ipv6.h>
#endif
#include <net/route.h>
#include <net/xfrm.h>

#include "checksum.h"
#include "datapath.h"
#include "tunnel.h"
#include "vlan.h"
#include "vport.h"
#include "vport-internal_dev.h"

#define PORT_TABLE_SIZE  1024

static struct hlist_head *port_table __read_mostly;

/*
 * These are just used as an optimization: they don't require any kind of
 * synchronization because we could have just as easily read the value before
 * the port change happened.
 */
static unsigned int key_local_remote_ports __read_mostly;
static unsigned int key_remote_ports __read_mostly;
static unsigned int key_multicast_ports __read_mostly;
static unsigned int local_remote_ports __read_mostly;
static unsigned int remote_ports __read_mostly;
static unsigned int null_ports __read_mostly;
static unsigned int multicast_ports __read_mostly;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,36)
#define rt_dst(rt) (rt->dst)
#else
#define rt_dst(rt) (rt->u.dst)
#endif

static struct vport *tnl_vport_to_vport(const struct tnl_vport *tnl_vport)
{
	return vport_from_priv(tnl_vport);
}

static void free_config_rcu(struct rcu_head *rcu)
{
	struct tnl_mutable_config *c = container_of(rcu, struct tnl_mutable_config, rcu);
	kfree(c);
}

/* Frees the portion of 'mutable' that requires RTNL and thus can't happen
 * within an RCU callback.  Fortunately this part doesn't require waiting for
 * an RCU grace period.
 */
static void free_mutable_rtnl(struct tnl_mutable_config *mutable)
{
	ASSERT_RTNL();
	if (ipv4_is_multicast(mutable->key.daddr) && mutable->mlink) {
		struct in_device *in_dev;
		in_dev = inetdev_by_index(port_key_get_net(&mutable->key), mutable->mlink);
		if (in_dev)
			ip_mc_dec_group(in_dev, mutable->key.daddr);
	}
}

static void assign_config_rcu(struct vport *vport,
			      struct tnl_mutable_config *new_config)
{
	struct tnl_vport *tnl_vport = tnl_vport_priv(vport);
	struct tnl_mutable_config *old_config;

	old_config = rtnl_dereference(tnl_vport->mutable);
	rcu_assign_pointer(tnl_vport->mutable, new_config);

	free_mutable_rtnl(old_config);
	call_rcu(&old_config->rcu, free_config_rcu);
}

static unsigned int *find_port_pool(const struct tnl_mutable_config *mutable)
{
	bool is_multicast = ipv4_is_multicast(mutable->key.daddr);

	if (mutable->flags & TNL_F_IN_KEY_MATCH) {
		if (mutable->key.saddr)
			return &local_remote_ports;
		else if (is_multicast)
			return &multicast_ports;
		else
			return &remote_ports;
	} else {
		if (mutable->key.saddr)
			return &key_local_remote_ports;
		else if (is_multicast)
			return &key_multicast_ports;
		else if (mutable->key.daddr)
			return &key_remote_ports;
		else
			return &null_ports;
	}
}

static u32 port_hash(const struct port_lookup_key *key)
{
	return jhash2((u32 *)key, (PORT_KEY_LEN / sizeof(u32)), 0);
}

static struct hlist_head *find_bucket(u32 hash)
{
	return &port_table[(hash & (PORT_TABLE_SIZE - 1))];
}

static void port_table_add_port(struct vport *vport)
{
	struct tnl_vport *tnl_vport = tnl_vport_priv(vport);
	const struct tnl_mutable_config *mutable;
	u32 hash;

	mutable = rtnl_dereference(tnl_vport->mutable);
	hash = port_hash(&mutable->key);
	hlist_add_head_rcu(&tnl_vport->hash_node, find_bucket(hash));

	(*find_port_pool(rtnl_dereference(tnl_vport->mutable)))++;
}

static void port_table_move_port(struct vport *vport,
		      struct tnl_mutable_config *new_mutable)
{
	struct tnl_vport *tnl_vport = tnl_vport_priv(vport);
	u32 hash;

	hash = port_hash(&new_mutable->key);
	hlist_del_init_rcu(&tnl_vport->hash_node);
	hlist_add_head_rcu(&tnl_vport->hash_node, find_bucket(hash));

	(*find_port_pool(rtnl_dereference(tnl_vport->mutable)))--;
	assign_config_rcu(vport, new_mutable);
	(*find_port_pool(rtnl_dereference(tnl_vport->mutable)))++;
}

static void port_table_remove_port(struct vport *vport)
{
	struct tnl_vport *tnl_vport = tnl_vport_priv(vport);

	hlist_del_init_rcu(&tnl_vport->hash_node);

	(*find_port_pool(rtnl_dereference(tnl_vport->mutable)))--;
}

static struct vport *port_table_lookup(struct port_lookup_key *key,
				       const struct tnl_mutable_config **pmutable)
{
	struct hlist_node *n;
	struct hlist_head *bucket;
	u32 hash = port_hash(key);
	struct tnl_vport *tnl_vport;

	bucket = find_bucket(hash);

	hlist_for_each_entry_rcu(tnl_vport, n, bucket, hash_node) {
		struct tnl_mutable_config *mutable;

		mutable = rcu_dereference_rtnl(tnl_vport->mutable);
		if (!memcmp(&mutable->key, key, PORT_KEY_LEN)) {
			*pmutable = mutable;
			return tnl_vport_to_vport(tnl_vport);
		}
	}

	return NULL;
}

struct vport *ovs_tnl_find_port(struct net *net, __be32 saddr, __be32 daddr,
				__be64 key, int tunnel_type,
				const struct tnl_mutable_config **mutable)
{
	struct port_lookup_key lookup;
	struct vport *vport;
	bool is_multicast = ipv4_is_multicast(saddr);

	port_key_set_net(&lookup, net);
	lookup.saddr = saddr;
	lookup.daddr = daddr;

	/* First try for exact match on in_key. */
	lookup.in_key = key;
	lookup.tunnel_type = tunnel_type | TNL_T_KEY_EXACT;
	if (!is_multicast && key_local_remote_ports) {
		vport = port_table_lookup(&lookup, mutable);
		if (vport)
			return vport;
	}
	if (key_remote_ports) {
		lookup.saddr = 0;
		vport = port_table_lookup(&lookup, mutable);
		if (vport)
			return vport;

		lookup.saddr = saddr;
	}

	/* Then try matches that wildcard in_key. */
	lookup.in_key = 0;
	lookup.tunnel_type = tunnel_type | TNL_T_KEY_MATCH;
	if (!is_multicast && local_remote_ports) {
		vport = port_table_lookup(&lookup, mutable);
		if (vport)
			return vport;
	}
	if (remote_ports) {
		lookup.saddr = 0;
		vport = port_table_lookup(&lookup, mutable);
		if (vport)
			return vport;
	}

	if (is_multicast) {
		lookup.saddr = 0;
		lookup.daddr = saddr;
		if (key_multicast_ports) {
			lookup.tunnel_type = tunnel_type | TNL_T_KEY_EXACT;
			lookup.in_key = key;
			vport = port_table_lookup(&lookup, mutable);
			if (vport)
				return vport;
		}
		if (multicast_ports) {
			lookup.tunnel_type = tunnel_type | TNL_T_KEY_MATCH;
			lookup.in_key = 0;
			vport = port_table_lookup(&lookup, mutable);
			if (vport)
				return vport;
		}
	}

	if (null_ports) {
		lookup.daddr = 0;
		lookup.saddr = 0;
		lookup.in_key = 0;
		lookup.tunnel_type = tunnel_type;
		vport = port_table_lookup(&lookup, mutable);
		if (vport)
			return vport;
	}
	return NULL;
}

static void ecn_decapsulate(struct sk_buff *skb)
{
	if (unlikely(INET_ECN_is_ce(OVS_CB(skb)->tun_key->ipv4_tos))) {
		__be16 protocol = skb->protocol;

		skb_set_network_header(skb, ETH_HLEN);

		if (protocol == htons(ETH_P_8021Q)) {
			if (unlikely(!pskb_may_pull(skb, VLAN_ETH_HLEN)))
				return;

			protocol = vlan_eth_hdr(skb)->h_vlan_encapsulated_proto;
			skb_set_network_header(skb, VLAN_ETH_HLEN);
		}

		if (protocol == htons(ETH_P_IP)) {
			if (unlikely(!pskb_may_pull(skb, skb_network_offset(skb)
			    + sizeof(struct iphdr))))
				return;

			IP_ECN_set_ce(ip_hdr(skb));
		}
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
		else if (protocol == htons(ETH_P_IPV6)) {
			if (unlikely(!pskb_may_pull(skb, skb_network_offset(skb)
			    + sizeof(struct ipv6hdr))))
				return;

			IP6_ECN_set_ce(ipv6_hdr(skb));
		}
#endif
	}
}

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

	ecn_decapsulate(skb);
	vlan_set_tci(skb, 0);

	if (unlikely(compute_ip_summed(skb, false))) {
		kfree_skb(skb);
		return;
	}

	ovs_vport_receive(vport, skb);
}

static struct rtable *find_route(struct net *net,
		__be32 *saddr, __be32 daddr, u8 ipproto,
		u8 tos)
{
	struct rtable *rt;
	/* Tunnel configuration keeps DSCP part of TOS bits, But Linux
	 * router expect RT_TOS bits only. */

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,39)
	struct flowi fl = { .nl_u = { .ip4_u = {
					.daddr = daddr,
					.saddr = *saddr,
					.tos   = RT_TOS(tos) } },
					.proto = ipproto };

	if (unlikely(ip_route_output_key(net, &rt, &fl)))
		return ERR_PTR(-EADDRNOTAVAIL);
	*saddr = fl.nl_u.ip4_u.saddr;
	return rt;
#else
	struct flowi4 fl = { .daddr = daddr,
			     .saddr = *saddr,
			     .flowi4_tos = RT_TOS(tos),
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
				       const struct tnl_mutable_config *mutable,
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

		nskb = skb_gso_segment(skb, 0);
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

static int send_frags(struct sk_buff *skb,
		      int tunnel_hlen)
{
	int sent_len;

	sent_len = 0;
	while (skb) {
		struct sk_buff *next = skb->next;
		int frag_len = skb->len - tunnel_hlen;
		int err;

		skb->next = NULL;
		memset(IPCB(skb), 0, sizeof(*IPCB(skb)));

		err = ip_local_out(skb);
		skb = next;
		if (unlikely(net_xmit_eval(err)))
			goto free_frags;
		sent_len += frag_len;
	}

	return sent_len;

free_frags:
	/*
	 * There's no point in continuing to send fragments once one has been
	 * dropped so just free the rest.  This may help improve the congestion
	 * that caused the first packet to be dropped.
	 */
	ovs_tnl_free_linked_skbs(skb);
	return sent_len;
}

int ovs_tnl_send(struct vport *vport, struct sk_buff *skb)
{
	struct tnl_vport *tnl_vport = tnl_vport_priv(vport);
	const struct tnl_mutable_config *mutable = rcu_dereference(tnl_vport->mutable);
	enum vport_err_type err = VPORT_E_TX_ERROR;
	struct rtable *rt;
	struct ovs_key_ipv4_tunnel tun_key;
	int sent_len = 0;
	int tunnel_hlen;
	__be16 frag_off;
	__be32 daddr;
	__be32 saddr;
	u8 ttl;
	u8 tos;

	/* Validate the protocol headers before we try to use them. */
	if (skb->protocol == htons(ETH_P_8021Q) &&
	    !vlan_tx_tag_present(skb)) {
		if (unlikely(!pskb_may_pull(skb, VLAN_ETH_HLEN)))
			goto error_free;

		skb->protocol = vlan_eth_hdr(skb)->h_vlan_encapsulated_proto;
		skb_set_network_header(skb, VLAN_ETH_HLEN);
	}

	if (skb->protocol == htons(ETH_P_IP)) {
		if (unlikely(!pskb_may_pull(skb, skb_network_offset(skb)
		    + sizeof(struct iphdr))))
			skb->protocol = 0;
	}
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
	else if (skb->protocol == htons(ETH_P_IPV6)) {
		if (unlikely(!pskb_may_pull(skb, skb_network_offset(skb)
		    + sizeof(struct ipv6hdr))))
			skb->protocol = 0;
	}
#endif

	/* If OVS_CB(skb)->tun_key is NULL, point it at the local tun_key here,
	 * and zero it out.
	 */
	if (!OVS_CB(skb)->tun_key) {
		memset(&tun_key, 0, sizeof(tun_key));
		OVS_CB(skb)->tun_key = &tun_key;
	}

	tunnel_hlen = tnl_vport->tnl_ops->hdr_len(mutable, OVS_CB(skb)->tun_key);
	if (unlikely(tunnel_hlen < 0)) {
		err = VPORT_E_TX_DROPPED;
		goto error_free;
	}
	tunnel_hlen += sizeof(struct iphdr);

	if (OVS_CB(skb)->tun_key->ipv4_dst) {
		daddr = OVS_CB(skb)->tun_key->ipv4_dst;
		saddr = OVS_CB(skb)->tun_key->ipv4_src;
		tos = OVS_CB(skb)->tun_key->ipv4_tos;
		ttl = OVS_CB(skb)->tun_key->ipv4_ttl;
                frag_off = OVS_CB(skb)->tun_key->tun_flags &
				OVS_TNL_F_DONT_FRAGMENT ?  htons(IP_DF) : 0;
	} else {
		u8 inner_tos;
		daddr = mutable->key.daddr;
		saddr = mutable->key.saddr;

		if (unlikely(!daddr)) {
			/* Trying to sent packet from Null-port without
			 * tunnel info? Drop this packet. */
			err = VPORT_E_TX_DROPPED;
			goto error_free;
		}

		/* ToS */
		if (skb->protocol == htons(ETH_P_IP))
			inner_tos = ip_hdr(skb)->tos;
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
		else if (skb->protocol == htons(ETH_P_IPV6))
			inner_tos = ipv6_get_dsfield(ipv6_hdr(skb));
#endif
		else
			inner_tos = 0;

		if (mutable->flags & TNL_F_TOS_INHERIT)
			tos = inner_tos;
		else
			tos = mutable->tos;

		tos = INET_ECN_encapsulate(tos, inner_tos);

		/* TTL */
		ttl = mutable->ttl;
		if (mutable->flags & TNL_F_TTL_INHERIT) {
			if (skb->protocol == htons(ETH_P_IP))
				ttl = ip_hdr(skb)->ttl;
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
			else if (skb->protocol == htons(ETH_P_IPV6))
				ttl = ipv6_hdr(skb)->hop_limit;
#endif
		}

		frag_off = mutable->flags & TNL_F_DF_DEFAULT ? htons(IP_DF) : 0;
	}

	/* Route lookup */
	rt = find_route(port_key_get_net(&mutable->key), &saddr, daddr,
			  tnl_vport->tnl_ops->ipproto, tos);
	if (IS_ERR(rt))
		goto error_free;

	/* Reset SKB */
	nf_reset(skb);
	secpath_reset(skb);
	skb_dst_drop(skb);
	skb_clear_rxhash(skb);

	/* Offloading */
	skb = handle_offloads(skb, mutable, rt, tunnel_hlen);
	if (IS_ERR(skb)) {
		skb = NULL;
		goto err_free_rt;
	}

	/* TTL Fixup. */
	if (!OVS_CB(skb)->tun_key->ipv4_dst) {
		if (!(mutable->flags & TNL_F_TTL_INHERIT)) {
			if (!ttl)
				ttl = ip4_dst_hoplimit(&rt_dst(rt));
		}
	}

	while (skb) {
		struct iphdr *iph;
		struct sk_buff *next_skb = skb->next;
		skb->next = NULL;

		if (unlikely(vlan_deaccel_tag(skb)))
			goto next;

		skb_push(skb, tunnel_hlen);
		skb_reset_network_header(skb);
		skb_set_transport_header(skb, sizeof(struct iphdr));

		if (next_skb)
			skb_dst_set(skb, dst_clone(&rt_dst(rt)));
		else
			skb_dst_set(skb, &rt_dst(rt));

		/* Push IP header. */
		iph = ip_hdr(skb);
		iph->version	= 4;
		iph->ihl	= sizeof(struct iphdr) >> 2;
		iph->protocol	= tnl_vport->tnl_ops->ipproto;
		iph->daddr	= daddr;
		iph->saddr	= saddr;
		iph->tos	= tos;
		iph->ttl	= ttl;
		iph->frag_off	= frag_off;
		ip_select_ident(iph, &rt_dst(rt), NULL);

		/* Push Tunnel header. */
		skb = tnl_vport->tnl_ops->build_header(vport, mutable,
							&rt_dst(rt), skb, tunnel_hlen);
		if (unlikely(!skb))
			goto next;

		sent_len += send_frags(skb, tunnel_hlen);

next:
		skb = next_skb;
	}

	if (unlikely(sent_len == 0))
		ovs_vport_record_error(vport, VPORT_E_TX_DROPPED);

	return sent_len;

err_free_rt:
	ip_rt_put(rt);
error_free:
	ovs_tnl_free_linked_skbs(skb);
	ovs_vport_record_error(vport, err);
	return sent_len;
}

static const struct nla_policy tnl_policy[OVS_TUNNEL_ATTR_MAX + 1] = {
	[OVS_TUNNEL_ATTR_FLAGS]    = { .type = NLA_U32 },
	[OVS_TUNNEL_ATTR_DST_IPV4] = { .type = NLA_U32 },
	[OVS_TUNNEL_ATTR_SRC_IPV4] = { .type = NLA_U32 },
	[OVS_TUNNEL_ATTR_OUT_KEY]  = { .type = NLA_U64 },
	[OVS_TUNNEL_ATTR_IN_KEY]   = { .type = NLA_U64 },
	[OVS_TUNNEL_ATTR_TOS]      = { .type = NLA_U8 },
	[OVS_TUNNEL_ATTR_TTL]      = { .type = NLA_U8 },
	[OVS_TUNNEL_ATTR_DST_PORT] = { .type = NLA_U16 },
};

/* Sets OVS_TUNNEL_ATTR_* fields in 'mutable', which must initially be
 * zeroed. */
static int tnl_set_config(struct net *net, struct nlattr *options,
			  const struct tnl_ops *tnl_ops,
			  const struct vport *cur_vport,
			  struct tnl_mutable_config *mutable)
{
	const struct vport *old_vport;
	const struct tnl_mutable_config *old_mutable;
	struct nlattr *a[OVS_TUNNEL_ATTR_MAX + 1];
	int err;

	port_key_set_net(&mutable->key, net);
	mutable->key.tunnel_type = tnl_ops->tunnel_type;
	if (!options)
		goto out;

	err = nla_parse_nested(a, OVS_TUNNEL_ATTR_MAX, options, tnl_policy);
	if (err)
		return err;

	/* Process attributes possibly useful for null_ports first */
	if (a[OVS_TUNNEL_ATTR_DST_PORT])
		mutable->dst_port =
			htons(nla_get_u16(a[OVS_TUNNEL_ATTR_DST_PORT]));

	if (a[OVS_TUNNEL_ATTR_DST_IPV4])
		mutable->key.daddr = nla_get_be32(a[OVS_TUNNEL_ATTR_DST_IPV4]);

	/* Skip the rest if configuring a null_port */
	if (!mutable->key.daddr)
		goto out;

	if (a[OVS_TUNNEL_ATTR_FLAGS])
		mutable->flags = nla_get_u32(a[OVS_TUNNEL_ATTR_FLAGS])
			& TNL_F_PUBLIC;

	if (a[OVS_TUNNEL_ATTR_SRC_IPV4]) {
		if (ipv4_is_multicast(mutable->key.daddr))
			return -EINVAL;
		mutable->key.saddr = nla_get_be32(a[OVS_TUNNEL_ATTR_SRC_IPV4]);
	}

	if (a[OVS_TUNNEL_ATTR_TOS]) {
		mutable->tos = nla_get_u8(a[OVS_TUNNEL_ATTR_TOS]);
		/* Reject ToS config with ECN bits set. */
		if (mutable->tos & INET_ECN_MASK)
			return -EINVAL;
	}

	if (a[OVS_TUNNEL_ATTR_TTL])
		mutable->ttl = nla_get_u8(a[OVS_TUNNEL_ATTR_TTL]);

	if (!a[OVS_TUNNEL_ATTR_IN_KEY]) {
		mutable->key.tunnel_type |= TNL_T_KEY_MATCH;
		mutable->flags |= TNL_F_IN_KEY_MATCH;
	} else {
		mutable->key.tunnel_type |= TNL_T_KEY_EXACT;
		mutable->key.in_key = nla_get_be64(a[OVS_TUNNEL_ATTR_IN_KEY]);
	}

	if (!a[OVS_TUNNEL_ATTR_OUT_KEY])
		mutable->flags |= TNL_F_OUT_KEY_ACTION;
	else
		mutable->out_key = nla_get_be64(a[OVS_TUNNEL_ATTR_OUT_KEY]);

	mutable->mlink = 0;
	if (ipv4_is_multicast(mutable->key.daddr)) {
		struct net_device *dev;
		struct rtable *rt;
		__be32 saddr = mutable->key.saddr;

		rt = find_route(port_key_get_net(&mutable->key),
			     &saddr, mutable->key.daddr,
			     tnl_ops->ipproto, mutable->tos);
		if (IS_ERR(rt))
			return -EADDRNOTAVAIL;
		dev = rt_dst(rt).dev;
		ip_rt_put(rt);
		if (__in_dev_get_rtnl(dev) == NULL)
			return -EADDRNOTAVAIL;
		mutable->mlink = dev->ifindex;
		ip_mc_inc_group(__in_dev_get_rtnl(dev), mutable->key.daddr);
	}

out:
	old_vport = port_table_lookup(&mutable->key, &old_mutable);
	if (old_vport && old_vport != cur_vport)
		return -EEXIST;

	return 0;
}

struct vport *ovs_tnl_create(const struct vport_parms *parms,
			     const struct vport_ops *vport_ops,
			     const struct tnl_ops *tnl_ops)
{
	struct vport *vport;
	struct tnl_vport *tnl_vport;
	struct tnl_mutable_config *mutable;
	int initial_frag_id;
	int err;

	vport = ovs_vport_alloc(sizeof(struct tnl_vport), vport_ops, parms);
	if (IS_ERR(vport)) {
		err = PTR_ERR(vport);
		goto error;
	}

	tnl_vport = tnl_vport_priv(vport);

	strcpy(tnl_vport->name, parms->name);
	tnl_vport->tnl_ops = tnl_ops;

	mutable = kzalloc(sizeof(struct tnl_mutable_config), GFP_KERNEL);
	if (!mutable) {
		err = -ENOMEM;
		goto error_free_vport;
	}

	get_random_bytes(&initial_frag_id, sizeof(int));
	atomic_set(&tnl_vport->frag_id, initial_frag_id);

	err = tnl_set_config(ovs_dp_get_net(parms->dp), parms->options, tnl_ops,
			     NULL, mutable);
	if (err)
		goto error_free_mutable;

	rcu_assign_pointer(tnl_vport->mutable, mutable);

	port_table_add_port(vport);
	return vport;

error_free_mutable:
	free_mutable_rtnl(mutable);
	kfree(mutable);
error_free_vport:
	ovs_vport_free(vport);
error:
	return ERR_PTR(err);
}

int ovs_tnl_set_options(struct vport *vport, struct nlattr *options)
{
	struct tnl_vport *tnl_vport = tnl_vport_priv(vport);
	const struct tnl_mutable_config *old_mutable;
	struct tnl_mutable_config *mutable;
	int err;

	old_mutable = rtnl_dereference(tnl_vport->mutable);
	if (!old_mutable->key.daddr)
		return -EINVAL;

	mutable = kzalloc(sizeof(struct tnl_mutable_config), GFP_KERNEL);
	if (!mutable) {
		err = -ENOMEM;
		goto error;
	}

	mutable->seq = old_mutable->seq + 1;

	/* Parse the others configured by userspace. */
	err = tnl_set_config(ovs_dp_get_net(vport->dp), options, tnl_vport->tnl_ops,
			     vport, mutable);
	if (err)
		goto error_free;

	if (port_hash(&mutable->key) != port_hash(&old_mutable->key))
		port_table_move_port(vport, mutable);
	else
		assign_config_rcu(vport, mutable);

	return 0;

error_free:
	free_mutable_rtnl(mutable);
	kfree(mutable);
error:
	return err;
}

int ovs_tnl_get_options(const struct vport *vport, struct sk_buff *skb)
{
	const struct tnl_vport *tnl_vport = tnl_vport_priv(vport);
	const struct tnl_mutable_config *mutable = rcu_dereference_rtnl(tnl_vport->mutable);

	if (mutable->dst_port && nla_put_u16(skb, OVS_TUNNEL_ATTR_DST_PORT,
					     ntohs(mutable->dst_port)))
		goto nla_put_failure;

	/* Skip the rest for null_ports */
	if (!mutable->key.daddr)
		return 0;

	if (nla_put_be32(skb, OVS_TUNNEL_ATTR_DST_IPV4, mutable->key.daddr))
		goto nla_put_failure;
	if (nla_put_u32(skb, OVS_TUNNEL_ATTR_FLAGS,
			mutable->flags & TNL_F_PUBLIC))
		goto nla_put_failure;
	if (!(mutable->flags & TNL_F_IN_KEY_MATCH) &&
	    nla_put_be64(skb, OVS_TUNNEL_ATTR_IN_KEY, mutable->key.in_key))
		goto nla_put_failure;
	if (!(mutable->flags & TNL_F_OUT_KEY_ACTION) &&
	    nla_put_be64(skb, OVS_TUNNEL_ATTR_OUT_KEY, mutable->out_key))
		goto nla_put_failure;
	if (mutable->key.saddr &&
	    nla_put_be32(skb, OVS_TUNNEL_ATTR_SRC_IPV4, mutable->key.saddr))
		goto nla_put_failure;
	if (mutable->tos && nla_put_u8(skb, OVS_TUNNEL_ATTR_TOS, mutable->tos))
		goto nla_put_failure;
	if (mutable->ttl && nla_put_u8(skb, OVS_TUNNEL_ATTR_TTL, mutable->ttl))
		goto nla_put_failure;

	return 0;

nla_put_failure:
	return -EMSGSIZE;
}

static void free_port_rcu(struct rcu_head *rcu)
{
	struct tnl_vport *tnl_vport = container_of(rcu,
						   struct tnl_vport, rcu);

	kfree((struct tnl_mutable __force *)tnl_vport->mutable);
	ovs_vport_free(tnl_vport_to_vport(tnl_vport));
}

void ovs_tnl_destroy(struct vport *vport)
{
	struct tnl_vport *tnl_vport = tnl_vport_priv(vport);
	struct tnl_mutable_config *mutable;

	mutable = rtnl_dereference(tnl_vport->mutable);
	port_table_remove_port(vport);
	free_mutable_rtnl(mutable);
	call_rcu(&tnl_vport->rcu, free_port_rcu);
}

const char *ovs_tnl_get_name(const struct vport *vport)
{
	const struct tnl_vport *tnl_vport = tnl_vport_priv(vport);
	return tnl_vport->name;
}

void ovs_tnl_free_linked_skbs(struct sk_buff *skb)
{
	while (skb) {
		struct sk_buff *next = skb->next;
		kfree_skb(skb);
		skb = next;
	}
}

int ovs_tnl_init(void)
{
	int i;

	port_table = kmalloc(PORT_TABLE_SIZE * sizeof(struct hlist_head *),
			     GFP_KERNEL);
	if (!port_table)
		return -ENOMEM;

	for (i = 0; i < PORT_TABLE_SIZE; i++)
		INIT_HLIST_HEAD(&port_table[i]);

	return 0;
}

void ovs_tnl_exit(void)
{
	kfree(port_table);
}
