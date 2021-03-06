#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <net/if_arp.h>
#include <omphalos/pci.h>
#include <omphalos/pcap.h>
#include <omphalos/diag.h>
#include <omphalos/privs.h>
#include <linux/if_packet.h>
#include <omphalos/netlink.h>
#include <omphalos/psocket.h>
#include <omphalos/netaddrs.h>
#include <omphalos/omphalos.h>
#include <omphalos/ethernet.h>
#include <omphalos/interface.h>

#ifndef PACKET_TX_RING
#define PACKET_TX_RING 13
#endif

// See packet(7) and Documentation/networking/packet_mmap.txt
int packet_socket(unsigned protocol){
	int fd;

	if((fd = socket(AF_PACKET,SOCK_RAW,ntohs(protocol))) < 0){
		diagnostic("Couldn't open packet socket (%s?)",strerror(errno));
		return -1;
	}
	return fd;
}

static int
get_block_size(unsigned fsize,unsigned *bsize){
	int b;

	// Ought be a power of two for performance. Must be a multiple of
	// page size. Ought be a multiple of tp_frame_size for efficiency.
	// Ought otherwise be as small as possible, or else the allocation
	// blocks/fails due to need for large VMA's.
	b = getpagesize();
	if(b < 0){
		diagnostic("Couldn't get page size (%s?)",strerror(errno));
		return -1;
	}
	*bsize = b;
	while(*bsize < fsize){
		if((*bsize << 1u) < *bsize){
			diagnostic("No valid configurations found");
			return -1;
		}
		*bsize <<= 1u;
	}
	return 0;
}

// Returns 0 on failure, otherwise size of the ringbuffer. On a failure,
// contents of treq are unspecified. blknum == 1024 for 4MiB rings.
static size_t
size_mmap_psocket(struct tpacket_req *treq,unsigned maxframe,unsigned blknum){
	unsigned fperblk;

	// Must be a multiple of TPACKET_ALIGNMENT, and the following must
	// hold: TPACKET_HDRLEN <= tp_frame_size <= tp_block_size.
	treq->tp_frame_size = TPACKET_ALIGN(TPACKET_HDRLEN + maxframe);
	if(get_block_size(treq->tp_frame_size,&treq->tp_block_size) < 0){
		return 0;
	}
	fperblk = treq->tp_block_size / treq->tp_frame_size;
	// Use the entire block, if there would otherwise be wasted space. This
	// is useful to catch radiotap, small GRO etc without PACKET_COPY_THRESH.
	treq->tp_frame_size = treq->tp_block_size / fperblk;
	// Array of pointers to blocks, allocated via slab -- cannot be
	// larger than largest slabbable allocation. FIXME do better
	treq->tp_block_nr = blknum / (treq->tp_block_size / getpagesize());
	// tp_frame_nr is derived from the other three parameters.
	treq->tp_frame_nr = (treq->tp_block_size / treq->tp_frame_size)
		* treq->tp_block_nr;
	return treq->tp_block_nr * treq->tp_block_size;
}

static size_t
mmap_psocket(int op,int idx,int fd,unsigned maxframe,void **map,
			struct tpacket_req *treq,unsigned blknum){
	size_t size;

	*map = MAP_FAILED;
	if((size = size_mmap_psocket(treq,maxframe,blknum)) == 0){
		return 0;
	}
	if(idx >= 0){
		struct sockaddr_ll sll;

		memset(&sll,0,sizeof(sll));
		sll.sll_family = AF_PACKET;
		sll.sll_ifindex = idx;
		if(bind(fd,(struct sockaddr *)&sll,sizeof(sll)) < 0){
			diagnostic("Couldn't bind idx %d (%s?)",idx,strerror(errno));
			return 0;
		}
	}else if(op != PACKET_RX_RING){
		diagnostic("Invalid idx with op %d: %d",op,idx);
		return -1;
	}
	if(op){
		if(setsockopt(fd,SOL_PACKET,op,treq,sizeof(*treq)) < 0){
			diagnostic("Couldn't set socket option (%s?)",strerror(errno));
			return 0;
		}
	}
	if((*map = mmap(0,size,PROT_READ|PROT_WRITE,
				MAP_SHARED | (op ? 0 : MAP_ANONYMOUS),
				op ? fd : -1,0)) == MAP_FAILED){
		diagnostic("Couldn't mmap %zub (%s?)",size,strerror(errno));
		return 0;
	}
	// FIXME MADV_HUGEPAGE support was dropped in 2.6.38.4, it seems.
#ifdef MADV_HUGEPAGE
	if(madvise(*map,size,MADV_HUGEPAGE)){
		//diagnostic("Couldn't advise hugepages for %zu (%s?)",size,strerror(errno));
	}
#endif
	return size;
}

size_t mmap_tx_psocket(int fd,int idx,unsigned maxframe,void **map,
					struct tpacket_req *treq){
	return mmap_psocket(0/*PACKET_TX_RING*/,idx,fd,maxframe,map,treq,256);
}

int unmap_psocket(void *map,size_t size){
	if(munmap(map,size)){
		diagnostic("Couldn't unmap %zub ring buffer (%s?)",size,strerror(errno));
		return -1;
	}
	return 0;
}

static int
recover_truncated_packet(interface *iface,int fd,unsigned tlen){
	int r;

	if(iface->truncbuflen < tlen){
		void **tmp;

		if((tmp = realloc(iface->truncbuf,tlen)) == NULL){
			return -1;
		}
		iface->truncbuf = tmp;
		iface->truncbuflen = tlen;
	}
	// Passing MSG_TRUNC ensures that we get the true length of the packet
	// from the wire (see packet(7)).
	if((r = recvfrom(fd,iface->truncbuf,iface->truncbuflen,MSG_DONTWAIT|MSG_TRUNC,NULL,0)) <= 0){
		diagnostic("Error in recvfrom(%s): %s",iface->name,strerror(errno));
		return r;
	}
	if((unsigned)r > iface->truncbuflen){
		diagnostic("Couldn't recover truncated packet (%d > %zu)",r,iface->truncbuflen);
		return -1;
	}
	return r;
}

// -1: error; don't call us anymore. 0: handled frame. 1: interrupted; we
// return for a cancellation check, and the frameptr oughtn't be advanced. The
// interface lock must be held upon entry.
int handle_ring_packet(interface *iface,int fd,void *frame){
	const struct omphalos_ctx *ctx = get_octx();
	const omphalos_iface *octx = &ctx->iface;
	struct tpacket_hdr *thdr = frame;
	omphalos_packet packet;
	int len;

	memset(&packet,0,sizeof(packet));
	packet.i = iface;
	while(thdr->tp_status == 0){
		struct pollfd pfd[1];
		int events,msec;

		pfd[0].fd = fd;
		pfd[0].revents = 0;
		pfd[0].events = POLLIN | POLLRDNORM | POLLERR;
		msec = IFACE_TIMESTAT_USECS / 1000;
		pthread_mutex_unlock(&iface->lock);
		events = poll(pfd,sizeof(pfd) / sizeof(*pfd),msec);
		pthread_mutex_lock(&iface->lock);
		if(events == 0){
			gettimeofday(&packet.tv,NULL);
			timestat_inc(&iface->fps,&packet.tv,0);
			timestat_inc(&iface->bps,&packet.tv,0);
			if(octx->packet_read){
				octx->packet_read(&packet);
			}
			return 1;
		}else if(events < 0){
			if(errno != EINTR){
				diagnostic("Error in poll() on %s (%s?)",
						iface->name,strerror(errno));
				return -1;
			}
			return 1;
		}else if(pfd[0].revents & POLLERR){
			// FIXME don't want to print this every time a device
			// is removed from underneath us, but also don't want
			// to race against notification...check to see if
			// device is down here? FIXME
			//diagnostic("Error polling psocket %d on %s",fd,i->name);
			return -1;
		}
	}
	++iface->frames;
	packet.tv.tv_sec = thdr->tp_sec;
	packet.tv.tv_usec = thdr->tp_usec;
	timestat_inc(&iface->fps,&packet.tv,1);
	if(thdr->tp_status & TP_STATUS_LOSING){
		struct tpacket_stats tstats;
		socklen_t slen;

		// FIXME only call once for each burst of TP_STATUS_LOSING
		slen = sizeof(tstats);
		if(getsockopt(fd,SOL_PACKET,PACKET_STATISTICS,&tstats,&slen)){
			diagnostic("Error reading stats on %s (%s?)",iface->name,strerror(errno));
		}else if(tstats.tp_drops){
			iface->drops += tstats.tp_drops;
			diagnostic("[%s] %u/%ju drops",iface->name,tstats.tp_drops,iface->drops);
		}
	}
	if((thdr->tp_status & TP_STATUS_COPY) || thdr->tp_snaplen != thdr->tp_len){
		++iface->truncated;
		if((len = recover_truncated_packet(iface,fd,thdr->tp_len)) <= 0){
			diagnostic("Partial capture on %s (%u/%ub)",
				iface->name,thdr->tp_snaplen,thdr->tp_len);
			frame = (char *)frame + thdr->tp_mac;
			len = thdr->tp_snaplen;
		}else{
			frame = iface->truncbuf;
			++iface->truncated_recovered;
			len = thdr->tp_len;
		}
	}else{
		frame = (char *)frame + thdr->tp_mac;
		len = thdr->tp_len;
	}
	timestat_inc(&iface->bps,&packet.tv,len);
	iface->bytes += len;
	iface->analyzer(&packet,frame,len);
	thdr->tp_status = TP_STATUS_KERNEL; // return the frame
	if(packet.l2s){
		l2srcpkt(packet.l2s);
	}
	if(packet.l2d){
		l2dstpkt(packet.l2d);
	}
	if(packet.l3s){
		l3_srcpkt(packet.l3s);
	}
	if(packet.l3d){
		l3_dstpkt(packet.l3d);
	}
	if(packet.malformed || packet.noproto){
		if(packet.malformed){
			++iface->malformed;
		}
		if(packet.noproto){
			++iface->noprotocol;
		}
		if(packet.pcap_ethproto){
			struct pcap_pkthdr pcap;
			size_t scribble = 0;
			struct pcap_ll pll;

			if(frame != iface->truncbuf){
				scribble = thdr->tp_mac;
				frame -= thdr->tp_mac;
			}else{
				scribble = 0;
			}
			pcap.caplen = pcap.len = len + scribble;
			pcap.ts = packet.tv;
			memset(&pll,0,sizeof(pll));
			pll.arphrd = htons(packet.i->arptype);
			pll.llen = htons(packet.i->addrlen);
			if(packet.l2s){
				hwaddrint hw = get_hwaddr(packet.l2s);
				memcpy(&pll.haddr,&hw,packet.i->addrlen > sizeof(pll.haddr) ?
						sizeof(pll.haddr) : packet.i->addrlen);
				// FIXME handle other pkttypes
				if(memcmp(&hw,packet.i->addr,packet.i->addrlen) == 0){
					pll.pkttype = htons(4);
				}
			}
			pll.ethproto = htons(packet.pcap_ethproto);
			// 'frame' starts at the L2 header, *not* the tpacket_thdr
			log_pcap_packet(&pcap,frame,packet.i->l2hlen + scribble,&pll);
			if(scribble){
				frame += thdr->tp_mac;
			}
		}
	}
	if(octx->packet_read){
		octx->packet_read(&packet);
	}
	return 0;
}

static int
packet_multicast(int fd,int ifindex){
	struct packet_mreq pm;

	memset(&pm,0,sizeof(pm));
	pm.mr_ifindex = ifindex;
	pm.mr_type = PACKET_MR_ALLMULTI;
	if(setsockopt(fd,SOL_PACKET,PACKET_ADD_MEMBERSHIP,&pm,sizeof(pm))){
		diagnostic("Couldn't PACKET_ADD_MEMBERSHIP (%s?)",strerror(errno));
		return -1;
	}
	return 0;
}

size_t mmap_rx_psocket(int fd,int idx,unsigned maxframe,void **map,
					struct tpacket_req *treq){
	size_t ret;
	int thresh;

	ret = mmap_psocket(PACKET_RX_RING,idx,fd,maxframe,map,treq,8192 * 4);
	if(ret == 0){
		return 0;
	}
	thresh = 1;
	if(setsockopt(fd,SOL_PACKET,PACKET_COPY_THRESH,&thresh,sizeof(thresh))){
		unmap_psocket(*map,ret);
		return -1;
	}
	if(packet_multicast(fd,idx)){
		unmap_psocket(*map,ret);
		return -1;
	}
	return ret;
}
