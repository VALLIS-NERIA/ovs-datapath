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

#ifndef TUNNEL_H
#define TUNNEL_H 1

#include <linux/version.h>
#include <net/net_namespace.h>
#include <net/netns/generic.h>

#include "flow.h"
#include "openvswitch/tunnel.h"
#include "vport.h"

/*
 * The absolute minimum fragment size.  Note that there are many other
 * definitions of the minimum MTU.
 */
#define IP_MIN_MTU 68

/*
 * One of these goes in struct tnl_ops and in tnl_find_port().
 * These values are in the same namespace as other TNL_T_* values, so
 * only the least significant 10 bits are available to define protocol
 * identifiers.
 */
#define TNL_T_PROTO_GRE		0
#define TNL_T_PROTO_GRE64	1
#define TNL_T_PROTO_VXLAN	3

/* These flags are only needed when calling tnl_find_port(). */
#define TNL_T_KEY_EXACT		(1 << 10)
#define TNL_T_KEY_MATCH		(1 << 11)

/* Private flags not exposed to userspace in this form. */
#define TNL_F_IN_KEY_MATCH	(1 << 16) /* Store the key in tun_id to
					   * match in flow table. */
#define TNL_F_OUT_KEY_ACTION	(1 << 17) /* Get the key from a SET_TUNNEL
					   * action. */

/* All public tunnel flags. */
#define TNL_F_PUBLIC (TNL_F_CSUM | TNL_F_TOS_INHERIT | TNL_F_TTL_INHERIT | \
		      TNL_F_DF_DEFAULT | TNL_F_IPSEC)

/**
 * struct port_lookup_key - Tunnel port key, used as hash table key.
 * @in_key: Key to match on input, 0 for wildcard.
 * @net: Network namespace of the port.
 * @saddr: IPv4 source address to match, 0 to accept any source address.
 * @daddr: IPv4 destination of tunnel.
 * @tunnel_type: Set of TNL_T_* flags that define lookup.
 */
struct port_lookup_key {
	__be64 in_key;
#ifdef CONFIG_NET_NS
	struct net *net;
#endif
	__be32 saddr;
	__be32 daddr;
	u32    tunnel_type;
};

#define PORT_KEY_LEN	(offsetof(struct port_lookup_key, tunnel_type) + \
			 FIELD_SIZEOF(struct port_lookup_key, tunnel_type))

static inline struct net *port_key_get_net(const struct port_lookup_key *key)
{
	return read_pnet(&key->net);
}

static inline void port_key_set_net(struct port_lookup_key *key, struct net *net)
{
	write_pnet(&key->net, net);
}

/**
 * struct tnl_mutable_config - modifiable configuration for a tunnel.
 * @key: Used as key for tunnel port.  Configured via OVS_TUNNEL_ATTR_*
 * attributes.
 * @rcu: RCU callback head for deferred destruction.
 * @tunnel_hlen: Tunnel header length.
 * @out_key: Key to use on output, 0 if this tunnel has no fixed output key.
 * @flags: TNL_F_* flags.
 * @tos: IPv4 TOS value to use for tunnel, 0 if no fixed TOS.
 * @ttl: IPv4 TTL value to use for tunnel, 0 if no fixed TTL.
 */
struct tnl_mutable_config {
	struct port_lookup_key key;
	struct rcu_head rcu;

	/* Configured via OVS_TUNNEL_ATTR_* attributes. */
	__be64	out_key;
	u32	flags;
	u8	tos;
	u8	ttl;
	__be16	dst_port;

	/* Multicast configuration. */
	int	mlink;
};

struct tnl_ops {
	u32 tunnel_type;	/* Put the TNL_T_PROTO_* type in here. */
	u8 ipproto;		/* The IP protocol for the tunnel. */

	/*
	 * Returns the length of the tunnel header that will be added in
	 * build_header() (i.e. excludes the IP header).  Returns a negative
	 * error code if the configuration is invalid.
	 */
	int (*hdr_len)(const struct tnl_mutable_config *,
		       const struct ovs_key_ipv4_tunnel *);
	/*
	 * Returns a linked list of SKBs with tunnel headers (multiple
	 * packets may be generated in the event of fragmentation).  Space
	 * will have already been allocated at the start of the packet equal
	 * to sizeof(struct iphdr) + value returned by hdr_len().  The IP
	 * header will have already been constructed.
	 */
	struct sk_buff *(*build_header)(const struct vport *,
					 const struct tnl_mutable_config *,
					 struct dst_entry *, struct sk_buff *,
					 int tunnel_hlen);
};

struct tnl_vport {
	struct rcu_head rcu;
	struct hlist_node hash_node;

	char name[IFNAMSIZ];
	const struct tnl_ops *tnl_ops;

	struct tnl_mutable_config __rcu *mutable;
};

struct vport *ovs_tnl_create(const struct vport_parms *, const struct vport_ops *,
			     const struct tnl_ops *);
void ovs_tnl_destroy(struct vport *);

int ovs_tnl_set_options(struct vport *, struct nlattr *);
int ovs_tnl_get_options(const struct vport *, struct sk_buff *);

const char *ovs_tnl_get_name(const struct vport *vport);
int ovs_tnl_send(struct vport *vport, struct sk_buff *skb);
void ovs_tnl_rcv(struct vport *vport, struct sk_buff *skb);

struct vport *ovs_tnl_find_port(struct net *net, __be32 saddr, __be32 daddr,
				__be64 key, int tunnel_type,
				const struct tnl_mutable_config **mutable);
bool ovs_tnl_frag_needed(struct vport *vport,
			 const struct tnl_mutable_config *mutable,
			 struct sk_buff *skb, unsigned int mtu);
void ovs_tnl_free_linked_skbs(struct sk_buff *skb);

int ovs_tnl_init(void);
void ovs_tnl_exit(void);
static inline struct tnl_vport *tnl_vport_priv(const struct vport *vport)
{
	return vport_priv(vport);
}

static inline void tnl_tun_key_init(struct ovs_key_ipv4_tunnel *tun_key,
				    const struct iphdr *iph, __be64 tun_id, u32 tun_flags)
{
	tun_key->tun_id = tun_id;
	tun_key->ipv4_src = iph->saddr;
	tun_key->ipv4_dst = iph->daddr;
	tun_key->ipv4_tos = iph->tos;
	tun_key->ipv4_ttl = iph->ttl;
	tun_key->tun_flags = tun_flags;

	/* clear struct padding. */
	memset((unsigned char*) tun_key + OVS_TUNNEL_KEY_SIZE, 0,
	       sizeof(*tun_key) - OVS_TUNNEL_KEY_SIZE);
}

static inline void tnl_get_param(const struct tnl_mutable_config *mutable,
				 const struct ovs_key_ipv4_tunnel *tun_key,
				 u32 *flags,  __be64 *out_key)
{
	if (tun_key->ipv4_dst) {
		*flags = 0;

		if (tun_key->tun_flags & OVS_TNL_F_KEY)
			*flags = TNL_F_OUT_KEY_ACTION;
		if (tun_key->tun_flags & OVS_TNL_F_CSUM)
			*flags |= TNL_F_CSUM;
		*out_key = tun_key->tun_id;
	} else {
		*flags = mutable->flags;
		if (mutable->flags & TNL_F_OUT_KEY_ACTION)
			*out_key = tun_key->tun_id;
		else
			*out_key = mutable->out_key;
	}
}

#endif /* tunnel.h */
