/* vi: set sw=4 ts=4: */
/*
 * Licensed under GPLv2 or later, see file LICENSE in this source tree.
 *
 * Author: Alexey Kuznetsov <kuznet@ms2.inr.ac.ru>
 * Busybox port: Nick Fedchik <nick@fedchik.org.ua>
 */
//config:config ARPING
//config:	bool "arping (9.3 kb)"
//config:	default y
//config:	select PLATFORM_LINUX
//config:	help
//config:	Ping hosts by ARP packets.

//applet:IF_ARPING(APPLET(arping, BB_DIR_USR_SBIN, BB_SUID_DROP))

//kbuild:lib-$(CONFIG_ARPING) += arping.o

//usage:#define arping_trivial_usage
//usage:       "[-fqbDUA] [-c CNT] [-w TIMEOUT] [-I IFACE] [-s SRC_IP] DST_IP"
//usage:#define arping_full_usage "\n\n"
//usage:       "Send ARP requests/replies\n"
//usage:     "\n	-f		Quit on first ARP reply"
//usage:     "\n	-q		Quiet"
//usage:     "\n	-b		Keep broadcasting, don't go unicast"
//usage:     "\n	-D		Exit with 1 if DST_IP replies"
//usage:     "\n	-U		Unsolicited ARP mode, update your neighbors"
//usage:     "\n	-A		ARP answer mode, update your neighbors"
//usage:     "\n	-c N		Stop after sending N ARP requests"
//usage:     "\n	-w TIMEOUT	Seconds to wait for ARP reply"
//usage:     "\n	-I IFACE	Interface to use (default eth0)"
//usage:     "\n	-s SRC_IP	Sender IP address"
//usage:     "\n	DST_IP		Target IP address"

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/ether.h>
#include <netpacket/packet.h>

#include "libbb.h"
#include "common_bufsiz.h"

/* We don't expect to see 1000+ seconds delay, unsigned is enough */
#define MONOTONIC_US() ((unsigned)monotonic_us())

enum {
	UNSOLICITED   = 1 << 0,
	DAD           = 1 << 1,
	ADVERT        = 1 << 2,
	QUIET         = 1 << 3,
	QUIT_ON_REPLY = 1 << 4,
	BCAST_ONLY    = 1 << 5,
	UNICASTING    = 1 << 6,
	TIMEOUT       = 1 << 7,
};
#define GETOPT32(str_timeout, device, source) \
	getopt32(argv, "^" \
		"UDAqfbc:+w:I:s:" \
		/* Dad also sets quit_on_reply, */ \
		/* Advert also sets unsolicited: */ \
		"\0" "=1:Df:AU", \
		&count, &str_timeout, &device, &source \
	);

struct globals {
	struct in_addr src;
	struct in_addr dst;
	struct sockaddr_ll me;
	struct sockaddr_ll he;
	int sock_fd;

	int count; // = -1;
	unsigned last;
	unsigned timeout_us;
	unsigned start;

	unsigned sent;
	unsigned brd_sent;
	unsigned received;
	unsigned brd_recv;
	unsigned req_recv;
} FIX_ALIASING;
#define G (*(struct globals*)bb_common_bufsiz1)
#define src        (G.src       )
#define dst        (G.dst       )
#define me         (G.me        )
#define he         (G.he        )
#define sock_fd    (G.sock_fd   )
#define count      (G.count     )
#define last       (G.last      )
#define timeout_us (G.timeout_us)
#define start      (G.start     )
#define sent       (G.sent      )
#define brd_sent   (G.brd_sent  )
#define received   (G.received  )
#define brd_recv   (G.brd_recv  )
#define req_recv   (G.req_recv  )
#define INIT_G() do { \
	setup_common_bufsiz(); \
	count = -1; \
} while (0)

static int send_pack(struct in_addr *src_addr,
			struct in_addr *dst_addr,
			struct sockaddr_ll *ME,
			struct sockaddr_ll *HE)
{
	int err;
	unsigned char buf[256];
	struct arphdr *ah = (struct arphdr *) buf;
	unsigned char *p;

	ah->ar_hrd = htons(ARPHRD_ETHER);
	ah->ar_pro = htons(ETH_P_IP);
	ah->ar_hln = ME->sll_halen;
	ah->ar_pln = 4;
	ah->ar_op = option_mask32 & ADVERT ? htons(ARPOP_REPLY) : htons(ARPOP_REQUEST);

	p = (unsigned char *) (ah + 1);
	p = mempcpy(p, &ME->sll_addr, ah->ar_hln);
	p = mempcpy(p, src_addr, 4);

	if (option_mask32 & ADVERT)
		p = mempcpy(p, &ME->sll_addr, ah->ar_hln);
	else
		p = mempcpy(p, &HE->sll_addr, ah->ar_hln);

	p = mempcpy(p, dst_addr, 4);

	err = sendto(sock_fd, buf, p - buf, 0, (struct sockaddr *) HE, sizeof(*HE));
	if (err == p - buf) {
		last = MONOTONIC_US();
		sent++;
		if (!(option_mask32 & UNICASTING))
			brd_sent++;
	}
	return err;
}

static void finish(void) NORETURN;
static void finish(void)
{
	if (!(option_mask32 & QUIET)) {
		printf("Sent %u probe(s) (%u broadcast(s))\n"
			"Received %u response(s)"
			" (%u request(s), %u broadcast(s))\n",
			sent, brd_sent,
			received,
			req_recv, brd_recv);
	}
	if (option_mask32 & DAD)
		exit(!!received);
	if (option_mask32 & UNSOLICITED)
		exit(EXIT_SUCCESS);
	exit(!received);
}

static void catcher(void)
{
	unsigned now;

	now = MONOTONIC_US();
	if (start == 0)
		start = now;

	if (count == 0 || (timeout_us && (now - start) > timeout_us))
		finish();

	/* count < 0 means "infinite count" */
	if (count > 0)
		count--;

	if (last == 0 || (now - last) > 500000) {
		send_pack(&src, &dst, &me, &he);
		if (count == 0 && (option_mask32 & UNSOLICITED))
			finish();
	}
	alarm(1);
}

static void recv_pack(unsigned char *buf, int len, struct sockaddr_ll *FROM)
{
	struct arphdr *ah = (struct arphdr *) buf;
	unsigned char *p = (unsigned char *) (ah + 1);
	struct in_addr src_ip, dst_ip;
	/* moves below assume in_addr is 4 bytes big, ensure that */
	struct BUG_in_addr_must_be_4 {
		char BUG_in_addr_must_be_4[
			sizeof(struct in_addr) == 4 ? 1 : -1
		];
		char BUG_s_addr_must_be_4[
			sizeof(src_ip.s_addr) == 4 ? 1 : -1
		];
	};

	/* Filter out wild packets */
	if (FROM->sll_pkttype != PACKET_HOST
	 && FROM->sll_pkttype != PACKET_BROADCAST
	 && FROM->sll_pkttype != PACKET_MULTICAST)
		return;

	/* Only these types are recognized */
	if (ah->ar_op != htons(ARPOP_REQUEST) && ah->ar_op != htons(ARPOP_REPLY))
		return;

	/* ARPHRD check and this darned FDDI hack here :-( */
	if (ah->ar_hrd != htons(FROM->sll_hatype)
	 && (FROM->sll_hatype != ARPHRD_FDDI || ah->ar_hrd != htons(ARPHRD_ETHER)))
		return;

	/* Protocol must be IP. */
	if (ah->ar_pro != htons(ETH_P_IP)
	 || (ah->ar_pln != 4)
	 || (ah->ar_hln != me.sll_halen)
	 || (len < (int)(sizeof(*ah) + 2 * (4 + ah->ar_hln))))
		return;

	move_from_unaligned32(src_ip.s_addr, p + ah->ar_hln);
	move_from_unaligned32(dst_ip.s_addr, p + ah->ar_hln + 4 + ah->ar_hln);

	if (dst.s_addr != src_ip.s_addr)
		return;
	if (!(option_mask32 & DAD)) {
		if ((src.s_addr != dst_ip.s_addr)
		 || (memcmp(p + ah->ar_hln + 4, &me.sll_addr, ah->ar_hln)))
			return;
	} else {
		/* DAD packet was:
		   src_ip = 0 (or some src)
		   src_hw = ME
		   dst_ip = tested address
		   dst_hw = <unspec>

		   We fail, if receive request/reply with:
		   src_ip = tested_address
		   src_hw != ME
		   if src_ip in request was not zero, check
		   also that it matches to dst_ip, otherwise
		   dst_ip/dst_hw do not matter.
		 */
		if ((memcmp(p, &me.sll_addr, me.sll_halen) == 0)
		 || (src.s_addr && src.s_addr != dst_ip.s_addr))
			return;
	}
	if (!(option_mask32 & QUIET)) {
		int s_printed = 0;

//TODO: arping from iputils-s20160308 print upprcase hex in MAC, follow them?
		printf("%scast re%s from %s [%02x:%02x:%02x:%02x:%02x:%02x]",
			FROM->sll_pkttype == PACKET_HOST ? "Uni" : "Broad",
			ah->ar_op == htons(ARPOP_REPLY) ? "ply" : "quest",
			inet_ntoa(src_ip),
			p[0], p[1], p[2], p[3], p[4], p[5]
		);
		if (dst_ip.s_addr != src.s_addr) {
			printf("for %s", inet_ntoa(dst_ip));
			s_printed = 1;
		}
		if (memcmp(p + ah->ar_hln + 4, me.sll_addr, ah->ar_hln)) {
			unsigned char *pp = p + ah->ar_hln + 4;
			if (!s_printed)
				printf(" for");
			printf(" [%02x:%02x:%02x:%02x:%02x:%02x]",
				pp[0], pp[1], pp[2], pp[3], pp[4], pp[5]
			);
		}

		if (last) {
			unsigned diff = MONOTONIC_US() - last;
			printf(" %u.%03ums\n", diff / 1000, diff % 1000);
		} else {
			puts(" UNSOLICITED?");
		}
		fflush_all();
	}
	received++;
	if (FROM->sll_pkttype != PACKET_HOST)
		brd_recv++;
	if (ah->ar_op == htons(ARPOP_REQUEST))
		req_recv++;
	if (option_mask32 & QUIT_ON_REPLY)
		finish();
	if (!(option_mask32 & BCAST_ONLY)) {
		memcpy(he.sll_addr, p, me.sll_halen);
		option_mask32 |= UNICASTING;
	}
}

int arping_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int arping_main(int argc UNUSED_PARAM, char **argv)
{
	const char *device = "eth0";
	char *source = NULL;
	char *target;
	unsigned char *packet;
	char *err_str;

	INIT_G();

	sock_fd = xsocket(AF_PACKET, SOCK_DGRAM, 0);

	// If you ever change BB_SUID_DROP to BB_SUID_REQUIRE,
	// drop suid root privileges here:
	//xsetuid(getuid());

	{
		unsigned opt;
		char *str_timeout;

		opt = GETOPT32(str_timeout, device, source);
		if (opt & TIMEOUT)
			timeout_us = xatou_range(str_timeout, 0, INT_MAX/2000000) * 1000000 + 500000;
	}

	target = argv[optind];
	err_str = xasprintf("interface %s %%s", device);
	xfunc_error_retval = 2;

	{
		struct ifreq ifr;

		memset(&ifr, 0, sizeof(ifr));
		strncpy_IFNAMSIZ(ifr.ifr_name, device);
		/* We use ifr.ifr_name in error msg so that problem
		 * with truncated name will be visible */
		ioctl_or_perror_and_die(sock_fd, SIOCGIFINDEX, &ifr, err_str, "not found");
		me.sll_ifindex = ifr.ifr_ifindex;

		xioctl(sock_fd, SIOCGIFFLAGS, (char *) &ifr);

		if (!(ifr.ifr_flags & IFF_UP)) {
			bb_error_msg_and_die(err_str, "is down");
		}
		if (ifr.ifr_flags & (IFF_NOARP | IFF_LOOPBACK)) {
			bb_error_msg(err_str, "is not ARPable");
			BUILD_BUG_ON(DAD != 2);
			/* exit 0 if DAD, else exit 2 */
			return (~option_mask32 & DAD);
		}
	}

	/* if (!inet_aton(target, &dst)) - not needed */ {
		len_and_sockaddr *lsa;
		lsa = xhost_and_af2sockaddr(target, 0, AF_INET);
		dst = lsa->u.sin.sin_addr;
		if (ENABLE_FEATURE_CLEAN_UP)
			free(lsa);
	}

	if (source && !inet_aton(source, &src)) {
		bb_error_msg_and_die("invalid source address %s", source);
	}

	if ((option_mask32 & (DAD|UNSOLICITED)) == UNSOLICITED && src.s_addr == 0)
		src = dst;

	if (!(option_mask32 & DAD) || src.s_addr) {
		struct sockaddr_in saddr;
		int probe_fd = xsocket(AF_INET, SOCK_DGRAM, 0);

		setsockopt_bindtodevice(probe_fd, device);
		memset(&saddr, 0, sizeof(saddr));
		saddr.sin_family = AF_INET;
		if (src.s_addr) {
			/* Check that this is indeed our IP */
			saddr.sin_addr = src;
			xbind(probe_fd, (struct sockaddr *) &saddr, sizeof(saddr));
		} else { /* !(option_mask32 & DAD) case */
			/* Find IP address on this iface */
			socklen_t alen = sizeof(saddr);

			saddr.sin_port = htons(1025);
			saddr.sin_addr = dst;

			if (setsockopt_SOL_SOCKET_1(probe_fd, SO_DONTROUTE) != 0)
				bb_perror_msg("setsockopt(%s)", "SO_DONTROUTE");
			xconnect(probe_fd, (struct sockaddr *) &saddr, sizeof(saddr));
			getsockname(probe_fd, (struct sockaddr *) &saddr, &alen);
			//never happens:
			//if (getsockname(probe_fd, (struct sockaddr *) &saddr, &alen) == -1)
			//	bb_perror_msg_and_die("getsockname");
			if (saddr.sin_family != AF_INET)
				bb_error_msg_and_die("no IP address configured");
			src = saddr.sin_addr;
		}
		close(probe_fd);
	}

	me.sll_family = AF_PACKET;
	//me.sll_ifindex = ifindex; - done before
	me.sll_protocol = htons(ETH_P_ARP);
	xbind(sock_fd, (struct sockaddr *) &me, sizeof(me));

	{
		socklen_t alen = sizeof(me);
		getsockname(sock_fd, (struct sockaddr *) &me, &alen);
		//never happens:
		//if (getsockname(sock_fd, (struct sockaddr *) &me, &alen) == -1)
		//	bb_perror_msg_and_die("getsockname");
	}
	if (me.sll_halen == 0) {
		bb_error_msg(err_str, "is not ARPable (no ll address)");
		BUILD_BUG_ON(DAD != 2);
		/* exit 0 if DAD, else exit 2 */
		return (~option_mask32 & DAD);
	}
	he = me;
	memset(he.sll_addr, -1, he.sll_halen);

	if (!(option_mask32 & QUIET)) {
		/* inet_ntoa uses static storage, can't use in same printf */
		printf("ARPING %s", inet_ntoa(dst));
		printf(" from %s %s\n", inet_ntoa(src), device);
	}

	packet = xmalloc(4096);

	signal_SA_RESTART_empty_mask(SIGINT,  (void (*)(int))finish);
	signal_SA_RESTART_empty_mask(SIGALRM, (void (*)(int))catcher);

	/* Send the first packet, arm ALRM */
	catcher();

	while (1) {
		sigset_t sset;
		struct sockaddr_ll from;
		socklen_t alen = sizeof(from);
		int cc;

		sigemptyset(&sset);
		sigaddset(&sset, SIGALRM);
		sigaddset(&sset, SIGINT);
		/* Unblock SIGALRM so that the previously called alarm()
		 * can prevent recvfrom from blocking forever in case the
		 * inherited procmask is blocking SIGALRM.
		 */
		sigprocmask(SIG_UNBLOCK, &sset, NULL);

		cc = recvfrom(sock_fd, packet, 4096, 0, (struct sockaddr *) &from, &alen);

		/* Don't allow SIGALRMs while we process the reply */
		sigprocmask(SIG_BLOCK, &sset, NULL);
		if (cc < 0) {
			bb_perror_msg("recvfrom");
			continue;
		}
		recv_pack(packet, cc, &from);
	}
}
