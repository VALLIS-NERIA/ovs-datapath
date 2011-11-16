/*
 * Copyright (c) 2007-2011 Nicira Networks.
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

#ifndef VPORT_NETDEV_H
#define VPORT_NETDEV_H 1

#include <linux/netdevice.h>

#include "vport.h"

struct vport *netdev_get_vport(struct net_device *dev);

struct netdev_vport {
	struct net_device *dev;
};

static inline struct netdev_vport *
netdev_vport_priv(const struct vport *vport)
{
	return vport_priv(vport);
}

int netdev_set_addr(struct vport *, const unsigned char *addr);
const char *netdev_get_name(const struct vport *);
const unsigned char *netdev_get_addr(const struct vport *);
const char *netdev_get_config(const struct vport *);
struct kobject *netdev_get_kobj(const struct vport *);
unsigned netdev_get_dev_flags(const struct vport *);
int netdev_is_running(const struct vport *);
unsigned char netdev_get_operstate(const struct vport *);
int netdev_get_ifindex(const struct vport *);
int netdev_get_mtu(const struct vport *);

#endif /* vport_netdev.h */
