#include <errno.h>
#include <iwlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/wireless.h>
#include <omphalos/wireless.h>
#include <omphalos/omphalos.h>
#include <omphalos/interface.h>

static inline int
get_wireless_extension(const omphalos_iface *octx,const char *name,int cmd,struct iwreq *req){
	int fd;

	if(strlen(name) >= sizeof(req->ifr_name)){
		octx->diagnostic("Name too long: %s",name);
		return -1;
	}
	if((fd = socket(AF_INET,SOCK_DGRAM,0)) < 0){
		octx->diagnostic("Couldn't get a socket (%s?)",strerror(errno));
		return -1;
	}
	strcpy(req->ifr_name,name);
	if(ioctl(fd,cmd,req)){
		//octx->diagnostic("ioctl() failed (%s?)",strerror(errno));
		close(fd);
		return -1;
	}
	if(close(fd)){
		octx->diagnostic("Couldn't close socket (%s?)",strerror(errno));
		return -1;
	}
	return 0;
}

static int
wireless_rate_info(const omphalos_iface *octx,const char *name,wless_info *wi){
	const struct iw_param *ip;
	struct iwreq req;

	if(get_wireless_extension(octx,name,SIOCGIWRATE,&req)){
		return -1;
	}
	ip = &req.u.bitrate;
	wi->bitrate = ip->value;
	return 0;
}

int handle_wireless_event(const omphalos_iface *octx,interface *i,
				const struct iw_event *iw,size_t len){
	if(len < IW_EV_LCP_LEN){
		octx->diagnostic("Wireless msg too short on %s (%zu)",i->name,len);
		return -1;
	}
	switch(iw->cmd){
	case SIOCGIWSCAN:{
		// FIXME handle scan results
	break;}case SIOCGIWAP:{
		// FIXME handle AP results
	break;}case SIOCGIWSPY:{
		// FIXME handle AP results
	break;}case SIOCSIWMODE:{
		// FIXME handle wireless mode change
	break;}case SIOCSIWFREQ:{
		// FIXME handle frequency/channel change
	break;}case IWEVASSOCRESPIE:{
		// FIXME handle IE reassociation results
	break;}case SIOCSIWESSID:{
		// FIXME handle ESSID change
	break;}case SIOCSIWRATE:{
		// FIXME doesn't this come as part of the netlink message? this
		// is an extra 3 system calls...
		wireless_rate_info(octx,i->name,&i->settings.wext);
	break;}case SIOCSIWTXPOW:{
		// FIXME handle TX power change
	break;}default:{
		octx->diagnostic("Unknown wireless event on %s: 0x%x",i->name,iw->cmd);
		return -1;
	} }
	if(octx->wireless_event){
		i->opaque = octx->wireless_event(i,iw->cmd,i->opaque);
	}
	return 0;
}

static inline uintmax_t
iwfreq_defreak(const struct iw_freq *iwf){
	uintmax_t ret = iwf->m;
	unsigned e = iwf->e;

	while(e--){
		ret *= 10;
	}
	return ret;
}

int iface_wireless_info(const omphalos_iface *octx,const char *name,wless_info *wi){
	struct iwreq req;

	memset(wi,0,sizeof(*wi));
	memset(&req,0,sizeof(req));
	if(get_wireless_extension(octx,name,SIOCGIWNAME,&req)){
		return -1;
	}
	if(wireless_rate_info(octx,name,wi)){
		octx->diagnostic("rate info failed on %s",name);
		return -1;
	}
	if(get_wireless_extension(octx,name,SIOCGIWMODE,&req)){
		return -1;
	}
	wi->mode = req.u.mode;
	if(get_wireless_extension(octx,name,SIOCGIWFREQ,&req)){
		return -1;
	}
	wi->freq = iwfreq_defreak(&req.u.freq);
	return 0;
}
