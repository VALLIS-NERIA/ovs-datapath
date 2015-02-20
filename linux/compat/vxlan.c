/*
 * Copyright (c) 2007-2013 Nicira, Inc.
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
 *
 * This code is derived from kernel vxlan module.
 */

#ifndef USE_UPSTREAM_VXLAN

#include <linux/version.h>

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/skbuff.h>
#include <linux/rculist.h>
#include <linux/netdevice.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <linux/igmp.h>
#include <linux/etherdevice.h>
#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <linux/hash.h>
#include <linux/ethtool.h>
#include <net/arp.h>
#include <net/ndisc.h>
#include <net/ip.h>
#include <net/gre.h>
#include <net/ip_tunnels.h>
#include <net/icmp.h>
#include <net/udp.h>
#include <net/rtnetlink.h>
#include <net/route.h>
#include <net/dsfield.h>
#include <net/inet_ecn.h>
#include <net/net_namespace.h>
#include <net/netns/generic.h>
#include <net/vxlan.h>

#include "compat.h"
#include "datapath.h"
#include "gso.h"
#include "vlan.h"

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,18,0)
/* VXLAN protocol header */
struct vxlanhdr {
	__be32 vx_flags;
	__be32 vx_vni;
};
#endif

/* Callback from net/ipv4/udp.c to receive packets */
static int vxlan_udp_encap_recv(struct sock *sk, struct sk_buff *skb)
{
	struct vxlan_sock *vs;
	struct vxlanhdr *vxh;
	u32 flags, vni;
	struct vxlan_metadata md = {0};

	/* Need Vxlan and inner Ethernet header to be present */
	if (!pskb_may_pull(skb, VXLAN_HLEN))
		goto error;

	vxh = (struct vxlanhdr *)(udp_hdr(skb) + 1);
	flags = ntohl(vxh->vx_flags);
	vni = ntohl(vxh->vx_vni);

	if (flags & VXLAN_HF_VNI) {
		flags &= ~VXLAN_HF_VNI;
	} else {
		/* VNI flag always required to be set */
		goto bad_flags;
	}

	if (iptunnel_pull_header(skb, VXLAN_HLEN, htons(ETH_P_TEB)))
		goto drop;

	vs = rcu_dereference_sk_user_data(sk);
	if (!vs)
		goto drop;

	/* For backwards compatibility, only allow reserved fields to be
	* used by VXLAN extensions if explicitly requested.
	*/
	if ((flags & VXLAN_HF_GBP) && (vs->flags & VXLAN_F_GBP)) {
		struct vxlanhdr_gbp *gbp;

		gbp = (struct vxlanhdr_gbp *)vxh;
		md.gbp = ntohs(gbp->policy_id);

		if (gbp->dont_learn)
			md.gbp |= VXLAN_GBP_DONT_LEARN;

		if (gbp->policy_applied)
			md.gbp |= VXLAN_GBP_POLICY_APPLIED;

		flags &= ~VXLAN_GBP_USED_BITS;
	}

	if (flags || (vni & 0xff)) {
		/* If there are any unprocessed flags remaining treat
		* this as a malformed packet. This behavior diverges from
		* VXLAN RFC (RFC7348) which stipulates that bits in reserved
		* in reserved fields are to be ignored. The approach here
		* maintains compatbility with previous stack code, and also
		* is more robust and provides a little more security in
		* adding extensions to VXLAN.
		*/

		goto bad_flags;
	}

	md.vni = vxh->vx_vni;
	vs->rcv(vs, skb, &md);
	return 0;

drop:
	/* Consume bad packet */
	kfree_skb(skb);
	return 0;
bad_flags:
	pr_debug("invalid vxlan flags=%#x vni=%#x\n",
		 ntohl(vxh->vx_flags), ntohl(vxh->vx_vni));

error:
	/* Return non vxlan pkt */
	return 1;
}

static void vxlan_sock_put(struct sk_buff *skb)
{
	sock_put(skb->sk);
}

/* On transmit, associate with the tunnel socket */
static void vxlan_set_owner(struct sock *sk, struct sk_buff *skb)
{
	skb_orphan(skb);
	sock_hold(sk);
	skb->sk = sk;
	skb->destructor = vxlan_sock_put;
}

static void vxlan_gso(struct sk_buff *skb)
{
	int udp_offset = skb_transport_offset(skb);
	struct udphdr *uh;

	uh = udp_hdr(skb);
	uh->len = htons(skb->len - udp_offset);

	/* csum segment if tunnel sets skb with csum. */
	if (unlikely(uh->check)) {
		struct iphdr *iph = ip_hdr(skb);

		uh->check = ~csum_tcpudp_magic(iph->saddr, iph->daddr,
					       skb->len - udp_offset,
					       IPPROTO_UDP, 0);
		uh->check = csum_fold(skb_checksum(skb, udp_offset,
				      skb->len - udp_offset, 0));

		if (uh->check == 0)
			uh->check = CSUM_MANGLED_0;

	}
	skb->ip_summed = CHECKSUM_NONE;
}

static struct sk_buff *handle_offloads(struct sk_buff *skb)
{
	return ovs_iptunnel_handle_offloads(skb, false, vxlan_gso);
}

static void vxlan_build_gbp_hdr(struct vxlanhdr *vxh, u32 vxflags,
				struct vxlan_metadata *md)
{
	struct vxlanhdr_gbp *gbp;

	if (!md->gbp)
		return;

	gbp = (struct vxlanhdr_gbp *)vxh;
	vxh->vx_flags |= htonl(VXLAN_HF_GBP);

	if (md->gbp & VXLAN_GBP_DONT_LEARN)
		gbp->dont_learn = 1;

	if (md->gbp & VXLAN_GBP_POLICY_APPLIED)
		gbp->policy_applied = 1;

	gbp->policy_id = htons(md->gbp & VXLAN_GBP_ID_MASK);
}

int vxlan_xmit_skb(struct vxlan_sock *vs,
		   struct rtable *rt, struct sk_buff *skb,
		   __be32 src, __be32 dst, __u8 tos, __u8 ttl, __be16 df,
		   __be16 src_port, __be16 dst_port,
		   struct vxlan_metadata *md, bool xnet, u32 vxflags)
{
	struct vxlanhdr *vxh;
	struct udphdr *uh;
	int min_headroom;
	int err;

	min_headroom = LL_RESERVED_SPACE(rt_dst(rt).dev) + rt_dst(rt).header_len
			+ VXLAN_HLEN + sizeof(struct iphdr)
			+ (skb_vlan_tag_present(skb) ? VLAN_HLEN : 0);

	/* Need space for new headers (invalidates iph ptr) */
	err = skb_cow_head(skb, min_headroom);
	if (unlikely(err)) {
		kfree_skb(skb);
		return err;
	}

	if (skb_vlan_tag_present(skb)) {
		if (unlikely(!vlan_insert_tag_set_proto(skb,
							skb->vlan_proto,
							skb_vlan_tag_get(skb))))
			return -ENOMEM;

		vlan_set_tci(skb, 0);
	}

	skb_reset_inner_headers(skb);

	vxh = (struct vxlanhdr *) __skb_push(skb, sizeof(*vxh));
	vxh->vx_flags = htonl(VXLAN_HF_VNI);
	vxh->vx_vni = md->vni;

	if (vxflags & VXLAN_F_GBP)
		vxlan_build_gbp_hdr(vxh, vxflags, md);

	__skb_push(skb, sizeof(*uh));
	skb_reset_transport_header(skb);
	uh = udp_hdr(skb);

	uh->dest = dst_port;
	uh->source = src_port;

	uh->len = htons(skb->len);
	uh->check = 0;

	vxlan_set_owner(vs->sock->sk, skb);

	skb = handle_offloads(skb);
	if (IS_ERR(skb))
		return PTR_ERR(skb);

	return iptunnel_xmit(vs->sock->sk, rt, skb, src, dst, IPPROTO_UDP,
			     tos, ttl, df, xnet);
}

static void rcu_free_vs(struct rcu_head *rcu)
{
	struct vxlan_sock *vs = container_of(rcu, struct vxlan_sock, rcu);

	kfree(vs);
}

static void vxlan_del_work(struct work_struct *work)
{
	struct vxlan_sock *vs = container_of(work, struct vxlan_sock, del_work);

	sk_release_kernel(vs->sock->sk);
	call_rcu(&vs->rcu, rcu_free_vs);
}

static struct vxlan_sock *vxlan_socket_create(struct net *net, __be16 port,
					      vxlan_rcv_t *rcv, void *data, u32 flags)
{
	struct vxlan_sock *vs;
	struct sock *sk;
	struct sockaddr_in vxlan_addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_ANY),
		.sin_port = port,
	};
	int rc;

	vs = kmalloc(sizeof(*vs), GFP_KERNEL);
	if (!vs) {
		pr_debug("memory alocation failure\n");
		return ERR_PTR(-ENOMEM);
	}

	INIT_WORK(&vs->del_work, vxlan_del_work);

	/* Create UDP socket for encapsulation receive. */
	rc = sock_create_kern(AF_INET, SOCK_DGRAM, IPPROTO_UDP, &vs->sock);
	if (rc < 0) {
		pr_debug("UDP socket create failed\n");
		kfree(vs);
		return ERR_PTR(rc);
	}

	/* Put in proper namespace */
	sk = vs->sock->sk;
	sk_change_net(sk, net);

	rc = kernel_bind(vs->sock, (struct sockaddr *) &vxlan_addr,
			sizeof(vxlan_addr));
	if (rc < 0) {
		pr_debug("bind for UDP socket %pI4:%u (%d)\n",
				&vxlan_addr.sin_addr, ntohs(vxlan_addr.sin_port), rc);
		sk_release_kernel(sk);
		kfree(vs);
		return ERR_PTR(rc);
	}
	vs->rcv = rcv;
	vs->data = data;
	vs->flags = (flags & VXLAN_F_RCV_FLAGS);

	/* Disable multicast loopback */
	inet_sk(sk)->mc_loop = 0;
	rcu_assign_sk_user_data(vs->sock->sk, vs);

	/* Mark socket as an encapsulation socket. */
	udp_sk(sk)->encap_type = 1;
	udp_sk(sk)->encap_rcv = vxlan_udp_encap_recv;
	udp_encap_enable();
	return vs;
}

struct vxlan_sock *vxlan_sock_add(struct net *net, __be16 port,
				  vxlan_rcv_t *rcv, void *data,
				  bool no_share, u32 flags)
{
	return vxlan_socket_create(net, port, rcv, data, flags);
}

void vxlan_sock_release(struct vxlan_sock *vs)
{
	ASSERT_OVSL();
	rcu_assign_sk_user_data(vs->sock->sk, NULL);

	queue_work(system_wq, &vs->del_work);
}

#endif /* !USE_UPSTREAM_VXLAN */
