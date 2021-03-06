#ifndef OMPHALOS_UI_NCURSES_CORE
#define OMPHALOS_UI_NCURSES_CORE

#ifdef __cplusplus
extern "C" {
#endif

#include <limits.h>
#include <stdarg.h>
#include <ncursesw/panel.h>
#include <ncursesw/ncurses.h>
#include <ui/ncurses/iface.h>

struct l2obj;
struct l2host;
struct l3host;
struct reelbox;
struct interface;
struct panel_state;
struct omphalos_packet;

// A box on the display reel. A box might be wholly visible, partially visible  endif
// but missing its bottom portion (bottom of the screen only), partially
// visible but missing its top portion (top of the screen only), wholly
// visible but split across the top/bottom sides, partially visible split across
// the top/bottom, and wholly invisible. A box is never drawn as larger than
// the main screen (ie, it will not appear partial while occupying the entirety
// of the main screen, even if it is logically larger).
//
// We never allow more than one line of blank space at the top of the screen,
// assuming there is data to be displayed. Blank space can never be at the top
// of the screen if there is less than one total screen of information.
//
// scrline is the logical location of the box's topmost line (border with name)
// on the containing pad (reel), taking positive values only. values larger
// than the screen's number of rows are meaningless for comparison purposes;
// ordering is then defined by the next and prev pointers alone (thus a
// possibly large number of offscreen interfaces needn't all be updated
// whenever locations change onscreen).
//
// If the first visible (lowest 'scrline' value) box has a scrline value
// greater than 1, then:
//
//  - if the value is 2 and the screen is full, things are fine.
//  - otherwise, make invisible boxes from the list visible in the blank space.
//  - if there is still space, use any invisible portions of the last box in
//     the list. if there is *still* space...
//  - there is less than a screen's worth of info. move all boxes up.
//
// Then display the boxes until scrline exceeds the number of rows in the
// containing screen, or we reach the end of the list.
//
// Get a reelbox's dimensions by calling getmaxyx() on its subwin member. These
// are the dimensions on the real screen, not the desirable dimensions on an
// infinitely large screen.
typedef struct reelbox {
	int scrline;			// FIXME eliminate this; use getbegy()
	WINDOW *subwin;			// subwin
	PANEL *panel;			// panel
	struct reelbox *next,*prev;	// circular list
	iface_state *is;		// backing interface state
	struct l2obj *selected;		// selected subentry
	int selline;			// line where the selection starts
					//  within the subwindow (if != NULL)
} reelbox;

// FIXME we ought precreate the subwindows, and show/hide them rather than
// creating and destroying them every time.
struct panel_state {
	PANEL *p;
	int ysize;			// number of lines of *text* (not win)
};

// These functions may only be called while the ncurses lock is held. They
// themselves will not attempt to do any locking. Furthermore, they will not
// call screen_update() -- that is the caller's responsibility (in this way,
// multiple operations can be chained without screen updates, for flicker-free
// graphics).
int draw_main_window(WINDOW *);
int setup_statusbar(int);
// FIXME can't use format attribute, see http://gcc.gnu.org/ml/gcc-patches/2001-12/msg01626.html
// __attribute__ ((format (printf,2,3)));
int wstatus_locked(WINDOW *,const char *,...);
int wvstatus_locked(WINDOW *w,const char *,va_list);
struct l4obj *service_callback_locked(const struct interface *,struct l2host *,
					struct l3host *,struct l4srv *);
struct l3obj *host_callback_locked(const struct interface *,struct l2host *,
					struct l3host *);
struct l2obj *neighbor_callback_locked(const struct interface *,struct l2host *);
void interface_removed_locked(iface_state *,struct panel_state **);
void *interface_cb_locked(struct interface *,iface_state *,struct panel_state *);
int packet_cb_locked(const struct interface *,struct omphalos_packet *,struct panel_state *);
void toggle_promisc_locked(WINDOW *w);
void sniff_interface_locked(WINDOW *w);
void down_interface_locked(WINDOW *w);
void resolve_selection(WINDOW *);
void reset_current_interface_stats(WINDOW *);
void use_next_iface_locked(WINDOW *,struct panel_state *);
void use_prev_iface_locked(WINDOW *,struct panel_state *);

// Actions on the current interface
void use_next_node_locked(void);
void use_prev_node_locked(void);
void use_next_nodepage_locked(void);
void use_prev_nodepage_locked(void);
void use_first_node_locked(void);
void use_last_node_locked(void);
int expand_iface_locked(void);
int collapse_iface_locked(void);
// Select the current interface for host-granularity browsing (up and down now
// move within the interface rather than among interfaces).
int select_iface_locked(void);
// Go back to interface-granularity browsing.
int deselect_iface_locked(void);

// Subpanels
int update_diags_locked(struct panel_state *);
void hide_panel_locked(struct panel_state *ps);
int display_env_locked(WINDOW *,struct panel_state *);
int display_help_locked(WINDOW *,struct panel_state *);
int display_diags_locked(WINDOW *,struct panel_state *);
int display_details_locked(WINDOW *,struct panel_state *);
int new_display_panel(WINDOW *,struct panel_state *,int,int,const wchar_t *);

void check_consistency(void); // Debugging -- all assert()s

#ifdef __cplusplus
}
#endif

#endif
