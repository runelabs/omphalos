#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <arpa/inet.h>
#include <linux/if_arp.h>
#include <omphalos/iana.h>
#include <linux/rtnetlink.h>
#include <omphalos/hwaddrs.h>
#include <omphalos/ethernet.h>
#include <omphalos/omphalos.h>
#include <omphalos/interface.h>

// No need to store addrlen, since all objects in a given arena have the
// same length of hardware address.
typedef struct l2host {
	uint64_t hwaddr;	// does anything have more than 64 bits at L2?
	char *name;		// some textual description FIXME eliminate
	const char *devname;	// description based off lladdress
	struct l2host *next;
	void *opaque;		// FIXME not sure about how this is being done
} l2host;

// FIXME replace internals with LRU acquisition...
static inline l2host *
create_l2host(const void *hwaddr,size_t addrlen){
	l2host *l2;

	if( (l2 = malloc(sizeof(*l2))) ){
		l2->hwaddr = 0;
		memcpy(&l2->hwaddr,hwaddr,addrlen);
		l2->name = NULL;
		l2->opaque = NULL;
	}
	return l2;
}

// FIXME strictly proof-of-concept. we'll want a trie- or hash-based
// lookup, backed by an arena-allocated LRU, etc...
l2host *lookup_l2host(const omphalos_iface *octx,interface *i,
			const void *hwaddr,int family,const void *name){
	l2host *l2,**prev;
	uint64_t hwcmp;

	hwcmp = 0;
	memcpy(&hwcmp,hwaddr,i->addrlen);
	for(prev = &i->l2hosts ; (l2 = *prev) ; prev = &l2->next){
		if(l2->hwaddr == hwcmp){
			// Move it to the front of the list, splicing it out
			*prev = l2->next;
			l2->next = i->l2hosts;
			i->l2hosts = l2;
			return l2;
		}
	}
	if( (l2 = create_l2host(hwaddr,i->addrlen)) ){
		if((i->flags & IFF_BROADCAST) && i->bcast &&
				memcmp(hwaddr,i->bcast,i->addrlen) == 0){
			l2->devname = "Link broadcast";
		}else if(i->arptype == ARPHRD_ETHER || i->arptype == ARPHRD_IEEE80211_RADIOTAP
				|| i->arptype == ARPHRD_IEEE80211 || i->arptype == ARPHRD_IEEE80211_PRISM){
			l2->devname = iana_lookup(hwaddr);
		}else{
			l2->devname = NULL;
		}
		l2->next = i->l2hosts;
		i->l2hosts = l2;
		if(name){
			name_l2host_local(octx,i,l2,family,name);
		}
		if(octx->neigh_event){
			l2->opaque = octx->neigh_event(i,l2);
		}
	}
	return l2;
}

void cleanup_l2hosts(l2host **list){
	l2host *l2,*tmp;

	for(l2 = *list ; l2 ; l2 = tmp){
		free(l2->name);
		tmp = l2->next;
		free(l2);
	}
	*list = NULL;
}

void l2ntop(const l2host *l2,size_t len,void *buf){
	unsigned idx;
	size_t s;

	s = HWADDRSTRLEN(len);
	for(idx = 0 ; idx < len ; ++idx){
		snprintf((char *)buf + idx * 3,s - idx * 3,"%02x:",
				((unsigned char *)&l2->hwaddr)[idx]);
	}
}

char *l2addrstr(const l2host *l2,size_t len){
	char *r;

	if( (r = malloc(HWADDRSTRLEN(len))) ){
		l2ntop(l2,len,r);
	}
	return r;
}

void *l2host_get_opaque(l2host *l2){
	return l2->opaque;
}

int l2hostcmp(const l2host *l21,const l2host *l22){
	return memcmp(&l21->hwaddr,&l22->hwaddr,IFHWADDRLEN); // FIXME len-param
}

int l2categorize(const interface *i,const l2host *l2){
	int ret;

	ret = categorize_ethaddr(&l2->hwaddr);
	if(ret == RTN_UNICAST){
		return memcmp(i->addr,&l2->hwaddr,i->addrlen) ? RTN_UNICAST : RTN_LOCAL;
	}else if(ret == RTN_MULTICAST){
		if((i->flags & IFF_BROADCAST) && i->bcast){
			return memcmp(i->bcast,&l2->hwaddr,i->addrlen) ? RTN_MULTICAST : RTN_BROADCAST;
		}
		return RTN_MULTICAST;
	}
	return ret;
}

static inline void
name_l2host_absolute(const omphalos_iface *octx,const interface *i,l2host *l2,
					const char *name){
	if( (l2->name = malloc(strlen(name) + 1)) ){
		strcpy(l2->name,name);
	}
	if(octx->neigh_event){
		l2->opaque = octx->neigh_event(i,l2);
	}
}

void name_l2host_local(const omphalos_iface *octx,const interface *i,l2host *l2,
					int family,const void *name){
	if(l2->name == NULL){
		char b[INET6_ADDRSTRLEN];

		assert(inet_ntop(family,name,b,sizeof(b)) == b);
		name_l2host_absolute(octx,i,l2,b);
	}
}

// This is for raw network addresses as seen on the wire, which may be from
// outside the local network. We want only the local network address(es) of the
// link address (in a rare case, it might not have any). For unicast link
// addresses, a route lookup will be performed using the wire network address.
// If the route returned is different from the wire address, an ARP probe is
// directed to the link-layer address (this is all handled by get_route()). ARP
// replies are link-layer only, and thus processed directly (name_l2host_local()).
void name_l2host(const omphalos_iface *octx,interface *i,l2host *l2,
				int family,const void *name){
	if(l2->name == NULL){
		struct sockaddr_storage ss;
		int cat;

		if((cat = categorize_ethaddr(&l2->hwaddr)) == RTN_UNICAST){
			// FIXME throwing out anything to which we have no
			// route means we basically don't work pre-config.
			// addresses pre-configuration have information, but
			// are inferior to those post-configuration. we need a
			// means of *updating* names whenever routes change,
			// or as close to true route cache behavior as we like
			if((name = get_route(octx,i,&l2->hwaddr,family,name,&ss)) == NULL){
				return;
			}
		}else if(cat == RTN_MULTICAST){
			// Look up family-appropriate multicast names
		}
		name_l2host_local(octx,i,l2,family,name);
	}
}

const char *get_name(const l2host *l2){
	return l2->name;
}

const char *get_devname(const l2host *l2){
	return l2->devname;
}
