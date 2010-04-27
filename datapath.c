/*
 * Copyright (c) 2007, 2008, 2009, 2010 Nicira Networks.
 * Distributed under the terms of the GNU GPL version 2.
 *
 * Significant portions of this file may be copied from parts of the Linux
 * kernel, by Linus Torvalds and others.
 */

/* Functions for managing the dp interface/device. */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/if_arp.h>
#include <linux/if_vlan.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/delay.h>
#include <linux/time.h>
#include <linux/etherdevice.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/percpu.h>
#include <linux/rcupdate.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/version.h>
#include <linux/ethtool.h>
#include <linux/random.h>
#include <linux/wait.h>
#include <asm/system.h>
#include <asm/div64.h>
#include <asm/bug.h>
#include <linux/netfilter_bridge.h>
#include <linux/netfilter_ipv4.h>
#include <linux/inetdevice.h>
#include <linux/list.h>
#include <linux/rculist.h>
#include <linux/workqueue.h>
#include <linux/dmi.h>
#include <net/inet_ecn.h>

#include "openvswitch/datapath-protocol.h"
#include "datapath.h"
#include "actions.h"
#include "flow.h"
#include "table.h"
#include "vport-internal_dev.h"

#include "compat.h"


int (*dp_ioctl_hook)(struct net_device *dev, struct ifreq *rq, int cmd);
EXPORT_SYMBOL(dp_ioctl_hook);

/* Datapaths.  Protected on the read side by rcu_read_lock, on the write side
 * by dp_mutex.
 *
 * dp_mutex nests inside the RTNL lock: if you need both you must take the RTNL
 * lock first.
 *
 * It is safe to access the datapath and dp_port structures with just
 * dp_mutex.
 */
static struct datapath *dps[ODP_MAX];
static DEFINE_MUTEX(dp_mutex);

/* Number of milliseconds between runs of the maintenance thread. */
#define MAINT_SLEEP_MSECS 1000

static int new_dp_port(struct datapath *, struct odp_port *, int port_no);

/* Must be called with rcu_read_lock or dp_mutex. */
struct datapath *get_dp(int dp_idx)
{
	if (dp_idx < 0 || dp_idx >= ODP_MAX)
		return NULL;
	return rcu_dereference(dps[dp_idx]);
}
EXPORT_SYMBOL_GPL(get_dp);

static struct datapath *get_dp_locked(int dp_idx)
{
	struct datapath *dp;

	mutex_lock(&dp_mutex);
	dp = get_dp(dp_idx);
	if (dp)
		mutex_lock(&dp->mutex);
	mutex_unlock(&dp_mutex);
	return dp;
}

/* Must be called with rcu_read_lock or RTNL lock. */
const char *dp_name(const struct datapath *dp)
{
	return vport_get_name(dp->ports[ODPP_LOCAL]->vport);
}

static inline size_t br_nlmsg_size(void)
{
	return NLMSG_ALIGN(sizeof(struct ifinfomsg))
	       + nla_total_size(IFNAMSIZ) /* IFLA_IFNAME */
	       + nla_total_size(MAX_ADDR_LEN) /* IFLA_ADDRESS */
	       + nla_total_size(4) /* IFLA_MASTER */
	       + nla_total_size(4) /* IFLA_MTU */
	       + nla_total_size(4) /* IFLA_LINK */
	       + nla_total_size(1); /* IFLA_OPERSTATE */
}

static int dp_fill_ifinfo(struct sk_buff *skb,
			  const struct dp_port *port,
			  int event, unsigned int flags)
{
	const struct datapath *dp = port->dp;
	int ifindex = vport_get_ifindex(port->vport);
	int iflink = vport_get_iflink(port->vport);
	struct ifinfomsg *hdr;
	struct nlmsghdr *nlh;

	if (ifindex < 0)
		return ifindex;

	if (iflink < 0)
		return iflink;

	nlh = nlmsg_put(skb, 0, 0, event, sizeof(*hdr), flags);
	if (nlh == NULL)
		return -EMSGSIZE;

	hdr = nlmsg_data(nlh);
	hdr->ifi_family = AF_BRIDGE;
	hdr->__ifi_pad = 0;
	hdr->ifi_type = ARPHRD_ETHER;
	hdr->ifi_index = ifindex;
	hdr->ifi_flags = vport_get_flags(port->vport);
	hdr->ifi_change = 0;

	NLA_PUT_STRING(skb, IFLA_IFNAME, vport_get_name(port->vport));
	NLA_PUT_U32(skb, IFLA_MASTER, vport_get_ifindex(dp->ports[ODPP_LOCAL]->vport));
	NLA_PUT_U32(skb, IFLA_MTU, vport_get_mtu(port->vport));
#ifdef IFLA_OPERSTATE
	NLA_PUT_U8(skb, IFLA_OPERSTATE,
		   vport_is_running(port->vport)
			? vport_get_operstate(port->vport)
			: IF_OPER_DOWN);
#endif

	NLA_PUT(skb, IFLA_ADDRESS, ETH_ALEN,
					vport_get_addr(port->vport));

	if (ifindex != iflink)
		NLA_PUT_U32(skb, IFLA_LINK,iflink);

	return nlmsg_end(skb, nlh);

nla_put_failure:
	nlmsg_cancel(skb, nlh);
	return -EMSGSIZE;
}

static void dp_ifinfo_notify(int event, struct dp_port *port)
{
	struct sk_buff *skb;
	int err = -ENOBUFS;

	skb = nlmsg_new(br_nlmsg_size(), GFP_KERNEL);
	if (skb == NULL)
		goto errout;

	err = dp_fill_ifinfo(skb, port, event, 0);
	if (err < 0) {
		/* -EMSGSIZE implies BUG in br_nlmsg_size() */
		WARN_ON(err == -EMSGSIZE);
		kfree_skb(skb);
		goto errout;
	}
	rtnl_notify(skb, &init_net, 0, RTNLGRP_LINK, NULL, GFP_KERNEL);
	return;
errout:
	if (err < 0)
		rtnl_set_sk_err(&init_net, RTNLGRP_LINK, err);
}

static void release_dp(struct kobject *kobj)
{
	struct datapath *dp = container_of(kobj, struct datapath, ifobj);
	kfree(dp);
}

static struct kobj_type dp_ktype = {
	.release = release_dp
};

static int create_dp(int dp_idx, const char __user *devnamep)
{
	struct odp_port internal_dev_port;
	char devname[IFNAMSIZ];
	struct datapath *dp;
	int err;
	int i;

	if (devnamep) {
		err = -EFAULT;
		if (strncpy_from_user(devname, devnamep, IFNAMSIZ) < 0)
			goto err;
		devname[IFNAMSIZ - 1] = '\0';
	} else {
		snprintf(devname, sizeof devname, "of%d", dp_idx);
	}

	rtnl_lock();
	mutex_lock(&dp_mutex);
	err = -ENODEV;
	if (!try_module_get(THIS_MODULE))
		goto err_unlock;

	/* Exit early if a datapath with that number already exists.
	 * (We don't use -EEXIST because that's ambiguous with 'devname'
	 * conflicting with an existing network device name.) */
	err = -EBUSY;
	if (get_dp(dp_idx))
		goto err_put_module;

	err = -ENOMEM;
	dp = kzalloc(sizeof *dp, GFP_KERNEL);
	if (dp == NULL)
		goto err_put_module;
	INIT_LIST_HEAD(&dp->port_list);
	mutex_init(&dp->mutex);
	dp->dp_idx = dp_idx;
	for (i = 0; i < DP_N_QUEUES; i++)
		skb_queue_head_init(&dp->queues[i]);
	init_waitqueue_head(&dp->waitqueue);

	/* Initialize kobject for bridge.  This will be added as
	 * /sys/class/net/<devname>/brif later, if sysfs is enabled. */
	dp->ifobj.kset = NULL;
	kobject_init(&dp->ifobj, &dp_ktype);

	/* Allocate table. */
	err = -ENOMEM;
	rcu_assign_pointer(dp->table, tbl_create(0));
	if (!dp->table)
		goto err_free_dp;

	/* Set up our datapath device. */
	BUILD_BUG_ON(sizeof(internal_dev_port.devname) != sizeof(devname));
	strcpy(internal_dev_port.devname, devname);
	internal_dev_port.flags = ODP_PORT_INTERNAL;
	err = new_dp_port(dp, &internal_dev_port, ODPP_LOCAL);
	if (err) {
		if (err == -EBUSY)
			err = -EEXIST;

		goto err_destroy_table;
	}

	dp->drop_frags = 0;
	dp->stats_percpu = alloc_percpu(struct dp_stats_percpu);
	if (!dp->stats_percpu)
		goto err_destroy_local_port;

	rcu_assign_pointer(dps[dp_idx], dp);
	mutex_unlock(&dp_mutex);
	rtnl_unlock();

	dp_sysfs_add_dp(dp);

	return 0;

err_destroy_local_port:
	dp_detach_port(dp->ports[ODPP_LOCAL], 1);
err_destroy_table:
	tbl_destroy(dp->table, NULL);
err_free_dp:
	kfree(dp);
err_put_module:
	module_put(THIS_MODULE);
err_unlock:
	mutex_unlock(&dp_mutex);
	rtnl_unlock();
err:
	return err;
}

static void do_destroy_dp(struct datapath *dp)
{
	struct dp_port *p, *n;
	int i;

	list_for_each_entry_safe (p, n, &dp->port_list, node)
		if (p->port_no != ODPP_LOCAL)
			dp_detach_port(p, 1);

	dp_sysfs_del_dp(dp);

	rcu_assign_pointer(dps[dp->dp_idx], NULL);

	dp_detach_port(dp->ports[ODPP_LOCAL], 1);

	tbl_destroy(dp->table, flow_free_tbl);

	for (i = 0; i < DP_N_QUEUES; i++)
		skb_queue_purge(&dp->queues[i]);
	for (i = 0; i < DP_MAX_GROUPS; i++)
		kfree(dp->groups[i]);
	free_percpu(dp->stats_percpu);
	kobject_put(&dp->ifobj);
	module_put(THIS_MODULE);
}

static int destroy_dp(int dp_idx)
{
	struct datapath *dp;
	int err;

	rtnl_lock();
	mutex_lock(&dp_mutex);
	dp = get_dp(dp_idx);
	err = -ENODEV;
	if (!dp)
		goto err_unlock;

	do_destroy_dp(dp);
	err = 0;

err_unlock:
	mutex_unlock(&dp_mutex);
	rtnl_unlock();
	return err;
}

static void release_dp_port(struct kobject *kobj)
{
	struct dp_port *p = container_of(kobj, struct dp_port, kobj);
	kfree(p);
}

static struct kobj_type brport_ktype = {
#ifdef CONFIG_SYSFS
	.sysfs_ops = &brport_sysfs_ops,
#endif
	.release = release_dp_port
};

/* Called with RTNL lock and dp_mutex. */
static int new_dp_port(struct datapath *dp, struct odp_port *odp_port, int port_no)
{
	struct vport *vport;
	struct dp_port *p;
	int err;

	vport = vport_locate(odp_port->devname);
	if (!vport) {
		vport_lock();

		if (odp_port->flags & ODP_PORT_INTERNAL)
			vport = __vport_add(odp_port->devname, "internal", NULL);
		else
			vport = __vport_add(odp_port->devname, "netdev", NULL);

		vport_unlock();

		if (IS_ERR(vport))
			return PTR_ERR(vport);
	}

	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	p->port_no = port_no;
	p->dp = dp;
	atomic_set(&p->sflow_pool, 0);

	err = vport_attach(vport, p);
	if (err) {
		kfree(p);
		return err;
	}

	rcu_assign_pointer(dp->ports[port_no], p);
	list_add_rcu(&p->node, &dp->port_list);
	dp->n_ports++;

	/* Initialize kobject for bridge.  This will be added as
	 * /sys/class/net/<devname>/brport later, if sysfs is enabled. */
	p->kobj.kset = NULL;
	kobject_init(&p->kobj, &brport_ktype);

	dp_ifinfo_notify(RTM_NEWLINK, p);

	return 0;
}

static int attach_port(int dp_idx, struct odp_port __user *portp)
{
	struct datapath *dp;
	struct odp_port port;
	int port_no;
	int err;

	err = -EFAULT;
	if (copy_from_user(&port, portp, sizeof port))
		goto out;
	port.devname[IFNAMSIZ - 1] = '\0';

	rtnl_lock();
	dp = get_dp_locked(dp_idx);
	err = -ENODEV;
	if (!dp)
		goto out_unlock_rtnl;

	for (port_no = 1; port_no < DP_MAX_PORTS; port_no++)
		if (!dp->ports[port_no])
			goto got_port_no;
	err = -EFBIG;
	goto out_unlock_dp;

got_port_no:
	err = new_dp_port(dp, &port, port_no);
	if (err)
		goto out_unlock_dp;

	if (!(port.flags & ODP_PORT_INTERNAL))
		set_internal_devs_mtu(dp);
	dp_sysfs_add_if(dp->ports[port_no]);

	err = __put_user(port_no, &portp->port);

out_unlock_dp:
	mutex_unlock(&dp->mutex);
out_unlock_rtnl:
	rtnl_unlock();
out:
	return err;
}

int dp_detach_port(struct dp_port *p, int may_delete)
{
	struct vport *vport = p->vport;
	int err;

	ASSERT_RTNL();

	if (p->port_no != ODPP_LOCAL)
		dp_sysfs_del_if(p);
	dp_ifinfo_notify(RTM_DELLINK, p);

	/* First drop references to device. */
	p->dp->n_ports--;
	list_del_rcu(&p->node);
	rcu_assign_pointer(p->dp->ports[p->port_no], NULL);

	err = vport_detach(vport);
	if (err)
		return err;

	/* Then wait until no one is still using it, and destroy it. */
	synchronize_rcu();

	if (may_delete) {
		const char *port_type = vport_get_type(vport);

		if (!strcmp(port_type, "netdev") || !strcmp(port_type, "internal")) {
			vport_lock();
			__vport_del(vport);
			vport_unlock();
		}
	}

	kobject_put(&p->kobj);

	return 0;
}

static int detach_port(int dp_idx, int port_no)
{
	struct dp_port *p;
	struct datapath *dp;
	int err;

	err = -EINVAL;
	if (port_no < 0 || port_no >= DP_MAX_PORTS || port_no == ODPP_LOCAL)
		goto out;

	rtnl_lock();
	dp = get_dp_locked(dp_idx);
	err = -ENODEV;
	if (!dp)
		goto out_unlock_rtnl;

	p = dp->ports[port_no];
	err = -ENOENT;
	if (!p)
		goto out_unlock_dp;

	err = dp_detach_port(p, 1);

out_unlock_dp:
	mutex_unlock(&dp->mutex);
out_unlock_rtnl:
	rtnl_unlock();
out:
	return err;
}

/* Must be called with rcu_read_lock and with bottom-halves disabled. */
void dp_process_received_packet(struct dp_port *p, struct sk_buff *skb)
{
	struct datapath *dp = p->dp;
	struct dp_stats_percpu *stats;
	struct odp_flow_key key;
	struct tbl_node *flow_node;

	WARN_ON_ONCE(skb_shared(skb));
	skb_warn_if_lro(skb);

	OVS_CB(skb)->dp_port = p;
	compute_ip_summed(skb, false);

	/* BHs are off so we don't have to use get_cpu()/put_cpu() here. */
	stats = percpu_ptr(dp->stats_percpu, smp_processor_id());

	if (flow_extract(skb, p ? p->port_no : ODPP_NONE, &key)) {
		if (dp->drop_frags) {
			kfree_skb(skb);
			stats->n_frags++;
			return;
		}
	}

	flow_node = tbl_lookup(rcu_dereference(dp->table), &key, flow_hash(&key), flow_cmp);
	if (flow_node) {
		struct sw_flow *flow = flow_cast(flow_node);
		struct sw_flow_actions *acts = rcu_dereference(flow->sf_acts);
		flow_used(flow, skb);
		execute_actions(dp, skb, &key, acts->actions, acts->n_actions,
				GFP_ATOMIC);
		stats->n_hit++;
	} else {
		stats->n_missed++;
		dp_output_control(dp, skb, _ODPL_MISS_NR, OVS_CB(skb)->tun_id);
	}
}

#if defined(CONFIG_XEN) && defined(HAVE_PROTO_DATA_VALID)
/* This code is based on a skb_checksum_setup from net/dev/core.c from a
 * combination of Lenny's 2.6.26 Xen kernel and Xen's
 * linux-2.6.18-92.1.10.el5.xs5.0.0.394.644.  We can't call this function
 * directly because it isn't exported in all versions. */
static int skb_pull_up_to(struct sk_buff *skb, void *ptr)
{
	if (ptr < (void *)skb->tail)
		return 1;
	if (__pskb_pull_tail(skb,
			     ptr - (void *)skb->data - skb_headlen(skb))) {
		return 1;
	} else {
		return 0;
	}
}

int vswitch_skb_checksum_setup(struct sk_buff *skb)
{
	struct iphdr *iph;
	unsigned char *th;
	int err = -EPROTO;
	__u16 csum_start, csum_offset;

	if (!skb->proto_csum_blank)
		return 0;

	if (skb->protocol != htons(ETH_P_IP))
		goto out;

	if (!skb_pull_up_to(skb, skb_network_header(skb) + sizeof(struct iphdr)))
		goto out;

	iph = ip_hdr(skb);
	th = skb_network_header(skb) + 4 * iph->ihl;

	csum_start = th - skb->head;
	switch (iph->protocol) {
	case IPPROTO_TCP:
		csum_offset = offsetof(struct tcphdr, check);
		break;
	case IPPROTO_UDP:
		csum_offset = offsetof(struct udphdr, check);
		break;
	default:
		if (net_ratelimit())
			printk(KERN_ERR "Attempting to checksum a non-"
			       "TCP/UDP packet, dropping a protocol"
			       " %d packet", iph->protocol);
		goto out;
	}

	if (!skb_pull_up_to(skb, th + csum_offset + 2))
		goto out;

	skb->ip_summed = CHECKSUM_PARTIAL;
	skb->proto_csum_blank = 0;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,22)
	skb->csum_start = csum_start;
	skb->csum_offset = csum_offset;
#else
	skb_set_transport_header(skb, csum_start - skb_headroom(skb));
	skb->csum = csum_offset;
#endif

	err = 0;

out:
	return err;
}
#endif /* CONFIG_XEN && HAVE_PROTO_DATA_VALID */

 /* Types of checksums that we can receive (these all refer to L4 checksums):
 * 1. CHECKSUM_NONE: Device that did not compute checksum, contains full
 *	(though not verified) checksum in packet but not in skb->csum.  Packets
 *	from the bridge local port will also have this type.
 * 2. CHECKSUM_COMPLETE (CHECKSUM_HW): Good device that computes checksums,
 *	also the GRE module.  This is the same as CHECKSUM_NONE, except it has
 *	a valid skb->csum.  Importantly, both contain a full checksum (not
 *	verified) in the packet itself.  The only difference is that if the
 *	packet gets to L4 processing on this machine (not in DomU) we won't
 *	have to recompute the checksum to verify.  Most hardware devices do not
 *	produce packets with this type, even if they support receive checksum
 *	offloading (they produce type #5).
 * 3. CHECKSUM_PARTIAL (CHECKSUM_HW): Packet without full checksum and needs to
 *	be computed if it is sent off box.  Unfortunately on earlier kernels,
 *	this case is impossible to distinguish from #2, despite having opposite
 *	meanings.  Xen adds an extra field on earlier kernels (see #4) in order
 *	to distinguish the different states.  The only real user of this type
 *	with bridging is Xen (on later kernels).
 * 4. CHECKSUM_UNNECESSARY (with proto_csum_blank true): This packet was
 *	generated locally by a Xen DomU and has a partial checksum.  If it is
 *	handled on this machine (Dom0 or DomU), then the checksum will not be
 *	computed.  If it goes off box, the checksum in the packet needs to be
 *	completed.  Calling skb_checksum_setup converts this to CHECKSUM_HW
 *	(CHECKSUM_PARTIAL) so that the checksum can be completed.  In later
 *	kernels, this combination is replaced with CHECKSUM_PARTIAL.
 * 5. CHECKSUM_UNNECESSARY (with proto_csum_blank false): Packet with a correct
 *	full checksum or using a protocol without a checksum.  skb->csum is
 *	undefined.  This is common from devices with receive checksum
 *	offloading.  This is somewhat similar to CHECKSUM_NONE, except that
 *	nobody will try to verify the checksum with CHECKSUM_UNNECESSARY.
 *
 * Note that on earlier kernels, CHECKSUM_COMPLETE and CHECKSUM_PARTIAL are
 * both defined as CHECKSUM_HW.  Normally the meaning of CHECKSUM_HW is clear
 * based on whether it is on the transmit or receive path.  After the datapath
 * it will be intepreted as CHECKSUM_PARTIAL.  If the packet already has a
 * checksum, we will panic.  Since we can receive packets with checksums, we
 * assume that all CHECKSUM_HW packets have checksums and map them to
 * CHECKSUM_NONE, which has a similar meaning (the it is only different if the
 * packet is processed by the local IP stack, in which case it will need to
 * be reverified).  If we receive a packet with CHECKSUM_HW that really means
 * CHECKSUM_PARTIAL, it will be sent with the wrong checksum.  However, there
 * shouldn't be any devices that do this with bridging.
 *
 * The bridge has similar behavior and this function closely resembles
 * skb_forward_csum().  It is slightly different because we are only concerned
 * with bridging and not other types of forwarding and can get away with
 * slightly more optimal behavior.*/
void
compute_ip_summed(struct sk_buff *skb, bool xmit)
{
	/* For our convenience these defines change repeatedly between kernel
	 * versions, so we can't just copy them over... */
	switch (skb->ip_summed) {
	case CHECKSUM_NONE:
		OVS_CB(skb)->ip_summed = OVS_CSUM_NONE;
		break;
	case CHECKSUM_UNNECESSARY:
		OVS_CB(skb)->ip_summed = OVS_CSUM_UNNECESSARY;
		break;
#ifdef CHECKSUM_HW
	/* In theory this could be either CHECKSUM_PARTIAL or CHECKSUM_COMPLETE.
	 * However, we should only get CHECKSUM_PARTIAL packets from Xen, which
	 * uses some special fields to represent this (see below).  Since we
	 * can only make one type work, pick the one that actually happens in
	 * practice.
	 *
	 * The one exception to this is if we are on the transmit path
	 * (basically after skb_checksum_setup() has been run) the type has
	 * already been converted, so we should stay with that. */
	case CHECKSUM_HW:
		if (!xmit)
			OVS_CB(skb)->ip_summed = OVS_CSUM_COMPLETE;
		else
			OVS_CB(skb)->ip_summed = OVS_CSUM_PARTIAL;

		break;
#else
	case CHECKSUM_COMPLETE:
		OVS_CB(skb)->ip_summed = OVS_CSUM_COMPLETE;
		break;
	case CHECKSUM_PARTIAL:
		OVS_CB(skb)->ip_summed = OVS_CSUM_PARTIAL;
		break;
#endif
	default:
		printk(KERN_ERR "openvswitch: unknown checksum type %d\n",
		       skb->ip_summed);
		/* None seems the safest... */
		OVS_CB(skb)->ip_summed = OVS_CSUM_NONE;
	}	

#if defined(CONFIG_XEN) && defined(HAVE_PROTO_DATA_VALID)
	/* Xen has a special way of representing CHECKSUM_PARTIAL on older
	 * kernels. It should not be set on the transmit path though. */
	if (skb->proto_csum_blank)
		OVS_CB(skb)->ip_summed = OVS_CSUM_PARTIAL;

	WARN_ON_ONCE(skb->proto_csum_blank && xmit);
#endif
}

void
forward_ip_summed(struct sk_buff *skb)
{
#ifdef CHECKSUM_HW
	if (OVS_CB(skb)->ip_summed == OVS_CSUM_COMPLETE)
		skb->ip_summed = CHECKSUM_NONE;
#endif
}

/* Append each packet in 'skb' list to 'queue'.  There will be only one packet
 * unless we broke up a GSO packet. */
static int
queue_control_packets(struct sk_buff *skb, struct sk_buff_head *queue,
		      int queue_no, u32 arg)
{
	struct sk_buff *nskb;
	int port_no;
	int err;

	if (OVS_CB(skb)->dp_port)
		port_no = OVS_CB(skb)->dp_port->port_no;
	else
		port_no = ODPP_LOCAL;

	do {
		struct odp_msg *header;

		nskb = skb->next;
		skb->next = NULL;

		/* If a checksum-deferred packet is forwarded to the
		 * controller, correct the pointers and checksum.  This happens
		 * on a regular basis only on Xen, on which VMs can pass up
		 * packets that do not have their checksum computed.
		 */
		err = vswitch_skb_checksum_setup(skb);
		if (err)
			goto err_kfree_skbs;

		if (skb->ip_summed == CHECKSUM_PARTIAL) {

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,22)
			/* Until 2.6.22, the start of the transport header was
			 * also the start of data to be checksummed.  Linux
			 * 2.6.22 introduced the csum_start field for this
			 * purpose, but we should point the transport header to
			 * it anyway for backward compatibility, as
			 * dev_queue_xmit() does even in 2.6.28. */
			skb_set_transport_header(skb, skb->csum_start -
						 skb_headroom(skb));
#endif

			err = skb_checksum_help(skb);
			if (err)
				goto err_kfree_skbs;
		}

		err = skb_cow(skb, sizeof *header);
		if (err)
			goto err_kfree_skbs;

		header = (struct odp_msg*)__skb_push(skb, sizeof *header);
		header->type = queue_no;
		header->length = skb->len;
		header->port = port_no;
		header->reserved = 0;
		header->arg = arg;
		skb_queue_tail(queue, skb);

		skb = nskb;
	} while (skb);
	return 0;

err_kfree_skbs:
	kfree_skb(skb);
	while ((skb = nskb) != NULL) {
		nskb = skb->next;
		kfree_skb(skb);
	}
	return err;
}

int
dp_output_control(struct datapath *dp, struct sk_buff *skb, int queue_no,
		  u32 arg)
{
	struct dp_stats_percpu *stats;
	struct sk_buff_head *queue;
	int err;

	WARN_ON_ONCE(skb_shared(skb));
	BUG_ON(queue_no != _ODPL_MISS_NR && queue_no != _ODPL_ACTION_NR && queue_no != _ODPL_SFLOW_NR);
	queue = &dp->queues[queue_no];
	err = -ENOBUFS;
	if (skb_queue_len(queue) >= DP_MAX_QUEUE_LEN)
		goto err_kfree_skb;

	forward_ip_summed(skb);

	/* Break apart GSO packets into their component pieces.  Otherwise
	 * userspace may try to stuff a 64kB packet into a 1500-byte MTU. */
	if (skb_is_gso(skb)) {
		struct sk_buff *nskb = skb_gso_segment(skb, 0);
		if (nskb) {
			kfree_skb(skb);
			skb = nskb;
			if (unlikely(IS_ERR(skb))) {
				err = PTR_ERR(skb);
				goto err;
			}
		} else {
			/* XXX This case might not be possible.  It's hard to
			 * tell from the skb_gso_segment() code and comment. */
		}
	}

	err = queue_control_packets(skb, queue, queue_no, arg);
	wake_up_interruptible(&dp->waitqueue);
	return err;

err_kfree_skb:
	kfree_skb(skb);
err:
	stats = percpu_ptr(dp->stats_percpu, get_cpu());
	stats->n_lost++;
	put_cpu();

	return err;
}

static int flush_flows(struct datapath *dp)
{
	struct tbl *old_table = rcu_dereference(dp->table);
	struct tbl *new_table;

	new_table = tbl_create(0);
	if (!new_table)
		return -ENOMEM;

	rcu_assign_pointer(dp->table, new_table);

	tbl_deferred_destroy(old_table, flow_free_tbl);

	return 0;
}

static int validate_actions(const struct sw_flow_actions *actions)
{
	unsigned int i;

	for (i = 0; i < actions->n_actions; i++) {
		const union odp_action *a = &actions->actions[i];
		switch (a->type) {
		case ODPAT_OUTPUT:
			if (a->output.port >= DP_MAX_PORTS)
				return -EINVAL;
			break;

		case ODPAT_OUTPUT_GROUP:
			if (a->output_group.group >= DP_MAX_GROUPS)
				return -EINVAL;
			break;

		case ODPAT_SET_VLAN_VID:
			if (a->vlan_vid.vlan_vid & htons(~VLAN_VID_MASK))
				return -EINVAL;
			break;

		case ODPAT_SET_VLAN_PCP:
			if (a->vlan_pcp.vlan_pcp
			    & ~(VLAN_PCP_MASK >> VLAN_PCP_SHIFT))
				return -EINVAL;
			break;

		case ODPAT_SET_NW_TOS:
			if (a->nw_tos.nw_tos & INET_ECN_MASK)
				return -EINVAL;
			break;

		default:
			if (a->type >= ODPAT_N_ACTIONS)
				return -EOPNOTSUPP;
			break;
		}
	}

	return 0;
}

static struct sw_flow_actions *get_actions(const struct odp_flow *flow)
{
	struct sw_flow_actions *actions;
	int error;

	actions = flow_actions_alloc(flow->n_actions);
	error = PTR_ERR(actions);
	if (IS_ERR(actions))
		goto error;

	error = -EFAULT;
	if (copy_from_user(actions->actions, flow->actions,
			   flow->n_actions * sizeof(union odp_action)))
		goto error_free_actions;
	error = validate_actions(actions);
	if (error)
		goto error_free_actions;

	return actions;

error_free_actions:
	kfree(actions);
error:
	return ERR_PTR(error);
}

static void get_stats(struct sw_flow *flow, struct odp_flow_stats *stats)
{
	if (flow->used.tv_sec) {
		stats->used_sec = flow->used.tv_sec;
		stats->used_nsec = flow->used.tv_nsec;
	} else {
		stats->used_sec = 0;
		stats->used_nsec = 0;
	}
	stats->n_packets = flow->packet_count;
	stats->n_bytes = flow->byte_count;
	stats->ip_tos = flow->ip_tos;
	stats->tcp_flags = flow->tcp_flags;
	stats->error = 0;
}

static void clear_stats(struct sw_flow *flow)
{
	flow->used.tv_sec = flow->used.tv_nsec = 0;
	flow->tcp_flags = 0;
	flow->ip_tos = 0;
	flow->packet_count = 0;
	flow->byte_count = 0;
}

static int expand_table(struct datapath *dp)
{
	struct tbl *old_table = rcu_dereference(dp->table);
	struct tbl *new_table;

	new_table = tbl_expand(old_table);
	if (IS_ERR(new_table))
		return PTR_ERR(new_table);

	rcu_assign_pointer(dp->table, new_table);
	tbl_deferred_destroy(old_table, NULL);

	return 0;
}

static int put_flow(struct datapath *dp, struct odp_flow_put __user *ufp)
{
	struct odp_flow_put uf;
	struct tbl_node *flow_node;
	struct sw_flow *flow;
	struct tbl *table;
	struct odp_flow_stats stats;
	int error;

	error = -EFAULT;
	if (copy_from_user(&uf, ufp, sizeof(struct odp_flow_put)))
		goto error;
	memset(uf.flow.key.reserved, 0, sizeof uf.flow.key.reserved);

	table = rcu_dereference(dp->table);
	flow_node = tbl_lookup(table, &uf.flow.key, flow_hash(&uf.flow.key), flow_cmp);
	if (!flow_node) {
		/* No such flow. */
		struct sw_flow_actions *acts;

		error = -ENOENT;
		if (!(uf.flags & ODPPF_CREATE))
			goto error;

		/* Expand table, if necessary, to make room. */
		if (tbl_count(table) >= tbl_n_buckets(table)) {
			error = expand_table(dp);
			if (error)
				goto error;
			table = rcu_dereference(dp->table);
		}

		/* Allocate flow. */
		error = -ENOMEM;
		flow = kmem_cache_alloc(flow_cache, GFP_KERNEL);
		if (flow == NULL)
			goto error;
		flow->key = uf.flow.key;
		spin_lock_init(&flow->lock);
		clear_stats(flow);

		/* Obtain actions. */
		acts = get_actions(&uf.flow);
		error = PTR_ERR(acts);
		if (IS_ERR(acts))
			goto error_free_flow;
		rcu_assign_pointer(flow->sf_acts, acts);

		/* Put flow in bucket. */
		error = tbl_insert(table, &flow->tbl_node, flow_hash(&flow->key));
		if (error)
			goto error_free_flow_acts;

		memset(&stats, 0, sizeof(struct odp_flow_stats));
	} else {
		/* We found a matching flow. */
		struct sw_flow_actions *old_acts, *new_acts;
		unsigned long int flags;

		flow = flow_cast(flow_node);

		/* Bail out if we're not allowed to modify an existing flow. */
		error = -EEXIST;
		if (!(uf.flags & ODPPF_MODIFY))
			goto error;

		/* Swap actions. */
		new_acts = get_actions(&uf.flow);
		error = PTR_ERR(new_acts);
		if (IS_ERR(new_acts))
			goto error;
		old_acts = rcu_dereference(flow->sf_acts);
		if (old_acts->n_actions != new_acts->n_actions ||
		    memcmp(old_acts->actions, new_acts->actions,
			   sizeof(union odp_action) * old_acts->n_actions)) {
			rcu_assign_pointer(flow->sf_acts, new_acts);
			flow_deferred_free_acts(old_acts);
		} else {
			kfree(new_acts);
		}

		/* Fetch stats, then clear them if necessary. */
		spin_lock_irqsave(&flow->lock, flags);
		get_stats(flow, &stats);
		if (uf.flags & ODPPF_ZERO_STATS)
			clear_stats(flow);
		spin_unlock_irqrestore(&flow->lock, flags);
	}

	/* Copy stats to userspace. */
	if (__copy_to_user(&ufp->flow.stats, &stats,
			   sizeof(struct odp_flow_stats)))
		return -EFAULT;
	return 0;

error_free_flow_acts:
	kfree(flow->sf_acts);
error_free_flow:
	kmem_cache_free(flow_cache, flow);
error:
	return error;
}

static int put_actions(const struct sw_flow *flow, struct odp_flow __user *ufp)
{
	union odp_action __user *actions;
	struct sw_flow_actions *sf_acts;
	u32 n_actions;

	if (__get_user(actions, &ufp->actions) ||
	    __get_user(n_actions, &ufp->n_actions))
		return -EFAULT;

	if (!n_actions)
		return 0;

	sf_acts = rcu_dereference(flow->sf_acts);
	if (__put_user(sf_acts->n_actions, &ufp->n_actions) ||
	    (actions && copy_to_user(actions, sf_acts->actions,
				     sizeof(union odp_action) *
				     min(sf_acts->n_actions, n_actions))))
		return -EFAULT;

	return 0;
}

static int answer_query(struct sw_flow *flow, u32 query_flags,
			struct odp_flow __user *ufp)
{
	struct odp_flow_stats stats;
	unsigned long int flags;

	spin_lock_irqsave(&flow->lock, flags);
	get_stats(flow, &stats);

	if (query_flags & ODPFF_ZERO_TCP_FLAGS) {
		flow->tcp_flags = 0;
	}
	spin_unlock_irqrestore(&flow->lock, flags);

	if (__copy_to_user(&ufp->stats, &stats, sizeof(struct odp_flow_stats)))
		return -EFAULT;
	return put_actions(flow, ufp);
}

static int del_flow(struct datapath *dp, struct odp_flow __user *ufp)
{
	struct tbl *table = rcu_dereference(dp->table);
	struct odp_flow uf;
	struct tbl_node *flow_node;
	struct sw_flow *flow;
	int error;

	error = -EFAULT;
	if (copy_from_user(&uf, ufp, sizeof uf))
		goto error;
	memset(uf.key.reserved, 0, sizeof uf.key.reserved);

	flow_node = tbl_lookup(table, &uf.key, flow_hash(&uf.key), flow_cmp);
	error = -ENOENT;
	if (!flow_node)
		goto error;

	error = tbl_remove(table, flow_node);
	if (error)
		goto error;

	/* XXX These statistics might lose a few packets, since other CPUs can
	 * be using this flow.  We used to synchronize_rcu() to make sure that
	 * we get completely accurate stats, but that blows our performance,
	 * badly. */

	flow = flow_cast(flow_node);
	error = answer_query(flow, 0, ufp);
	flow_deferred_free(flow);

error:
	return error;
}

static int query_flows(struct datapath *dp, const struct odp_flowvec *flowvec)
{
	struct tbl *table = rcu_dereference(dp->table);
	int i;
	for (i = 0; i < flowvec->n_flows; i++) {
		struct __user odp_flow *ufp = &flowvec->flows[i];
		struct odp_flow uf;
		struct tbl_node *flow_node;
		int error;

		if (__copy_from_user(&uf, ufp, sizeof uf))
			return -EFAULT;
		memset(uf.key.reserved, 0, sizeof uf.key.reserved);

		flow_node = tbl_lookup(table, &uf.key, flow_hash(&uf.key), flow_cmp);
		if (!flow_node)
			error = __put_user(ENOENT, &ufp->stats.error);
		else
			error = answer_query(flow_cast(flow_node), uf.flags, ufp);
		if (error)
			return -EFAULT;
	}
	return flowvec->n_flows;
}

struct list_flows_cbdata {
	struct odp_flow __user *uflows;
	int n_flows;
	int listed_flows;
};

static int list_flow(struct tbl_node *node, void *cbdata_)
{
	struct sw_flow *flow = flow_cast(node);
	struct list_flows_cbdata *cbdata = cbdata_;
	struct odp_flow __user *ufp = &cbdata->uflows[cbdata->listed_flows++];
	int error;

	if (__copy_to_user(&ufp->key, &flow->key, sizeof flow->key))
		return -EFAULT;
	error = answer_query(flow, 0, ufp);
	if (error)
		return error;

	if (cbdata->listed_flows >= cbdata->n_flows)
		return cbdata->listed_flows;
	return 0;
}

static int list_flows(struct datapath *dp, const struct odp_flowvec *flowvec)
{
	struct list_flows_cbdata cbdata;
	int error;

	if (!flowvec->n_flows)
		return 0;

	cbdata.uflows = flowvec->flows;
	cbdata.n_flows = flowvec->n_flows;
	cbdata.listed_flows = 0;
	error = tbl_foreach(rcu_dereference(dp->table), list_flow, &cbdata);
	return error ? error : cbdata.listed_flows;
}

static int do_flowvec_ioctl(struct datapath *dp, unsigned long argp,
			    int (*function)(struct datapath *,
					    const struct odp_flowvec *))
{
	struct odp_flowvec __user *uflowvec;
	struct odp_flowvec flowvec;
	int retval;

	uflowvec = (struct odp_flowvec __user *)argp;
	if (!access_ok(VERIFY_WRITE, uflowvec, sizeof *uflowvec) ||
	    copy_from_user(&flowvec, uflowvec, sizeof flowvec))
		return -EFAULT;

	if (flowvec.n_flows > INT_MAX / sizeof(struct odp_flow))
		return -EINVAL;

	if (!access_ok(VERIFY_WRITE, flowvec.flows,
		       flowvec.n_flows * sizeof(struct odp_flow)))
		return -EFAULT;

	retval = function(dp, &flowvec);
	return (retval < 0 ? retval
		: retval == flowvec.n_flows ? 0
		: __put_user(retval, &uflowvec->n_flows));
}

static int do_execute(struct datapath *dp, const struct odp_execute *executep)
{
	struct odp_execute execute;
	struct odp_flow_key key;
	struct sk_buff *skb;
	struct sw_flow_actions *actions;
	struct ethhdr *eth;
	int err;

	err = -EFAULT;
	if (copy_from_user(&execute, executep, sizeof execute))
		goto error;

	err = -EINVAL;
	if (execute.length < ETH_HLEN || execute.length > 65535)
		goto error;

	err = -ENOMEM;
	actions = flow_actions_alloc(execute.n_actions);
	if (!actions)
		goto error;

	err = -EFAULT;
	if (copy_from_user(actions->actions, execute.actions,
			   execute.n_actions * sizeof *execute.actions))
		goto error_free_actions;

	err = validate_actions(actions);
	if (err)
		goto error_free_actions;

	err = -ENOMEM;
	skb = alloc_skb(execute.length, GFP_KERNEL);
	if (!skb)
		goto error_free_actions;

	if (execute.in_port < DP_MAX_PORTS)
		OVS_CB(skb)->dp_port = dp->ports[execute.in_port];
	else
		OVS_CB(skb)->dp_port = NULL;

	err = -EFAULT;
	if (copy_from_user(skb_put(skb, execute.length), execute.data,
			   execute.length))
		goto error_free_skb;

	skb_reset_mac_header(skb);
	eth = eth_hdr(skb);

	/* Normally, setting the skb 'protocol' field would be handled by a
	 * call to eth_type_trans(), but it assumes there's a sending
	 * device, which we may not have. */
	if (ntohs(eth->h_proto) >= 1536)
		skb->protocol = eth->h_proto;
	else
		skb->protocol = htons(ETH_P_802_2);

	flow_extract(skb, execute.in_port, &key);
	err = execute_actions(dp, skb, &key, actions->actions,
			      actions->n_actions, GFP_KERNEL);
	kfree(actions);
	return err;

error_free_skb:
	kfree_skb(skb);
error_free_actions:
	kfree(actions);
error:
	return err;
}

static int get_dp_stats(struct datapath *dp, struct odp_stats __user *statsp)
{
	struct tbl *table = rcu_dereference(dp->table);
	struct odp_stats stats;
	int i;

	stats.n_flows = tbl_count(table);
	stats.cur_capacity = tbl_n_buckets(table);
	stats.max_capacity = TBL_MAX_BUCKETS;
	stats.n_ports = dp->n_ports;
	stats.max_ports = DP_MAX_PORTS;
	stats.max_groups = DP_MAX_GROUPS;
	stats.n_frags = stats.n_hit = stats.n_missed = stats.n_lost = 0;
	for_each_possible_cpu(i) {
		const struct dp_stats_percpu *s;
		s = percpu_ptr(dp->stats_percpu, i);
		stats.n_frags += s->n_frags;
		stats.n_hit += s->n_hit;
		stats.n_missed += s->n_missed;
		stats.n_lost += s->n_lost;
	}
	stats.max_miss_queue = DP_MAX_QUEUE_LEN;
	stats.max_action_queue = DP_MAX_QUEUE_LEN;
	return copy_to_user(statsp, &stats, sizeof stats) ? -EFAULT : 0;
}

/* MTU of the dp pseudo-device: ETH_DATA_LEN or the minimum of the ports */
int dp_min_mtu(const struct datapath *dp)
{
	struct dp_port *p;
	int mtu = 0;

	ASSERT_RTNL();

	list_for_each_entry_rcu (p, &dp->port_list, node) {
		int dev_mtu;

		/* Skip any internal ports, since that's what we're trying to
		 * set. */
		if (is_internal_vport(p->vport))
			continue;

		dev_mtu = vport_get_mtu(p->vport);
		if (!mtu || dev_mtu < mtu)
			mtu = dev_mtu;
	}

	return mtu ? mtu : ETH_DATA_LEN;
}

/* Sets the MTU of all datapath devices to the minimum of the ports.  Must
 * be called with RTNL lock and dp_mutex. */
void set_internal_devs_mtu(const struct datapath *dp)
{
	struct dp_port *p;
	int mtu;

	ASSERT_RTNL();

	mtu = dp_min_mtu(dp);

	list_for_each_entry_rcu (p, &dp->port_list, node) {
		if (is_internal_vport(p->vport))
			vport_set_mtu(p->vport, mtu);
	}
}

static int
put_port(const struct dp_port *p, struct odp_port __user *uop)
{
	struct odp_port op;

	memset(&op, 0, sizeof op);

	rcu_read_lock();
	strncpy(op.devname, vport_get_name(p->vport), sizeof op.devname);
	rcu_read_unlock();

	op.port = p->port_no;
	op.flags = is_internal_vport(p->vport) ? ODP_PORT_INTERNAL : 0;

	return copy_to_user(uop, &op, sizeof op) ? -EFAULT : 0;
}

static int
query_port(struct datapath *dp, struct odp_port __user *uport)
{
	struct odp_port port;

	if (copy_from_user(&port, uport, sizeof port))
		return -EFAULT;

	if (port.devname[0]) {
		struct vport *vport;
		struct dp_port *dp_port;
		int err = 0;

		port.devname[IFNAMSIZ - 1] = '\0';

		vport_lock();
		rcu_read_lock();

		vport = vport_locate(port.devname);
		if (!vport) {
			err = -ENODEV;
			goto error_unlock;
		}

		dp_port = vport_get_dp_port(vport);
		if (!dp_port || dp_port->dp != dp) {
			err = -ENOENT;
			goto error_unlock;
		}

		port.port = dp_port->port_no;

error_unlock:
		rcu_read_unlock();
		vport_unlock();

		if (err)
			return err;
	} else {
		if (port.port >= DP_MAX_PORTS)
			return -EINVAL;
		if (!dp->ports[port.port])
			return -ENOENT;
	}

	return put_port(dp->ports[port.port], uport);
}

static int
list_ports(struct datapath *dp, struct odp_portvec __user *pvp)
{
	struct odp_portvec pv;
	struct dp_port *p;
	int idx;

	if (copy_from_user(&pv, pvp, sizeof pv))
		return -EFAULT;

	idx = 0;
	if (pv.n_ports) {
		list_for_each_entry_rcu (p, &dp->port_list, node) {
			if (put_port(p, &pv.ports[idx]))
				return -EFAULT;
			if (idx++ >= pv.n_ports)
				break;
		}
	}
	return put_user(dp->n_ports, &pvp->n_ports);
}

/* RCU callback for freeing a dp_port_group */
static void free_port_group(struct rcu_head *rcu)
{
	struct dp_port_group *g = container_of(rcu, struct dp_port_group, rcu);
	kfree(g);
}

static int
set_port_group(struct datapath *dp, const struct odp_port_group __user *upg)
{
	struct odp_port_group pg;
	struct dp_port_group *new_group, *old_group;
	int error;

	error = -EFAULT;
	if (copy_from_user(&pg, upg, sizeof pg))
		goto error;

	error = -EINVAL;
	if (pg.n_ports > DP_MAX_PORTS || pg.group >= DP_MAX_GROUPS)
		goto error;

	error = -ENOMEM;
	new_group = kmalloc(sizeof *new_group + sizeof(u16) * pg.n_ports,
			    GFP_KERNEL);
	if (!new_group)
		goto error;

	new_group->n_ports = pg.n_ports;
	error = -EFAULT;
	if (copy_from_user(new_group->ports, pg.ports,
			   sizeof(u16) * pg.n_ports))
		goto error_free;

	old_group = rcu_dereference(dp->groups[pg.group]);
	rcu_assign_pointer(dp->groups[pg.group], new_group);
	if (old_group)
		call_rcu(&old_group->rcu, free_port_group);
	return 0;

error_free:
	kfree(new_group);
error:
	return error;
}

static int
get_port_group(struct datapath *dp, struct odp_port_group *upg)
{
	struct odp_port_group pg;
	struct dp_port_group *g;
	u16 n_copy;

	if (copy_from_user(&pg, upg, sizeof pg))
		return -EFAULT;

	if (pg.group >= DP_MAX_GROUPS)
		return -EINVAL;

	g = dp->groups[pg.group];
	n_copy = g ? min_t(int, g->n_ports, pg.n_ports) : 0;
	if (n_copy && copy_to_user(pg.ports, g->ports, n_copy * sizeof(u16)))
		return -EFAULT;

	if (put_user(g ? g->n_ports : 0, &upg->n_ports))
		return -EFAULT;

	return 0;
}

static int get_listen_mask(const struct file *f)
{
	return (long)f->private_data;
}

static void set_listen_mask(struct file *f, int listen_mask)
{
	f->private_data = (void*)(long)listen_mask;
}

static long openvswitch_ioctl(struct file *f, unsigned int cmd,
			   unsigned long argp)
{
	int dp_idx = iminor(f->f_dentry->d_inode);
	struct datapath *dp;
	int drop_frags, listeners, port_no;
	unsigned int sflow_probability;
	int err;

	/* Handle commands with special locking requirements up front. */
	switch (cmd) {
	case ODP_DP_CREATE:
		err = create_dp(dp_idx, (char __user *)argp);
		goto exit;

	case ODP_DP_DESTROY:
		err = destroy_dp(dp_idx);
		goto exit;

	case ODP_PORT_ATTACH:
		err = attach_port(dp_idx, (struct odp_port __user *)argp);
		goto exit;

	case ODP_PORT_DETACH:
		err = get_user(port_no, (int __user *)argp);
		if (!err)
			err = detach_port(dp_idx, port_no);
		goto exit;

	case ODP_VPORT_ADD:
		err = vport_add((struct odp_vport_add __user *)argp);
		goto exit;

	case ODP_VPORT_MOD:
		err = vport_mod((struct odp_vport_mod __user *)argp);
		goto exit;

	case ODP_VPORT_DEL:
		err = vport_del((char __user *)argp);
		goto exit;

	case ODP_VPORT_STATS_GET:
		err = vport_stats_get((struct odp_vport_stats_req __user *)argp);
		goto exit;

	case ODP_VPORT_ETHER_GET:
		err = vport_ether_get((struct odp_vport_ether __user *)argp);
		goto exit;

	case ODP_VPORT_ETHER_SET:
		err = vport_ether_set((struct odp_vport_ether __user *)argp);
		goto exit;

	case ODP_VPORT_MTU_GET:
		err = vport_mtu_get((struct odp_vport_mtu __user *)argp);
		goto exit;

	case ODP_VPORT_MTU_SET:
		err = vport_mtu_set((struct odp_vport_mtu __user *)argp);
		goto exit;
	}

	dp = get_dp_locked(dp_idx);
	err = -ENODEV;
	if (!dp)
		goto exit;

	switch (cmd) {
	case ODP_DP_STATS:
		err = get_dp_stats(dp, (struct odp_stats __user *)argp);
		break;

	case ODP_GET_DROP_FRAGS:
		err = put_user(dp->drop_frags, (int __user *)argp);
		break;

	case ODP_SET_DROP_FRAGS:
		err = get_user(drop_frags, (int __user *)argp);
		if (err)
			break;
		err = -EINVAL;
		if (drop_frags != 0 && drop_frags != 1)
			break;
		dp->drop_frags = drop_frags;
		err = 0;
		break;

	case ODP_GET_LISTEN_MASK:
		err = put_user(get_listen_mask(f), (int __user *)argp);
		break;

	case ODP_SET_LISTEN_MASK:
		err = get_user(listeners, (int __user *)argp);
		if (err)
			break;
		err = -EINVAL;
		if (listeners & ~ODPL_ALL)
			break;
		err = 0;
		set_listen_mask(f, listeners);
		break;

	case ODP_GET_SFLOW_PROBABILITY:
		err = put_user(dp->sflow_probability, (unsigned int __user *)argp);
		break;

	case ODP_SET_SFLOW_PROBABILITY:
		err = get_user(sflow_probability, (unsigned int __user *)argp);
		if (!err)
			dp->sflow_probability = sflow_probability;
		break;

	case ODP_PORT_QUERY:
		err = query_port(dp, (struct odp_port __user *)argp);
		break;

	case ODP_PORT_LIST:
		err = list_ports(dp, (struct odp_portvec __user *)argp);
		break;

	case ODP_PORT_GROUP_SET:
		err = set_port_group(dp, (struct odp_port_group __user *)argp);
		break;

	case ODP_PORT_GROUP_GET:
		err = get_port_group(dp, (struct odp_port_group __user *)argp);
		break;

	case ODP_FLOW_FLUSH:
		err = flush_flows(dp);
		break;

	case ODP_FLOW_PUT:
		err = put_flow(dp, (struct odp_flow_put __user *)argp);
		break;

	case ODP_FLOW_DEL:
		err = del_flow(dp, (struct odp_flow __user *)argp);
		break;

	case ODP_FLOW_GET:
		err = do_flowvec_ioctl(dp, argp, query_flows);
		break;

	case ODP_FLOW_LIST:
		err = do_flowvec_ioctl(dp, argp, list_flows);
		break;

	case ODP_EXECUTE:
		err = do_execute(dp, (struct odp_execute __user *)argp);
		break;

	default:
		err = -ENOIOCTLCMD;
		break;
	}
	mutex_unlock(&dp->mutex);
exit:
	return err;
}

static int dp_has_packet_of_interest(struct datapath *dp, int listeners)
{
	int i;
	for (i = 0; i < DP_N_QUEUES; i++) {
		if (listeners & (1 << i) && !skb_queue_empty(&dp->queues[i]))
			return 1;
	}
	return 0;
}

ssize_t openvswitch_read(struct file *f, char __user *buf, size_t nbytes,
		      loff_t *ppos)
{
	/* XXX is there sufficient synchronization here? */
	int listeners = get_listen_mask(f);
	int dp_idx = iminor(f->f_dentry->d_inode);
	struct datapath *dp = get_dp(dp_idx);
	struct sk_buff *skb;
	struct iovec __user iov;
	size_t copy_bytes;
	int retval;

	if (!dp)
		return -ENODEV;

	if (nbytes == 0 || !listeners)
		return 0;

	for (;;) {
		int i;

		for (i = 0; i < DP_N_QUEUES; i++) {
			if (listeners & (1 << i)) {
				skb = skb_dequeue(&dp->queues[i]);
				if (skb)
					goto success;
			}
		}

		if (f->f_flags & O_NONBLOCK) {
			retval = -EAGAIN;
			goto error;
		}

		wait_event_interruptible(dp->waitqueue,
					 dp_has_packet_of_interest(dp,
								   listeners));

		if (signal_pending(current)) {
			retval = -ERESTARTSYS;
			goto error;
		}
	}
success:
	copy_bytes = min_t(size_t, skb->len, nbytes);
	iov.iov_base = buf;
	iov.iov_len = copy_bytes;
	retval = skb_copy_datagram_iovec(skb, 0, &iov, iov.iov_len);
	if (!retval)
		retval = copy_bytes;
	kfree_skb(skb);

error:
	return retval;
}

static unsigned int openvswitch_poll(struct file *file, poll_table *wait)
{
	/* XXX is there sufficient synchronization here? */
	int dp_idx = iminor(file->f_dentry->d_inode);
	struct datapath *dp = get_dp(dp_idx);
	unsigned int mask;

	if (dp) {
		mask = 0;
		poll_wait(file, &dp->waitqueue, wait);
		if (dp_has_packet_of_interest(dp, get_listen_mask(file)))
			mask |= POLLIN | POLLRDNORM;
	} else {
		mask = POLLIN | POLLRDNORM | POLLHUP;
	}
	return mask;
}

struct file_operations openvswitch_fops = {
	/* XXX .aio_read = openvswitch_aio_read, */
	.read  = openvswitch_read,
	.poll  = openvswitch_poll,
	.unlocked_ioctl = openvswitch_ioctl,
	/* XXX .fasync = openvswitch_fasync, */
};

static int major;

static int __init dp_init(void)
{
	struct sk_buff *dummy_skb;
	int err;

	BUILD_BUG_ON(sizeof(struct ovs_skb_cb) > sizeof(dummy_skb->cb));

	printk("Open vSwitch %s, built "__DATE__" "__TIME__"\n", VERSION BUILDNR);

	err = flow_init();
	if (err)
		goto error;

	err = vport_init();
	if (err)
		goto error_flow_exit;

	err = register_netdevice_notifier(&dp_device_notifier);
	if (err)
		goto error_vport_exit;

	major = register_chrdev(0, "openvswitch", &openvswitch_fops);
	if (err < 0)
		goto error_unreg_notifier;

	return 0;

error_unreg_notifier:
	unregister_netdevice_notifier(&dp_device_notifier);
error_vport_exit:
	vport_exit();
error_flow_exit:
	flow_exit();
error:
	return err;
}

static void dp_cleanup(void)
{
	rcu_barrier();
	unregister_chrdev(major, "openvswitch");
	unregister_netdevice_notifier(&dp_device_notifier);
	vport_exit();
	flow_exit();
}

module_init(dp_init);
module_exit(dp_cleanup);

MODULE_DESCRIPTION("Open vSwitch switching datapath");
MODULE_LICENSE("GPL");
