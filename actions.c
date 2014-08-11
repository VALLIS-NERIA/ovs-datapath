/*
 * Copyright (c) 2007-2014 Nicira, Inc.
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

#include <linux/skbuff.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/openvswitch.h>
#include <linux/sctp.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/in6.h>
#include <linux/if_arp.h>
#include <linux/if_vlan.h>
#include <net/ip.h>
#include <net/ipv6.h>
#include <net/checksum.h>
#include <net/dsfield.h>
#include <net/sctp/checksum.h>

#include "datapath.h"
#include "gso.h"
#include "mpls.h"
#include "vlan.h"
#include "vport.h"

static void flow_key_set_priority(struct sk_buff *skb, u32 priority)
{
	OVS_CB(skb)->pkt_key->phy.priority = priority;
}

static void flow_key_set_skb_mark(struct sk_buff *skb, u32 skb_mark)
{
	OVS_CB(skb)->pkt_key->phy.skb_mark = skb_mark;
}

static void flow_key_set_eth_src(struct sk_buff *skb, const u8 addr[])
{
	ether_addr_copy(OVS_CB(skb)->pkt_key->eth.src, addr);
}

static void flow_key_set_eth_dst(struct sk_buff *skb, const u8 addr[])
{
	ether_addr_copy(OVS_CB(skb)->pkt_key->eth.dst, addr);
}

static void flow_key_set_vlan_tci(struct sk_buff *skb, __be16 tci)
{
	OVS_CB(skb)->pkt_key->eth.tci = tci;
}

static void flow_key_set_mpls_top_lse(struct sk_buff *skb, __be32 top_lse)
{
	OVS_CB(skb)->pkt_key->mpls.top_lse = top_lse;
}

static void flow_key_set_ipv4_src(struct sk_buff *skb, __be32 addr)
{
	OVS_CB(skb)->pkt_key->ipv4.addr.src = addr;
}

static void flow_key_set_ipv4_dst(struct sk_buff *skb, __be32 addr)
{
	OVS_CB(skb)->pkt_key->ipv4.addr.src = addr;
}

static void flow_key_set_ip_tos(struct sk_buff *skb, u8 tos)
{
	OVS_CB(skb)->pkt_key->ip.tos = tos;
}

static void flow_key_set_ip_ttl(struct sk_buff *skb, u8 ttl)
{
	OVS_CB(skb)->pkt_key->ip.ttl = ttl;
}

static void flow_key_set_ipv6_src(struct sk_buff *skb,
				  const __be32 addr[4])
{
	memcpy(&OVS_CB(skb)->pkt_key->ipv6.addr.src, addr, sizeof(__be32[4]));
}

static void flow_key_set_ipv6_dst(struct sk_buff *skb,
				  const __be32 addr[4])
{
	memcpy(&OVS_CB(skb)->pkt_key->ipv6.addr.dst, addr, sizeof(__be32[4]));
}

static void flow_key_set_ipv6_fl(struct sk_buff *skb,
				 const struct ipv6hdr *nh)
{
	OVS_CB(skb)->pkt_key->ipv6.label = *(__be32 *)nh &
					   htonl(IPV6_FLOWINFO_FLOWLABEL);
}

static void flow_key_set_tp_src(struct sk_buff *skb, __be16 port)
{
	OVS_CB(skb)->pkt_key->tp.src = port;
}

static void flow_key_set_tp_dst(struct sk_buff *skb, __be16 port)
{
	OVS_CB(skb)->pkt_key->tp.dst = port;
}

static void invalidate_skb_flow_key(struct sk_buff *skb)
{
	OVS_CB(skb)->pkt_key->eth.type = htons(0);
}

static bool is_skb_flow_key_valid(struct sk_buff *skb)
{
	return !!OVS_CB(skb)->pkt_key->eth.type;
}

static int do_execute_actions(struct datapath *dp, struct sk_buff *skb,
			      const struct nlattr *attr, int len);

static int make_writable(struct sk_buff *skb, int write_len)
{
	if (!skb_cloned(skb) || skb_clone_writable(skb, write_len))
		return 0;

	return pskb_expand_head(skb, 0, 0, GFP_ATOMIC);
}

/* The end of the mac header.
 *
 * For non-MPLS skbs this will correspond to the network header.
 * For MPLS skbs it will be before the network_header as the MPLS
 * label stack lies between the end of the mac header and the network
 * header. That is, for MPLS skbs the end of the mac header
 * is the top of the MPLS label stack.
 */
static unsigned char *mac_header_end(const struct sk_buff *skb)
{
	return skb_mac_header(skb) + skb->mac_len;
}

static int push_mpls(struct sk_buff *skb,
		     const struct ovs_action_push_mpls *mpls)
{
	__be32 *new_mpls_lse;
	struct ethhdr *hdr;

	if (skb_cow_head(skb, MPLS_HLEN) < 0)
		return -ENOMEM;

	skb_push(skb, MPLS_HLEN);
	memmove(skb_mac_header(skb) - MPLS_HLEN, skb_mac_header(skb),
		skb->mac_len);
	skb_reset_mac_header(skb);

	new_mpls_lse = (__be32 *)mac_header_end(skb);
	*new_mpls_lse = mpls->mpls_lse;

	if (skb->ip_summed == CHECKSUM_COMPLETE)
		skb->csum = csum_add(skb->csum, csum_partial(new_mpls_lse,
							     MPLS_HLEN, 0));

	hdr = eth_hdr(skb);
	hdr->h_proto = mpls->mpls_ethertype;
	if (!ovs_skb_get_inner_protocol(skb))
		ovs_skb_set_inner_protocol(skb, skb->protocol);
	skb->protocol = mpls->mpls_ethertype;
	invalidate_skb_flow_key(skb);
	return 0;
}

static int pop_mpls(struct sk_buff *skb, const __be16 ethertype)
{
	struct ethhdr *hdr;
	int err;

	err = make_writable(skb, skb->mac_len + MPLS_HLEN);
	if (unlikely(err))
		return err;

	if (skb->ip_summed == CHECKSUM_COMPLETE)
		skb->csum = csum_sub(skb->csum,
				     csum_partial(mac_header_end(skb),
						  MPLS_HLEN, 0));

	memmove(skb_mac_header(skb) + MPLS_HLEN, skb_mac_header(skb),
		skb->mac_len);

	__skb_pull(skb, MPLS_HLEN);
	skb_reset_mac_header(skb);

	/* mac_header_end() is used to locate the ethertype
	 * field correctly in the presence of VLAN tags.
	 */
	hdr = (struct ethhdr *)(mac_header_end(skb) - ETH_HLEN);
	hdr->h_proto = ethertype;
	if (eth_p_mpls(skb->protocol))
		skb->protocol = ethertype;
	invalidate_skb_flow_key(skb);
	return 0;
}

static int set_mpls(struct sk_buff *skb, const __be32 *mpls_lse)
{
	__be32 *stack = (__be32 *)mac_header_end(skb);
	int err;

	err = make_writable(skb, skb->mac_len + MPLS_HLEN);
	if (unlikely(err))
		return err;

	if (skb->ip_summed == CHECKSUM_COMPLETE) {
		__be32 diff[] = { ~(*stack), *mpls_lse };
		skb->csum = ~csum_partial((char *)diff, sizeof(diff),
					  ~skb->csum);
	}

	*stack = *mpls_lse;
	flow_key_set_mpls_top_lse(skb, *stack);
	return 0;
}

/* remove VLAN header from packet and update csum accordingly. */
static int __pop_vlan_tci(struct sk_buff *skb, __be16 *current_tci)
{
	struct vlan_hdr *vhdr;
	int err;

	err = make_writable(skb, VLAN_ETH_HLEN);
	if (unlikely(err))
		return err;

	if (skb->ip_summed == CHECKSUM_COMPLETE)
		skb->csum = csum_sub(skb->csum, csum_partial(skb->data
					+ (2 * ETH_ALEN), VLAN_HLEN, 0));

	vhdr = (struct vlan_hdr *)(skb->data + ETH_HLEN);
	*current_tci = vhdr->h_vlan_TCI;

	memmove(skb->data + VLAN_HLEN, skb->data, 2 * ETH_ALEN);
	__skb_pull(skb, VLAN_HLEN);

	vlan_set_encap_proto(skb, vhdr);
	skb->mac_header += VLAN_HLEN;
	/* Update mac_len for subsequent MPLS actions */
	skb->mac_len -= VLAN_HLEN;

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
		   skb->len < VLAN_ETH_HLEN)) {
		flow_key_set_vlan_tci(skb, 0);
		return 0;
	}

	invalidate_skb_flow_key(skb);
	err = __pop_vlan_tci(skb, &tci);
	if (unlikely(err))
		return err;

	__vlan_hwaccel_put_tag(skb, htons(ETH_P_8021Q), ntohs(tci));
	return 0;
}

static int push_vlan(struct sk_buff *skb, const struct ovs_action_push_vlan *vlan)
{
	if (unlikely(vlan_tx_tag_present(skb))) {
		u16 current_tag;

		/* push down current VLAN tag */
		current_tag = vlan_tx_tag_get(skb);

		if (!__vlan_put_tag(skb, skb->vlan_proto, current_tag))
			return -ENOMEM;

		/* Update mac_len for subsequent MPLS actions */
		skb->mac_len += VLAN_HLEN;

		if (skb->ip_summed == CHECKSUM_COMPLETE)
			skb->csum = csum_add(skb->csum, csum_partial(skb->data
					+ (2 * ETH_ALEN), VLAN_HLEN, 0));

		invalidate_skb_flow_key(skb);
	} else {
		flow_key_set_vlan_tci(skb,  vlan->vlan_tci);
	}
	__vlan_hwaccel_put_tag(skb, vlan->vlan_tpid, ntohs(vlan->vlan_tci) & ~VLAN_TAG_PRESENT);
	return 0;
}

static int set_eth_addr(struct sk_buff *skb,
			const struct ovs_key_ethernet *eth_key)
{
	int err;
	err = make_writable(skb, ETH_HLEN);
	if (unlikely(err))
		return err;

	skb_postpull_rcsum(skb, eth_hdr(skb), ETH_ALEN * 2);

	ether_addr_copy(eth_hdr(skb)->h_source, eth_key->eth_src);
	ether_addr_copy(eth_hdr(skb)->h_dest, eth_key->eth_dst);

	ovs_skb_postpush_rcsum(skb, eth_hdr(skb), ETH_ALEN * 2);

	flow_key_set_eth_src(skb, eth_key->eth_src);
	flow_key_set_eth_dst(skb, eth_key->eth_dst);
	return 0;
}

static void set_ip_addr(struct sk_buff *skb, struct iphdr *nh,
			__be32 *addr, __be32 new_addr)
{
	int transport_len = skb->len - skb_transport_offset(skb);

	if (nh->protocol == IPPROTO_TCP) {
		if (likely(transport_len >= sizeof(struct tcphdr)))
			inet_proto_csum_replace4(&tcp_hdr(skb)->check, skb,
						 *addr, new_addr, 1);
	} else if (nh->protocol == IPPROTO_UDP) {
		if (likely(transport_len >= sizeof(struct udphdr))) {
			struct udphdr *uh = udp_hdr(skb);

			if (uh->check || skb->ip_summed == CHECKSUM_PARTIAL) {
				inet_proto_csum_replace4(&uh->check, skb,
							 *addr, new_addr, 1);
				if (!uh->check)
					uh->check = CSUM_MANGLED_0;
			}
		}
	}

	csum_replace4(&nh->check, *addr, new_addr);
	skb_clear_hash(skb);
	*addr = new_addr;
}

static void update_ipv6_checksum(struct sk_buff *skb, u8 l4_proto,
				 __be32 addr[4], const __be32 new_addr[4])
{
	int transport_len = skb->len - skb_transport_offset(skb);

	if (l4_proto == IPPROTO_TCP) {
		if (likely(transport_len >= sizeof(struct tcphdr)))
			inet_proto_csum_replace16(&tcp_hdr(skb)->check, skb,
						  addr, new_addr, 1);
	} else if (l4_proto == IPPROTO_UDP) {
		if (likely(transport_len >= sizeof(struct udphdr))) {
			struct udphdr *uh = udp_hdr(skb);

			if (uh->check || skb->ip_summed == CHECKSUM_PARTIAL) {
				inet_proto_csum_replace16(&uh->check, skb,
							  addr, new_addr, 1);
				if (!uh->check)
					uh->check = CSUM_MANGLED_0;
			}
		}
	}
}

static void set_ipv6_addr(struct sk_buff *skb, u8 l4_proto,
			  __be32 addr[4], const __be32 new_addr[4],
			  bool recalculate_csum)
{
	if (likely(recalculate_csum))
		update_ipv6_checksum(skb, l4_proto, addr, new_addr);

	skb_clear_hash(skb);
	memcpy(addr, new_addr, sizeof(__be32[4]));
}

static void set_ipv6_tc(struct ipv6hdr *nh, u8 tc)
{
	nh->priority = tc >> 4;
	nh->flow_lbl[0] = (nh->flow_lbl[0] & 0x0F) | ((tc & 0x0F) << 4);
}

static void set_ipv6_fl(struct ipv6hdr *nh, u32 fl)
{
	nh->flow_lbl[0] = (nh->flow_lbl[0] & 0xF0) | (fl & 0x000F0000) >> 16;
	nh->flow_lbl[1] = (fl & 0x0000FF00) >> 8;
	nh->flow_lbl[2] = fl & 0x000000FF;
}

static void set_ip_ttl(struct sk_buff *skb, struct iphdr *nh, u8 new_ttl)
{
	csum_replace2(&nh->check, htons(nh->ttl << 8), htons(new_ttl << 8));
	nh->ttl = new_ttl;
}

static int set_ipv4(struct sk_buff *skb, const struct ovs_key_ipv4 *ipv4_key)
{
	struct iphdr *nh;
	int err;

	err = make_writable(skb, skb_network_offset(skb) +
				 sizeof(struct iphdr));
	if (unlikely(err))
		return err;

	nh = ip_hdr(skb);

	if (ipv4_key->ipv4_src != nh->saddr) {
		set_ip_addr(skb, nh, &nh->saddr, ipv4_key->ipv4_src);
		flow_key_set_ipv4_src(skb, ipv4_key->ipv4_src);
	}

	if (ipv4_key->ipv4_dst != nh->daddr) {
		set_ip_addr(skb, nh, &nh->daddr, ipv4_key->ipv4_dst);
		flow_key_set_ipv4_dst(skb, ipv4_key->ipv4_dst);
	}

	if (ipv4_key->ipv4_tos != nh->tos) {
		ipv4_change_dsfield(nh, 0, ipv4_key->ipv4_tos);
		flow_key_set_ip_tos(skb, nh->tos);
	}

	if (ipv4_key->ipv4_ttl != nh->ttl) {
		set_ip_ttl(skb, nh, ipv4_key->ipv4_ttl);
		flow_key_set_ip_ttl(skb, ipv4_key->ipv4_ttl);
	}

	return 0;
}

static int set_ipv6(struct sk_buff *skb, const struct ovs_key_ipv6 *ipv6_key)
{
	struct ipv6hdr *nh;
	int err;
	__be32 *saddr;
	__be32 *daddr;

	err = make_writable(skb, skb_network_offset(skb) +
			    sizeof(struct ipv6hdr));
	if (unlikely(err))
		return err;

	nh = ipv6_hdr(skb);
	saddr = (__be32 *)&nh->saddr;
	daddr = (__be32 *)&nh->daddr;

	if (memcmp(ipv6_key->ipv6_src, saddr, sizeof(ipv6_key->ipv6_src))) {
		set_ipv6_addr(skb, ipv6_key->ipv6_proto, saddr,
			      ipv6_key->ipv6_src, true);
		flow_key_set_ipv6_src(skb, ipv6_key->ipv6_src);
	}

	if (memcmp(ipv6_key->ipv6_dst, daddr, sizeof(ipv6_key->ipv6_dst))) {
		unsigned int offset = 0;
		int flags = OVS_IP6T_FH_F_SKIP_RH;
		bool recalc_csum = true;

		if (ipv6_ext_hdr(nh->nexthdr))
			recalc_csum = ipv6_find_hdr(skb, &offset,
						    NEXTHDR_ROUTING, NULL,
						    &flags) != NEXTHDR_ROUTING;

		set_ipv6_addr(skb, ipv6_key->ipv6_proto, daddr,
			      ipv6_key->ipv6_dst, recalc_csum);
		flow_key_set_ipv6_dst(skb, ipv6_key->ipv6_dst);
	}

	set_ipv6_tc(nh, ipv6_key->ipv6_tclass);
	flow_key_set_ip_tos(skb, ipv6_get_dsfield(nh));

	set_ipv6_fl(nh, ntohl(ipv6_key->ipv6_label));
	flow_key_set_ipv6_fl(skb, nh);

	nh->hop_limit = ipv6_key->ipv6_hlimit;
	flow_key_set_ip_ttl(skb, ipv6_key->ipv6_hlimit);
	return 0;
}

/* Must follow make_writable() since that can move the skb data. */
static void set_tp_port(struct sk_buff *skb, __be16 *port,
			 __be16 new_port, __sum16 *check)
{
	inet_proto_csum_replace2(check, skb, *port, new_port, 0);
	*port = new_port;
	skb_clear_hash(skb);
}

static void set_udp_port(struct sk_buff *skb, __be16 *port, __be16 new_port)
{
	struct udphdr *uh = udp_hdr(skb);

	if (uh->check && skb->ip_summed != CHECKSUM_PARTIAL) {
		set_tp_port(skb, port, new_port, &uh->check);

		if (!uh->check)
			uh->check = CSUM_MANGLED_0;
	} else {
		*port = new_port;
		skb_clear_hash(skb);
	}
}

static int set_udp(struct sk_buff *skb, const struct ovs_key_udp *udp_port_key)
{
	struct udphdr *uh;
	int err;

	err = make_writable(skb, skb_transport_offset(skb) +
				 sizeof(struct udphdr));
	if (unlikely(err))
		return err;

	uh = udp_hdr(skb);
	if (udp_port_key->udp_src != uh->source) {
		set_udp_port(skb, &uh->source, udp_port_key->udp_src);
		flow_key_set_tp_src(skb, udp_port_key->udp_src);
	}

	if (udp_port_key->udp_dst != uh->dest) {
		set_udp_port(skb, &uh->dest, udp_port_key->udp_dst);
		flow_key_set_tp_dst(skb, udp_port_key->udp_dst);
	}

	return 0;
}

static int set_tcp(struct sk_buff *skb, const struct ovs_key_tcp *tcp_port_key)
{
	struct tcphdr *th;
	int err;

	err = make_writable(skb, skb_transport_offset(skb) +
				 sizeof(struct tcphdr));
	if (unlikely(err))
		return err;

	th = tcp_hdr(skb);
	if (tcp_port_key->tcp_src != th->source) {
		set_tp_port(skb, &th->source, tcp_port_key->tcp_src, &th->check);
		flow_key_set_tp_src(skb, tcp_port_key->tcp_src);
	}

	if (tcp_port_key->tcp_dst != th->dest) {
		set_tp_port(skb, &th->dest, tcp_port_key->tcp_dst, &th->check);
		flow_key_set_tp_dst(skb, tcp_port_key->tcp_dst);
	}

	return 0;
}

static int set_sctp(struct sk_buff *skb,
		    const struct ovs_key_sctp *sctp_port_key)
{
	struct sctphdr *sh;
	int err;
	unsigned int sctphoff = skb_transport_offset(skb);

	err = make_writable(skb, sctphoff + sizeof(struct sctphdr));
	if (unlikely(err))
		return err;

	sh = sctp_hdr(skb);
	if (sctp_port_key->sctp_src != sh->source ||
	    sctp_port_key->sctp_dst != sh->dest) {
		__le32 old_correct_csum, new_csum, old_csum;

		old_csum = sh->checksum;
		old_correct_csum = sctp_compute_cksum(skb, sctphoff);

		sh->source = sctp_port_key->sctp_src;
		sh->dest = sctp_port_key->sctp_dst;

		new_csum = sctp_compute_cksum(skb, sctphoff);

		/* Carry any checksum errors through. */
		sh->checksum = old_csum ^ old_correct_csum ^ new_csum;

		skb_clear_hash(skb);
		flow_key_set_tp_src(skb, sctp_port_key->sctp_src);
		flow_key_set_tp_dst(skb, sctp_port_key->sctp_dst);
	}

	return 0;
}

static void do_output(struct datapath *dp, struct sk_buff *skb, int out_port)
{
	struct vport *vport = ovs_vport_rcu(dp, out_port);

	if (likely(vport))
		ovs_vport_send(vport, skb);
	else
		kfree_skb(skb);
}

static int output_userspace(struct datapath *dp, struct sk_buff *skb,
			    const struct nlattr *attr)
{
	struct dp_upcall_info upcall;
	const struct nlattr *a;
	int rem;

	upcall.cmd = OVS_PACKET_CMD_ACTION;
	upcall.userdata = NULL;
	upcall.portid = 0;

	for (a = nla_data(attr), rem = nla_len(attr); rem > 0;
		 a = nla_next(a, &rem)) {
		switch (nla_type(a)) {
		case OVS_USERSPACE_ATTR_USERDATA:
			upcall.userdata = a;
			break;

		case OVS_USERSPACE_ATTR_PID:
			upcall.portid = nla_get_u32(a);
			break;
		}
	}

	return ovs_dp_upcall(dp, skb, &upcall);
}

static bool last_action(const struct nlattr *a, int rem)
{
	return a->nla_len == rem;
}

static int sample(struct datapath *dp, struct sk_buff *skb,
		  const struct nlattr *attr)
{
	struct sw_flow_key sample_key;
	const struct nlattr *acts_list = NULL;
	const struct nlattr *a;
	struct sk_buff *sample_skb;
	int rem;

	for (a = nla_data(attr), rem = nla_len(attr); rem > 0;
		 a = nla_next(a, &rem)) {
		switch (nla_type(a)) {
		case OVS_SAMPLE_ATTR_PROBABILITY:
			if (prandom_u32() >= nla_get_u32(a))
				return 0;
			break;

		case OVS_SAMPLE_ATTR_ACTIONS:
			acts_list = a;
			break;
		}
	}

	rem = nla_len(acts_list);
	a = nla_data(acts_list);

	/* Actions list is either empty or only contains a single user-space
	 * action, the latter being a special case as it is the only known
	 * usage of the sample action.
	 * In these special cases don't clone the skb as there are no
	 * side-effects in the nested actions.
	 * Otherwise, clone in case the nested actions have side effects. */
	if (likely(rem == 0 ||
		   (nla_type(a) == OVS_ACTION_ATTR_USERSPACE &&
		    last_action(a, rem)))) {
		sample_skb = skb;
		skb_get(skb);
	} else {
		sample_skb = skb_clone(skb, GFP_ATOMIC);
		if (!sample_skb)
			/* Skip the sample action when out of memory. */
			return 0;

		sample_key = *OVS_CB(skb)->pkt_key;
		OVS_CB(sample_skb)->pkt_key = &sample_key;
	}

	/* Note that do_execute_actions() never consumes skb.
	 * In the case where skb has been cloned above it is the clone that
	 * is consumed.  Otherwise the skb_get(skb) call prevents
	 * consumption by do_execute_actions(). Thus, it is safe to simply
	 * return the error code and let the caller (also
	 * do_execute_actions()) free skb on error. */
	return do_execute_actions(dp, sample_skb, a, rem);
}

static void execute_hash(struct sk_buff *skb, const struct nlattr *attr)
{
	struct sw_flow_key *key = OVS_CB(skb)->pkt_key;
	struct ovs_action_hash *hash_act = nla_data(attr);
	u32 hash = 0;

	/* OVS_HASH_ALG_L4 is the only possible hash algorithm.  */
	hash = skb_get_hash(skb);
	hash = jhash_1word(hash, hash_act->hash_basis);
	if (!hash)
		hash = 0x1;

	key->ovs_flow_hash = hash;
}

static int execute_set_action(struct sk_buff *skb,
				 const struct nlattr *nested_attr)
{
	int err = 0;

	switch (nla_type(nested_attr)) {
	case OVS_KEY_ATTR_PRIORITY:
		skb->priority = nla_get_u32(nested_attr);
		flow_key_set_priority(skb, skb->priority);
		break;

	case OVS_KEY_ATTR_SKB_MARK:
		skb->mark = nla_get_u32(nested_attr);
		flow_key_set_skb_mark(skb, skb->mark);
		break;

	case OVS_KEY_ATTR_TUNNEL_INFO:
		OVS_CB(skb)->egress_tun_info = nla_data(nested_attr);
		break;

	case OVS_KEY_ATTR_ETHERNET:
		err = set_eth_addr(skb, nla_data(nested_attr));
		break;

	case OVS_KEY_ATTR_IPV4:
		err = set_ipv4(skb, nla_data(nested_attr));
		break;

	case OVS_KEY_ATTR_IPV6:
		err = set_ipv6(skb, nla_data(nested_attr));
		break;

	case OVS_KEY_ATTR_TCP:
		err = set_tcp(skb, nla_data(nested_attr));
		break;

	case OVS_KEY_ATTR_UDP:
		err = set_udp(skb, nla_data(nested_attr));
		break;

	case OVS_KEY_ATTR_SCTP:
		err = set_sctp(skb, nla_data(nested_attr));
		break;

	case OVS_KEY_ATTR_MPLS:
		err = set_mpls(skb, nla_data(nested_attr));
		break;
	}

	return err;
}

static void flow_key_clone_recirc(struct sk_buff *skb, u32 recirc_id,
				  struct sw_flow_key *recirc_key)
{
	*recirc_key = *OVS_CB(skb)->pkt_key;
	recirc_key->recirc_id = recirc_id;
	OVS_CB(skb)->pkt_key = recirc_key;
}

static void flow_key_set_recirc_id(struct sk_buff *skb, u32 recirc_id)
{
	OVS_CB(skb)->pkt_key->recirc_id = recirc_id;
}

static int execute_recirc(struct datapath *dp, struct sk_buff *skb,
			  const struct nlattr *a, int rem)
{
	struct sw_flow_key recirc_key;
	int err;

	if (!last_action(a, rem)) {
		/* Recirc action is the not the last action
		 * of the action list. */
		skb = skb_clone(skb, GFP_ATOMIC);

		/* Skip the recirc action when out of memory, but
		 * continue on with the rest of the action list. */
		if (!skb)
			return 0;
	}

	if (is_skb_flow_key_valid(skb)) {
		if (!last_action(a, rem))
			flow_key_clone_recirc(skb, nla_get_u32(a), &recirc_key);
		else
			flow_key_set_recirc_id(skb, nla_get_u32(a));
	} else {
		struct sw_flow_key *pkt_key = OVS_CB(skb)->pkt_key;

		err = ovs_flow_key_extract_recirc(nla_get_u32(a), pkt_key,
						  skb, &recirc_key);
		if (err) {
			kfree_skb(skb);
			return err;
		}
	}

	ovs_dp_process_packet(skb, true);
	return 0;
}

/* Execute a list of actions against 'skb'. */
static int do_execute_actions(struct datapath *dp, struct sk_buff *skb,
			const struct nlattr *attr, int len)
{
	/* Every output action needs a separate clone of 'skb', but the common
	 * case is just a single output action, so that doing a clone and
	 * then freeing the original skbuff is wasteful.  So the following code
	 * is slightly obscure just to avoid that. */
	int prev_port = -1;
	const struct nlattr *a;
	int rem;

	for (a = attr, rem = len; rem > 0;
	     a = nla_next(a, &rem)) {
		int err = 0;

		if (unlikely(prev_port != -1)) {
			struct sk_buff *out_skb = skb_clone(skb, GFP_ATOMIC);

			if (out_skb)
				do_output(dp, out_skb, prev_port);

			prev_port = -1;
		}

		switch (nla_type(a)) {
		case OVS_ACTION_ATTR_OUTPUT:
			prev_port = nla_get_u32(a);
			break;

		case OVS_ACTION_ATTR_USERSPACE:
			output_userspace(dp, skb, a);
			break;

		case OVS_ACTION_ATTR_HASH:
			execute_hash(skb, a);
			break;

		case OVS_ACTION_ATTR_PUSH_MPLS:
			err = push_mpls(skb, nla_data(a));
			break;

		case OVS_ACTION_ATTR_POP_MPLS:
			err = pop_mpls(skb, nla_get_be16(a));
			break;

		case OVS_ACTION_ATTR_PUSH_VLAN:
			err = push_vlan(skb, nla_data(a));
			if (unlikely(err)) /* skb already freed. */
				return err;
			break;

		case OVS_ACTION_ATTR_POP_VLAN:
			err = pop_vlan(skb);
			break;

		case OVS_ACTION_ATTR_RECIRC:
			err = execute_recirc(dp, skb, a, rem);
			break;

		case OVS_ACTION_ATTR_SET:
			err = execute_set_action(skb, nla_data(a));
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

	if (prev_port != -1)
		do_output(dp, skb, prev_port);
	else
		consume_skb(skb);

	return 0;
}

/* We limit the number of times that we pass into execute_actions()
 * to avoid blowing out the stack in the event that we have a loop.
 *
 * Each loop adds some (estimated) cost to the kernel stack.
 * The loop terminates when the max cost is exceeded.
 * */
#define RECIRC_STACK_COST 1
#define DEFAULT_STACK_COST 4
/* Allow up to 4 regular services, and up to 3 recirculations */
#define MAX_STACK_COST (DEFAULT_STACK_COST * 4 + RECIRC_STACK_COST * 3)

struct loop_counter {
	u8 stack_cost;		/* loop stack cost. */
	bool looping;		/* Loop detected? */
};

static DEFINE_PER_CPU(struct loop_counter, loop_counters);

static int loop_suppress(struct datapath *dp, struct sw_flow_actions *actions)
{
	if (net_ratelimit())
		pr_warn("%s: flow loop detected, dropping\n",
				ovs_dp_name(dp));
	actions->actions_len = 0;
	return -ELOOP;
}

/* Execute a list of actions against 'skb'. */
int ovs_execute_actions(struct datapath *dp, struct sk_buff *skb, bool recirc)
{
	struct sw_flow_actions *acts = rcu_dereference(OVS_CB(skb)->flow->sf_acts);
	const u8 stack_cost = recirc ? RECIRC_STACK_COST : DEFAULT_STACK_COST;
	struct loop_counter *loop;
	int error;

	/* Check whether we've looped too much. */
	loop = &__get_cpu_var(loop_counters);
	loop->stack_cost += stack_cost;
	if (unlikely(loop->stack_cost > MAX_STACK_COST))
		loop->looping = true;
	if (unlikely(loop->looping)) {
		error = loop_suppress(dp, acts);
		kfree_skb(skb);
		goto out_loop;
	}

	error = do_execute_actions(dp, skb, acts->actions, acts->actions_len);

	/* Check whether sub-actions looped too much. */
	if (unlikely(loop->looping))
		error = loop_suppress(dp, acts);

out_loop:
	/* Decrement loop stack cost. */
	loop->stack_cost -= stack_cost;
	if (!loop->stack_cost)
		loop->looping = false;

	return error;
}
