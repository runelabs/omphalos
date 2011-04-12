#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <linux/if_addr.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

int netlink_socket(void){
	struct sockaddr_nl sa;
	int fd;

	if((fd = socket(AF_NETLINK,SOCK_RAW,NETLINK_ROUTE)) < 0){
		fprintf(stderr,"Couldn't open NETLINK_ROUTE socket (%s?)\n",strerror(errno));
		return -1;
	}
	memset(&sa,0,sizeof(sa));
	sa.nl_family = AF_NETLINK;
	sa.nl_groups = RTNLGRP_MAX;
	if(bind(fd,(const struct sockaddr *)&sa,sizeof(sa))){
		fprintf(stderr,"Couldn't bind NETLINK_ROUTE socket %d (%s?)\n",fd,strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

#define nldiscover(msg,famtype,famfield) do {\
	struct { struct nlmsghdr nh ; struct famtype m ; } req = { \
		.nh = { .nlmsg_len = NLMSG_LENGTH(sizeof(req.m)), \
			.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_DUMP, \
			.nlmsg_type = msg, }, \
		.m = { .famfield = AF_UNSPEC, }, }; \
	int r; \
	if((r = send(fd,&req,req.nh.nlmsg_len,0)) < 0){ \
		fprintf(stderr,"Failure writing " #msg " to %d (%s?)\n",\
				fd,strerror(errno)); \
	} \
	return r; \
}while(0)

int discover_addrs(int fd){
	nldiscover(RTM_GETADDR,ifaddrmsg,ifa_family);
}

int discover_links(int fd){
	struct {
		struct nlmsghdr nh;
		struct ifinfomsg ii;
	} req = {
		.nh = {
			.nlmsg_len = NLMSG_LENGTH(sizeof(req.ii)),
			.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_DUMP,
			.nlmsg_type = RTM_GETLINK,
		},
		.ii = {
			.ifi_family = AF_UNSPEC,
		},
	};
	int r;

	if((r = send(fd,&req,sizeof(req),0)) < 0){
		fprintf(stderr,"Failure writing RTM_GETLINK message to %d (%s?)\n",fd,strerror(errno));
	}
	return r;
}

/* This is all pretty omphalos-specific from here on out */
#include <stdlib.h>
#include <sys/uio.h>
#include <net/ethernet.h>
#include <linux/if_arp.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <omphalos/interface.h>

typedef struct arptype {
	unsigned ifi_type;
	const char *name;
} arptype;

static arptype arptypes[] = {
	{
		.ifi_type = ARPHRD_LOOPBACK,
		.name = "loopback",
	},{
		.ifi_type = ARPHRD_ETHER,
		.name = "Ethernet",
	},{
		.ifi_type = ARPHRD_IEEE80211,
		.name = "IEEE 802.11",
	},
};

static inline const arptype *
lookup_arptype(unsigned arphrd){
	unsigned idx;

	for(idx = 0 ; idx < sizeof(arptypes) / sizeof(*arptypes) ; ++idx){
		const arptype *at = arptypes + idx;

		if(at->ifi_type == arphrd){
			return at;
		}
	}
	return NULL;
}

static int
handle_rtm_newneigh(const struct nlmsghdr *nl){
	const struct ndmsg *nd = NLMSG_DATA(nl);
	interface *iface;

	if((iface = iface_by_idx(nd->ndm_ifindex)) == NULL){
		fprintf(stderr,"Invalid interface index: %d\n",nd->ndm_ifindex);
		return -1;
	}
	printf("[%s] NEIGHBOR ADDED\n",iface->name);
	// FIXME
	return 0;
}

static int
handle_rtm_delneigh(const struct nlmsghdr *nl){
	const struct ndmsg *nd = NLMSG_DATA(nl);
	interface *iface;

	if((iface = iface_by_idx(nd->ndm_ifindex)) == NULL){
		fprintf(stderr,"Invalid interface index: %d\n",nd->ndm_ifindex);
		return -1;
	}
	printf("[%s] NEIGHBOR DELETED\n",iface->name);
	// FIXME
	return 0;
}

static int
handle_rtm_deladdr(const struct nlmsghdr *nl){
	const struct ifaddrmsg *ia = NLMSG_DATA(nl);
	interface *iface;

	if((iface = iface_by_idx(ia->ifa_index)) == NULL){
		fprintf(stderr,"Invalid interface index: %d\n",ia->ifa_index);
		return -1;
	}
	printf("[%s] ADDRESS DELETED\n",iface->name);
	// FIXME
	return 0;
}

static int
handle_rtm_newaddr(const struct nlmsghdr *nl){
	const struct ifaddrmsg *ia = NLMSG_DATA(nl);
	interface *iface;

	if((iface = iface_by_idx(ia->ifa_index)) == NULL){
		fprintf(stderr,"Invalid interface index: %d\n",ia->ifa_index);
		return -1;
	}
	printf("[%s] ADDRESS ADDED\n",iface->name);
	// FIXME
	return 0;
}

static int
handle_rtm_dellink(const struct nlmsghdr *nl){
	const struct ifinfomsg *ii = NLMSG_DATA(nl);
	interface *iface;

	if((iface = iface_by_idx(ii->ifi_index)) == NULL){
		fprintf(stderr,"Invalid interface index: %d\n",ii->ifi_index);
		return -1;
	}
	printf("Link %d (%s) was removed\n",ii->ifi_index,iface->name);
	// FIXME do we care?
	return 0;
}

static int
handle_wireless_event(interface *i,void *data,int len){
	fprintf(stderr,"WIRELESS EVENT on %s (%p/%d)!\n",i->name,data,len);
	return 0;
}

#define IFF_FLAG(flags,f) ((flags) & (IFF_##f) ? #f" " : "")
static int
handle_rtm_newlink(const struct nlmsghdr *nl){
	const struct ifinfomsg *ii = NLMSG_DATA(nl);
	const struct rtattr *ra;
	const arptype *at;
	interface *iface;
	int rlen;

	if((iface = iface_by_idx(ii->ifi_index)) == NULL){
		fprintf(stderr,"Invalid interface index: %d\n",ii->ifi_index);
		return -1;
	}
	rlen = nl->nlmsg_len - NLMSG_LENGTH(sizeof(*ii));
	ra = (struct rtattr *)((char *)(NLMSG_DATA(nl)) + sizeof(*ii));
	while(RTA_OK(ra,rlen)){
		switch(ra->rta_type){
			case IFLA_ADDRESS:{
				char *addr;

				if((addr = malloc(RTA_PAYLOAD(ra))) == NULL){
					fprintf(stderr,"Address too long: %lu\n",RTA_PAYLOAD(ra));
					return -1;
				}
				memcpy(addr,RTA_DATA(ra),RTA_PAYLOAD(ra));
				free(iface->addr);
				iface->addr = addr;
				iface->addrlen = RTA_PAYLOAD(ra);
				break;
			}case IFLA_BROADCAST:{
				break;
			}case IFLA_IFNAME:{
				char *name;

				if((name = strdup(RTA_DATA(ra))) == NULL){
					fprintf(stderr,"Name too long: %s\n",(char *)RTA_DATA(ra));
					return -1;
				}
				free(iface->name);
				iface->name = name;
				break;
			}case IFLA_MTU:{
				if(RTA_PAYLOAD(ra) != sizeof(int)){
					fprintf(stderr,"Expected %zu MTU bytes, got %lu\n",
							sizeof(int),RTA_PAYLOAD(ra));
				}
				iface->mtu = *(int *)RTA_DATA(ra);
				break;
			}case IFLA_LINK:{
				break;
			}case IFLA_TXQLEN:{
				break;
			}case IFLA_MAP:{
				break;
			}case IFLA_WEIGHT:{
				break;
			}case IFLA_QDISC:{
				break;
			}case IFLA_STATS:{
				break;
			}case IFLA_WIRELESS:{
				if(handle_wireless_event(iface,RTA_DATA(ra),RTA_PAYLOAD(ra)) < 0){
					return -1;
				}
				break;
			}case IFLA_OPERSTATE:{
				break;
			}case IFLA_LINKMODE:{
				break;
			}case IFLA_LINKINFO:{
				break;
			}case IFLA_NET_NS_PID:{
				break;
			}case IFLA_IFALIAS:{
				break;
			}case IFLA_NUM_VF:{
				break;
			}case IFLA_VFINFO_LIST:{
				break;
			}case IFLA_STATS64:{
				break;
			}case IFLA_VF_PORTS:{
				break;
			}case IFLA_PORT_SELF:{
				break;
			}case IFLA_AF_SPEC:{
				break;
			}default:{
				fprintf(stderr,"Unknown rtatype %u\n",ra->rta_type);
				break;
			}
		}
		ra = RTA_NEXT(ra,rlen);
	}
	if(rlen){
		fprintf(stderr,"%d excess bytes on newlink message\n",rlen);
	}
	iface->arptype = ii->ifi_type;
	if((at = lookup_arptype(iface->arptype)) == NULL){
		fprintf(stderr,"Unknown dev type %u\n",iface->arptype);
	}else{
		char *hwaddr;

		if((hwaddr = hwaddrstr(iface)) == NULL){
			return -1;
		}
		printf("[%3d][%8s][%s] %s %d %s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s\n",
			ii->ifi_index,
			iface->name,
			at->name,
			hwaddr,
			iface->mtu,
			IFF_FLAG(ii->ifi_flags,UP),
			IFF_FLAG(ii->ifi_flags,BROADCAST),
			IFF_FLAG(ii->ifi_flags,DEBUG),
			IFF_FLAG(ii->ifi_flags,LOOPBACK),
			IFF_FLAG(ii->ifi_flags,POINTOPOINT),
			IFF_FLAG(ii->ifi_flags,NOTRAILERS),
			IFF_FLAG(ii->ifi_flags,RUNNING),
			IFF_FLAG(ii->ifi_flags,PROMISC),
			IFF_FLAG(ii->ifi_flags,ALLMULTI),
			IFF_FLAG(ii->ifi_flags,MASTER),
			IFF_FLAG(ii->ifi_flags,SLAVE),
			IFF_FLAG(ii->ifi_flags,MULTICAST),
			IFF_FLAG(ii->ifi_flags,PORTSEL),
			IFF_FLAG(ii->ifi_flags,AUTOMEDIA),
			IFF_FLAG(ii->ifi_flags,DYNAMIC),
			IFF_FLAG(ii->ifi_flags,LOWER_UP),
			IFF_FLAG(ii->ifi_flags,DORMANT),
			IFF_FLAG(ii->ifi_flags,ECHO)
			);
		free(hwaddr);
	}
	return 0;
}
#undef IFF_FLAG

int handle_netlink_event(int fd){
	char buf[4096]; // FIXME numerous problems
	struct iovec iov[1] = { { buf, sizeof(buf) } };
	struct sockaddr_nl sa;
	struct msghdr msg = {
		&sa,	sizeof(sa),	iov,	sizeof(iov) / sizeof(*iov), NULL, 0, 0
	};
	struct nlmsghdr *nh;
	int r,inmulti,res;

	res = 0;
	// For handling multipart messages
	inmulti = 0;
	while((r = recvmsg(fd,&msg,MSG_DONTWAIT)) > 0){
		// NLMSG_LENGTH sanity checks enforced via NLMSG_OK() and
		// _NEXT() -- we needn't check amount read within the loop
		for(nh = (struct nlmsghdr *)buf ; NLMSG_OK(nh,(unsigned)r) ; nh = NLMSG_NEXT(nh,r)){
			//printf("MSG TYPE %d\n",(int)nh->nlmsg_type);
			if(nh->nlmsg_flags & NLM_F_MULTI){
				inmulti = 1;
			}
			switch(nh->nlmsg_type){
			case RTM_NEWLINK:{
				res |= handle_rtm_newlink(nh);
				break;
			}case RTM_DELLINK:{
				res |= handle_rtm_dellink(nh);
				break;
			}case RTM_NEWADDR:{
				res |= handle_rtm_newaddr(nh);
				break;
			}case RTM_DELADDR:{
				res |= handle_rtm_deladdr(nh);
				break;
			}case RTM_NEWNEIGH:{
				res |= handle_rtm_newneigh(nh);
				break;
			}case RTM_DELNEIGH:{
				res |= handle_rtm_delneigh(nh);
				break;
			}case NLMSG_DONE:{
				if(!inmulti){
					fprintf(stderr,"Warning: DONE outside multipart on %d\n",fd);
				}
				inmulti = 0;
				break;
			}case NLMSG_ERROR:{
				struct nlmsgerr *nerr = NLMSG_DATA(nh);

				if(nerr->error == 0){
					printf("ACK on netlink %d msgid %u type %u\n",
						fd,nerr->msg.nlmsg_seq,nerr->msg.nlmsg_type);
				}else{
					fprintf(stderr,"Error message on netlink %d msgid %u (%s?)\n",
						fd,nerr->msg.nlmsg_seq,strerror(-nerr->error));
					res = -1;
				}
				break;
			}default:{
				fprintf(stderr,"Unknown netlink msgtype %u on %d\n",nh->nlmsg_type,fd);
				res = -1;
			}}
			// FIXME handle read data
		}
	}
	if(inmulti){
		fprintf(stderr,"Warning: unterminated multipart on %d\n",fd);
		res = -1;
	}
	if(r < 0 && errno != EAGAIN){
		fprintf(stderr,"Error reading netlink socket %d (%s?)\n",
				fd,strerror(errno));
		res = -1;
	}else if(r == 0){
		fprintf(stderr,"EOF on netlink socket %d\n",fd);
		// FIXME reopen...?
		res = -1;
	}
	return res;
}
