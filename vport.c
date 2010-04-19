/*
 * Copyright (c) 2010 Nicira Networks.
 * Distributed under the terms of the GNU GPL version 2.
 *
 * Significant portions of this file may be copied from parts of the Linux
 * kernel, by Linus Torvalds and others.
 */

#include <linux/dcache.h>
#include <linux/etherdevice.h>
#include <linux/if.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/percpu.h>
#include <linux/rtnetlink.h>

#include "vport.h"

extern struct vport_ops netdev_vport_ops;
extern struct vport_ops internal_vport_ops;
extern struct vport_ops gre_vport_ops;

static struct vport_ops *base_vport_ops_list[] = {
	&netdev_vport_ops,
	&internal_vport_ops,
	&gre_vport_ops,
};

static const struct vport_ops **vport_ops_list;
static int n_vport_types;

static struct hlist_head *dev_table;
#define VPORT_HASH_BUCKETS 1024

/* Both RTNL lock and vport_mutex need to be held when updating dev_table.
 *
 * If you use vport_locate and then perform some operations, you need to hold
 * one of these locks if you don't want the vport to be deleted out from under
 * you.
 *
 * If you get a reference to a vport through a dp_port, it is protected
 * by RCU and you need to hold rcu_read_lock instead when reading.
 *
 * If multiple locks are taken, the hierarchy is:
 * 1. RTNL
 * 2. DP
 * 3. vport
 */
static DEFINE_MUTEX(vport_mutex);

/**
 *	vport_lock - acquire vport lock
 *
 * Acquire global vport lock.  See above comment about locking requirements
 * and specific function definitions.  May sleep.
 */
void
vport_lock(void)
{
	mutex_lock(&vport_mutex);
}

/**
 *	vport_unlock - release vport lock
 *
 * Release lock acquired with vport_lock.
 */
void
vport_unlock(void)
{
	mutex_unlock(&vport_mutex);
}

#define ASSERT_VPORT() do { \
	if (unlikely(!mutex_is_locked(&vport_mutex))) { \
		printk(KERN_ERR "openvswitch: vport lock not held at %s (%d)\n", \
			__FILE__, __LINE__); \
		dump_stack(); \
	} \
} while(0)

/**
 *	vport_init - initialize vport subsystem
 *
 * Called at module load time to initialize the vport subsystem and any
 * compiled in vport types.
 */
int
vport_init(void)
{
	int err;
	int i;

	dev_table = kzalloc(VPORT_HASH_BUCKETS * sizeof(struct hlist_head),
			    GFP_KERNEL);
	if (!dev_table) {
		err = -ENOMEM;
		goto error;
	}

	vport_ops_list = kmalloc(ARRAY_SIZE(base_vport_ops_list) *
				 sizeof(struct vport_ops *), GFP_KERNEL);
	if (!vport_ops_list) {
		err = -ENOMEM;
		goto error_dev_table;
	}

	for (i = 0; i < ARRAY_SIZE(base_vport_ops_list); i++) {
		struct vport_ops *new_ops = base_vport_ops_list[i];

		if (new_ops->get_stats && new_ops->flags & VPORT_F_GEN_STATS) {
			printk(KERN_INFO "openvswitch: both get_stats() and VPORT_F_GEN_STATS defined on vport %s, dropping VPORT_F_GEN_STATS\n", new_ops->type);
			new_ops->flags &= ~VPORT_F_GEN_STATS;
		}

		if (new_ops->init)
			err = new_ops->init();
		else
			err = 0;

		if (!err)
			vport_ops_list[n_vport_types++] = new_ops;
		else if (new_ops->flags & VPORT_F_REQUIRED) {
			vport_exit();
			goto error;
		}
	}

	return 0;

error_dev_table:
	kfree(dev_table);
error:
	return err;
}

static void
vport_del_all(void)
{
	int i;

	rtnl_lock();
	vport_lock();

	for (i = 0; i < VPORT_HASH_BUCKETS; i++) {
		struct hlist_head *bucket = &dev_table[i];
		struct vport *vport;
		struct hlist_node *node, *next;

		hlist_for_each_entry_safe(vport, node, next, bucket, hash_node)
			__vport_del(vport);
	}

	vport_unlock();
	rtnl_unlock();
}

/**
 *	vport_exit - shutdown vport subsystem
 *
 * Called at module exit time to shutdown the vport subsystem and any
 * initialized vport types.
 */
void
vport_exit(void)
{
	int i;

	vport_del_all();

	for (i = 0; i < n_vport_types; i++) {
		if (vport_ops_list[i]->exit)
			vport_ops_list[i]->exit();
	}

	kfree(vport_ops_list);
	kfree(dev_table);
}

/**
 *	vport_add - add vport device (for userspace callers)
 *
 * @uvport_config: New port configuration.
 *
 * Creates a new vport with the specified configuration (which is dependent
 * on device type).  This function is for userspace callers and assumes no
 * locks are held.
 */
int
vport_add(const struct odp_vport_add __user *uvport_config)
{
	struct odp_vport_add vport_config;
	struct vport *vport;
	int err = 0;

	if (copy_from_user(&vport_config, uvport_config, sizeof(struct odp_vport_add)))
		return -EFAULT;

	vport_config.port_type[VPORT_TYPE_SIZE - 1] = '\0';
	vport_config.devname[IFNAMSIZ - 1] = '\0';

	rtnl_lock();

	vport = vport_locate(vport_config.devname);
	if (vport) {
		err = -EEXIST;
		goto out;
	}

	vport_lock();
	vport = __vport_add(vport_config.devname, vport_config.port_type,
			    vport_config.config);
	vport_unlock();

	if (IS_ERR(vport))
		err = PTR_ERR(vport);

out:
	rtnl_unlock();
	return err;
}

/**
 *	vport_mod - modify existing vport device (for userspace callers)
 *
 * @uvport_config: New configuration for vport
 *
 * Modifies an existing device with the specified configuration (which is
 * dependent on device type).  This function is for userspace callers and
 * assumes no locks are held.
 */
int
vport_mod(const struct odp_vport_mod __user *uvport_config)
{
	struct odp_vport_mod vport_config;
	struct vport *vport;
	int err;

	if (copy_from_user(&vport_config, uvport_config, sizeof(struct odp_vport_mod)))
		return -EFAULT;

	vport_config.devname[IFNAMSIZ - 1] = '\0';

	rtnl_lock();

	vport = vport_locate(vport_config.devname);
	if (!vport) {
		err = -ENODEV;
		goto out;
	}

	vport_lock();
	err = __vport_mod(vport, vport_config.config);
	vport_unlock();

out:
	rtnl_unlock();
	return err;
}

/**
 *	vport_del - delete existing vport device (for userspace callers)
 *
 * @udevname: Name of device to delete
 *
 * Deletes the specified device.  Detaches the device from a datapath first
 * if it is attached.  Deleting the device will fail if it does not exist or it
 * is the datapath local port.  It is also possible to fail for less obvious
 * reasons, such as lack of memory.  This function is for userspace callers and
 * assumes no locks are held.
 */
int
vport_del(const char __user *udevname)
{
	char devname[IFNAMSIZ];
	struct vport *vport;
	struct dp_port *dp_port;
	int err = 0;

	if (strncpy_from_user(devname, udevname, IFNAMSIZ - 1) < 0)
		return -EFAULT;
	devname[IFNAMSIZ - 1] = '\0';

	rtnl_lock();

	vport = vport_locate(devname);
	if (!vport) {
		err = -ENODEV;
		goto out;
	}

	dp_port = vport_get_dp_port(vport);
	if (dp_port) {
		struct datapath *dp = dp_port->dp;

		mutex_lock(&dp->mutex);

		if (!strcmp(dp_name(dp), devname)) {
			err = -EINVAL;
			goto dp_port_out;
		}

		err = dp_detach_port(dp_port, 0);

dp_port_out:
		mutex_unlock(&dp->mutex);

		if (err)
			goto out;
	}

	vport_lock();
	err = __vport_del(vport);
	vport_unlock();

out:
	rtnl_unlock();
	return err;
}

/**
 *	vport_stats_get - retrieve device stats (for userspace callers)
 *
 * @ustats_req: Stats request parameters.
 *
 * Retrieves transmit, receive, and error stats for the given device.  This
 * function is for userspace callers and assumes no locks are held.
 */
int
vport_stats_get(struct odp_vport_stats_req __user *ustats_req)
{
	struct odp_vport_stats_req stats_req;
	struct vport *vport;
	int err;

	if (copy_from_user(&stats_req, ustats_req, sizeof(struct odp_vport_stats_req)))
		return -EFAULT;

	stats_req.devname[IFNAMSIZ - 1] = '\0';

	vport_lock();

	vport = vport_locate(stats_req.devname);
	if (!vport) {
		err = -ENODEV;
		goto out;
	}

	if (vport->ops->get_stats)
		err = vport->ops->get_stats(vport, &stats_req.stats);
	else if (vport->ops->flags & VPORT_F_GEN_STATS) {
		int i;

		memset(&stats_req.stats, 0, sizeof(struct odp_vport_stats));

		for_each_possible_cpu(i) {
			const struct vport_percpu_stats *percpu_stats;

			percpu_stats = per_cpu_ptr(vport->percpu_stats, i);
			stats_req.stats.rx_bytes	+= percpu_stats->rx_bytes;
			stats_req.stats.rx_packets	+= percpu_stats->rx_packets;
			stats_req.stats.tx_bytes	+= percpu_stats->tx_bytes;
			stats_req.stats.tx_packets	+= percpu_stats->tx_packets;
		}

		spin_lock_bh(&vport->err_stats.lock);

		stats_req.stats.rx_dropped	= vport->err_stats.rx_dropped;
		stats_req.stats.rx_errors	= vport->err_stats.rx_errors
						+ vport->err_stats.rx_frame_err
						+ vport->err_stats.rx_over_err
						+ vport->err_stats.rx_crc_err;
		stats_req.stats.rx_frame_err	= vport->err_stats.rx_frame_err;
		stats_req.stats.rx_over_err	= vport->err_stats.rx_over_err;
		stats_req.stats.rx_crc_err	= vport->err_stats.rx_crc_err;
		stats_req.stats.tx_dropped	= vport->err_stats.tx_dropped;
		stats_req.stats.tx_errors	= vport->err_stats.tx_errors;
		stats_req.stats.collisions	= vport->err_stats.collisions;

		spin_unlock_bh(&vport->err_stats.lock);

		err = 0;
	} else
		err = -EOPNOTSUPP;

out:
	vport_unlock();

	if (!err)
		if (copy_to_user(ustats_req, &stats_req, sizeof(struct odp_vport_stats_req)))
			err = -EFAULT;

	return err;
}

/**
 *	vport_ether_get - retrieve device Ethernet address (for userspace callers)
 *
 * @uvport_ether: Ethernet address request parameters.
 *
 * Retrieves the Ethernet address of the given device.  This function is for
 * userspace callers and assumes no locks are held.
 */
int
vport_ether_get(struct odp_vport_ether __user *uvport_ether)
{
	struct odp_vport_ether vport_ether;
	struct vport *vport;
	int err = 0;

	if (copy_from_user(&vport_ether, uvport_ether, sizeof(struct odp_vport_ether)))
		return -EFAULT;

	vport_ether.devname[IFNAMSIZ - 1] = '\0';

	vport_lock();

	vport = vport_locate(vport_ether.devname);
	if (!vport) {
		err = -ENODEV;
		goto out;
	}

	memcpy(vport_ether.ether_addr, vport_get_addr(vport), ETH_ALEN);

out:
	vport_unlock();

	if (!err)
		if (copy_to_user(uvport_ether, &vport_ether, sizeof(struct odp_vport_ether)))
			err = -EFAULT;

	return err;
}

/**
 *	vport_ether_set - set device Ethernet address (for userspace callers)
 *
 * @uvport_ether: Ethernet address request parameters.
 *
 * Sets the Ethernet address of the given device.  Some devices may not support
 * setting the Ethernet address, in which case the result will always be
 * -EOPNOTSUPP.  This function is for userspace callers and assumes no locks
 * are held.
 */
int
vport_ether_set(struct odp_vport_ether __user *uvport_ether)
{
	struct odp_vport_ether vport_ether;
	struct vport *vport;
	int err;

	if (copy_from_user(&vport_ether, uvport_ether, sizeof(struct odp_vport_ether)))
		return -EFAULT;

	vport_ether.devname[IFNAMSIZ - 1] = '\0';

	rtnl_lock();
	vport_lock();

	vport = vport_locate(vport_ether.devname);
	if (!vport) {
		err = -ENODEV;
		goto out;
	}

	err = vport_set_addr(vport, vport_ether.ether_addr);

out:
	vport_unlock();
	rtnl_unlock();
	return err;
}

/**
 *	vport_mut_get - retrieve device MTU (for userspace callers)
 *
 * @uvport_mtu: MTU request parameters.
 *
 * Retrieves the MTU of the given device.  This function is for userspace
 * callers and assumes no locks are held.
 */
int
vport_mtu_get(struct odp_vport_mtu __user *uvport_mtu)
{
	struct odp_vport_mtu vport_mtu;
	struct vport *vport;
	int err = 0;

	if (copy_from_user(&vport_mtu, uvport_mtu, sizeof(struct odp_vport_mtu)))
		return -EFAULT;

	vport_mtu.devname[IFNAMSIZ - 1] = '\0';

	vport_lock();

	vport = vport_locate(vport_mtu.devname);
	if (!vport) {
		err = -ENODEV;
		goto out;
	}

	vport_mtu.mtu = vport_get_mtu(vport);

out:
	vport_unlock();

	if (!err)
		if (copy_to_user(uvport_mtu, &vport_mtu, sizeof(struct odp_vport_mtu)))
			err = -EFAULT;

	return err;
}

/**
 *	vport_mtu_set - set device MTU (for userspace callers)
 *
 * @uvport_mtu: MTU request parameters.
 *
 * Sets the MTU of the given device.  Some devices may not support setting the
 * MTU, in which case the result will always be -EOPNOTSUPP.  This function is
 * for userspace callers and assumes no locks are held.
 */
int
vport_mtu_set(struct odp_vport_mtu __user *uvport_mtu)
{
	struct odp_vport_mtu vport_mtu;
	struct vport *vport;
	int err;

	if (copy_from_user(&vport_mtu, uvport_mtu, sizeof(struct odp_vport_mtu)))
		return -EFAULT;

	vport_mtu.devname[IFNAMSIZ - 1] = '\0';

	rtnl_lock();
	vport_lock();

	vport = vport_locate(vport_mtu.devname);
	if (!vport) {
		err = -ENODEV;
		goto out;
	}

	err = vport_set_mtu(vport, vport_mtu.mtu);

out:
	vport_unlock();
	rtnl_unlock();
	return err;
}

static struct hlist_head *
hash_bucket(const char *name)
{
	unsigned int hash = full_name_hash(name, strlen(name));
	return &dev_table[hash & (VPORT_HASH_BUCKETS - 1)];
}

/**
 *	vport_locate - find a port that has already been created
 *
 * @name: name of port to find
 *
 * Either RTNL or vport lock must be acquired before calling this function
 * and held while using the found port.  See the locking comments at the
 * top of the file.
 */
struct vport *
vport_locate(const char *name)
{
	struct hlist_head *bucket = hash_bucket(name);
	struct vport *vport;
	struct hlist_node *node;

	if (unlikely(!mutex_is_locked(&vport_mutex) && !rtnl_is_locked())) {
		printk(KERN_ERR "openvswitch: neither RTNL nor vport lock held in vport_locate\n");
		dump_stack();
	}

	hlist_for_each_entry(vport, node, bucket, hash_node)
		if (!strcmp(name, vport_get_name(vport)))
			return vport;

	return NULL;
}

static void
register_vport(struct vport *vport)
{
	hlist_add_head(&vport->hash_node, hash_bucket(vport_get_name(vport)));
}

static void
unregister_vport(struct vport *vport)
{
	hlist_del(&vport->hash_node);
}

/**
 *	vport_alloc - allocate and initialize new vport
 *
 * @priv_size: Size of private data area to allocate.
 * @ops: vport device ops
 *
 * Allocate and initialize a new vport defined by @ops.  The vport will contain
 * a private data area of size @priv_size that can be accessed using
 * vport_priv().  vports that are no longer needed should be released with
 * vport_free().
 */
struct vport *
vport_alloc(int priv_size, const struct vport_ops *ops)
{
	struct vport *vport;
	size_t alloc_size;

	alloc_size = sizeof(struct vport);
	if (priv_size) {
		alloc_size = ALIGN(alloc_size, VPORT_ALIGN);
		alloc_size += priv_size;
	}

	vport = kzalloc(alloc_size, GFP_KERNEL);
	if (!vport)
		return ERR_PTR(-ENOMEM);

	vport->ops = ops;

	if (vport->ops->flags & VPORT_F_GEN_STATS) {
		vport->percpu_stats = alloc_percpu(struct vport_percpu_stats);
		if (!vport->percpu_stats)
			return ERR_PTR(-ENOMEM);

		spin_lock_init(&vport->err_stats.lock);
	}

	return vport;
}

/**
 *	vport_free - uninitialize and free vport
 *
 * @vport: vport to free
 *
 * Frees a vport allocated with vport_alloc() when it is no longer needed.
 */
void
vport_free(struct vport *vport)
{
	if (vport->ops->flags & VPORT_F_GEN_STATS)
		free_percpu(vport->percpu_stats);

	kfree(vport);
}

/**
 *	__vport_add - add vport device (for kernel callers)
 *
 * @name: Name of new device.
 * @type: Type of new device (to be matched against types in registered vport
 * ops).
 * @config: Device type specific configuration.  Userspace pointer.
 *
 * Creates a new vport with the specified configuration (which is dependent
 * on device type).  Both RTNL and vport locks must be held.
 */
struct vport *
__vport_add(const char *name, const char *type, const void __user *config)
{
	struct vport *vport;
	int err = 0;
	int i;

	ASSERT_RTNL();
	ASSERT_VPORT();

	for (i = 0; i < n_vport_types; i++) {
		if (!strcmp(vport_ops_list[i]->type, type)) {
			vport = vport_ops_list[i]->create(name, config);
			if (IS_ERR(vport)) {
				err = PTR_ERR(vport);
				goto out;
			}

			register_vport(vport);
			return vport;
		}
	}

	err = -EAFNOSUPPORT;

out:
	return ERR_PTR(err);
}

/**
 *	__vport_mod - modify existing vport device (for kernel callers)
 *
 * @vport: vport to modify.
 * @config: Device type specific configuration.  Userspace pointer.
 *
 * Modifies an existing device with the specified configuration (which is
 * dependent on device type).  Both RTNL and vport locks must be held.
 */
int
__vport_mod(struct vport *vport, const void __user *config)
{
	ASSERT_RTNL();
	ASSERT_VPORT();

	if (vport->ops->modify)
		return vport->ops->modify(vport, config);
	else
		return -EOPNOTSUPP;
}

/**
 *	__vport_del - delete existing vport device (for kernel callers)
 *
 * @vport: vport to delete.
 *
 * Deletes the specified device.  The device must not be currently attached to
 * a datapath.  It is possible to fail for reasons such as lack of memory.
 * Both RTNL and vport locks must be held.
 */
int
__vport_del(struct vport *vport)
{
	ASSERT_RTNL();
	ASSERT_VPORT();
	BUG_ON(vport_get_dp_port(vport));

	unregister_vport(vport);

	return vport->ops->destroy(vport);
}

/**
 *	vport_attach - attach a vport to a datapath
 *
 * @vport: vport to attach.
 * @dp_port: Datapath port to attach the vport to.
 *
 * Attaches a vport to a specific datapath so that packets may be exchanged.
 * Both ports must be currently unattached.  @dp_port must be successfully
 * attached to a vport before it is connected to a datapath and must not be
 * modified while connected.  RTNL lock and the appropriate DP mutex must be held.
 */
int
vport_attach(struct vport *vport, struct dp_port *dp_port)
{
	ASSERT_RTNL();

	if (dp_port->vport)
		return -EBUSY;

	if (vport_get_dp_port(vport))
		return -EBUSY;

	if (vport->ops->attach) {
		int err;

		err = vport->ops->attach(vport);
		if (err)
			return err;
	}

	dp_port->vport = vport;
	rcu_assign_pointer(vport->dp_port, dp_port);

	return 0;
}

/**
 *	vport_detach - detach a vport from a datapath
 *
 * @vport: vport to detach.
 *
 * Detaches a vport from a datapath.  May fail for a variety of reasons,
 * including lack of memory.  RTNL lock and the appropriate DP mutex must be held.
 */
int
vport_detach(struct vport *vport)
{
	struct dp_port *dp_port;

	ASSERT_RTNL();

	dp_port = vport_get_dp_port(vport);
	if (!dp_port)
		return -EINVAL;

	dp_port->vport = NULL;
	rcu_assign_pointer(vport->dp_port, NULL);

	if (vport->ops->detach)
		return vport->ops->detach(vport);
	else
		return 0;
}

/**
 *	vport_set_mtu - set device MTU (for kernel callers)
 *
 * @vport: vport on which to set MTU.
 * @mtu: New MTU.
 *
 * Sets the MTU of the given device.  Some devices may not support setting the
 * MTU, in which case the result will always be -EOPNOTSUPP.  RTNL lock must
 * be held.
 */
int
vport_set_mtu(struct vport *vport, int mtu)
{
	ASSERT_RTNL();

	if (mtu < 68)
		return -EINVAL;

	if (vport->ops->set_mtu)
		return vport->ops->set_mtu(vport, mtu);
	else
		return -EOPNOTSUPP;
}

/**
 *	vport_set_addr - set device Ethernet address (for kernel callers)
 *
 * @vport: vport on which to set Ethernet address.
 * @addr: New address.
 *
 * Sets the Ethernet address of the given device.  Some devices may not support
 * setting the Ethernet address, in which case the result will always be
 * -EOPNOTSUPP.  RTNL lock must be held.
 */
int
vport_set_addr(struct vport *vport, const unsigned char *addr)
{
	ASSERT_RTNL();

	if (!is_valid_ether_addr(addr))
		return -EADDRNOTAVAIL;

	if (vport->ops->set_addr)
		return vport->ops->set_addr(vport, addr);
	else
		return -EOPNOTSUPP;
}

/**
 *	vport_get_name - retrieve device name
 *
 * @vport: vport from which to retrieve the name.
 *
 * Retrieves the name of the given device.  Either RTNL lock or rcu_read_lock
 * must be held for the entire duration that the name is in use.
 */
const char *
vport_get_name(const struct vport *vport)
{
	return vport->ops->get_name(vport);
}

/**
 *	vport_get_type - retrieve device type
 *
 * @vport: vport from which to retrieve the type.
 *
 * Retrieves the type of the given device.  Either RTNL lock or rcu_read_lock
 * must be held for the entire duration that the type is in use.
 */
const char *
vport_get_type(const struct vport *vport)
{
	return vport->ops->type;
}

/**
 *	vport_get_addr - retrieve device Ethernet address (for kernel callers)
 *
 * @vport: vport from which to retrieve the Ethernet address.
 *
 * Retrieves the Ethernet address of the given device.  Either RTNL lock or
 * rcu_read_lock must be held for the entire duration that the Ethernet address
 * is in use.
 */
const unsigned char *
vport_get_addr(const struct vport *vport)
{
	return vport->ops->get_addr(vport);
}

/**
 *	vport_get_dp_port - retrieve attached datapath port
 *
 * @vport: vport from which to retrieve the datapath port.
 *
 * Retrieves the attached datapath port or null if not attached.  Either RTNL
 * lock or rcu_read_lock must be held for the entire duration that the datapath
 * port is being accessed.
 */
struct dp_port *
vport_get_dp_port(const struct vport *vport)
{
	return rcu_dereference(vport->dp_port);
}

/**
 *	vport_get_kobj - retrieve associated kobj
 *
 * @vport: vport from which to retrieve the associated kobj
 *
 * Retrieves the associated kobj or null if no kobj.  The returned kobj is
 * valid for as long as the vport exists.
 */
struct kobject *
vport_get_kobj(const struct vport *vport)
{
	if (vport->ops->get_kobj)
		return vport->ops->get_kobj(vport);
	else
		return NULL;
}

/**
 *	vport_get_flags - retrieve device flags
 *
 * @vport: vport from which to retrieve the flags
 *
 * Retrieves the flags of the given device.  Either RTNL lock or rcu_read_lock
 * must be held.
 */
unsigned
vport_get_flags(const struct vport *vport)
{
	return vport->ops->get_dev_flags(vport);
}

/**
 *	vport_get_flags - check whether device is running
 *
 * @vport: vport on which to check status.
 *
 * Checks whether the given device is running.  Either RTNL lock or
 * rcu_read_lock must be held.
 */
int
vport_is_running(const struct vport *vport)
{
	return vport->ops->is_running(vport);
}

/**
 *	vport_get_flags - retrieve device operating state
 *
 * @vport: vport from which to check status
 *
 * Retrieves the RFC2863 operstate of the given device.  Either RTNL lock or
 * rcu_read_lock must be held.
 */
unsigned char
vport_get_operstate(const struct vport *vport)
{
	return vport->ops->get_operstate(vport);
}

/**
 *	vport_get_ifindex - retrieve device system interface index
 *
 * @vport: vport from which to retrieve index
 *
 * Retrieves the system interface index of the given device.  Not all devices
 * will have system indexes, in which case the index of the datapath local
 * port is returned.  Returns a negative index on error.  Either RTNL lock or
 * rcu_read_lock must be held.
 */
int
vport_get_ifindex(const struct vport *vport)
{
	const struct dp_port *dp_port;

	if (vport->ops->get_ifindex)
		return vport->ops->get_ifindex(vport);

	/* If we don't actually have an ifindex, use the local port's.
	 * Userspace doesn't check it anyways. */
	dp_port = vport_get_dp_port(vport);
	if (!dp_port)
		return -EAGAIN;

	return vport_get_ifindex(dp_port->dp->ports[ODPP_LOCAL]->vport);
}

/**
 *	vport_get_iflink - retrieve device system link index
 *
 * @vport: vport from which to retrieve index
 *
 * Retrieves the system link index of the given device.  The link is the index
 * of the interface on which the packet will actually be sent.  In most cases
 * this is the same as the ifindex but may be different for tunnel devices.
 * Returns a negative index on error.  Either RTNL lock or rcu_read_lock must
 * be held.
 */
int
vport_get_iflink(const struct vport *vport)
{
	if (vport->ops->get_iflink)
		return vport->ops->get_iflink(vport);

	/* If we don't have an iflink, use the ifindex.  In most cases they
	 * are the same. */
	return vport_get_ifindex(vport);
}

/**
 *	vport_get_mtu - retrieve device MTU (for kernel callers)
 *
 * @vport: vport from which to retrieve MTU
 *
 * Retrieves the MTU of the given device.  Either RTNL lock or rcu_read_lock
 * must be held.
 */
int
vport_get_mtu(const struct vport *vport)
{
	return vport->ops->get_mtu(vport);
}

/**
 *	vport_receive - pass up received packet to the datapath for processing
 *
 * @vport: vport that received the packet
 * @skb: skb that was received
 *
 * Must be called with rcu_read_lock and bottom halves disabled.  The packet
 * cannot be shared and skb->data should point to the Ethernet header.
 */
void
vport_receive(struct vport *vport, struct sk_buff *skb)
{
	struct dp_port *dp_port = vport_get_dp_port(vport);

	if (!dp_port)
		return;

	if (vport->ops->flags & VPORT_F_GEN_STATS) {
		struct vport_percpu_stats *stats;

		local_bh_disable();

		stats = per_cpu_ptr(vport->percpu_stats, smp_processor_id());
		stats->rx_packets++;
		stats->rx_bytes += skb->len;

		local_bh_enable();
	}

	if (!(vport->ops->flags & VPORT_F_TUN_ID))
		OVS_CB(skb)->tun_id = 0;

	dp_process_received_packet(dp_port, skb);
}

/**
 *	vport_send - send a packet on a device
 *
 * @vport: vport on which to send the packet
 * @skb: skb to send
 *
 * Sends the given packet and returns the length of data sent.  Either RTNL
 * lock or rcu_read_lock must be held.
 */
int
vport_send(struct vport *vport, struct sk_buff *skb)
{
	int sent;

	sent = vport->ops->send(vport, skb);

	if (vport->ops->flags & VPORT_F_GEN_STATS && sent > 0) {
		struct vport_percpu_stats *stats;

		local_bh_disable();

		stats = per_cpu_ptr(vport->percpu_stats, smp_processor_id());
		stats->tx_packets++;
		stats->tx_bytes += sent;

		local_bh_enable();
	}

	return sent;
}

/**
 *	vport_record_error - indicate device error to generic stats layer
 *
 * @vport: vport that encountered the error
 * @err_type: one of enum vport_err_type types to indicate the error type
 *
 * If using the vport generic stats layer indicate that an error of the given
 * type has occured.
 */
void
vport_record_error(struct vport *vport, enum vport_err_type err_type)
{
	if (vport->ops->flags & VPORT_F_GEN_STATS) {

		spin_lock_bh(&vport->err_stats.lock);

		switch (err_type) {
		case VPORT_E_RX_DROPPED:
			vport->err_stats.rx_dropped++;
			break;

		case VPORT_E_RX_ERROR:
			vport->err_stats.rx_errors++;
			break;

		case VPORT_E_RX_FRAME:
			vport->err_stats.rx_frame_err++;
			break;

		case VPORT_E_RX_OVER:
			vport->err_stats.rx_over_err++;
			break;

		case VPORT_E_RX_CRC:
			vport->err_stats.rx_crc_err++;
			break;

		case VPORT_E_TX_DROPPED:
			vport->err_stats.tx_dropped++;
			break;

		case VPORT_E_TX_ERROR:
			vport->err_stats.tx_errors++;
			break;

		case VPORT_E_COLLISION:
			vport->err_stats.collisions++;
			break;
		};

		spin_unlock_bh(&vport->err_stats.lock);
	}
}

/**
 *	vport_gen_ether_addr - generate an Ethernet address
 *
 * @addr: location to store generated address
 *
 * Generates a random Ethernet address for use when creating a device that
 * has no natural address.
 */
void
vport_gen_ether_addr(u8 *addr)
{
	random_ether_addr(addr);

	/* Set the OUI to the Nicira one. */
	addr[0] = 0x00;
	addr[1] = 0x23;
	addr[2] = 0x20;

	/* Set the top bit to indicate random address. */
	addr[3] |= 0x80;
}
