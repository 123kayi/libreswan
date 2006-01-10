/*
 * IPSEC MAST code.
 * Copyright (C) 2005 Michael Richardson <mcr@xelerance.com>
 * 
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

char ipsec_mast_c_version[] = "RCSID $Id: ipsec_mast.c,v 1.7 2005/04/29 05:10:22 mcr Exp $";

#define __NO_VERSION__
#include <linux/module.h>
#include <linux/config.h>	/* for CONFIG_IP_FORWARD */
#include <linux/version.h>
#include <linux/kernel.h> /* printk() */

#include "openswan/ipsec_param.h"

#ifdef MALLOC_SLAB
# include <linux/slab.h> /* kmalloc() */
#else /* MALLOC_SLAB */
# include <linux/malloc.h> /* kmalloc() */
#endif /* MALLOC_SLAB */
#include <linux/errno.h>  /* error codes */
#include <linux/types.h>  /* size_t */
#include <linux/interrupt.h> /* mark_bh */

#include <net/tcp.h>
#include <net/udp.h>
#include <linux/skbuff.h>

#include <linux/netdevice.h>   /* struct device, struct net_device_stats, dev_queue_xmit() and other headers */
#include <linux/etherdevice.h> /* eth_type_trans */
#include <linux/ip.h>          /* struct iphdr */
#include <linux/skbuff.h>

#include <openswan.h>

#ifdef NET_21
# include <linux/in6.h>
# define ip_chk_addr inet_addr_type
# define IS_MYADDR RTN_LOCAL
# include <net/dst.h>
# undef dev_kfree_skb
# define dev_kfree_skb(a,b) kfree_skb(a)
# define PHYSDEV_TYPE
#endif /* NET_21 */

#include <net/icmp.h>		/* icmp_send() */
#include <net/ip.h>
#ifdef NETDEV_23
# include <linux/netfilter_ipv4.h>
#endif /* NETDEV_23 */

#include <linux/if_arp.h>

#include "openswan/ipsec_kversion.h"
#include "openswan/radij.h"
#include "openswan/ipsec_life.h"
#include "openswan/ipsec_xform.h"
#include "openswan/ipsec_eroute.h"
#include "openswan/ipsec_encap.h"
#include "openswan/ipsec_radij.h"
#include "openswan/ipsec_sa.h"
#include "openswan/ipsec_xmit.h"
#include "openswan/ipsec_mast.h"
#include "openswan/ipsec_ipe4.h"
#include "openswan/ipsec_ah.h"
#include "openswan/ipsec_esp.h"
#include "openswan/ipsec_kern24.h"

#include <pfkeyv2.h>
#include <pfkey.h>

#include "openswan/ipsec_proto.h"
#ifdef CONFIG_IPSEC_NAT_TRAVERSAL
#include <linux/udp.h>
#endif

int ipsec_mastdevice_count = -1;
int debug_mast;

static __u32 zeroes[64];

DEBUG_NO_STATIC int
ipsec_mast_open(struct net_device *dev)
{
        struct mastpriv *prv = dev->priv; 

	prv = prv;

	/*
	 * Can't open until attached.
	 */

	KLIPS_PRINT(debug_mast & DB_MAST_INIT,
		    "klips_debug:ipsec_mast_open: "
		    "dev = %s\n",
		    dev->name);

	return 0;
}

DEBUG_NO_STATIC int
ipsec_mast_close(struct net_device *dev)
{
	return 0;
}

static inline int ipsec_mast_xmit2(struct sk_buff *skb)
{
	return dst_output(skb);
}

enum ipsec_xmit_value
ipsec_mast_send(struct ipsec_xmit_state*ixs)
{
	struct flowi fl;

	/* new route/dst cache code from James Morris */
	ixs->skb->dev = ixs->physdev;

 	fl.oif = ixs->physdev->iflink;
 	fl.nl_u.ip4_u.daddr = ixs->skb->nh.iph->daddr;
 	fl.nl_u.ip4_u.saddr = ixs->pass ? 0 : ixs->skb->nh.iph->saddr;
 	fl.nl_u.ip4_u.tos = RT_TOS(ixs->skb->nh.iph->tos);
 	fl.proto = ixs->skb->nh.iph->protocol;
 	if ((ixs->error = ip_route_output_key(&ixs->route, &fl))) {
		ixs->stats->tx_errors++;
		KLIPS_PRINT(debug_mast & DB_MAST_XMIT,
			    "klips_debug:ipsec_xmit_send: "
			    "ip_route_output failed with error code %d, rt->u.dst.dev=%s, dropped\n",
			    ixs->error,
			    ixs->route->u.dst.dev->name);
		return IPSEC_XMIT_ROUTEERR;
	}

	if(ixs->dev == ixs->route->u.dst.dev) {
		ip_rt_put(ixs->route);
		/* This is recursion, drop it. */
		ixs->stats->tx_errors++;
		KLIPS_PRINT(debug_mast & DB_MAST_XMIT,
			    "klips_debug:ipsec_xmit_send: "
			    "suspect recursion, dev=rt->u.dst.dev=%s, dropped\n",
			    ixs->dev->name);
		return IPSEC_XMIT_RECURSDETECT;
	}

	dst_release(ixs->skb->dst);
	ixs->skb->dst = &ixs->route->u.dst;
	ixs->stats->tx_bytes += ixs->skb->len;
	if(ixs->skb->len < ixs->skb->nh.raw - ixs->skb->data) {
		ixs->stats->tx_errors++;
		printk(KERN_WARNING
		       "klips_error:ipsec_xmit_send: "
		       "tried to __skb_pull nh-data=%ld, %d available.  This should never happen, please report.\n",
		       (unsigned long)(ixs->skb->nh.raw - ixs->skb->data),
		       ixs->skb->len);
		return IPSEC_XMIT_PUSHPULLERR;
	}

	__skb_pull(ixs->skb, ixs->skb->nh.raw - ixs->skb->data);

#ifdef SKB_RESET_NFCT
	nf_conntrack_put(ixs->skb->nfct);
	ixs->skb->nfct = NULL;
#endif /* SKB_RESET_NFCT */

	KLIPS_PRINT(debug_mast & DB_MAST_XMIT,
		    "klips_debug:ipsec_xmit_send: "
		    "...done, calling ip_send() on device:%s\n",
		    ixs->skb->dev ? ixs->skb->dev->name : "NULL");

	KLIPS_IP_PRINT(debug_mast & DB_MAST_XMIT, ixs->skb->nh.iph);

	{
		int err;

		err = NF_HOOK(PF_INET, NF_IP_LOCAL_OUT, ixs->skb, NULL, ixs->route->u.dst.dev,
			      ipsec_mast_xmit2);
		if(err != NET_XMIT_SUCCESS && err != NET_XMIT_CN) {
			if(net_ratelimit())
				printk(KERN_ERR
				       "klips_error:ipsec_xmit_send: "
				       "ip_send() failed, err=%d\n", 
				       -err);
			ixs->stats->tx_errors++;
			ixs->stats->tx_aborted_errors++;
			ixs->skb = NULL;
			return IPSEC_XMIT_IPSENDFAILURE;
		}
	}

	ixs->stats->tx_packets++;

	ixs->skb = NULL;
	
	return IPSEC_XMIT_OK;
}

void
ipsec_mast_cleanup(struct ipsec_xmit_state*ixs)
{
#if defined(HAS_NETIF_QUEUE) || defined (HAVE_NETIF_QUEUE)
	netif_wake_queue(ixs->dev);
#else /* defined(HAS_NETIF_QUEUE) || defined (HAVE_NETIF_QUEUE) */
	ixs->dev->tbusy = 0;
#endif /* defined(HAS_NETIF_QUEUE) || defined (HAVE_NETIF_QUEUE) */

	if(ixs->saved_header) {
		kfree(ixs->saved_header);
	}
	if(ixs->skb) {
		dev_kfree_skb(ixs->skb, FREE_WRITE);
	}
	if(ixs->oskb) {
		dev_kfree_skb(ixs->oskb, FREE_WRITE);
	}
	if (ixs->ips.ips_ident_s.data) {
		kfree(ixs->ips.ips_ident_s.data);
	}
	if (ixs->ips.ips_ident_d.data) {
		kfree(ixs->ips.ips_ident_d.data);
	}
}

/*
 *	This function assumes it is being called from dev_queue_xmit()
 *	and that skb is filled properly by that function.
 */
int
ipsec_mast_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct ipsec_xmit_state ixs_mem;
	struct ipsec_xmit_state *ixs = &ixs_mem;
	enum ipsec_xmit_value stat = IPSEC_XMIT_OK;
	IPsecSAref_t SAref;

	if(skb == NULL) {
		printk("mast start_xmit passed NULL\n");
		return 0;
	}
		
	memset(&ixs_mem, 0, sizeof(struct ipsec_xmit_state));

	ixs->skb = skb;
	SAref = NFmark2IPsecSAref(skb->nfmark);
	ipsec_xmit_sanity_check_skb(ixs);

	ixs->ipsp = ipsec_sa_getbyref(SAref);
	if(ixs->ipsp == NULL) {
		printk("no SA for saref=%d\n", SAref);
		ipsec_kfree_skb(skb);
		return 0;
	}

	stat = ipsec_xmit_encap_bundle_2(ixs);
	if(stat != IPSEC_XMIT_OK) {
		/* SA processing failed */
	}

	ipsec_sa_put(ixs->ipsp);
	
	return 0;
}

DEBUG_NO_STATIC struct net_device_stats *
ipsec_mast_get_stats(struct net_device *dev)
{
	return &(((struct mastpriv *)(dev->priv))->mystats);
}

#if 0
/*
 * Revectored calls.
 * For each of these calls, a field exists in our private structure.
 */
DEBUG_NO_STATIC int
ipsec_mast_hard_header(struct sk_buff *skb, struct net_device *dev,
	unsigned short type, void *daddr, void *saddr, unsigned len)
{
	struct mastpriv *prv = dev->priv;
	struct net_device_stats *stats;	/* This device's statistics */
	int ret = 0;
	
	if(skb == NULL) {
		KLIPS_PRINT(debug_mast & DB_MAST_REVEC,
			    "klips_debug:ipsec_mast_hard_header: "
			    "no skb...\n");
		return -ENODATA;
	}

	if(dev == NULL) {
		KLIPS_PRINT(debug_mast & DB_MAST_REVEC,
			    "klips_debug:ipsec_mast_hard_header: "
			    "no device...\n");
		return -ENODEV;
	}

	KLIPS_PRINT(debug_mast & DB_MAST_REVEC,
		    "klips_debug:ipsec_mast_hard_header: "
		    "skb->dev=%s\n",
		    dev->name);
	
	if(prv == NULL) {
		KLIPS_PRINT(debug_mast & DB_MAST_REVEC,
			    "klips_debug:ipsec_mast_hard_header: "
			    "no private space associated with dev=%s\n",
			    dev->name ? dev->name : "NULL");
		return -ENODEV;
	}

	stats = (struct net_device_stats *) &(prv->mystats);

	/* check if we have to send a IPv6 packet. It might be a Router
	   Solicitation, where the building of the packet happens in
	   reverse order:
	   1. ll hdr,
	   2. IPv6 hdr,
	   3. ICMPv6 hdr
	   -> skb->nh.raw is still uninitialized when this function is
	   called!!  If this is no IPv6 packet, we can print debugging
	   messages, otherwise we skip all debugging messages and just
	   build the ll header */
	if(type != ETH_P_IPV6) {
		/* execute this only, if we don't have to build the
		   header for a IPv6 packet */
		if(!prv->hard_header) {
			KLIPS_PRINT(debug_mast & DB_MAST_REVEC,
				    "klips_debug:ipsec_mast_hard_header: "
				    "physical device has been detached, packet dropped 0p%p->0p%p len=%d type=%d dev=%s->NULL ",
				    saddr,
				    daddr,
				    len,
				    type,
				    dev->name);
			KLIPS_PRINTMORE(debug_mast & DB_MAST_REVEC,
					"ip=%08x->%08x\n",
					(__u32)ntohl(skb->nh.iph->saddr),
					(__u32)ntohl(skb->nh.iph->daddr) );
			stats->tx_dropped++;
			return -ENODEV;
		}
	} else {
		KLIPS_PRINT(debug_mast,
			    "klips_debug:ipsec_mast_hard_header: "
			    "is IPv6 packet, skip debugging messages, only revector and build linklocal header.\n");
	}                                                                      

	return ret;
}

DEBUG_NO_STATIC int
ipsec_mast_rebuild_header(struct sk_buff *skb)
{
	struct mastpriv *prv = skb->dev->priv;

	prv = prv;
	return 0;
}

DEBUG_NO_STATIC int
ipsec_mast_set_mac_address(struct net_device *dev, void *addr)
{
	struct mastpriv *prv = dev->priv;
	
	prv = prv;
	return 0;

}

DEBUG_NO_STATIC void
ipsec_mast_cache_update(struct hh_cache *hh, struct net_device *dev, unsigned char *  haddr)
{
	struct mastpriv *prv = dev->priv;
	
	if(dev == NULL) {
		KLIPS_PRINT(debug_mast & DB_MAST_REVEC,
			    "klips_debug:ipsec_mast_cache_update: "
			    "no device...");
		return;
	}

	if(prv == NULL) {
		KLIPS_PRINT(debug_mast & DB_MAST_REVEC,
			    "klips_debug:ipsec_mast_cache_update: "
			    "no private space associated with dev=%s",
			    dev->name ? dev->name : "NULL");
		return;
	}

	KLIPS_PRINT(debug_mast & DB_MAST_REVEC,
		    "klips_debug:ipsec_mast: "
		    "Revectored cache_update\n");
	return;
}
#endif

DEBUG_NO_STATIC int
ipsec_mast_neigh_setup(struct neighbour *n)
{
	KLIPS_PRINT(debug_mast & DB_MAST_REVEC,
		    "klips_debug:ipsec_mast_neigh_setup:\n");

        if (n->nud_state == NUD_NONE) {
                n->ops = &arp_broken_ops;
                n->output = n->ops->output;
        }
        return 0;
}

DEBUG_NO_STATIC int
ipsec_mast_neigh_setup_dev(struct net_device *dev, struct neigh_parms *p)
{
	KLIPS_PRINT(debug_mast & DB_MAST_REVEC,
		    "klips_debug:ipsec_mast_neigh_setup_dev: "
		    "setting up %s\n",
		    dev ? dev->name : "NULL");

        if (p->tbl->family == AF_INET) {
                p->neigh_setup = ipsec_mast_neigh_setup;
                p->ucast_probes = 0;
                p->mcast_probes = 0;
        }
        return 0;
}

DEBUG_NO_STATIC int
ipsec_mast_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd)
{
	struct ipsecmastconf *cf = (struct ipsecmastconf *)&ifr->ifr_data;
	struct ipsecpriv *prv = dev->priv;

	cf = cf;
	prv=prv;
	
	if(dev == NULL) {
		KLIPS_PRINT(debug_mast & DB_MAST_INIT,
			    "klips_debug:ipsec_mast_ioctl: "
			    "device not supplied.\n");
		return -ENODEV;
	}

	KLIPS_PRINT(debug_mast & DB_MAST_INIT,
		    "klips_debug:ipsec_mast_ioctl: "
		    "tncfg service call #%d for dev=%s\n",
		    cmd,
		    dev->name ? dev->name : "NULL");

	switch (cmd) {
	default:
		KLIPS_PRINT(debug_mast & DB_MAST_INIT,
			    "klips_debug:ipsec_mast_ioctl: "
			    "unknown command %d.\n",
			    cmd);
		return -EOPNOTSUPP;
	  
	}
}

int
ipsec_mast_device_event(struct notifier_block *unused, unsigned long event, void *ptr)
{
	struct net_device *dev = ptr;
	struct mastpriv *priv = dev->priv;

	priv = priv;

	if (dev == NULL) {
		KLIPS_PRINT(debug_mast & DB_MAST_INIT,
			    "klips_debug:ipsec_mast_device_event: "
			    "dev=NULL for event type %ld.\n",
			    event);
		return(NOTIFY_DONE);
	}

	/* check for loopback devices */
	if (dev && (dev->flags & IFF_LOOPBACK)) {
		return(NOTIFY_DONE);
	}

	switch (event) {
	case NETDEV_DOWN:
		/* look very carefully at the scope of these compiler
		   directives before changing anything... -- RGB */

	case NETDEV_UNREGISTER:
		switch (event) {
		case NETDEV_DOWN:
			KLIPS_PRINT(debug_mast & DB_MAST_INIT,
				    "klips_debug:ipsec_mast_device_event: "
				    "NETDEV_DOWN dev=%s flags=%x\n",
				    dev->name,
				    dev->flags);
			if(strncmp(dev->name, "ipsec", strlen("ipsec")) == 0) {
				printk(KERN_CRIT "IPSEC EVENT: KLIPS device %s shut down.\n",
				       dev->name);
			}
			break;
		case NETDEV_UNREGISTER:
			KLIPS_PRINT(debug_mast & DB_MAST_INIT,
				    "klips_debug:ipsec_mast_device_event: "
				    "NETDEV_UNREGISTER dev=%s flags=%x\n",
				    dev->name,
				    dev->flags);
			break;
		}
		break;

	case NETDEV_UP:
		KLIPS_PRINT(debug_mast & DB_MAST_INIT,
			    "klips_debug:ipsec_mast_device_event: "
			    "NETDEV_UP dev=%s\n",
			    dev->name);
		break;

	case NETDEV_REBOOT:
		KLIPS_PRINT(debug_mast & DB_MAST_INIT,
			    "klips_debug:ipsec_mast_device_event: "
			    "NETDEV_REBOOT dev=%s\n",
			    dev->name);
		break;

	case NETDEV_CHANGE:
		KLIPS_PRINT(debug_mast & DB_MAST_INIT,
			    "klips_debug:ipsec_mast_device_event: "
			    "NETDEV_CHANGE dev=%s flags=%x\n",
			    dev->name,
			    dev->flags);
		break;

	case NETDEV_REGISTER:
		KLIPS_PRINT(debug_mast & DB_MAST_INIT,
			    "klips_debug:ipsec_mast_device_event: "
			    "NETDEV_REGISTER dev=%s\n",
			    dev->name);
		break;

	case NETDEV_CHANGEMTU:
		KLIPS_PRINT(debug_mast & DB_MAST_INIT,
			    "klips_debug:ipsec_mast_device_event: "
			    "NETDEV_CHANGEMTU dev=%s to mtu=%d\n",
			    dev->name,
			    dev->mtu);
		break;

	case NETDEV_CHANGEADDR:
		KLIPS_PRINT(debug_mast & DB_MAST_INIT,
			    "klips_debug:ipsec_mast_device_event: "
			    "NETDEV_CHANGEADDR dev=%s\n",
			    dev->name);
		break;

	case NETDEV_GOING_DOWN:
		KLIPS_PRINT(debug_mast & DB_MAST_INIT,
			    "klips_debug:ipsec_mast_device_event: "
			    "NETDEV_GOING_DOWN dev=%s\n",
			    dev->name);
		break;

	case NETDEV_CHANGENAME:
		KLIPS_PRINT(debug_mast & DB_MAST_INIT,
			    "klips_debug:ipsec_mast_device_event: "
			    "NETDEV_CHANGENAME dev=%s\n",
			    dev->name);
		break;

	default:
		KLIPS_PRINT(debug_mast & DB_MAST_INIT,
			    "klips_debug:ipsec_mast_device_event: "
			    "event type %ld unrecognised for dev=%s\n",
			    event,
			    dev->name);
		break;
	}
	return NOTIFY_DONE;
}

/*
 *	Called when an ipsec mast device is initialized.
 *	The ipsec mast device structure is passed to us.
 */
int
ipsec_mast_probe(struct net_device *dev)
{
	int i;

	KLIPS_PRINT(debug_mast,
		    "klips_debug:ipsec_mast_init: "
		    "allocating %lu bytes initialising device: %s\n",
		    (unsigned long) sizeof(struct mastpriv),
		    dev->name ? dev->name : "NULL");

	/* Add our mast functions to the device */
	dev->open		= ipsec_mast_open;
	dev->stop		= ipsec_mast_close;
	dev->hard_start_xmit	= ipsec_mast_start_xmit;
	dev->get_stats		= ipsec_mast_get_stats;

	dev->priv = kmalloc(sizeof(struct mastpriv), GFP_KERNEL);
	if (dev->priv == NULL)
		return -ENOMEM;
	memset((caddr_t)(dev->priv), 0, sizeof(struct mastpriv));

	for(i = 0; i < sizeof(zeroes); i++) {
		((__u8*)(zeroes))[i] = 0;
	}
	
	dev->set_multicast_list = NULL;
	dev->do_ioctl		= ipsec_mast_ioctl;
	dev->hard_header	= NULL;
	dev->rebuild_header 	= NULL;
	dev->set_mac_address 	= NULL;
	dev->header_cache_update= NULL;
	dev->neigh_setup        = ipsec_mast_neigh_setup_dev;
	dev->hard_header_len 	= 0;
	dev->mtu		= 0;
	dev->addr_len		= 0;
	dev->type		= ARPHRD_NONE;
	dev->tx_queue_len	= 10;		
	memset((caddr_t)(dev->broadcast),0xFF, ETH_ALEN);	/* what if this is not attached to ethernet? */

	/* New-style flags. */
	dev->flags		= IFF_NOARP;

	/* We're done.  Have I forgotten anything? */
	return 0;
}

int ipsec_mast_create(int num) 
{
	struct net_device *im;

	im = (struct net_device *)kmalloc(sizeof(struct net_device),GFP_KERNEL);
	if(im == NULL) {
		printk(KERN_ERR "failed to allocate space for mast%d device\n", num);
		return -ENOMEM;
	}
		
	memset((caddr_t)im, 0, sizeof(struct net_device));
	snprintf(im->name, IFNAMSIZ, MAST_DEV_FORMAT, num);

	im->next = NULL;
	im->init = ipsec_mast_probe;

	if(ipsec_mastdevice_count < num+1) {
		ipsec_mastdevice_count = num+1;
	}
	
	if(register_netdev(im) != 0) {
		printk(KERN_ERR "ipsec_mast: failed to register %s\n",
		       im->name);
		return -EIO;
	}

	return 0;
}
	

int 
ipsec_mast_init_devices(void)
{
	/*
	 * mast0 is used for transport mode stuff, and generally is
	 * the default unless the user decides to create more.
	 */
	ipsec_mast_create(0);
  
	return 0;
}

/* void */
int
ipsec_mast_cleanup_devices(void)
{
	int error = 0;
	int i;
	char name[10];
	struct net_device *dev_mast;
	
	for(i = 0; i < ipsec_mastdevice_count; i++) {
		sprintf(name, MAST_DEV_FORMAT, i);
		if((dev_mast = ipsec_dev_get(name)) == NULL) {
			continue;
		}
		unregister_netdev(dev_mast);
		kfree(dev_mast->priv);
		dev_mast->priv=NULL;
	}
	return error;
}

/*
 * $Log: ipsec_mast.c,v $
 *
 * Local Variables:
 * c-file-style: "linux"
 * End:
 *
 */

