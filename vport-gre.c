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

#include <linux/if.h>
#include <linux/skbuff.h>
#include <linux/ip.h>
#include <linux/if_tunnel.h>
#include <linux/if_vlan.h>
#include <linux/in.h>

#include <net/icmp.h>
#include <net/ip.h>
#include <net/protocol.h>

#include "datapath.h"
#include "tunnel.h"
#include "vport.h"

/*
 * The GRE header is composed of a series of sections: a base and then a variable
 * number of options.
 */
#define GRE_HEADER_SECTION 4

struct gre_base_hdr {
	__be16 flags;
	__be16 protocol;
};

static int gre_hdr_len(const struct ovs_key_ipv4_tunnel *tun_key)
{
	int len = GRE_HEADER_SECTION;

	if (tun_key->tun_flags & OVS_TNL_F_KEY)
		len += GRE_HEADER_SECTION;
	if (tun_key->tun_flags & OVS_TNL_F_CSUM)
		len += GRE_HEADER_SECTION;
	return len;
}

static int gre64_hdr_len(const struct ovs_key_ipv4_tunnel *tun_key)
{
	/* Set key for GRE64 tunnels, even when key if is zero. */
	int len = GRE_HEADER_SECTION +		/* GRE Hdr */
		  GRE_HEADER_SECTION +		/* GRE Key */
		  GRE_HEADER_SECTION;		/* GRE SEQ */

	if (tun_key->tun_flags & OVS_TNL_F_CSUM)
		len += GRE_HEADER_SECTION;

	return len;
}

/* Returns the least-significant 32 bits of a __be64. */
static __be32 be64_get_low32(__be64 x)
{
#ifdef __BIG_ENDIAN
	return (__force __be32)x;
#else
	return (__force __be32)((__force u64)x >> 32);
#endif
}

static __be32 be64_get_high32(__be64 x)
{
#ifdef __BIG_ENDIAN
	return (__force __be32)((__force u64)x >> 32);
#else
	return (__force __be32)x;
#endif
}

static void __gre_build_header(struct sk_buff *skb,
			       int tunnel_hlen,
			       bool is_gre64)
{
	const struct ovs_key_ipv4_tunnel *tun_key = OVS_CB(skb)->tun_key;
	__be32 *options = (__be32 *)(skb_network_header(skb) + tunnel_hlen
			- GRE_HEADER_SECTION);
	struct gre_base_hdr *greh = (struct gre_base_hdr *) skb_transport_header(skb);
	greh->protocol = htons(ETH_P_TEB);
	greh->flags = 0;

	/* Work backwards over the options so the checksum is last. */
	if (tun_key->tun_flags & OVS_TNL_F_KEY || is_gre64) {
		greh->flags |= GRE_KEY;
		if (is_gre64) {
			/* Set higher 32 bits to seq. */
			*options = be64_get_high32(tun_key->tun_id);
			options--;
			greh->flags |= GRE_SEQ;
		}
		*options = be64_get_low32(tun_key->tun_id);
		options--;
	}

	if (tun_key->tun_flags & OVS_TNL_F_CSUM) {
		greh->flags |= GRE_CSUM;
		*options = 0;
		*(__sum16 *)options = csum_fold(skb_checksum(skb,
						skb_transport_offset(skb),
						skb->len - skb_transport_offset(skb),
						0));
	}
}

static void gre_build_header(const struct vport *vport,
			     struct sk_buff *skb,
			     int tunnel_hlen)
{
	__gre_build_header(skb, tunnel_hlen, false);
}

static void gre64_build_header(const struct vport *vport,
			       struct sk_buff *skb,
			       int tunnel_hlen)
{
	__gre_build_header(skb, tunnel_hlen, true);
}

static __be64 key_to_tunnel_id(__be32 key, __be32 seq)
{
#ifdef __BIG_ENDIAN
	return (__force __be64)((__force u64)seq << 32 | (__force u32)key);
#else
	return (__force __be64)((__force u64)key << 32 | (__force u32)seq);
#endif
}

static int parse_header(struct iphdr *iph, __be16 *flags, __be64 *tun_id,
			bool *is_gre64)
{
	/* IP and ICMP protocol handlers check that the IHL is valid. */
	struct gre_base_hdr *greh = (struct gre_base_hdr *)((u8 *)iph + (iph->ihl << 2));
	__be32 *options = (__be32 *)(greh + 1);
	int hdr_len;

	*flags = greh->flags;

	if (unlikely(greh->flags & (GRE_VERSION | GRE_ROUTING)))
		return -EINVAL;

	if (unlikely(greh->protocol != htons(ETH_P_TEB)))
		return -EINVAL;

	hdr_len = GRE_HEADER_SECTION;

	if (greh->flags & GRE_CSUM) {
		hdr_len += GRE_HEADER_SECTION;
		options++;
	}

	if (greh->flags & GRE_KEY) {
		__be32 seq;
		__be32 gre_key;

		gre_key = *options;
		hdr_len += GRE_HEADER_SECTION;
		options++;

		if (greh->flags & GRE_SEQ) {
			seq = *options;
			*is_gre64 = true;
		} else {
			seq = 0;
			*is_gre64 = false;
		}
		*tun_id = key_to_tunnel_id(gre_key, seq);
	} else {
		*tun_id = 0;
		/* Ignore GRE seq if there is no key present. */
		*is_gre64 = false;
	}

	if (greh->flags & GRE_SEQ)
		hdr_len += GRE_HEADER_SECTION;

	return hdr_len;
}

static bool check_checksum(struct sk_buff *skb)
{
	struct iphdr *iph = ip_hdr(skb);
	struct gre_base_hdr *greh = (struct gre_base_hdr *)(iph + 1);
	__sum16 csum = 0;

	if (greh->flags & GRE_CSUM) {
		switch (skb->ip_summed) {
		case CHECKSUM_COMPLETE:
			csum = csum_fold(skb->csum);

			if (!csum)
				break;
			/* Fall through. */

		case CHECKSUM_NONE:
			skb->csum = 0;
			csum = __skb_checksum_complete(skb);
			skb->ip_summed = CHECKSUM_COMPLETE;
			break;
		}
	}

	return (csum == 0);
}

static u32 gre_flags_to_tunnel_flags(__be16 gre_flags, bool is_gre64)
{
	u32 tunnel_flags = 0;

	if (gre_flags & GRE_KEY || is_gre64)
		tunnel_flags = OVS_TNL_F_KEY;

	if (gre_flags & GRE_CSUM)
		tunnel_flags |= OVS_TNL_F_CSUM;

	return tunnel_flags;
}

/* Called with rcu_read_lock and BH disabled. */
static int gre_rcv(struct sk_buff *skb)
{
	struct ovs_net *ovs_net;
	struct vport *vport;
	int hdr_len;
	struct iphdr *iph;
	struct ovs_key_ipv4_tunnel tun_key;
	__be16 gre_flags;
	u32 tnl_flags;
	__be64 key;
	bool is_gre64;

	if (unlikely(!pskb_may_pull(skb, sizeof(struct gre_base_hdr) + ETH_HLEN)))
		goto error;
	if (unlikely(!check_checksum(skb)))
		goto error;

	hdr_len = parse_header(ip_hdr(skb), &gre_flags, &key, &is_gre64);
	if (unlikely(hdr_len < 0))
		goto error;

	ovs_net = net_generic(dev_net(skb->dev), ovs_net_id);
	if (is_gre64)
		vport = rcu_dereference(ovs_net->vport_net.gre64_vport);
	else
		vport = rcu_dereference(ovs_net->vport_net.gre_vport);
	if (unlikely(!vport))
		goto error;

	if (unlikely(!pskb_may_pull(skb, hdr_len + ETH_HLEN)))
		goto error;

	iph = ip_hdr(skb);
	tnl_flags = gre_flags_to_tunnel_flags(gre_flags, is_gre64);
	tnl_tun_key_init(&tun_key, iph, key, tnl_flags);

	skb_pull_rcsum(skb, hdr_len);

	ovs_tnl_rcv(vport, skb, &tun_key);
	return 0;

error:
	kfree_skb(skb);
	return 0;
}

static const struct net_protocol gre_protocol_handlers = {
	.handler	=	gre_rcv,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,32)
	.netns_ok	=	1,
#endif
};

static int gre_ports;
static int gre_init(void)
{
	int err;

	gre_ports++;
	if (gre_ports > 1)
		return 0;

	err = inet_add_protocol(&gre_protocol_handlers, IPPROTO_GRE);
	if (err)
		pr_warn("cannot register gre protocol handler\n");

	return err;
}

static void gre_exit(void)
{
	gre_ports--;
	if (gre_ports > 0)
		return;

	inet_del_protocol(&gre_protocol_handlers, IPPROTO_GRE);
}

static const char *gre_get_name(const struct vport *vport)
{
	return vport_priv(vport);
}

static struct vport *gre_create(const struct vport_parms *parms)
{
	struct net *net = ovs_dp_get_net(parms->dp);
	struct ovs_net *ovs_net;
	struct vport *vport;
	int err;

	err = gre_init();
	if (err)
		return ERR_PTR(err);

	ovs_net = net_generic(net, ovs_net_id);
	if (ovsl_dereference(ovs_net->vport_net.gre_vport)) {
		vport = ERR_PTR(-EEXIST);
		goto error;
	}

	vport = ovs_vport_alloc(IFNAMSIZ, &ovs_gre_vport_ops, parms);
	if (IS_ERR(vport))
		goto error;

	strncpy(vport_priv(vport), parms->name, IFNAMSIZ);
	rcu_assign_pointer(ovs_net->vport_net.gre_vport, vport);
	return vport;

error:
	gre_exit();
	return vport;
}

static void gre_tnl_destroy(struct vport *vport)
{
	struct net *net = ovs_dp_get_net(vport->dp);
	struct ovs_net *ovs_net;

	ovs_net = net_generic(net, ovs_net_id);

	rcu_assign_pointer(ovs_net->vport_net.gre_vport, NULL);
	ovs_vport_deferred_free(vport);
	gre_exit();
}

static int gre_tnl_send(struct vport *vport, struct sk_buff *skb)
{
	int hlen;

	if (unlikely(!OVS_CB(skb)->tun_key))
		return -EINVAL;

	hlen = gre_hdr_len(OVS_CB(skb)->tun_key);
	return ovs_tnl_send(vport, skb, IPPROTO_GRE, hlen, gre_build_header);
}

const struct vport_ops ovs_gre_vport_ops = {
	.type		= OVS_VPORT_TYPE_GRE,
	.create		= gre_create,
	.destroy	= gre_tnl_destroy,
	.get_name	= gre_get_name,
	.send		= gre_tnl_send,
};

/* GRE64 vport. */
static struct vport *gre64_create(const struct vport_parms *parms)
{
	struct net *net = ovs_dp_get_net(parms->dp);
	struct ovs_net *ovs_net;
	struct vport *vport;
	int err;

	err = gre_init();
	if (err)
		return ERR_PTR(err);

	ovs_net = net_generic(net, ovs_net_id);
	if (ovsl_dereference(ovs_net->vport_net.gre64_vport)) {
		vport = ERR_PTR(-EEXIST);
		goto error;
	}

	vport = ovs_vport_alloc(IFNAMSIZ, &ovs_gre64_vport_ops, parms);
	if (IS_ERR(vport))
		goto error;

	strncpy(vport_priv(vport), parms->name, IFNAMSIZ);
	rcu_assign_pointer(ovs_net->vport_net.gre64_vport, vport);
	return vport;
error:
	gre_exit();
	return vport;
}

static void gre64_tnl_destroy(struct vport *vport)
{
	struct net *net = ovs_dp_get_net(vport->dp);
	struct ovs_net *ovs_net;

	ovs_net = net_generic(net, ovs_net_id);

	rcu_assign_pointer(ovs_net->vport_net.gre64_vport, NULL);
	ovs_vport_deferred_free(vport);
	gre_exit();
}

static int gre64_tnl_send(struct vport *vport, struct sk_buff *skb)
{
	int hlen;

	if (unlikely(!OVS_CB(skb)->tun_key))
		return -EINVAL;

	hlen = gre64_hdr_len(OVS_CB(skb)->tun_key);
	return ovs_tnl_send(vport, skb, IPPROTO_GRE, hlen, gre64_build_header);
}

const struct vport_ops ovs_gre64_vport_ops = {
	.type		= OVS_VPORT_TYPE_GRE64,
	.create		= gre64_create,
	.destroy	= gre64_tnl_destroy,
	.get_name	= gre_get_name,
	.send		= gre64_tnl_send,
};
