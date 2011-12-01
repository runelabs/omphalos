#include <stdlib.h>
#include <assert.h>
#include <limits.h>
#include <sys/socket.h>
#include <linux/version.h>
#include <linux/nl80211.h>
#include <linux/rtnetlink.h>
#include <ui/ncurses/core.h>
#include <ui/ncurses/util.h>
#include <ui/ncurses/color.h>
#include <omphalos/service.h>
#include <omphalos/hwaddrs.h>
#include <ncursesw/ncurses.h>
#include <ui/ncurses/iface.h>
#include <omphalos/netaddrs.h>
#include <omphalos/wireless.h>
#include <omphalos/interface.h>

typedef struct l4obj {
	struct l4obj *next;
	struct l4srv *l4;
} l4obj;

typedef struct l3obj {
	struct l3obj *next;
	struct l3host *l3;
	struct l4obj *l4objs;
	struct l2obj *l2;		// FIXME coverup of real failure
} l3obj;

typedef struct l2obj {
	struct l2obj *next,*prev;
	struct l2host *l2;
	unsigned lines;			// number of lines node would take up
	int cat;			// cached result of l2categorize()
	struct l3obj *l3objs;
} l2obj;

l2obj *l2obj_next(l2obj *l2){
	return l2->next;
}

l2obj *l2obj_prev(l2obj *l2){
	return l2->prev;
}

int l2obj_lines(const l2obj *l2){
	return l2->lines;
}

enum {
	EXPANSION_NONE,
	EXPANSION_NODES,
	EXPANSION_HOSTS,
	EXPANSION_SERVICES
	// Update EXPANSION_MAX if you add one at the end
};

#define EXPANSION_MAX EXPANSION_SERVICES

iface_state *create_interface_state(interface *i){
	iface_state *ret;
	const char *tstr;

	if((tstr = lookup_arptype(i->arptype,NULL)) == NULL){
		return NULL;
	}
	if( (ret = malloc(sizeof(*ret))) ){
		ret->srvs = ret->hosts = ret->nodes = 0;
		ret->l2objs = NULL;
		ret->devaction = 0;
		ret->typestr = tstr;
		ret->lastprinted.tv_sec = ret->lastprinted.tv_usec = 0;
		ret->iface = i;
		ret->expansion = EXPANSION_MAX;
	}
	return ret;
}

static l2obj *
get_l2obj(const interface *i,const iface_state *is,struct l2host *l2){
	l2obj *l;

	if( (l = malloc(sizeof(*l))) ){
		l->lines = is->expansion > EXPANSION_NONE ? 1 : 0;
		l->cat = l2categorize(i,l2);
		l->l3objs = NULL;
		l->l2 = l2;
	}
	return l;
}

static inline void
free_l4obj(l4obj *l4){
	free(l4);
}

static inline void
free_l3obj(l3obj *l){
	l4obj *l4 = l->l4objs;

	while(l4){
		l4obj *tmp = l4->next;
		free_l4obj(l4);
		l4 = tmp;
	}
	free(l);
}

static inline void
free_l2obj(l2obj *l2){
	l3obj *l3 = l2->l3objs;

	while(l3){
		l3obj *tmp = l3->next;
		free_l3obj(l3);
		l3 = tmp;
	}
	free(l2);
}

static l4obj *
get_l4obj(struct l4srv *srv){
	l4obj *l;

	if( (l = malloc(sizeof(*l))) ){
		l->l4 = srv;
	}
	return l;
}

static l3obj *
get_l3obj(struct l3host *l3){
	l3obj *l;

	if( (l = malloc(sizeof(*l))) ){
		l->l3 = l3;
		l->l4objs = NULL;
	}
	return l;
}

static unsigned
node_lines(int e,const l2obj *l){
	const l3obj *l3;
	unsigned lines;

	if(e == EXPANSION_NONE){
		return 0;
	}
	lines = 1;
	if(e > EXPANSION_NODES){
		for(l3 = l->l3objs ; l3 ; l3 = l3->next){
			++lines;
			if(e > EXPANSION_HOSTS){
				lines += !!l3->l4objs;
			}
		}
	}
	return lines;
}

// returns < 0 if c0 < c1, 0 if c0 == c1, > 0 if c0 > c1
static inline int
l2catcmp(int c0,int c1){
	// not a surjection! some values are shared.
	static const int vals[__RTN_MAX] = {
		0,			// RTN_UNSPEC
		__RTN_MAX - 1,		// RTN_UNICAST
		__RTN_MAX,		// RTN_LOCAL
		__RTN_MAX - 5,		// RTN_BROADCAST
		__RTN_MAX - 4,		// RTN_ANYCAST
		__RTN_MAX - 3,		// RTN_MULTICAST
		__RTN_MAX - 2,		// RTN_BLACKHOLE
		__RTN_MAX - 2,		// RTN_UNREACHABLE
		__RTN_MAX - 2,		// RTN_PROHIBIT
					// 0 the rest of the way...
	};
	return vals[c0] - vals[c1];
}

l2obj *add_l2_to_iface(const interface *i,iface_state *is,struct l2host *l2h){
	l2obj *l2;

	if( (l2 = get_l2obj(i,is,l2h)) ){
		if(++is->nodes == 1){
			is->l2objs = l2;
			l2->next = l2->prev = NULL;
		}else{
			l2obj **prev,*prec;

			for(prec = NULL, prev = &is->l2objs ; *prev ; prec = *prev, prev = &(*prev)->next){
				// we want the inverse of l2catcmp()'s priorities
				if(l2catcmp(l2->cat,(*prev)->cat) > 0){
					break;
				}else if(l2catcmp(l2->cat,(*prev)->cat) == 0){
					if(l2hostcmp(l2->l2,(*prev)->l2,i->addrlen) <= 0){
						break;
					}
				}
			}
			if( (l2->next = *prev) ){
				l2->prev = (*prev)->prev;
				(*prev)->prev = l2;
			}else{
				l2->prev = prec;
			}
			*prev = l2;
		}
	}
	return l2;
}

l3obj *add_l3_to_iface(iface_state *is,l2obj *l2,struct l3host *l3h){
	l3obj *l3;

	if( (l3 = get_l3obj(l3h)) ){
		l3->next = l2->l3objs;
		l2->l3objs = l3;
		l3->l2 = l2;
		if(is->expansion >= EXPANSION_HOSTS){
			++l2->lines;
		}
		++is->hosts;
	}
	return l3;
}

l4obj *add_service_to_iface(iface_state *is,struct l2obj *l2,struct l3obj *l3,
						struct l4srv *srv){
	l4obj *l4;

	// FIXME we ought be able to use this assert (or preferably get rid of
	// l3->l2 entirely), but it's being triggered (see bug 415). until we
	// get that resolved, we use the bogon check....
	// assert(l3->l2 == l2);
	if(l3->l2 != l2){
		return NULL;
	}
	if( (l4 = get_l4obj(srv)) ){
		l4obj **prev = &l3->l4objs;

		if(*prev == NULL){
			++is->srvs;
			if(is->expansion >= EXPANSION_SERVICES){
				++l3->l2->lines;
			}
		}else do{
			struct l4srv *c = (*prev)->l4;

			if(l4_getproto(srv) < l4_getproto(c)){
				break;
			}else if(l4_getproto(srv) == l4_getproto(c)){
				if(l4_getport(srv) < l4_getport(c)){
					break;
				}else if(l4_getport(srv) == l4_getport(c)){
					if(wcscmp(l4srvstr(srv),l4srvstr(c)) < 0){
						break;
					}
				}
			}
		}while( *(prev = &(*prev)->next) );
		l4->next = *prev;
		*prev = l4;
	}
	assert(node_lines(is->expansion,l3->l2) == l3->l2->lines);
	assert(node_lines(is->expansion,l2) == l2->lines);
	return l4;
}

static void
print_host_services(WINDOW *w,const l3obj *l,int *line,int rows,int cols,
				wchar_t selectchar,int attrs,int minline){
	const struct l4obj *l4;
	const wchar_t *srv;
	int n;

	if(*line >= rows){
		return;
	}
	if(*line < minline){
		*line += !!l->l4objs;
		return;
	}
	n = 0;
	for(l4 = l->l4objs ; l4 ; l4 = l4->next){
		if(l4_getproto(l4->l4) == IPPROTO_IP){
			assert(wattrset(w,attrs | A_BOLD) == OK);
		}else{
			assert(wattrset(w,attrs) == OK);
		}
		srv = l4srvstr(l4->l4);
		if(n){
			if((unsigned)cols < 2 + wcslen(srv)){ // two for ", "
				break;
			}
			cols -= 2 + wcslen(srv);
			n += 2 + wcslen(srv);
			assert(wprintw(w,", %ls",srv) != ERR);
		}else{
			if((unsigned)cols < 2 + 5 + wcslen(srv)){ // two for borders
				break;
			}
			cols -= 2 + 5 + wcslen(srv);
			n += 2 + 5 + wcslen(srv);
			assert(mvwprintw(w,*line,1,"%lc    %ls",selectchar,srv) != ERR);
			++*line;
		}
	}
	if(n && cols > 0){
		assert(wprintw(w,"%-*.*s",cols,cols,"") == OK);
	}
}

// line: line on which this node starts, within the WINDOW w of {rows x cols}
static void
print_iface_host(const interface *i,const iface_state *is,WINDOW *w,
		const l2obj *l,int line,int rows,int cols,int selected,
		int minline,unsigned endp){
	int aattrs,al3attrs,arattrs,attrs,l3attrs,rattrs,sattrs;
	char hw[HWADDRSTRLEN(i->addrlen)];
	const wchar_t *devname;
	wchar_t selectchar;
	char legend;
	l3obj *l3;

	// lines_for_interface() counts up nodes, hosts, and up to one line of
	// services per host. if we're a partial, we won't be displaying the
	// first or last (or both) lines of this output. each line that *would*
	// be printed increases 'line'. don't print if line doesn't make up for
	// the degree of top-partialness (line >= 0), but continue. break once
	// line is greater than the last available line, since we won't print
	// anymore.
	if(line >= rows - !endp){
		return;
	}
	switch(l->cat){
		case RTN_UNICAST:
			attrs = COLOR_PAIR(UCAST_COLOR);
			l3attrs = COLOR_PAIR(UCAST_L3_COLOR);
			rattrs = COLOR_PAIR(UCAST_RES_COLOR);
			sattrs = COLOR_PAIR(USELECTED_COLOR);
			aattrs = COLOR_PAIR(UCAST_ALTROW_COLOR);
			al3attrs = COLOR_PAIR(UCAST_ALTROW_L3_COLOR);
			arattrs = COLOR_PAIR(UCAST_ALTROW_RES_COLOR);
			devname = get_devname(l->l2);
			legend = 'U';
			break;
		case RTN_LOCAL:
			attrs = COLOR_PAIR(LCAST_COLOR) | OUR_BOLD;
			l3attrs = COLOR_PAIR(LCAST_L3_COLOR) | OUR_BOLD;
			rattrs = COLOR_PAIR(LCAST_RES_COLOR) | OUR_BOLD;
			sattrs = COLOR_PAIR(LSELECTED_COLOR);
			aattrs = COLOR_PAIR(LCAST_ALTROW_COLOR) | OUR_BOLD;
			al3attrs = COLOR_PAIR(LCAST_ALTROW_L3_COLOR) | OUR_BOLD;
			arattrs = COLOR_PAIR(LCAST_ALTROW_RES_COLOR) | OUR_BOLD;
			if(interface_virtual_p(i) ||
				(devname = get_devname(l->l2)) == NULL){
				devname = i->topinfo.devname;
			}
			legend = 'L';
			break;
		case RTN_MULTICAST:
			attrs = COLOR_PAIR(MCAST_COLOR);
			l3attrs = COLOR_PAIR(MCAST_L3_COLOR);
			rattrs = COLOR_PAIR(MCAST_RES_COLOR);
			sattrs = COLOR_PAIR(MSELECTED_COLOR);
			aattrs = COLOR_PAIR(MCAST_ALTROW_COLOR);
			al3attrs = COLOR_PAIR(MCAST_ALTROW_L3_COLOR);
			arattrs = COLOR_PAIR(MCAST_ALTROW_RES_COLOR);
			devname = get_devname(l->l2);
			legend = 'M';
			break;
		case RTN_BROADCAST:
			attrs = COLOR_PAIR(BCAST_COLOR);
			l3attrs = COLOR_PAIR(BCAST_L3_COLOR);
			rattrs = COLOR_PAIR(BCAST_RES_COLOR);
			sattrs = COLOR_PAIR(BSELECTED_COLOR);
			aattrs = COLOR_PAIR(BCAST_ALTROW_COLOR);
			al3attrs = COLOR_PAIR(BCAST_ALTROW_L3_COLOR);
			arattrs = COLOR_PAIR(BCAST_ALTROW_RES_COLOR);
			devname = get_devname(l->l2);
			legend = 'B';
			break;
		default:
			assert(0 && "Unknown l2 category");
			break;
	}
	if(selected){
		aattrs = attrs = sattrs;
		al3attrs = l3attrs = sattrs;
		arattrs = rattrs = sattrs;
		selectchar = l->l3objs && is->expansion >= EXPANSION_HOSTS
				? L'⎧' : L'⎨';
	}else{
		selectchar = L' ';
	}
	if(!interface_up_p(i)){
		attrs = (attrs & A_BOLD) | COLOR_PAIR(BULKTEXT_COLOR);
		l3attrs = (l3attrs & A_BOLD) | COLOR_PAIR(BULKTEXT_COLOR);
		rattrs = (rattrs & A_BOLD) | COLOR_PAIR(BULKTEXT_COLOR);
		aattrs = (aattrs & A_BOLD) | COLOR_PAIR(BULKTEXT_ALTROW_COLOR);
		al3attrs = (al3attrs & A_BOLD) | COLOR_PAIR(BULKTEXT_ALTROW_COLOR);
		arattrs = (arattrs & A_BOLD) | COLOR_PAIR(BULKTEXT_ALTROW_COLOR);
	}
	assert(wattrset(w,!(line % 2) ? attrs : aattrs) != ERR);
	if(line >= minline){
		int len;

		l2ntop(l->l2,i->addrlen,hw);
		if(devname){
			len = cols - PREFIXSTRLEN * 2 - 6 - HWADDRSTRLEN(i->addrlen);
			if(!interface_up_p(i)){
				len += PREFIXSTRLEN * 2 + 1;
			}
			assert(mvwprintw(w,line,1,"%lc%c %s %-*.*ls",
				selectchar,legend,hw,len,len,devname) != ERR);
		}else{
			len = cols - PREFIXSTRLEN * 2 - 6;
			if(!interface_up_p(i)){
				len += PREFIXSTRLEN * 2 + 1;
			}
			assert(mvwprintw(w,line,1,"%lc%c %-*.*s",
				selectchar,legend,len,len,hw) != ERR);
		}
		if(interface_up_p(i)){
			char sbuf[PREFIXSTRLEN + 1],dbuf[PREFIXSTRLEN + 1];
			if(get_srcpkts(l->l2) == 0 && (l->cat == RTN_MULTICAST || l->cat == RTN_BROADCAST)){
				wprintw(w,"%-*.*s"PREFIXFMT,PREFIXSTRLEN + 1,PREFIXSTRLEN + 1,
						"",prefix(get_dstpkts(l->l2),1,dbuf,sizeof(dbuf),1));
			}else{
				wprintw(w,PREFIXFMT" "PREFIXFMT,prefix(get_srcpkts(l->l2),1,sbuf,sizeof(sbuf),1),
						prefix(get_dstpkts(l->l2),1,dbuf,sizeof(dbuf),1));
			}
		}
	}
	++line;
	if(is->expansion >= EXPANSION_HOSTS){
		for(l3 = l->l3objs ; l3 ; l3 = l3->next){
			char nw[INET6_ADDRSTRLEN + 1]; // FIXME
			const wchar_t *name;

			if(selectchar != L' '){
				if(l3->next || (l3->l4objs && is->expansion >= EXPANSION_SERVICES)){
					selectchar = L'⎪';
				}else{
					selectchar = L'⎩';
				}
			}
			if(line >= rows - !endp){
				break;
			}
			if(line >= minline){
				int len,wlen;

				assert(wattrset(w,!(line % 2) ? l3attrs : al3attrs) != ERR);
				l3ntop(l3->l3,nw,sizeof(nw));
				if((name = get_l3name(l3->l3)) == NULL){
					name = L"";
				}
				assert(mvwprintw(w,line,1,"%lc   %s ",
					selectchar,nw) != ERR);
				assert(wattrset(w,!(line % 2) ? rattrs : arattrs) != ERR);
				len = cols - PREFIXSTRLEN * 2 - 8 - strlen(nw);
				wlen = len - wcswidth(name,wcslen(name));
				if(wlen < 0){
					wlen = 0;
				}
				assert(wprintw(w,"%.*ls%*.*s",len,name,wlen,wlen,"") != ERR);
				assert(wattrset(w,!(line % 2) ? l3attrs : al3attrs) != ERR);
				{
					char sbuf[PREFIXSTRLEN + 1];
					char dbuf[PREFIXSTRLEN + 1];
					if(l3_get_srcpkt(l3->l3) == 0 && (l->cat == RTN_MULTICAST || l->cat == RTN_BROADCAST)){
						wprintw(w,"%-*.*s"PREFIXFMT,PREFIXSTRLEN + 1,PREFIXSTRLEN + 1,
								"",prefix(l3_get_dstpkt(l3->l3),1,dbuf,sizeof(dbuf),1));
					}else{
						wprintw(w,PREFIXFMT" "PREFIXFMT,
								prefix(l3_get_srcpkt(l3->l3),1,sbuf,sizeof(sbuf),1),
								prefix(l3_get_dstpkt(l3->l3),1,dbuf,sizeof(dbuf),1));
					}
				}
			}
			++line;
			if(is->expansion >= EXPANSION_SERVICES){
				if(selectchar != L' ' && !l3->next){
					selectchar = L'⎩';
				}
				print_host_services(w,l3,&line,rows - !endp,cols,selectchar,
						!(line % 2) ? attrs : aattrs,minline);
			}
		}
	}
}

// FIXME all these casts! :/ appalling
static void
print_iface_hosts(const interface *i,const iface_state *is,const reelbox *rb,
					WINDOW *w,int rows,int cols,
					unsigned topp,unsigned endp){
	// If the interface is down, we don't lead with the summary line
	const int sumline = !!interface_up_p(i);
	const struct l2obj *cur;
	long line;

	if(is->expansion < EXPANSION_NODES){
		return;
	}
	// First, print the selected interface (if there is one)
	cur = rb->selected;
	line = rb->selline + sumline;
	while(cur && line + (long)cur->lines >= !!topp + sumline){
		print_iface_host(i,is,w,cur,line,rows,cols,cur == rb->selected,
					!!topp + sumline,endp);
		// here we traverse, then account...
		if( (cur = cur->prev) ){
			line -= cur->lines;
		}
	}
	line = rb->selected ? (rb->selline + (long)rb->selected->lines) :
						-(long)topp + 1;
	line += sumline;
	cur = (rb->selected ? rb->selected->next : is->l2objs);
	while(cur && line < rows){
		print_iface_host(i,is,w,cur,line,rows,cols,0,0,endp);
		// here, we account before we traverse. this is correct.
		line += cur->lines;
		cur = cur->next;
	}
}

static int
iface_optstr(WINDOW *w,const char *str,int hcolor,int bcolor){
	if(wcolor_set(w,bcolor,NULL) != OK){
		return ERR;
	}
	if(waddch(w,'|') == ERR){
		return ERR;
	}
	if(wcolor_set(w,hcolor,NULL) != OK){
		return ERR;
	}
	if(waddstr(w,str) == ERR){
		return ERR;
	}
	return OK;
}

static const char *
duplexstr(unsigned dplx){
	switch(dplx){
		case DUPLEX_FULL: return "full"; break;
		case DUPLEX_HALF: return "half"; break;
		default: break;
	}
	return "";
}

// Abovetop: lines hidden at the top of the screen
// Belowend: lines hidden at the bottom of the screen
static void
iface_box(const interface *i,const iface_state *is,WINDOW *w,int active,
				unsigned abovetop,unsigned belowend){
	int bcolor,hcolor,rows,cols;
	size_t buslen;
	int attrs;

	getmaxyx(w,rows,cols);
	bcolor = interface_up_p(i) ? UBORDER_COLOR : DBORDER_COLOR;
	hcolor = interface_up_p(i) ? UHEADING_COLOR : DHEADING_COLOR;
	attrs = active ? A_REVERSE : A_BOLD;
	assert(wattrset(w,attrs | COLOR_PAIR(bcolor)) == OK);
	if(abovetop == 0){
		if(belowend == 0){
			assert(bevel(w) == OK);
		}else{
			assert(bevel_nobottom(w) == OK);
		}
	}else{
		if(belowend == 0){
			assert(bevel_notop(w) == OK);
		}else{
			assert(bevel_noborder(w) == OK);
		}
	}
	assert(wattroff(w,A_REVERSE) == OK);

	if(abovetop == 0){
		if(active){
			assert(wattron(w,A_BOLD) == OK);
		}
		assert(mvwprintw(w,0,1,"[") != ERR);
		assert(wcolor_set(w,hcolor,NULL) == OK);
		if(active){
			assert(wattron(w,A_BOLD) == OK);
		}else{
			assert(wattroff(w,A_BOLD) == OK);
		}
		assert(waddstr(w,i->name) != ERR);
		assert(wprintw(w," (%s",is->typestr) != ERR);
		if(strlen(i->drv.driver)){
			assert(waddch(w,' ') != ERR);
			assert(waddstr(w,i->drv.driver) != ERR);
			if(strlen(i->drv.version)){
				assert(wprintw(w," %s",i->drv.version) != ERR);
			}
			if(strlen(i->drv.fw_version)){
				assert(wprintw(w," fw %s",i->drv.fw_version) != ERR);
			}
		}
		assert(waddch(w,')') != ERR);
		assert(wcolor_set(w,bcolor,NULL) != ERR);
		if(active){
			assert(wattron(w,A_BOLD) == OK);
		}
		assert(wprintw(w,"]") != ERR);
		assert(wmove(w,0,cols - 4) != ERR);
		assert(wattron(w,A_BOLD) == OK);
		assert(waddwstr(w,is->expansion == EXPANSION_MAX ? L"[-]" :
					is->expansion == 0 ? L"[+]" : L"[±]") != ERR);
		assert(wattron(w,attrs) != ERR);
		assert(wattroff(w,A_REVERSE) != ERR);
	}
	if(belowend == 0){
		assert(mvwprintw(w,rows - 1,2,"[") != ERR);
		assert(wcolor_set(w,hcolor,NULL) != ERR);
		if(active){
			assert(wattron(w,A_BOLD) == OK);
		}else{
			assert(wattroff(w,A_BOLD) == OK);
		}
		assert(wprintw(w,"mtu %d",i->mtu) != ERR);
		if(interface_up_p(i)){
			char buf[U64STRLEN + 1];

			assert(iface_optstr(w,"up",hcolor,bcolor) != ERR);
			if(i->settings_valid == SETTINGS_VALID_ETHTOOL){
				if(!interface_carrier_p(i)){
					assert(waddstr(w," (no carrier)") != ERR);
				}else{
					assert(wprintw(w," (%sb %s)",prefix(i->settings.ethtool.speed * 1000000u,1,buf,sizeof(buf),1),
								duplexstr(i->settings.ethtool.duplex)) != ERR);
				}
			}else if(i->settings_valid == SETTINGS_VALID_WEXT){
				if(i->settings.wext.mode == NL80211_IFTYPE_MONITOR){
					assert(wprintw(w," (%s)",modestr(i->settings.wext.mode)) != ERR);
				}else if(!interface_carrier_p(i)){
					assert(wprintw(w," (%s, no carrier)",modestr(i->settings.wext.mode)) != ERR);
				}else{
					assert(wprintw(w," (%sb %s ",prefix(i->settings.wext.bitrate,1,buf,sizeof(buf),1),
								modestr(i->settings.wext.mode)) != ERR);
					if(i->settings.wext.freq <= MAX_WIRELESS_CHANNEL){
						assert(wprintw(w,"ch %ju)",i->settings.wext.freq) != ERR);
					}else{
						assert(wprintw(w,"%sHz)",prefix(i->settings.wext.freq,1,buf,sizeof(buf),1)) != ERR);
					}
				}
			}
		}else{
			assert(iface_optstr(w,"down",hcolor,bcolor) != ERR);
			if(i->settings_valid == SETTINGS_VALID_WEXT){
				assert(wprintw(w," (%s)",modestr(i->settings.wext.mode)) != ERR);
			}
		}
		if(interface_promisc_p(i)){
			assert(iface_optstr(w,"promisc",hcolor,bcolor) != ERR);
		}
		assert(wcolor_set(w,bcolor,NULL) != ERR);
		if(active){
			assert(wattron(w,A_BOLD) == OK);
		}
		assert(wprintw(w,"]") != ERR);
		if( (buslen = strlen(i->drv.bus_info)) ){
			if(active){
				// FIXME Want the text to be bold -- currently unreadable
				assert(wattrset(w,A_REVERSE | COLOR_PAIR(bcolor)) != ERR);
			}else{
				assert(wattrset(w,COLOR_PAIR(bcolor) | A_BOLD) != ERR);
			}
			if(i->busname){
				buslen += strlen(i->busname) + 1;
				assert(mvwprintw(w,rows - 1,cols - (buslen + 2),
						"%s:%s",i->busname,i->drv.bus_info) != ERR);
			}else{
				assert(mvwprintw(w,rows - 1,cols - (buslen + 2),
						"%s",i->drv.bus_info) != ERR);
			}
		}
	}
}

static void
print_iface_state(const interface *i,const iface_state *is,WINDOW *w,
				int rows,int cols,unsigned topp){
	char buf[U64STRLEN + 1],buf2[U64STRLEN + 1];
	unsigned long usecdomain;

	if(rows < 2 || topp > 1){
		return;
	}
	assert(wattrset(w,A_BOLD | COLOR_PAIR(IFACE_COLOR)) != ERR);
	// FIXME broken if bps domain ever != fps domain. need unite those
	// into one FTD stat by letting it take an object...
	// FIXME this leads to a "ramp-up" period where we approach steady state
	usecdomain = i->bps.usec * i->bps.total;
	assert(mvwprintw(w,!topp,1,"%u node%s. Last %lus: %7sb/s (%sp)",
		is->nodes,is->nodes == 1 ? "" : "s",
		usecdomain / 1000000,
		prefix(timestat_val(&i->bps) * CHAR_BIT * 1000000 * 100 / usecdomain,100,buf,sizeof(buf),0),
		prefix(timestat_val(&i->fps),1,buf2,sizeof(buf2),1)) != ERR);
	assert(mvwprintw(w,1,cols - PREFIXSTRLEN * 2 - 5,"Total: Src     Dst") != ERR);
}

void free_iface_state(iface_state *is){
	l2obj *l2 = is->l2objs;

	while(l2){
		l2obj *tmp = l2->next;
		free_l2obj(l2);
		l2 = tmp;
	}
}

int redraw_iface(const reelbox *rb,int active){
	const iface_state *is = rb->is;
	const interface *i = is->iface;
	int rows,cols,scrrows,scrcols;
	unsigned topp,endp;

	if(panel_hidden(rb->panel)){
		return OK;
	}
	getmaxyx(stdscr,scrrows,scrcols);
	if(iface_wholly_visible_p(scrrows,rb) || active){ // completely visible
		topp = endp = 0;
	}else if(getbegy(rb->subwin) == 1){ // no top
		topp = iface_lines_unbounded(is) - getmaxy(rb->subwin);
		endp = 0;
	}else{
		topp = 0;
		endp = 1; // no bottom FIXME
	}
	getmaxyx(rb->subwin,rows,cols);
	assert(cols < scrcols); // FIXME
	assert(werase(rb->subwin) != ERR);
	iface_box(i,is,rb->subwin,active,topp,endp);
	print_iface_hosts(i,is,rb,rb->subwin,rows,cols,topp,endp);
	if(interface_up_p(i)){
		print_iface_state(i,is,rb->subwin,rows,cols,topp);
	}
	return OK;
}

// Move this interface, possibly hiding it. Negative delta indicates movement
// up, positive delta moves down. rows and cols describe the containing window.
void move_interface(reelbox *rb,int targ,int rows,int cols,int delta,int active){
	const iface_state *is;
	int nlines,rr;
       
	is = rb->is;
	//fprintf(stderr,"  moving %s (%d) from %d to %d (%d)\n",is->iface->name,
	//		iface_lines_bounded(is,rows),getbegy(rb->subwin),targ,delta);
	assert(rb->is);
	assert(rb->is->rb == rb);
	assert(werase(rb->subwin) != ERR);
	screen_update();
	if(iface_wholly_visible_p(rows,rb)){
		assert(move_panel(rb->panel,targ,1) != ERR);
		if(getmaxy(rb->subwin) != iface_lines_bounded(is,rows)){
			assert(wresize(rb->subwin,iface_lines_bounded(is,rows),PAD_COLS(cols)) == OK);
			if(panel_hidden(rb->panel)){
				assert(show_panel(rb->panel) == OK);
			}
		}
		assert(redraw_iface(rb,active) == OK);
		return;
	}
	rr = getmaxy(rb->subwin);
	if(delta > 0){ // moving down
		if(targ >= rows - 1){
			assert(hide_panel(rb->panel) != ERR);
			return;
		}
		nlines = rows - targ - 1; // sans-bottom partial
	}else{
		if((rr + getbegy(rb->subwin)) <= -delta){
			assert(hide_panel(rb->panel) != ERR);
			return;
		}
		if(targ < 1){
			nlines = rr + (targ - 1);
			targ = 1;
		}else{
			nlines = iface_lines_bounded(is,rows - targ + 1);
		}
	}
	if(nlines < 1){
		assert(hide_panel(rb->panel) != ERR);
		return;
	}else if(nlines > rr){
		assert(move_panel(rb->panel,targ,1) == OK);
		assert(wresize(rb->subwin,nlines,PAD_COLS(cols)) == OK);
	}else if(nlines < rr){
		assert(wresize(rb->subwin,nlines,PAD_COLS(cols)) == OK);
		assert(move_panel(rb->panel,targ,1) == OK);
	}else{
		assert(move_panel(rb->panel,targ,1) == OK);
	}
	assert(redraw_iface(rb,active) == OK);
	assert(show_panel(rb->panel) == OK);
	return;
}

// This is the number of lines we'd have in an optimal world; we might have
// fewer available to us on this screen at this time.
int lines_for_interface(const iface_state *is){
	int lines = 2 + interface_up_p(is->iface);

	switch(is->expansion){ // Intentional fallthrus
		case EXPANSION_SERVICES:
			lines += is->srvs;
		case EXPANSION_HOSTS:
			lines += is->hosts;
		case EXPANSION_NODES:
			lines += is->nodes;
		case EXPANSION_NONE:
			return lines;
	}
	assert(0);
	return -1;
}

// Is the interface window entirely visible? We can't draw it otherwise, as it
// will obliterate the global bounding box.
int iface_wholly_visible_p(int rows,const reelbox *rb){
	const iface_state *is = rb->is;

	// return iface_lines_bounded(is,rows) <= getmaxy(rb->subwin);
	if(rb->scrline + iface_lines_bounded(is,rows) >= rows){
		return 0;
	}else if(rb->scrline < 1){
		return 0;
	}else if(rb->scrline == 1 && iface_lines_bounded(is,rows) != getmaxy(rb->subwin)){
		return 0;
	}
	return 1;
}

static void
recompute_selection(iface_state *is){
	int newsel = !!interface_up_p(is->iface);
	l2obj *l;

	for(l = is->l2objs ; l ; l = l->next){
		l->lines = node_lines(is->expansion,l);
		if(l != is->rb->selected){
			// FIXME don't count the lines if they're hidden!
			newsel += l->lines;
		}else{
			is->rb->selline = newsel;
		}
	}
}

void expand_interface(iface_state *is){
	if(is->expansion == EXPANSION_MAX){
		return;
	}
	++is->expansion;
	recompute_selection(is);
}

void collapse_interface(iface_state *is){
	if(is->expansion == 0){
		return;
	}
	--is->expansion;
	recompute_selection(is);
}
