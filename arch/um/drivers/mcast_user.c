/*
 * user-mode-linux networking multicast transport
 * Copyright (C) 2001 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 * Copyright (C) 2001 by Harald Welte <laforge@gnumonks.org>
 *
 * based on the existing uml-networking code, which is
 * Copyright (C) 2001 Lennert Buytenhek (buytenh@gnu.org) and
 * James Leu (jleu@mindspring.net).
 * Copyright (C) 2001 by various other people who didn't put their name here.
 *
 * Licensed under the GPL.
 *
 */

#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "kern_constants.h"
#include "mcast.h"
#include "net_user.h"
#include "um_malloc.h"
#include "user.h"

static struct sockaddr_in *new_sockaddr_sin(void)
{
	struct sockaddr_in *sin;

	sin = uml_kmalloc(sizeof(struct sockaddr_in), UM_GFP_KERNEL);
	if (sin == NULL) {
		printk(UM_KERN_ERR "new_addr: allocation of sockaddr_in "
		       "failed\n");
		return NULL;
	}
        return sin;
}

static struct sockaddr_in *new_addr(char *addr, unsigned short port)
{
	struct sockaddr_in *sin = new_sockaddr_sin();
        if(!sin) return NULL;

	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = in_aton(addr);
	sin->sin_port = htons(port);
	return sin;
}

static int mcast_user_init(void *data, void *dev)
{
	struct mcast_data *pri = data;

	pri->mcast_addr = new_addr(pri->addr, pri->port);
	pri->dev = dev;
	return 0;
}

static void mcast_remove(void *data)
{
	struct mcast_data *pri = data;

	kfree(pri->mcast_addr);
	pri->mcast_addr = NULL;
}

static int mcast_open(void *data)
{
	struct mcast_data *pri = data;
	struct sockaddr_in *sin = pri->mcast_addr;
	struct ip_mreq mreq;
	int fd, yes = 1, err = -EINVAL;
        int i;
        int socklen = sizeof(struct sockaddr_in);
        int portnum = ntohs(sin->sin_port);
        //int no = 0;

	if ((sin->sin_addr.s_addr == 0) || (sin->sin_port == 0))
		goto out;

        /* first create a socket to use for outgoing communications. */
	fd = socket(AF_INET, SOCK_DGRAM, 0);

	if (fd < 0) {
		err = -errno;
		printk(UM_KERN_ERR "mcast_open : data send socket failed, "
		       "errno = %d\n", errno);
		goto out;
	}

	/* set ttl according to config */
	if (setsockopt(fd, SOL_IP, IP_MULTICAST_TTL, &pri->ttl,
		       sizeof(pri->ttl)) < 0) {
		err = -errno;
		printk(UM_KERN_ERR "mcast_open: IP_MULTICAST_TTL failed, "
		       "error = %d\n", errno);
		goto out_close;
	}

	/* set LOOP, so data does get fed back to local sockets */
        /* althouth it would be nice to set this to "no", if we do
         * that then no other UML on this host will get data, which
         * is not what we want.  Instead, to avoid hearing our own packets
         * echo back, we bind our local port, and filter stuff from ourselves.
         */
	if (setsockopt(fd, SOL_IP, IP_MULTICAST_LOOP, &yes, sizeof(yes)) < 0) {
		err = -errno;
		printk(UM_KERN_ERR "mcast_open: IP_MULTICAST_LOOP failed, "
		       "error = %d\n", errno);
		goto out_close;
	}

        pri->sender_addr = new_sockaddr_sin();
        for(i=0; i<64; i++) {
                struct sockaddr_in *sender = pri->sender_addr;

                memset(sender, 0, sizeof(*sender));
                sender->sin_port = htons(++portnum);

                /* try to bind socket to an address */
                if (bind(fd, (struct sockaddr *) sender, sizeof(*sender)) < 0) {
                        if(errno == EADDRINUSE) continue;
                        err = -errno;
                        printk(UM_KERN_ERR "mcast_open : data bind failed on port=%u, "
                               "errno = %d\n", portnum, errno);
                        goto out_close;
                }
                break;
        }

        if(getsockname(fd, (struct sockaddr*)pri->sender_addr, &socklen) < 0) {
		err = -errno;
		printk(UM_KERN_ERR "mcast_open: could not get address of sending socket, "
		       "error = %d\n", errno);
		goto out_close;
	}

#if 0
        {
                static char sentfrombuf[256];
                struct sockaddr_in *sender = pri->sender_addr;
                inet_ntop(AF_INET, pri->sender_addr,
                          sentfrombuf, sizeof(sentfrombuf));
                printk(UM_KERN_ERR "sending from %s:%u\n", sentfrombuf,
                       ntohs(sender->sin_port));
        }
#endif

	pri->send_fd = fd;

        /* now open a socket to receive on */

	fd = socket(AF_INET, SOCK_DGRAM, 0);

	if (fd < 0) {
		err = -errno;
		printk(UM_KERN_ERR "mcast_open : data socket failed, "
		       "errno = %d\n", errno);
		goto out;
	}

	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
		err = -errno;
		printk(UM_KERN_ERR "mcast_open: SO_REUSEADDR failed, "
		       "errno = %d\n", errno);
		goto out_close;
	}

	/* set ttl according to config */
	if (setsockopt(fd, SOL_IP, IP_MULTICAST_TTL, &pri->ttl,
		       sizeof(pri->ttl)) < 0) {
		err = -errno;
		printk(UM_KERN_ERR "mcast_open: IP_MULTICAST_TTL failed, "
		       "error = %d\n", errno);
		goto out_close;
	}

	/* set LOOP, so data does get fed back to local sockets */
        /* not sure if we need to set this on the receive socket...? */
        /* althouth it would be nice to set this to "no", if we do
         * that then no other UML on this host will get data, which
         * is not what we want.  Instead, to avoid hearing our own packets
         * echo back, we bind our local port, and filter stuff from ourselves.
         */
	if (setsockopt(fd, SOL_IP, IP_MULTICAST_LOOP, &yes, sizeof(yes)) < 0) {
		err = -errno;
		printk(UM_KERN_ERR "mcast_open: IP_MULTICAST_LOOP failed, "
		       "error = %d\n", errno);
		goto out_close;
	}

        /* bind socket to mcast address */
        if (bind(fd, (struct sockaddr *) sin, sizeof(*sin)) < 0) {
                err = -errno;
                printk(UM_KERN_ERR "mcast_open : recv bind failed, "
                       "errno = %d\n", errno);
                goto out_close;
        }

	/* subscribe to the multicast group */
	mreq.imr_multiaddr.s_addr = sin->sin_addr.s_addr;
	mreq.imr_interface.s_addr = 0;
	if (setsockopt(fd, SOL_IP, IP_ADD_MEMBERSHIP,
		       &mreq, sizeof(mreq)) < 0) {
		err = -errno;
		printk(UM_KERN_ERR "mcast_open: IP_ADD_MEMBERSHIP failed, "
		       "error = %d\n", errno);
		printk(UM_KERN_ERR "There appears not to be a multicast-"
		       "capable network interface on the host.\n");
		printk(UM_KERN_ERR "eth0 should be configured in order to use "
		       "the multicast transport.\n");
		goto out_close;
	}

	return fd;

 out_close:
	close(fd);
 out:
	return err;
}

static void mcast_close(int fd, void *data)
{
	struct ip_mreq mreq;
	struct mcast_data *pri = data;
	struct sockaddr_in *sin = pri->mcast_addr;

	mreq.imr_multiaddr.s_addr = sin->sin_addr.s_addr;
	mreq.imr_interface.s_addr = 0;
	if (setsockopt(fd, SOL_IP, IP_DROP_MEMBERSHIP,
		       &mreq, sizeof(mreq)) < 0) {
		printk(UM_KERN_ERR "mcast_open: IP_DROP_MEMBERSHIP failed, "
		       "error = %d\n", errno);
	}

	close(fd);
}

int mcast_user_write(int fd, void *buf, int len, struct mcast_data *pri)
{
	struct sockaddr_in *data_addr = pri->mcast_addr;

	return net_sendto(pri->send_fd, buf, len, data_addr, sizeof(*data_addr));
}

int mcast_user_read(int fd, void *buf, int len, struct mcast_data *pri)
{
        static char sentfrombuf[256];
        struct sockaddr_in sentfrom;
        int sentfromlen = sizeof(sentfrom);
        struct sockaddr_in *send_addr = pri->sender_addr;
        int size = net_recvfrom2(fd, buf, len, (struct sockaddr *)&sentfrom, &sentfromlen);

        inet_ntop(AF_INET, &sentfrom.sin_addr,
                  sentfrombuf, sizeof(sentfrombuf));

#if 0
        printk(UM_KERN_ERR "packet from %s:%u vs %u (len=%d)\n", sentfrombuf,
               ntohs(sentfrom.sin_port),
               ntohs(send_addr->sin_port), sentfromlen);
#endif

        /* 
         * the problem is that we don't know what IP address we will
         * actually transmit from, so we can't actually check this.
         * if we knew (because the user told us), then we could use it.
         * otherwise, we are in fact transmitting from whatever IP address
         * is bound to the interface on which the multicast is occuring.
         */
        if(
                /*memcmp(&send_addr->sin_addr, &sentfrom.sin_addr, sizeof(struct in_addr))==0  && */
           sentfrom.sin_port == send_addr->sin_port) {
#if 0                
                printk(UM_KERN_ERR "self packet dropped\n");
#endif
                return 0;
        }

        return size;
}

const struct net_user_info mcast_user_info = {
	.init		= mcast_user_init,
	.open		= mcast_open,
	.close	 	= mcast_close,
	.remove	 	= mcast_remove,
	.add_address	= NULL,
	.delete_address = NULL,
	.mtu		= ETH_MAX_PACKET,
	.max_packet	= ETH_MAX_PACKET + ETH_HEADER_OTHER,
};
