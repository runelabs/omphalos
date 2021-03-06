#include <errno.h>
#include <ctype.h>
#include <net/if.h>
#include <assert.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <locale.h>
#include <string.h>
#include <signal.h>
#include <limits.h>
#include <pthread.h>
#include <langinfo.h>
#include <sys/time.h>
#include <sys/socket.h>

// The wireless extensions headers are not so fantastic. This workaround comes
// to us courtesy of Jean II in iwlib.h. Ugh.
#ifndef __user
#define __user
#endif
#include <asm/types.h>
#include <wireless.h>

#include <sys/utsname.h>
#include <linux/version.h>
#include <ncursesw/panel.h>
#include <linux/rtnetlink.h>
#include <ui/ncurses/util.h>
#include <ui/ncurses/core.h>
#include <omphalos/timing.h>
#include <ui/ncurses/color.h>
#include <ncursesw/ncurses.h>
#include <omphalos/hwaddrs.h>
#include <ui/ncurses/iface.h>
#include <gnu/libc-version.h>
#include <omphalos/ethtool.h>
#include <omphalos/netaddrs.h>
#include <omphalos/omphalos.h>
#include <ui/ncurses/network.h>
#include <omphalos/interface.h>
#include <ui/ncurses/channels.h>

#define KEY_ESC 27

#define ERREXIT endwin() ; fprintf(stderr,"ncurses failure|%s|%d\n",__func__,__LINE__); abort() ; goto err

#define PANEL_STATE_INITIALIZER { .p = NULL, .ysize = -1, }

static struct panel_state help = PANEL_STATE_INITIALIZER;
static struct panel_state diags = PANEL_STATE_INITIALIZER;
static struct panel_state details = PANEL_STATE_INITIALIZER;
static struct panel_state network = PANEL_STATE_INITIALIZER;
static struct panel_state bridging = PANEL_STATE_INITIALIZER;
static struct panel_state channels = PANEL_STATE_INITIALIZER;
static struct panel_state environment = PANEL_STATE_INITIALIZER;

// Add ((format (printf))) attributes to ncurses functions, which sadly
// lack them (at least as of Debian's 5.9-1).
extern int wprintw(WINDOW *,const char *,...) __attribute__ ((format (printf,2,3)));
extern int mvwprintw(WINDOW *,int,int,const char *,...) __attribute__ ((format (printf,4,5)));

static struct panel_state *active;

// FIXME granularize things, make packet handler iret-like
static pthread_mutex_t bfl = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

static pthread_t inputtid;

// Old host versioning display info
static const char *glibc_version,*glibc_release; // Currently unused
static struct utsname sysuts; // Currently unused

static inline void
lock_ncurses(void){
	assert(pthread_mutex_lock(&bfl) == 0);
	check_consistency();
}

static inline void
unlock_ncurses(void){
	if(active){
		assert(top_panel(active->p) != ERR);
	}
	screen_update();
	check_consistency();
	assert(pthread_mutex_unlock(&bfl) == 0);
}

// NULL fmt clears the status bar. wvstatus is an unlocked entry point, and
// thus calls screen_update() on exit.
static int
wvstatus(WINDOW *w,const char *fmt,va_list va){
	int ret;

	lock_ncurses();
	ret = wvstatus_locked(w,fmt,va);
	if(diags.p && fmt){
		ret |= update_diags_locked(&diags);
	}
	unlock_ncurses();
	return ret;
}

// NULL fmt clears the status bar. wstatus is an unlocked entry point, and thus
// calls screen_update() on exit.
static int
wstatus(WINDOW *w,const char *fmt,...){
	va_list va;
	int ret;

	va_start(va,fmt);
	ret = wvstatus(w,fmt,va); // calls screen_update()
	va_end(va);
	return ret;
}

static void
resize_screen_locked(WINDOW *w){
	/*int rows,cols;

	getmaxyx(w,rows,cols);*/
	draw_main_window(w);
}

// Completely redraw the screen, for instance after a corruption (see wrefresh
// man pageL: "If the argument to wrefresh is curscr, the screen is immediately
// cleared and repainted from scratch."
static void
redraw_screen_locked(void){
	wrefresh(curscr);
}

struct ncurses_input_marshal {
	WINDOW *w;
	pthread_t maintid;
};


static void
toggle_panel(WINDOW *w,struct panel_state *ps,int (*psfxn)(WINDOW *,struct panel_state *)){
	if(ps->p){
		hide_panel_locked(ps);
		active = NULL;
	}else{
		hide_panel_locked(active);
		active = ((psfxn(w,ps) == OK) ? ps : NULL);
	}
}

// Only meaningful if there are both interfaces and a subdisplay
static void
toggle_focus(void){
}

// Only meaningful if there are both interfaces and a subdisplay
static void
toggle_subwindow_pinning(void){
}

static void *
ncurses_input_thread(void *unsafe_marsh){
	struct ncurses_input_marshal *nim = unsafe_marsh;
	WINDOW *w = nim->w;
	int ch;

	active = NULL; // No subpanels initially
	while((ch = getch()) != 'q' && ch != 'Q'){
	switch(ch){
		case KEY_HOME:
			lock_ncurses();
			if(selecting()){
				use_first_node_locked();
			}
			unlock_ncurses();
			break;
		case KEY_END:
			lock_ncurses();
			if(selecting()){
				use_last_node_locked();
			}
			unlock_ncurses();
			break;
		case KEY_PPAGE:
			lock_ncurses();
			if(selecting()){
				use_prev_nodepage_locked();
			}
			unlock_ncurses();
			break;
		case KEY_NPAGE:
			lock_ncurses();
			if(selecting()){
				use_next_nodepage_locked();
			}
			unlock_ncurses();
			break;
		case KEY_UP: case 'k':
			lock_ncurses();
			if(!selecting()){
				use_prev_iface_locked(w,&details);
			}else{
				use_prev_node_locked();
			}
			unlock_ncurses();
			break;
		case KEY_DOWN: case 'j':
			lock_ncurses();
			if(!selecting()){
				use_next_iface_locked(w,&details);
			}else{
				use_next_node_locked();
			}
			unlock_ncurses();
			break;
		case KEY_RESIZE:
			lock_ncurses();{
				resize_screen_locked(w);
			}unlock_ncurses();
			break;
		case 9: // Tab FIXME
			lock_ncurses();
				toggle_focus();
			unlock_ncurses();
			break;
		case 12: // Ctrl-L FIXME
			lock_ncurses();{
				redraw_screen_locked();
			}unlock_ncurses();
			break;
		case '\r': case '\n': case KEY_ENTER:
			lock_ncurses();{
				select_iface_locked();
			}unlock_ncurses();
			break;
		case KEY_ESC: case KEY_BACKSPACE:
			lock_ncurses();{
				deselect_iface_locked();
			}unlock_ncurses();
			break;
		case 'l':
			lock_ncurses();
				toggle_panel(w,&diags,display_diags_locked);
			unlock_ncurses();
			break;
		case 'D':
			lock_ncurses();
				resolve_selection(w);
			unlock_ncurses();
			break;
		case 'r':
			lock_ncurses();
				reset_current_interface_stats(w);
			unlock_ncurses();
			break;
		case 'P':
			lock_ncurses();
				toggle_subwindow_pinning();
			unlock_ncurses();
			break;
		case 'p':
			lock_ncurses();
				toggle_promisc_locked(w);
			unlock_ncurses();
			break;
		case 'd':
			lock_ncurses();
				down_interface_locked(w);
			unlock_ncurses();
			break;
		case 's':
			lock_ncurses();
				sniff_interface_locked(w);
			unlock_ncurses();
			break;
		case '+':
		case KEY_RIGHT:
			lock_ncurses();
				expand_iface_locked();
			unlock_ncurses();
			break;
		case '-':
		case KEY_LEFT:
			lock_ncurses();
				collapse_iface_locked();
			unlock_ncurses();
			break;
		case 'v':{
			lock_ncurses();
				toggle_panel(w,&details,display_details_locked);
			unlock_ncurses();
			break;
		}case 'n':{
			lock_ncurses();
				toggle_panel(w,&network,display_network_locked);
			unlock_ncurses();
			break;
		}case 'e':{
			lock_ncurses();
				toggle_panel(w,&environment,display_env_locked);
			unlock_ncurses();
			break;
		}case 'w':{
			lock_ncurses();
				toggle_panel(w,&channels,display_channels_locked);
			unlock_ncurses();
			break;
		}case 'b':{
			lock_ncurses();
				toggle_panel(w,&bridging,display_bridging_locked);
			unlock_ncurses();
			break;
		}case 'h':{
			lock_ncurses();
				toggle_panel(w,&help,display_help_locked);
			unlock_ncurses();
			break;
		}default:{
			const char *hstr = !help.p ? " ('h' for help)" : "";
			// wstatus() locks/unlocks, and calls screen_update()
			if(isprint(ch)){
				wstatus(w,"unknown command '%c'%s",ch,hstr);
			}else{
				wstatus(w,"unknown scancode %d%s",ch,hstr);
			}
			break;
		}
	}
	}
	wstatus(w,"%s","shutting down");
	// we can't use raise() here, as that sends the signal only
	// to ourselves, and we have it masked.
	pthread_kill(nim->maintid,SIGINT);
	pthread_exit(NULL);
}

// Cleanup which ought be performed even if we had a failure elsewhere, or
// indeed never started.
static int
mandatory_cleanup(WINDOW **w){
	int ret = 0;

	pthread_mutex_lock(&bfl);
	if(*w){
		if(delwin(*w) != OK){
			ret = -1;
		}
		*w = NULL;
	}
	if(stdscr){
		if(delwin(stdscr) != OK){
			ret = -2;
		}
		stdscr = NULL;
	}
	if(endwin() != OK){
		ret = -3;
	}
	pthread_mutex_unlock(&bfl);
	switch(ret){
	case -3: fprintf(stderr,"Couldn't end main window\n"); break;
	case -2: fprintf(stderr,"Couldn't delete main window\n"); break;
	case -1: fprintf(stderr,"Couldn't delete main pad\n"); break;
	case 0: break;
	default: fprintf(stderr,"Couldn't cleanup ncurses\n"); break;
	}
	return ret;
}

static WINDOW *
ncurses_setup(void){
	struct ncurses_input_marshal *nim;
	const char *errstr = NULL;
	WINDOW *w = NULL;

	fprintf(stderr,"Entering ncurses mode...\n");
	if(initscr() == NULL){
		fprintf(stderr,"Couldn't initialize ncurses\n");
		return NULL;
	}
	if(cbreak() != OK){
		errstr = "Couldn't disable input buffering\n";
		goto err;
	}
	if(noecho() != OK){
		errstr = "Couldn't disable input echoing\n";
		goto err;
	}
	if(intrflush(stdscr,TRUE) != OK){
		errstr = "Couldn't set flush-on-interrupt\n";
		goto err;
	}
	if(scrollok(stdscr,FALSE) != OK){
		errstr = "Couldn't disable scrolling\n";
		goto err;
	}
	if(nonl() != OK){
		errstr = "Couldn't disable nl translation\n";
		goto err;
	}
	if(start_color() != OK){
		errstr = "Couldn't initialize ncurses color\n";
		goto err;
	}
	if(use_default_colors()){
		errstr = "Couldn't initialize ncurses colordefs\n";
		goto err;
	}
	w = stdscr;
	ESCDELAY = 100;
	keypad(stdscr,TRUE);
	if(nodelay(stdscr,FALSE) != OK){
		errstr = "Couldn't set blocking input\n";
		goto err;
	}
	if(curs_set(0) == ERR){
		errstr = "Couldn't disable cursor\n";
		goto err;
	}
	if(setup_statusbar(COLS)){
		errstr = "Couldn't setup status bar\n";
		goto err;
	}
	if(COLORS < 16){
		assert(init_pair(BORDER_COLOR,COLOR_GREEN,-1) == OK);
		assert(init_pair(HEADER_COLOR,COLOR_BLUE,-1) == OK);
		assert(init_pair(FOOTER_COLOR,COLOR_YELLOW,-1) == OK);
		assert(init_pair(DBORDER_COLOR,COLOR_WHITE,-1) == OK);
		assert(init_pair(DHEADING_COLOR,COLOR_WHITE,-1) == OK);
		assert(init_pair(UBORDER_COLOR,COLOR_CYAN,-1) == OK);
		assert(init_pair(UHEADING_COLOR,COLOR_BLUE,-1) == OK);
		assert(init_pair(PBORDER_COLOR,COLOR_YELLOW,-1) == OK);
		assert(init_pair(PHEADING_COLOR,COLOR_RED,-1) == OK);
		assert(init_pair(BULKTEXT_COLOR,COLOR_WHITE,-1) == OK);
		assert(init_pair(BULKTEXT_ALTROW_COLOR,COLOR_WHITE,-1) == OK);
		assert(init_pair(IFACE_COLOR,COLOR_WHITE,-1) == OK);
		assert(init_pair(LCAST_COLOR,COLOR_CYAN,-1) == OK); // will use A_BOLD via OUR_BOLD
		assert(init_pair(UCAST_COLOR,COLOR_CYAN,-1) == OK);
		assert(init_pair(MCAST_COLOR,COLOR_BLUE,-1) == OK);
		assert(init_pair(BCAST_COLOR,COLOR_MAGENTA,-1) == OK);
		assert(init_pair(LSELECTED_COLOR,-1,COLOR_CYAN) == OK);
		assert(init_pair(USELECTED_COLOR,-1,COLOR_CYAN) == OK);
		assert(init_pair(MSELECTED_COLOR,-1,COLOR_BLUE) == OK);
		assert(init_pair(BSELECTED_COLOR,-1,COLOR_MAGENTA) == OK);
		assert(init_pair(LCAST_L3_COLOR,COLOR_CYAN,-1) == OK);
		assert(init_pair(UCAST_L3_COLOR,COLOR_CYAN,-1) == OK);
		assert(init_pair(MCAST_L3_COLOR,COLOR_BLUE,-1) == OK);
		assert(init_pair(BCAST_L3_COLOR,COLOR_MAGENTA,-1) == OK);
		assert(init_pair(LCAST_RES_COLOR,COLOR_CYAN,-1) == OK);
		assert(init_pair(UCAST_RES_COLOR,COLOR_CYAN,-1) == OK);
		assert(init_pair(MCAST_RES_COLOR,COLOR_BLUE,-1) == OK);
		assert(init_pair(BCAST_RES_COLOR,COLOR_MAGENTA,-1) == OK);
		assert(init_pair(LCAST_ALTROW_COLOR,COLOR_CYAN,-1) == OK);
		assert(init_pair(UCAST_ALTROW_COLOR,COLOR_CYAN,-1) == OK);
		assert(init_pair(MCAST_ALTROW_COLOR,COLOR_BLUE,-1) == OK);
		assert(init_pair(BCAST_ALTROW_COLOR,COLOR_MAGENTA,-1) == OK);
		assert(init_pair(LCAST_ALTROW_L3_COLOR,COLOR_CYAN,-1) == OK);
		assert(init_pair(UCAST_ALTROW_L3_COLOR,COLOR_CYAN,-1) == OK);
		assert(init_pair(MCAST_ALTROW_L3_COLOR,COLOR_BLUE,-1) == OK);
		assert(init_pair(BCAST_ALTROW_L3_COLOR,COLOR_MAGENTA,-1) == OK);
		assert(init_pair(LCAST_ALTROW_RES_COLOR,COLOR_CYAN,-1) == OK);
		assert(init_pair(UCAST_ALTROW_RES_COLOR,COLOR_CYAN,-1) == OK);
		assert(init_pair(MCAST_ALTROW_RES_COLOR,COLOR_BLUE,-1) == OK);
		assert(init_pair(BCAST_ALTROW_RES_COLOR,COLOR_MAGENTA,-1) == OK);
		assert(init_pair(SUBDISPLAY_COLOR,COLOR_WHITE,-1) == OK);
	}else{
		int z;

		assert(init_pair(BORDER_COLOR,COLOR_ALUMINIUM,-1) == OK);
		assert(init_pair(HEADER_COLOR,COLOR_MODBLUE,-1) == OK);
		assert(init_pair(FOOTER_COLOR,COLOR_MODYELLOW,-1) == OK);
		assert(init_pair(DBORDER_COLOR,COLOR_ALUMINIUM,-1) == OK);
		assert(init_pair(DHEADING_COLOR,COLOR_MODWHITE,-1) == OK);
		assert(init_pair(UBORDER_COLOR,COLOR_MODCYAN,-1) == OK);
		assert(init_pair(UHEADING_COLOR,COLOR_ORANGE,-1) == OK);
		assert(init_pair(PBORDER_COLOR,COLOR_MODBLUE,-1) == OK);
		assert(init_pair(PHEADING_COLOR,COLOR_MODRED,-1) == OK);
		assert(init_pair(BULKTEXT_COLOR,COLOR_MODWHITE,-1) == OK);
		assert(init_pair(BULKTEXT_ALTROW_COLOR,COLOR_MODWHITE,COLOR_PALEALUMINIUM) == OK);
		assert(init_pair(IFACE_COLOR,COLOR_MODWHITE,-1) == OK);
		assert(init_pair(LCAST_COLOR,COLOR_CHAMELEON,-1) == OK);
		assert(init_pair(UCAST_COLOR,COLOR_MODBLUE,-1) == OK);
		assert(init_pair(MCAST_COLOR,COLOR_SKYBLUE,-1) == OK);
		assert(init_pair(BCAST_COLOR,COLOR_MODVIOLET,-1) == OK);
		assert(init_pair(LSELECTED_COLOR,COLOR_MODWHITE,COLOR_CHAMELEON_50) == OK);
		assert(init_pair(USELECTED_COLOR,COLOR_MODWHITE,COLOR_BLUE_50) == OK);
		assert(init_pair(MSELECTED_COLOR,COLOR_MODWHITE,COLOR_SKYBLUE_50) == OK);
		assert(init_pair(BSELECTED_COLOR,COLOR_MODWHITE,COLOR_VIOLET_50) == OK);
		assert(init_pair(LCAST_L3_COLOR,COLOR_CHAMELEON_75,-1) == OK);
		assert(init_pair(UCAST_L3_COLOR,COLOR_BLUE_75,-1) == OK);
		assert(init_pair(MCAST_L3_COLOR,COLOR_SKYBLUE_75,-1) == OK);
		assert(init_pair(BCAST_L3_COLOR,COLOR_VIOLET_75,-1) == OK);
		assert(init_pair(LCAST_RES_COLOR,COLOR_CHAMELEON_50,-1) == OK);
		assert(init_pair(UCAST_RES_COLOR,COLOR_BLUE_50,-1) == OK);
		assert(init_pair(MCAST_RES_COLOR,COLOR_SKYBLUE_50,-1) == OK);
		assert(init_pair(BCAST_RES_COLOR,COLOR_VIOLET_50,-1) == OK);

		// Disable altrow stuff for now. It's hard to read and ugly.
		assert(init_pair(LCAST_ALTROW_COLOR,COLOR_CHAMELEON,-1) == OK);
		assert(init_pair(UCAST_ALTROW_COLOR,COLOR_MODBLUE,-1) == OK);
		assert(init_pair(MCAST_ALTROW_COLOR,COLOR_SKYBLUE,-1) == OK);
		assert(init_pair(BCAST_ALTROW_COLOR,COLOR_MODVIOLET,-1) == OK);
		assert(init_pair(LCAST_ALTROW_L3_COLOR,COLOR_CHAMELEON_75,-1) == OK);
		assert(init_pair(UCAST_ALTROW_L3_COLOR,COLOR_BLUE_75,-1) == OK);
		assert(init_pair(MCAST_ALTROW_L3_COLOR,COLOR_SKYBLUE_75,-1) == OK);
		assert(init_pair(BCAST_ALTROW_L3_COLOR,COLOR_VIOLET_75,-1) == OK);
		assert(init_pair(LCAST_ALTROW_RES_COLOR,COLOR_CHAMELEON_50,-1) == OK);
		assert(init_pair(UCAST_ALTROW_RES_COLOR,COLOR_BLUE_50,-1) == OK);
		assert(init_pair(MCAST_ALTROW_RES_COLOR,COLOR_SKYBLUE_50,-1) == OK);
		assert(init_pair(BCAST_ALTROW_RES_COLOR,COLOR_VIOLET_50,-1) == OK);
		assert(init_pair(SUBDISPLAY_COLOR,COLOR_MODWHITE,-1) == OK);

		for(z = FIRST_FREE_COLOR ; z < COLORS && z < COLOR_PAIRS ; ++z){
			assert(init_pair(z,z,-1) == OK);
		}
	}
	if(draw_main_window(w)){
		errstr = "Couldn't use ncurses\n";
		goto err;
	}
	if((nim = malloc(sizeof(*nim))) == NULL){
		goto err;
	}
	nim->w = w;
	nim->maintid = pthread_self();
	// Panels aren't yet being used, so we need call refresh() to
	// paint the main window.
	refresh();
	if(pthread_create(&inputtid,NULL,ncurses_input_thread,nim)){
		errstr = "Couldn't create UI thread\n";
		free(nim);
		goto err;
	}
	// FIXME install SIGWINCH() handler...?
	return w;

err:
	mandatory_cleanup(&w);
	fprintf(stderr,"%s",errstr);
	return NULL;
}

static void
packet_callback(omphalos_packet *op){
	pthread_mutex_lock(&bfl); // don't always want screen_update()
	if(packet_cb_locked(op->i,op,&details)){
		if(active){
			assert(top_panel(active->p) != ERR);
		}
		screen_update();
	}
	pthread_mutex_unlock(&bfl);
}

static void *
interface_callback(interface *i,void *unsafe){
	void *r;

	lock_ncurses();
		r = interface_cb_locked(i,unsafe,&details);
	unlock_ncurses();
	return r;
}

static void *
wireless_callback(interface *i,unsigned wcmd __attribute__ ((unused)),void *unsafe){
	void *r;

	lock_ncurses();
		r = interface_cb_locked(i,unsafe,&details);
	unlock_ncurses();
	return r;
}

static void *
service_callback(const interface *i,struct l2host *l2,struct l3host *l3,
				struct l4srv *l4){
	void *ret;

	pthread_mutex_lock(&bfl);
	if( (ret = service_callback_locked(i,l2,l3,l4)) ){
		if(active){
			assert(top_panel(active->p) != ERR);
		}
		screen_update();
	}
	pthread_mutex_unlock(&bfl);
	return ret;
}

static void *
host_callback(const interface *i,struct l2host *l2,struct l3host *l3){
	void *ret;

	pthread_mutex_lock(&bfl);
	if( (ret = host_callback_locked(i,l2,l3)) ){
		if(active){
			assert(top_panel(active->p) != ERR);
		}
		screen_update();
	}
	pthread_mutex_unlock(&bfl);
	return ret;
}

static void *
neighbor_callback(const interface *i,struct l2host *l2){
	void *ret;

	pthread_mutex_lock(&bfl);
	if( (ret = neighbor_callback_locked(i,l2)) ){
		if(active){
			assert(top_panel(active->p) != ERR);
		}
		screen_update();
	}
	pthread_mutex_unlock(&bfl);
	return ret;
}

static void
interface_removed_callback(const interface *i __attribute__ ((unused)),void *unsafe){
	lock_ncurses();
		interface_removed_locked(unsafe,details.p ? &active : NULL);
	unlock_ncurses();
}

static void
vdiag_callback(const char *fmt,va_list v){
	wvstatus(stdscr,fmt,v);
}

static void
network_callback(void){
	lock_ncurses();
		if(active == &network){
			assert(update_network_details(panel_window(network.p)) == OK);
		}
	unlock_ncurses();
}

int main(int argc,char * const *argv){
	const char *codeset;
	omphalos_ctx pctx;

	assert(fwide(stdout,-1) < 0);
	assert(fwide(stderr,-1) < 0);
	if(setlocale(LC_ALL,"") == NULL || ((codeset = nl_langinfo(CODESET)) == NULL)){
		fprintf(stderr,"Couldn't initialize locale (%s?)\n",strerror(errno));
		return EXIT_FAILURE;
	}
	if(strcmp(codeset,"UTF-8")){
		fprintf(stderr,"Only UTF-8 is supported; got %s\n",codeset);
		return EXIT_FAILURE;
	}
	if(uname(&sysuts)){
		fprintf(stderr,"Couldn't get OS info (%s?)\n",strerror(errno));
		return EXIT_FAILURE;
	}
	glibc_version = gnu_get_libc_version();
	glibc_release = gnu_get_libc_release();
	if(omphalos_setup(argc,argv,&pctx)){
		return EXIT_FAILURE;
	}
	pctx.iface.packet_read = packet_callback;
	pctx.iface.iface_event = interface_callback;
	pctx.iface.iface_removed = interface_removed_callback;
	pctx.iface.vdiagnostic = vdiag_callback;
	pctx.iface.wireless_event = wireless_callback;
	pctx.iface.srv_event = service_callback;
	pctx.iface.neigh_event = neighbor_callback;
	pctx.iface.host_event = host_callback;
	pctx.iface.network_event = network_callback;
	if(ncurses_setup() == NULL){
		return EXIT_FAILURE;
	}
	if(omphalos_init(&pctx)){
		int err = errno;

		mandatory_cleanup(&stdscr);
		fprintf(stderr,"Error in omphalos_init() (%s?)\n",strerror(err));
		return EXIT_FAILURE;
	}
	omphalos_cleanup(&pctx);
	if(mandatory_cleanup(&stdscr)){
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
