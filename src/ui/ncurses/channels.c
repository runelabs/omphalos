#include <assert.h>
#include <string.h>
#include <net/if.h>
#include <ui/ncurses/core.h>
#include <ui/ncurses/util.h>
#include <omphalos/wireless.h>
#include <omphalos/interface.h>
#include <ui/ncurses/channels.h>

#define WIRELESSROWS 5 // FIXME

static unsigned ifaces_used;
static const struct iface_state *ifaces[WIRELESSROWS];

// We take advantage of the fact that bgn, an, and y all support multiples of
// 14 channels to avoid doing a fully dynamic layout. Unfortunately, this
// assumes at least 70 columns (14 * 4 + IFNAMSIZ + 1).
#define FREQSPERROW 14	// FIXME do it dynamically based on cols

static int
channel_row(WINDOW *w,unsigned freqrow,int srow,int scol){
	unsigned f;

	assert(wmove(w,srow,scol + IFNAMSIZ + 1) == OK);
	for(f = freqrow * FREQSPERROW ; f < (freqrow + 1) * FREQSPERROW ; ++f){
		unsigned chan = wireless_chan_byidx(f);

		assert(chan && chan < 1000);
		assert(wprintw(w," %3u",chan) == OK);
	}
	return 0;
}

static int
channel_details(WINDOW *w){
	unsigned freqs,freqrows,z;
	const int row = 1;
	int r,c,col;

	getmaxyx(w,r,c);
	col = c - (START_COL + FREQSPERROW * 4 + IFNAMSIZ + 1);
	assert(col >= 0);
	assert(wattrset(w,SUBDISPLAY_ATTR) == OK);
	freqs = wireless_freq_count();
	assert(freqs == FREQSPERROW * WIRELESSROWS);
	freqrows = freqs / FREQSPERROW;
	if((z = r) >= WIRELESSROWS){
		z = WIRELESSROWS - 1;
	}
	switch(z){ // Intentional fallthroughs all the way through 0
		case (WIRELESSROWS - 1):{
			channel_row(w,freqrows - 1,row + z,col);
			--z;
		}case 3:{
			channel_row(w,freqrows - 2,row + z,col);
			--z;
		}case 2:{
			channel_row(w,freqrows - 3,row + z,col);
			--z;
		}case 1:{
			channel_row(w,freqrows - 4,row + z,col);
			--z;
		}case 0:{
			channel_row(w,freqrows - 5,row + z,col);
			--z;
			break;
		}default:{
			return ERR;
		}
	}
	return OK;
}

int display_channels_locked(WINDOW *w,struct panel_state *ps){
	memset(ps,0,sizeof(*ps));
	if(new_display_panel(w,ps,WIRELESSROWS,0,L"press 'w' to dismiss display")){
		goto err;
	}
	if(channel_details(panel_window(ps->p))){
		goto err;
	}
	return OK;

err:
	if(ps->p){
		WINDOW *psw = panel_window(ps->p);

		hide_panel(ps->p);
		del_panel(ps->p);
		delwin(psw);
	}
	memset(ps,0,sizeof(*ps));
	return ERR;
}

int add_channel_support(struct iface_state *is){
	if(is->iface->settings_valid != SETTINGS_VALID_WEXT &&
			is->iface->settings_valid != SETTINGS_VALID_NL80211){
		return 0;
	}
	if(ifaces_used == sizeof(ifaces) / sizeof(*ifaces)){
		return 0;
	}
	ifaces[ifaces_used++] = is;
	// FIXME update if active!
	return 0;
}

int del_channel_support(struct iface_state *is){
	unsigned z;

	for(z = 0 ; z < ifaces_used ; ++z){
		if(ifaces[z] == is){
			if(z < ifaces_used - 1){
				memmove(ifaces + z,ifaces + z + 1,sizeof(*ifaces) * (ifaces_used - z - 1));
			}
			--ifaces_used;
			// FIXME update if active!
			break;
		}
	}
	return 0;
}
