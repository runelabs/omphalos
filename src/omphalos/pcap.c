#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <pcap/pcap.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <omphalos/ip.h>
#include <omphalos/util.h>
#include <asm/byteorder.h>
#include <omphalos/irda.h>
#include <omphalos/pcap.h>
#include <omphalos/diag.h>
#include <linux/if_ether.h>
#include <omphalos/hwaddrs.h>
#include <omphalos/ethernet.h>
#include <omphalos/radiotap.h>
#include <omphalos/omphalos.h>
#include <omphalos/netaddrs.h>
#include <omphalos/interface.h>

static pcap_dumper_t *dumper;
static pthread_mutex_t dumplock = PTHREAD_MUTEX_INITIALIZER;

static interface pcap_file_interface;

typedef struct pcap_marshal {
	interface *i;
	const omphalos_iface *octx;
	analyzefxn handler;
} pcap_marshal;

static void
postprocess(pcap_marshal *pm,omphalos_packet *packet,interface *iface,
			const struct pcap_pkthdr *h,const void *bytes){
	struct pcap_pkthdr phdr;

	if(packet->l2s){
		l2srcpkt(packet->l2s);
	}
	if(packet->l2d){
		l2dstpkt(packet->l2d);
	}
	if(packet->l3s){
		l3_srcpkt(packet->l3s);
	}
	if(packet->l3d){
		l3_dstpkt(packet->l3d);
	}
	if(packet->noproto || packet->malformed){
		struct pcap_ll pll;
		hwaddrint hw;

		memset(&pll,0,sizeof(pll));
		memcpy(&phdr,h,sizeof(phdr));
		if(packet->noproto){
			++iface->noprotocol;
		}
		if(packet->malformed){
			++iface->malformed;
		}
		pll.pkttype = packet_sll_type(packet);
		pll.arphrd = htons(packet->i->arptype);
		pll.llen = htons(packet->i->addrlen);
		hw = packet->l2s ? get_hwaddr(packet->l2s) : 0;
		memcpy(&pll.haddr,&hw,packet->i->addrlen > sizeof(pll.haddr) ? sizeof(pll.haddr) : packet->i->addrlen);
		pll.ethproto = htons(packet->l3proto);
		log_pcap_packet(&phdr,(void *)bytes,packet->i->l2hlen,&pll);
	}
	if(pm->octx->packet_read){
		pm->octx->packet_read(packet);
	}
}

// FIXME need to call back even on truncations etc. move function pointer
// to pcap_marshal and unify call to redirect + packet_read().
static void
handle_pcap_direct(u_char *gi,const struct pcap_pkthdr *h,const u_char *bytes){
	pcap_marshal *pm = (pcap_marshal *)gi;
	interface *iface = pm->i; // interface for the pcap file
	struct pcap_pkthdr phdr;
	omphalos_packet packet;

	++iface->frames;
	//diagnostic("Frame %ju",iface->frames);
	if(h->caplen != h->len){
		++iface->truncated;
		diagnostic("Partial capture (%u/%ub)",h->caplen,h->len);
		return;
	}
	memset(&packet,0,sizeof(packet));
	packet.i = iface;
	pm->handler(&packet,bytes,h->len);
	gettimeofday(&phdr.ts,NULL);
	phdr.len = h->len;
	phdr.caplen = h->caplen;
	postprocess(pm,&packet,iface,&phdr,bytes);
}

static void
handle_pcap_cooked(u_char *gi,const struct pcap_pkthdr *h,const u_char *bytes){
	char bcast[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	pcap_marshal *pm = (pcap_marshal *)gi;
	interface *iface = pm->i; // interface for the pcap file
	const struct pcapsll { // taken from pcap-linktype(7), "LINKTYPE_LINUX_SLL"
		uint16_t pkttype;
		uint16_t arptype;
		uint16_t hwlen;
		char hwaddr[8];
		uint16_t proto;
	} *sll;
	char addr[8];
	omphalos_packet packet;

	++iface->frames;
	if(h->caplen != h->len || h->caplen < sizeof(*sll)){
		diagnostic("Partial capture (%u/%ub)",h->caplen,h->len);
		++iface->truncated;
		return;
	}
	sll = (const struct pcapsll *)bytes;
	if(h->len < sizeof(*sll) || ntohs(sll->hwlen) > sizeof(sll->hwaddr)){
		++iface->malformed;
		return;
	}
	memset(&packet,0,sizeof(packet));
	packet.i = iface;
	packet.i->addrlen = ntohs(sll->hwlen);
	assert(packet.i->addrlen <= sizeof(addr));
	memcpy(addr,sll->hwaddr,packet.i->addrlen);
	packet.i->addr = addr;
	packet.i->bcast = bcast;
	packet.l2s = lookup_l2host(iface,sll->hwaddr);
	packet.l2d = packet.l2s;
	packet.l3proto = ntohs(sll->proto);
	// proto is in network byte-order. rather than possibly switch it
	// every time, we provide the cases in network byte-order
	switch(sll->proto){
		case __constant_ntohs(ETH_P_IP):{
			handle_ipv4_packet(&packet,bytes + sizeof(*sll),h->len - sizeof(*sll));
			break;
		}case __constant_ntohs(ETH_P_IPV6):{
			handle_ipv6_packet(&packet,bytes + sizeof(*sll),h->len - sizeof(*sll));
			break;
		}default:{
			++iface->noprotocol;
			break;
		}
	}
	postprocess(pm,&packet,iface,h,bytes);
	iface->addr = iface->bcast = NULL;
}

int handle_pcap_file(const omphalos_ctx *pctx){
	pcap_handler fxn;
	char ebuf[PCAP_ERRBUF_SIZE];
	pcap_marshal pmarsh = {
		.octx = &pctx->iface,
		.i = &pcap_file_interface,
	};
	pcap_t *pcap;

	free(pmarsh.i->name);
	diagnostic("Processing pcap file %s",pctx->pcapfn);
	memset(pmarsh.i,0,sizeof(*pmarsh.i));
	pmarsh.i->fd4 = pmarsh.i->fd6 = pmarsh.i->fd = pmarsh.i->rfd = -1;
	pmarsh.i->flags = IFF_BROADCAST | IFF_UP | IFF_LOWER_UP;
	// FIXME set up remainder of interface as best we can...
	if((pmarsh.i->name = strdup(pctx->pcapfn)) == NULL){
		return -1;
	}
	if((pcap = pcap_open_offline(pctx->pcapfn,ebuf)) == NULL){
		diagnostic("Couldn't open pcap input %s (%s?)",pctx->pcapfn,ebuf);
		return -1;
	}
	fxn = NULL;
	switch(pcap_datalink(pcap)){
		case DLT_EN10MB:{
			fxn = handle_pcap_direct;
			pmarsh.handler = handle_ethernet_packet;
			pmarsh.i->addrlen = ETH_ALEN;
			pmarsh.i->addr = malloc(pmarsh.i->addrlen);
			pmarsh.i->bcast = malloc(pmarsh.i->addrlen);
			pmarsh.i->l2hlen = ETH_HLEN;
			memset(pmarsh.i->addr,0,pmarsh.i->addrlen);
			memset(pmarsh.i->bcast,0xff,pmarsh.i->addrlen);
			break;
		}case DLT_LINUX_SLL:{
			fxn = handle_pcap_cooked;
			break;
		}case DLT_IEEE802_11_RADIO:{
			pmarsh.handler = handle_radiotap_packet;
			fxn = handle_pcap_direct;
			pmarsh.i->addrlen = ETH_ALEN;
			pmarsh.i->addr = malloc(pmarsh.i->addrlen);
			pmarsh.i->bcast = malloc(pmarsh.i->addrlen);
			pmarsh.i->l2hlen = ETH_HLEN;
			memset(pmarsh.i->addr,0,pmarsh.i->addrlen);
			memset(pmarsh.i->bcast,0xff,pmarsh.i->addrlen);
			break;
		}case DLT_LINUX_IRDA:{
			pmarsh.handler = handle_irda_packet;
			fxn = handle_pcap_direct;
			pmarsh.i->addrlen = 4;
			pmarsh.i->addr = malloc(pmarsh.i->addrlen);
			pmarsh.i->bcast = malloc(pmarsh.i->addrlen);
			pmarsh.i->l2hlen = 15; // FIXME ???
			memset(pmarsh.i->addr,0,pmarsh.i->addrlen);
			memset(pmarsh.i->bcast,0xff,pmarsh.i->addrlen);
			break;
		}default:{
			diagnostic("Unhandled datalink type: %d",pcap_datalink(pcap));
			break;
		}
	}
	if(fxn == NULL){
		pcap_close(pcap);
		return -1;
	}
	if(pcap_loop(pcap,-1,fxn,(u_char *)&pmarsh)){
		diagnostic("Error processing pcap file %s (%s?)",pctx->pcapfn,pcap_geterr(pcap));
		pcap_close(pcap);
		return -1;
	}
	pcap_close(pcap);
	return 0;
}

int print_pcap_stats(FILE *fp,interface *agg){
	const interface *iface;

	iface = &pcap_file_interface;
	if(iface->name){
		if(print_iface_stats(fp,iface,agg,"file") < 0){
			return -1;
		}
	}
	return 0;
}

int init_pcap(const omphalos_ctx *pctx){
	if(pctx->plog){
		dumper = pctx->plog;
	}
	return 0;
}

void cleanup_pcap(const omphalos_ctx *pctx){
	free_iface(&pcap_file_interface);
	pthread_mutex_lock(&dumplock);
	if(dumper){
		pcap_dump_flush(dumper);
		dumper = NULL;
	}
	if(pctx->plogp){
		pcap_close(pctx->plogp);
	}
	pthread_mutex_unlock(&dumplock);
}

// Must convert the layer 2 header into a DLT_LINUX_SLL (sockaddr_ll) portable
// pseudoheader. We copy the real header over to a temporary buffer, write our
// new header, log from the new origin, then copy the real header back in.
//
// This requires an original header of at least 16 bytes. This is longer than
// Ethernet and just about everything else, but thankfully we have tpacket_hdr
// in the PACKET_RX_MMAP case. Otherwise, this won't work and we need fallback
// to a payload copy.
//
// It is possible that we are using DLT_LINUX_SLL as a source. In that case,
// pass 0 as l2len, and no transformation will take place.
int log_pcap_packet(struct pcap_pkthdr *h,void *sp,size_t l2len,const struct pcap_ll *pll){
	struct pcap_ll *sll;
	void *newframe;
	void *rhdr;

	if(!dumper){
		return 0;
	}
	if(l2len >= sizeof(*sll)){
		if((rhdr = Malloc(l2len)) == NULL){
			return -1;
		}
		memcpy(rhdr,sp,l2len); // preserve the true header
		newframe = (char *)sp + (l2len - sizeof(*sll));
		memcpy(newframe,pll,sizeof(*sll));
		h->caplen -= (l2len - sizeof(*sll));
		h->len -= (l2len - sizeof(*sll));
	}else if(l2len){ // fall back to payload copy
		uint32_t plen;

		assert(h->caplen >= l2len);
		plen = h->caplen - l2len;
		if((rhdr = Malloc(sizeof(*sll) + plen)) == NULL){
			return -1;
		}
		newframe = rhdr;
		memcpy(newframe,pll,sizeof(*sll));
		memcpy((char *)newframe + sizeof(*sll),(char *)sp + l2len,plen);
		h->caplen = sizeof(*sll) + plen;
		h->len = sizeof(*sll) + plen;
		sp = NULL;
	}else{
		newframe = sp;
		rhdr = NULL;
		sp = NULL;
	}
	if(pthread_mutex_lock(&dumplock)){
		if(sp){
			memcpy(sp,rhdr,l2len);
		}
		free(rhdr);
		return -1;
	}
	pcap_dump((u_char *)dumper,h,newframe);
	if(pthread_mutex_unlock(&dumplock)){
		if(sp){
			memcpy(sp,rhdr,l2len);
		}
		free(rhdr);
		return -1;
	}
	if(sp){
		memcpy(sp,rhdr,l2len);
	}
	free(rhdr);
	return 0;
}
