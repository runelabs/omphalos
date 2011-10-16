#include <assert.h>
#include <netinet/ip.h>
#include <linux/tcp.h>
#include <linux/igmp.h>
#include <netinet/ip6.h>
#include <omphalos/ip.h>
#include <omphalos/udp.h>
#include <omphalos/tcp.h>
#include <omphalos/gre.h>
#include <omphalos/pim.h>
#include <omphalos/icmp.h>
#include <omphalos/util.h>
#include <omphalos/hwaddrs.h>
#include <omphalos/netaddrs.h>
#include <omphalos/ethernet.h>
#include <omphalos/omphalos.h>
#include <omphalos/interface.h>

#define DEFAULT_IP4_TTL 64
#define DEFAULT_IP6_TTL 64

static void
handle_igmp_packet(const omphalos_iface *octx,omphalos_packet *op,const void *frame,size_t len){
	const struct igmphdr *igmp = frame;

	if(len < sizeof(*igmp)){
		octx->diagnostic("%s malformed with %zu",__func__,len);
		op->malformed = 1;
		return;
	}
	// FIXME
}

void handle_ipv6_packet(const omphalos_iface *octx,omphalos_packet *op,
				const void *frame,size_t len){
	const struct ip6_hdr *ip = frame;
	uint16_t plen;
	unsigned ver;
	uint8_t next;

	if(len < sizeof(*ip)){
		op->malformed = 1;
		octx->diagnostic("%s malformed with %zu on %s",__func__,len,op->i->name);
		return;
	}
	ver = ntohl(ip->ip6_ctlun.ip6_un1.ip6_un1_flow) >> 28u;
	if(ver != 6){
		op->noproto = 1;
		octx->diagnostic("%s noversion for %u on %s",__func__,ver,op->i->name);
		return;
	}
	plen = ntohs(ip->ip6_ctlun.ip6_un1.ip6_un1_plen);
	if(len < plen + sizeof(*ip)){
		op->malformed = 1;
		octx->diagnostic("%s malformed with %zu != %u on %s",__func__,len,plen,op->i->name);
		return;
	}
	// FIXME check extension headers...
	op->l3s = lookup_l3host(octx,op->i,op->l2s,AF_INET6,&ip->ip6_src);
	op->l3d = lookup_l3host(octx,op->i,op->l2d,AF_INET6,&ip->ip6_dst);
	const void *nhdr = (const unsigned char *)frame + (len - plen);
	next = ip->ip6_ctlun.ip6_un1.ip6_un1_nxt;

	// FIXME don't call down if we're fragmented
	while(nhdr){
		switch(next){
		case IPPROTO_TCP:{
			handle_tcp_packet(octx,op,nhdr,plen);
			nhdr = NULL;
		break; }case IPPROTO_UDP:{
			handle_udp_packet(octx,op,nhdr,plen);
			nhdr = NULL;
		break; }case IPPROTO_ICMP:{
			handle_icmp_packet(octx,op,nhdr,plen);
			nhdr = NULL;
		break; }case IPPROTO_ICMP6:{
			handle_icmp6_packet(octx,op,nhdr,plen);
			nhdr = NULL;
		break; }case IPPROTO_GRE:{
			handle_gre_packet(octx,op,nhdr,plen);
			nhdr = NULL;
		break; }case IPPROTO_IGMP:{
			handle_igmp_packet(octx,op,nhdr,plen);
			nhdr = NULL;
		break; }case IPPROTO_PIM:{
			handle_pim_packet(octx,op,nhdr,plen);
			nhdr = NULL;
		break; }case IPPROTO_HOPOPTS:{
			const struct ip6hbh {
				uint8_t nexthdr;
				uint8_t hdrlen;
				// FIXME header data follows
			} *hbh = nhdr;
			if(plen < sizeof(*hbh) || plen < hbh->hdrlen){
				op->malformed = 1;
				octx->diagnostic("%s malformed with len %zu on %s",__func__,plen,op->i->name);
				return;
			}
			plen -= hbh->hdrlen;
			nhdr = (const unsigned char *)nhdr + hbh->hdrlen;
			next = hbh->nexthdr;
		break; }default:{
			op->noproto = 1;
			octx->diagnostic("%s %s noproto for %u",__func__,
					op->i->name,next);
			return;
		break; } }
	}
}

void handle_ipv4_packet(const omphalos_iface *octx,omphalos_packet *op,
				const void *frame,size_t len){
	const struct iphdr *ip = frame;
	unsigned hlen;

	if(len < sizeof(*ip)){
		op->malformed = 1;
		octx->diagnostic("%s %s malformed with %zu",__func__,
				op->i->name,len);
		return;
	}
	if(ip->version != 4){
		op->noproto = 1;
		octx->diagnostic("%s %s noversion for %u",__func__,
				op->i->name,ip->version);
		return;
	}
	hlen = ip->ihl << 2u;
	if(len < hlen){
		op->malformed = 1;
		octx->diagnostic("%s %s malformed with %zu vs %u",__func__,
				op->i->name,len,hlen);
		return;
	}
	if(check_ethernet_padup(len,ntohs(ip->tot_len))){
		op->malformed = 1;
		octx->diagnostic("%s %s malformed with %zu vs %hu",__func__,
				op->i->name,len,ntohs(ip->tot_len));
		return;
	}
	op->l3s = lookup_l3host(octx,op->i,op->l2s,AF_INET,&ip->saddr);
	op->l3d = lookup_l3host(octx,op->i,op->l2d,AF_INET,&ip->daddr);

	const void *nhdr = (const unsigned char *)frame + hlen;
	const size_t nlen = ntohs(ip->tot_len) - hlen;

	// FIXME don't call down if we're fragmented
	switch(ip->protocol){
	case IPPROTO_TCP:{
		handle_tcp_packet(octx,op,nhdr,nlen);
	break; }case IPPROTO_UDP:{
		handle_udp_packet(octx,op,nhdr,nlen);
	break; }case IPPROTO_ICMP:{
		handle_icmp_packet(octx,op,nhdr,nlen);
	break; }case IPPROTO_GRE:{
		handle_gre_packet(octx,op,nhdr,nlen);
	break; }case IPPROTO_IGMP:{
		handle_igmp_packet(octx,op,nhdr,nlen);
	break; }case IPPROTO_PIM:{
		handle_pim_packet(octx,op,nhdr,nlen);
	break; }default:{
		op->noproto = 1;
		octx->diagnostic("%s %s noproto for %u",__func__,
				op->i->name,ip->protocol);
	break; } }
}

// Doesn't set ->tot_len; that must be done by the caller. Prepare ->check for
// checksum evaluation, but we cannot yet actually evaluate it (FIXME though we
// could calculate differential).
int prep_ipv4_header(void *frame,size_t flen,uint32_t src,uint32_t dst,unsigned proto){
	struct iphdr *ip;

	if(src == 0){
		return -1; // FIXME we need allow this once route races are solved
	}
	if(flen < sizeof(*ip)){
		return -1;
	}
	ip = frame;
	memset(ip,0,sizeof(*ip));
	ip->version = 4;
	ip->ihl = sizeof(*ip) >> 2u;
	ip->ttl = DEFAULT_IP4_TTL;
	ip->id = random();
	ip->saddr = src;
	ip->daddr = dst;
	ip->protocol = proto;
	return ip->ihl << 2u;
}

// Doesn't set ->tot_len; that must be done by the caller. Prepare ->check for
// checksum evaluation, but we cannot yet actually evaluate it (FIXME though we
// could calculate differential).
int prep_ipv6_header(void *frame,size_t flen,uint128_t src,uint128_t dst,unsigned proto){
	struct ip6_hdr *ip;

	if(flen < sizeof(*ip)){
		return -1;
	}
	ip = frame;
	memset(ip,0,sizeof(*ip));
	ip->ip6_ctlun.ip6_un1.ip6_un1_flow = (6u << 28u);
	ip->ip6_ctlun.ip6_un1.ip6_un1_hlim = DEFAULT_IP6_TTL;
	ip->ip6_ctlun.ip6_un1.ip6_un1_nxt = proto;
	memcpy(ip->ip6_src.s6_addr32,&src,sizeof(src));
	memcpy(ip->ip6_dst.s6_addr32,&dst,sizeof(dst));
	return sizeof(*ip);
}
