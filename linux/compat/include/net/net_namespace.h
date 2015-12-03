#ifndef __NET_NET_NAMESPACE_WRAPPER_H
#define __NET_NET_NAMESPACE_WRAPPER_H 1

#include_next <net/net_namespace.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,33)
/* for 2.6.32* */
struct rpl_pernet_operations {
	int (*init)(struct net *net);
	void (*exit)(struct net *net);
	int *id;
	size_t size;
	struct pernet_operations ops;
};
#define pernet_operations rpl_pernet_operations

#define register_pernet_device rpl_register_pernet_gen_device
#define unregister_pernet_device rpl_unregister_pernet_gen_device

#define register_pernet_subsys rpl_register_pernet_gen_device
#define unregister_pernet_subsys rpl_unregister_pernet_gen_device

#define compat_init_net ovs_compat_init_net
int ovs_compat_init_net(struct net *net, struct rpl_pernet_operations *pnet);
#define compat_exit_net ovs_compat_exit_net
void ovs_compat_exit_net(struct net *net, struct rpl_pernet_operations *pnet);

#define DEFINE_COMPAT_PNET_REG_FUNC(TYPE)					\
									\
static struct rpl_pernet_operations *pnet_gen_##TYPE;			\
static int compat_init_net_gen_##TYPE(struct net *net)	\
{									\
	return compat_init_net(net, pnet_gen_##TYPE);			\
}									\
									\
static void compat_exit_net_gen_##TYPE(struct net *net)	\
{									\
	compat_exit_net(net, pnet_gen_##TYPE);				\
}									\
									\
static int rpl_register_pernet_gen_##TYPE(struct rpl_pernet_operations *rpl_pnet)	\
{										\
	pnet_gen_##TYPE = rpl_pnet;						\
	rpl_pnet->ops.init = compat_init_net_gen_##TYPE;			\
	rpl_pnet->ops.exit = compat_exit_net_gen_##TYPE;			\
	return register_pernet_gen_##TYPE(pnet_gen_##TYPE->id, &rpl_pnet->ops); \
}											\
											\
static void rpl_unregister_pernet_gen_##TYPE(struct rpl_pernet_operations *rpl_pnet)		\
{											\
	unregister_pernet_gen_##TYPE(*pnet_gen_##TYPE->id, &rpl_pnet->ops);		\
}
#else
#define DEFINE_COMPAT_PNET_REG_FUNC(TYPE)
#endif /* 2.6.33 */

#ifndef HAVE_POSSIBLE_NET_T
typedef struct {
#ifdef CONFIG_NET_NS
	struct net *net;
#endif
} possible_net_t;

static inline void rpl_write_pnet(possible_net_t *pnet, struct net *net)
{
#ifdef CONFIG_NET_NS
	pnet->net = net;
#endif
}

static inline struct net *rpl_read_pnet(const possible_net_t *pnet)
{
#ifdef CONFIG_NET_NS
	return pnet->net;
#else
	return &init_net;
#endif
}
#else /* Linux >= 4.1 */
#define rpl_read_pnet read_pnet
#define rpl_write_pnet write_pnet
#endif /* Linux >= 4.1 */

#endif /* net/net_namespace.h wrapper */
