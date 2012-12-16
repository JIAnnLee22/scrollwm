/*******************************************************************\
* SCROLLWM - a floating WM with a single large scrollable workspace
*
* Author: Jesse McClure, copyright 2012
* License: GPLv3
*
* Based on code from TinyWM and TTWM
* TinyWM is written by Nick Welch <mack@incise.org>, 2005.
* TTWM is written by Jesse McClure, 2011-2012
\*******************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>

#define MAX(a, b) ((a) > (b) ? (a) : (b))

enum {Background, Default, Hidden, Normal, Sticky, Urgent, Title, TagList, LASTColor };
enum {MOff, MWMove, MWResize, MDMove, MDResize };

typedef struct {
	unsigned int mod;
	KeySym keysym;
	void (*func)(const char *);
	const char *arg;
} Key;

typedef struct {
	unsigned int mod, button;
	void (*func)(const char *);
	const char *arg;
} Button;

typedef struct Client Client;
struct Client {
	char *title;
	int tlen;
	int x, y;
	float w, h;
	int tags;
	Client *next;
	Window win;
};

typedef struct Checkpoint Checkpoint;
struct Checkpoint {
	int x,y;
	float zoom;
	char key;
	Checkpoint *next;
};

static void buttonpress(XEvent *);
static void buttonrelease(XEvent *);
static void destroynotify(XEvent *);
static void enternotify(XEvent *);
static void expose(XEvent *);
static void keypress(XEvent *);
static void maprequest(XEvent *);
static void motionnotify(XEvent *);
static void propertynotify(XEvent *);
static void unmapnotify(XEvent *);

static void animate(int,int);
static void animatefocus();
static void checkpoint(const char *);
static void checkpoint_set(const char *);
static void checkpoint_update(int,int,float);
static void cycle(const char *);
static void cycle_tile(const char *);
static void desktop(const char *);
static void draw(Client *);
static void focusclient(Client *);
static void killclient(const char *);
static void monocle(const char *);
static Bool onscreen(Client *);
static void quit(const char *);
static void scrollwindows(Client *,int,int);
static void spawn(const char *);
static void tag(const char *);
static void tagconfig(const char *);
static void tile_one(Client *);
static void tile(const char *);
static void toggletag(const char *);
static void unmanage(Client *);
static void window(const char *);
static Client *wintoclient(Window);
static void zoomwindow(Client *,float,int,int);
static void zoom(Client *,float,int,int);

static const int max_status_line = 512;
#include "config.h"

static Display * dpy;
static Window root, bar;
static Pixmap buf, sbar;
static int scr, sw, sh;
static GC gc;
static Colormap cmap;
static XFontStruct *fontstruct;
static int fontheight, barheight;
static XWindowAttributes attr;
static XButtonEvent start;
static int mousemode;
static Client *clients=NULL;
static Client *focused;
static Checkpoint *checks=NULL;
static Bool running = True;
static int tags_stik = 0, tags_hide = 0, tags_urg = 0;
static int curtag = 0;
static int ntilemode = 0;
static int statuswidth = 0;
static FILE *inpipe;
static void (*handler[LASTEvent]) (XEvent *) = {
	[ButtonPress]		= buttonpress,
	[ButtonRelease]		= buttonrelease,
	[DestroyNotify]		= destroynotify,
	[EnterNotify]		= enternotify,
	[Expose]			= expose,
	[KeyPress]			= keypress,
	[MapRequest]		= maprequest,
	[PropertyNotify]	= propertynotify,
	[MotionNotify]		= motionnotify,
	[UnmapNotify]		= unmapnotify,
};

void animate(int tx,int ty) {
	if (!animations) {
		scrollwindows(clients,tx,ty);
		return;
	}
	int dx = (tx == 0 ? 0 : (tx > 0 ? animatespeed+1 : -(animatespeed+1)));
	int dy = (ty == 0 ? 0 : (ty > 0 ? animatespeed+1 : -(animatespeed+1)));
	while (abs(tx) > animatespeed || abs(ty) > animatespeed) {
		scrollwindows(clients,dx,dy);
		tx -= dx; ty -= dy;
		if (abs(tx) < animatespeed+1) dx = 0;
		if (abs(ty) < animatespeed+1) dy = 0;
	}
	scrollwindows(clients,tx,ty);
}

void animatefocus() {
	if ( !animations || !focused || !scrolltofocused || onscreen(focused)) return;
	int tx=-focused->x+tilegap, ty=-focused->y+(showbar?barheight:0)+tilegap;
	animate(tx,ty);
}

void buttonpress(XEvent *e) {
	XButtonEvent *ev = &e->xbutton;
	Client *c;
	if ((c=wintoclient(ev->subwindow))) focused = c;
	if (!(ev->state || focused)) return;
	int i;
	start = *ev;
	for (i = 0; i < sizeof(buttons)/sizeof(buttons[0]); i++)
		if ( (ev->button == buttons[i].button) && buttons[i].func &&
				buttons[i].mod == ((ev->state&~Mod2Mask)&~LockMask) )
			buttons[i].func(buttons[i].arg);
	if (c) focusclient(c);
	if (mousemode != MOff)
		XGrabPointer(dpy,root,True,PointerMotionMask | ButtonReleaseMask,
			GrabModeAsync,GrabModeAsync, None, None, CurrentTime);
}

void buttonrelease(XEvent *e) {
	XUngrabPointer(dpy, CurrentTime);
	mousemode = MOff;
}

static char checkpoint_helper(const char *arg) {
	if (arg == NULL) {
		XGrabKeyboard(dpy,root,True,GrabModeAsync,GrabModeAsync,CurrentTime);
		XEvent e;
		while (!XCheckTypedEvent(dpy,KeyPress,&e));
		XKeyEvent *ev = &e.xkey;
		char *cs = XKeysymToString(XkbKeycodeToKeysym(dpy,(KeyCode)ev->keycode,0,0));
		XUngrabKeyboard(dpy,CurrentTime);
		if (cs) return cs[0];
		else return '0';
	}
	else {
		return arg[0];
	}
}

void checkpoint_init() {
	int i;
	Checkpoint *cp;
	for (i = 0; i < 6; i++) {
		cp = (Checkpoint *) calloc(1,sizeof(Checkpoint));
		cp->zoom = 1.0;
		cp->key =  48 + i;
		cp->y = sh*(i-1);
		cp->next = checks;
		checks = cp;
	}
	checks->y=0; /* checkpoint 0 and 1 are initialy equivalent */
}

void checkpoint(const char *arg) {
	char key = checkpoint_helper(arg);
	Checkpoint *cp;
	Client *prev = focused;
	for (cp = checks; cp; cp = cp->next) {
		if (cp->key == key) {
			animate(-cp->x,-cp->y);
			zoom(clients,1/cp->zoom,0,0);
			focused = clients;
			while (focused && !(onscreen(focused) || (focused->tags & tags_hide)) )
				focused=focused->next;
			if (focused) focusclient(focused);
			else focused=prev;
			draw(clients);
			return;
		}
	}
}

void checkpoint_set(const char *arg) {
	char key = checkpoint_helper(arg);
	if (key == '0') return; /* never reset checkpoint zero */
	Checkpoint *cp;
	for (cp = checks; cp; cp = cp->next)
		if (cp->key == key) {
			cp->x = 0;
			cp->y = 0;
			cp->zoom = 1.0;
			return;
		}
	cp = (Checkpoint *) calloc(1,sizeof(Checkpoint));
	cp->next = checks;
	cp->zoom = 1.0;
	cp->key = key;
	checks = cp;
}

void checkpoint_update(int x, int y, float zoom) {
	Checkpoint *cp;
	for (cp = checks; cp; cp = cp->next) {
		if (zoom != 1) {
			cp->zoom*=zoom;
			cp->x = (cp->x-x)*zoom + x;
			cp->y = (cp->y-y)*zoom + y;
		}
		else {
			cp->x+=x;
			cp->y+=y;
		}
	}
}

void cycle(const char *arg) {
	if (!focused) return;
	Client *prev = focused;
	if (arg[0] == 'a') {
		focused = focused->next;
		if (!focused) focused = clients;
	}
	else if (arg[0] == 'v') {
		while ( (focused=focused->next) && (focused->tags & tags_hide) );
		if (!focused) {
			focused = clients;
			if (clients && (clients->tags & tags_hide) )
				while ((focused=focused->next)->tags & tags_hide );
		}
	}
	else if (arg[0] == 's') {
		while (focused && (!onscreen(focused=focused->next) || (focused->tags & tags_hide)) );
		if (!focused) {
			if ( !onscreen(focused=clients) || (focused->tags & tags_hide) )
				while (focused && (!onscreen(focused=focused->next) || (focused->tags & tags_hide)) );
		}
	}
	else if (arg[0] == 't') {
		while ( (focused=focused->next) && !(focused->tags & prev->tags) );
		if (!focused) {
			focused = clients;
			if ( clients && !(clients->tags & prev->tags) )
				while ( (focused=focused->next) && !(focused->tags & prev->tags) );
		}
	}	
	if (!focused) focused = prev;
	focusclient(focused);
	animatefocus();
	draw(clients);
}

void cycle_tile(const char *arg) {
	if (!tile_modes[++ntilemode]) ntilemode=0;
	tile(tile_modes[ntilemode]);
}

void destroynotify(XEvent *e) {
	Client *c;
	if (!(c=wintoclient(e->xunmap.window))) return;
	if (!e->xunmap.send_event) unmanage(c);
}

void desktop(const char *arg) {
	if (arg[0] == 'm') mousemode = MDMove;
	else if (arg[0] == 'r') mousemode = MDResize;
	else if (arg[0] == 'g') zoom(clients,1.1,start.x_root,start.y_root);
	else if (arg[0] == 's') zoom(clients,.92,start.x_root,start.y_root);
}

void draw(Client *stack) {
	if (focused) tags_urg &= ~focused->tags;
	tags_urg &= ~(1<<curtag);
	/* WINDOWS */
	XColor color;
	int tags_occ = 0;
	int loc[9] = {0,0,0,0,0,0,0,0,0}, cx,cy;
	XSetWindowAttributes wa;
	while (stack) {
		cx = stack->x + stack->w/2;
		cy = stack->y + stack->h/2;
		loc[(cx<0?0:(cx<sw?1:2))*3 + (cy<0?0:(cy<sh?1:2))]++;
		tags_occ |= stack->tags;
		if (stack->tags & tags_hide) {
			XMoveWindow(dpy,stack->win,sw+2,0);
			stack = stack->next;
			continue;
		}
		XMoveResizeWindow(dpy,stack->win,stack->x,stack->y,
			MAX(stack->w,win_min),MAX(stack->h,win_min));
		XAllocNamedColor(dpy,cmap,
			colors[	(highlightfocused && stack == focused ? Hidden : 
					(stack->tags & tags_stik ? Sticky : Normal))],&color,&color);
		wa.border_pixel = color.pixel;
		XChangeWindowAttributes(dpy,stack->win,CWBorderPixel,&wa);
		stack = stack->next;
	}
	/* STATUS BAR */
	XAllocNamedColor(dpy,cmap,colors[Background],&color,&color);
	XSetForeground(dpy,gc,color.pixel);
	XFillRectangle(dpy,buf,gc,0,0,sw,barheight);
	/* tags */
	int i, x=10,w=0;
	int col;
	for (i = 0; tag_name[i]; i++) {
		if (!(tags_occ & (1<<i)) && curtag != i) continue;
		col = (tags_urg & (1<<i) ? Urgent :
				(tags_hide & (1<<i) ? Hidden :
				(tags_stik & (1<<i) ? Sticky : 
				(tags_occ  & (1<<i) ? Normal : Default ))));
		XAllocNamedColor(dpy,cmap,colors[col],&color,&color);
		XSetForeground(dpy,gc,color.pixel);
		XDrawString(dpy,buf,gc,x,fontheight,tag_name[i],strlen(tag_name[i]));
		w = XTextWidth(fontstruct,tag_name[i],strlen(tag_name[i]));
		if (curtag == i)
			XFillRectangle(dpy,buf,gc,x-2,fontheight+1,w+4,barheight-fontheight);
		x+=w+10;
	}
	/* overview "icon" */
	if (clients) {
		x = MAX(x+20,sw/10);
		XAllocNamedColor(dpy,cmap,colors[Default],&color,&color);
		XSetForeground(dpy,gc,color.pixel);
		XDrawRectangle(dpy,buf,gc,x,fontheight-9,6,6);
		XDrawRectangle(dpy,buf,gc,x,fontheight-6,6,6);
		XDrawRectangle(dpy,buf,gc,x+3,fontheight-9,6,6);
		XDrawRectangle(dpy,buf,gc,x+3,fontheight-6,6,6);
		XAllocNamedColor(dpy,cmap,colors[Hidden],&color,&color);
		XSetForeground(dpy,gc,color.pixel);
		for (i = 0; i < 3; i++) for (w = 0; w < 3; w++) if (loc[i*3+w])
		XFillRectangle(dpy,buf,gc,x+3*i,fontheight-9+3*w,4,4);
		x+=20;
	}
	/* title */
	if (focused) {
		XAllocNamedColor(dpy,cmap,colors[Title],&color,&color);
		XSetForeground(dpy,gc,color.pixel);
		if (focused->title) {
			XDrawString(dpy,buf,gc,x,fontheight,focused->title,strlen(focused->title));
			x += XTextWidth(fontstruct,focused->title,strlen(focused->title)) + 10;
		}
		else {
			XDrawString(dpy,buf,gc,x,fontheight,"UNNAMED",7);
			x += XTextWidth(fontstruct,"UNNAMED",7) + 10;
		}
		XAllocNamedColor(dpy,cmap,colors[TagList],&color,&color);
		XSetForeground(dpy,gc,color.pixel);
		XDrawString(dpy,buf,gc,x,fontheight,"[",1);
		x += XTextWidth(fontstruct,"[",1);
		/* tag list */
		for (i = 0; tag_name[i]; i++) if (focused->tags & (1<<i)) {
			XDrawString(dpy,buf,gc,x,fontheight,tag_name[i],strlen(tag_name[i]));
			x += XTextWidth(fontstruct,tag_name[i],strlen(tag_name[i]));
			XDrawString(dpy,buf,gc,x,fontheight,", ",2);
			w = XTextWidth(fontstruct,", ",2);
			x += w;
		}
		x -= w;
		XAllocNamedColor(dpy,cmap,colors[Background],&color,&color);
		XSetForeground(dpy,gc,color.pixel);
		XFillRectangle(dpy,buf,gc,x,0,10,barheight);
		XAllocNamedColor(dpy,cmap,colors[TagList],&color,&color);
		XSetForeground(dpy,gc,color.pixel);
		XDrawString(dpy,buf,gc,x,fontheight,"]",1);
	}
	/* USER STATUS INFO */
	if (statuswidth)
		XCopyArea(dpy,sbar,buf,gc,0,0,statuswidth,barheight,sw-statuswidth,0);
	XCopyArea(dpy,buf,bar,gc,0,0,sw,barheight,0,0);
	XRaiseWindow(dpy,bar);
	XFlush(dpy);
}

void enternotify(XEvent *e) {
	if (!focusfollowmouse) return;
	Client *c = wintoclient(e->xcrossing.window);
	if (c) {
		focusclient(c);
		draw(clients);
	}
}

void expose(XEvent *e) {
	draw(clients);
}

void focusclient(Client *c) {
	focused = c;
	if (!c) return;
	XSetInputFocus(dpy,c->win,RevertToPointerRoot,CurrentTime);
	XRaiseWindow(dpy,c->win);
	XRaiseWindow(dpy,bar);
}

void keypress(XEvent *e) {
	unsigned int i;
	XKeyEvent *ev = &e->xkey;
	KeySym keysym = XkbKeycodeToKeysym(dpy,(KeyCode)ev->keycode,0,0);
	for (i = 0; i < sizeof(keys)/sizeof(keys[0]); i++)
		if ( (keysym==keys[i].keysym) && keys[i].func &&
				keys[i].mod == ((ev->state&~Mod2Mask)&~LockMask) )
			keys[i].func(keys[i].arg);
}

void killclient(const char *arg) {
        if (!focused) return;
        XEvent ev;
        ev.type = ClientMessage;
        ev.xclient.window = focused->win;
        ev.xclient.message_type = XInternAtom(dpy, "WM_PROTOCOLS", True);
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = XInternAtom(dpy, "WM_DELETE_WINDOW", True);
        ev.xclient.data.l[1] = CurrentTime;
        XSendEvent(dpy,focused->win,False,NoEventMask,&ev);
}

void maprequest(XEvent *e) {
	Client *c;
	static XWindowAttributes wa;
	XMapRequestEvent *ev = &e->xmaprequest;
	if (!XGetWindowAttributes(dpy, ev->window, &wa)) return;
	if (wa.override_redirect) return;
	if (!wintoclient(ev->window)) {
		if (!(c=calloc(1,sizeof(Client)))) exit(1);
		c->win = ev->window;
		XGetWindowAttributes(dpy,c->win, &attr);
		c->x = attr.x; c->y = attr.y;
		c->w = attr.width; c->h = attr.height;
		if (c->y < (topbar ? barheight : 0) +tilegap && showbar) {
			c->y = (topbar ? barheight : 0) + tilegap;
			c->x = tilegap;
		}
		c->tags = (1<<curtag);
		if (XFetchName(dpy,c->win,&c->title)) c->tlen = strlen(c->title);
		XSelectInput(dpy,c->win,PropertyChangeMask | EnterWindowMask);
		c->next = clients;
		clients = c;
		XSetWindowBorderWidth(dpy,c->win,borderwidth);
		XMapWindow(dpy,c->win);
		focusclient(c);
	}
	draw(clients);
}

void monocle(const char *arg) {
	if (focused) tile_one(focused);
	draw(clients);
}

void motionnotify(XEvent *e) {
	int xdiff, ydiff;
	while(XCheckTypedEvent(dpy,MotionNotify,e));
	xdiff = e->xbutton.x_root - start.x_root;
	ydiff = e->xbutton.y_root - start.y_root;
	if (mousemode == MWMove) {
		focused->x+=xdiff; focused->y+=ydiff; draw(clients);
	}
	else if (mousemode == MWResize) {
		focused->w+=xdiff; focused->h+=ydiff; draw(clients);
	}
	else if (mousemode == MDMove) {
		scrollwindows(clients,xdiff,ydiff);
	}
	start.x_root+=xdiff; start.y_root+=ydiff;
}

Bool onscreen(Client *c) {
	if (!c) return False;
	if ((c->x + c->w/2) > 0 && (c->x + c->w/2) < sw	&&
		(c->y + c->h/2) > 0 && (c->y + c->h/2) < sh )
		return True;
	return False;
}

void propertynotify(XEvent *e) {
    XPropertyEvent *ev = &e->xproperty;
    Client *c;
    if ( !(c=wintoclient(ev->window)) ) return;
    if (ev->atom == XA_WM_NAME) {
        XFree(c->title);
        c->title = NULL;
        c->tlen = 0;
        if (XFetchName(dpy,c->win,&c->title)) c->tlen = strlen(c->title);
        draw(clients);
    }
    else if (ev->atom == XA_WM_HINTS) {
        XWMHints *hint;
        if ( (hint=XGetWMHints(dpy,c->win)) && (hint->flags & XUrgencyHint) )
			tags_urg |= c->tags;
        draw(clients);
    }
}

void quit(const char *arg) {
	running = False;
}

void scrollwindows(Client *stack, int x, int y) {
	while (stack) {
		if (!(stack->tags & tags_stik)) {
			stack->x+=x;
			stack->y+=y;
		}
		stack = stack->next;
	}
	checkpoint_update(x,y,1);
	draw(clients);
}

void spawn(const char *arg) {
	system(arg);
}

void status(char *msg) {
	char *col = (char *) calloc(8,sizeof(char));
	char *t,*c = msg;
	int l;
	XColor color;
	statuswidth = 0;
	XAllocNamedColor(dpy,cmap,colors[Background],&color,&color);
	XSetForeground(dpy,gc,color.pixel);
	XFillRectangle(dpy,sbar,gc,0,0,sw/2,barheight);
	XAllocNamedColor(dpy,cmap,colors[Default],&color,&color);
	XSetForeground(dpy,gc,color.pixel);
	while(*c != '\n') {
		if (*c == '{') {
			if (*(++c) == '#') {
				strncpy(col,c,7);
				XAllocNamedColor(dpy,cmap,col,&color,&color);
				XSetForeground(dpy,gc,color.pixel);
			}
			c = strchr(c,'}')+1;
		}
		else {
			if ((t=strchr(c,'{'))==NULL) t=strchr(c,'\n');
			l = (t == NULL ? 0 : t-c);
			if (l) {
				XDrawString(dpy,sbar,gc,statuswidth,fontheight,c,l);
				statuswidth+=XTextWidth(fontstruct,c,l);
			}
			c+=l;
		}
	}
	free(col);
	draw(clients);
}

void tag(const char *arg) {
	curtag = arg[0] - 49;
	tags_urg &= ~(1<<curtag);
	Client *c, *t=NULL;
	if (clients) {
		for (c = clients; c; c = c->next) if (c->tags & (1<<curtag)) {
			if (!t) t = c;
			XRaiseWindow(dpy,c->win);
		}
		if (! (focused->tags & (1<<curtag)) && t ) focusclient(t);
	}
	draw(clients);
}

void tagconfig(const char *arg) {
	int i;
	if (arg[0] == 'h') tags_hide |= (1<<curtag);
	else if (arg[0] == 's') tags_stik |= (1<<curtag);
	else if (arg[0] == 'n') {
		tags_stik &= ~(1<<curtag);
		tags_hide &= ~(1<<curtag);
	}
	else if (arg[0] == 't')
		showbar = !showbar;
	else if (arg[0] == 'm')
		topbar = !topbar;
	else if (arg[0] == 'o') for (i = 0; tag_name[i]; i++) {
		if (i != curtag) tags_hide |= (1<<i);
		else tags_hide &= ~(1<<i);
	}
	if (showbar) XMoveWindow(dpy,bar,0,(topbar ? 0 : sh - barheight));
	else XMoveWindow(dpy,bar,0,(topbar ? -barheight: sh));
	draw(clients);
}

void tile_one(Client *stack) {
	stack->x = tilegap;
	stack->y = (showbar && topbar ? barheight : 0) + tilegap;
	stack->w = sw - 2*(tilegap + borderwidth);
	stack->h = sh - (showbar ? barheight: 0) - 2*(tilegap + borderwidth);
}

void tile_bstack(Client *stack, int count) {
	int w = (sw - tilegap)/(count-1);
	int h = (sh - (showbar && topbar ? barheight : 0) - tilegap)/2 - (tilegap + 2*borderwidth);
	stack->x = tilegap;
	stack->y = (showbar && topbar ? barheight : 0) + tilegap;
	stack->w = sw - 2*(tilegap + borderwidth);
	stack->h = h + tilebias;
	int i=0;
	while ((stack=stack->next)) {
		stack->x = tilegap + i*w;
		stack->y = (showbar && topbar ? barheight : 0) + h + 2*(tilegap+borderwidth) + tilebias;
		stack->w = MAX(w - tilegap - 2*borderwidth,win_min);
		stack->h = h - tilebias;
		i++;
		if (!stack->next) stack->w = MAX(sw - stack->x - tilegap - 2*borderwidth,win_min);
	}
}

void tile_flow(Client *stack, int count) {
	int x = 0;
	while (stack) {
		stack->x = tilegap + sw*(x++);
		stack->y = (showbar && topbar ? barheight : 0) + tilegap;
		stack->w = sw - 2*(tilegap + borderwidth);
		stack->h = sh - (showbar ? barheight: 0) - 2*(tilegap + borderwidth);
		stack = stack->next;
	}
}

void tile_rstack(Client *stack, int count) {
	int w = (sw - tilegap)/2 - (tilegap + 2*borderwidth);
	int h = (sh - (showbar && topbar ? barheight : 0) - tilegap)/(count-1);
	stack->x = tilegap;
	stack->y = (showbar && topbar ? barheight : 0) + tilegap;
	stack->w = w + tilebias;
	stack->h = sh - (showbar ? barheight: 0) - 2*(tilegap + borderwidth);
	int i=0;
	while ((stack=stack->next)) {
		stack->x = w + 2*(tilegap+borderwidth) + tilebias;
		stack->y = (showbar && topbar ? barheight : 0) + tilegap + i*h;
		stack->w = w - tilebias;
		stack->h = MAX(h - tilegap - 2*borderwidth,win_min);
		i++;
		if (!stack->next)
			stack->h = MAX(sh - (topbar ? 0: barheight) - stack->y - tilegap - 2*borderwidth,win_min);
	}
}

void tile_ttwm(Client *stack, int count) {
	int w = (sw - tilegap)/2 - (tilegap + 2*borderwidth);
	stack->x = tilegap;
	stack->y = (showbar && topbar ? barheight : 0) + tilegap;
	stack->w = w + tilebias;
	stack->h = sh - (showbar ? barheight: 0) - 2*(tilegap + borderwidth);
	int i=0;
	XRaiseWindow(dpy,stack->next->win);
	while ((stack=stack->next)) {
		stack->x = w + 2*(tilegap+borderwidth) + tilebias;
		stack->y = (showbar && topbar ? barheight : 0) + tilegap;
		stack->w = w - tilebias;
		stack->h = sh - (showbar ? barheight: 0) - 2*(tilegap + borderwidth);
		i++;
	}
}

void tile(const char *arg) {
/* this is a very plain tiling function, just as a placeholder for now */
	int i;
	Client *c;
	for (c = clients, i = 0; c; c = c->next, i++);
	if (i == 0) return;
	else if (i == 1) { tile_one(clients); return; }
	if (arg[0] == 't') tile_ttwm(clients,i);
	else if (arg[0] == 'r') tile_rstack(clients,i);
	else if (arg[0] == 'b') tile_bstack(clients,i);
	else if (arg[0] == 'f') tile_flow(clients,i);
	else if (arg[0] == 'i') tilebias += 2;
	else if (arg[0] == 'd') tilebias -= 2;
	draw(clients);
}

void toggletag(const char *arg) {
	if (!focused) return;
	int t = arg[0] - 49;
	focused->tags = focused->tags ^ (1<<t);
	draw(clients);
}

void unmanage(Client *c) {
	Client *t;
	if (c == focused) focusclient(c->next);
	if (c == clients) clients = c->next;
	else {
		for (t = clients; t && t->next != c; t = t->next);
		t->next = c->next;
	}
	XFree(c->title);
	free(c);
	c = NULL;
	if (!focused) if ( (focused=clients) ) cycle("screen");
	draw(clients);
}

void unmapnotify(XEvent *e) {
	Client *c;
	if (!(c=wintoclient(e->xunmap.window))) return;
	if (!e->xunmap.send_event) unmanage(c);
}

void window(const char *arg) {
	if (arg[0] == 'm') mousemode = MWMove;
	else if (arg[0] == 'r') mousemode = MWResize;
	else if (arg[0] == 'g') zoomwindow(focused,1.1,start.x_root,start.y_root);
	else if (arg[0] == 's') zoomwindow(focused,.92,start.x_root,start.y_root);
	else if (arg[0] == 'z') {
		focused->x=-2; focused->w=sw;
		focused->y=(showbar && topbar ? barheight-2 : -2);
		focused->h=(showbar ? sh-barheight : sh+4);
	}
}

Client *wintoclient(Window w) {
	Client *c;
	for (c = clients; c && c->win != w; c = c->next);
	if (c) return c;
	else return NULL;
}

void zoomwindow(Client *c, float factor, int x, int y) {
	c->w *= factor; c->h *= factor;
	if (c->w < zoom_min) c->w = zoom_min;
	if (c->h < zoom_min) c->h = zoom_min;
	c->x = (c->x-x) * factor + x;
	c->y = (c->y-y) * factor + y;
}


void zoom(Client *stack, float factor, int x, int y) {
	while (stack) {
		if (!(stack->tags & tags_stik)) zoomwindow(stack,factor,x,y);
		stack = stack->next;
	}
	checkpoint_update(x,y,factor);
	draw(clients);
}

int xerror(Display *d, XErrorEvent *ev) {
	char msg[1024];
	XGetErrorText(dpy,ev->error_code,msg,sizeof(msg));
	fprintf(stderr,"====== SCROLLWM ERROR =====\nrequest=%d error=%d\n%s\n===========================\n",
		ev->request_code,ev->error_code,msg);
	return 0;
}


int main(int argc, const char **argv) {
	if (argc > 1) inpipe = popen(argv[1],"r");
	else inpipe = stdin;
	/* init X */
    if(!(dpy = XOpenDisplay(0x0))) return 1;
	scr = DefaultScreen(dpy);
	sw = DisplayWidth(dpy,scr);
	sh = DisplayHeight(dpy,scr);
    root = DefaultRootWindow(dpy);
	XSetErrorHandler(xerror);
	XDefineCursor(dpy,root,XCreateFontCursor(dpy,scrollwm_cursor));
	/* gc init */
	cmap = DefaultColormap(dpy,scr);
	XGCValues val;
	val.font = XLoadFont(dpy,font);
	fontstruct = XQueryFont(dpy,val.font);
	fontheight = fontstruct->ascent+1;
	barheight = fontstruct->ascent+fontstruct->descent+2;
	gc = XCreateGC(dpy,root,GCFont,&val);
	/* buffers and windows */
	bar = XCreateSimpleWindow(dpy,root,0,0,sw,barheight,0,0,0);
	buf = XCreatePixmap(dpy,root,sw,barheight,DefaultDepth(dpy,scr));
	sbar = XCreatePixmap(dpy,root,sw/2,barheight,DefaultDepth(dpy,scr));
	XSetWindowAttributes wa;
	wa.override_redirect = True;
	wa.event_mask = ExposureMask;
	XChangeWindowAttributes(dpy,bar,CWOverrideRedirect|CWEventMask,&wa);
	XMapWindow(dpy,bar);
	wa.event_mask = FocusChangeMask | SubstructureNotifyMask | ButtonPressMask |
			ButtonReleaseMask | PropertyChangeMask | SubstructureRedirectMask |
			StructureNotifyMask;
	XChangeWindowAttributes(dpy,root,CWEventMask,&wa);
	XSelectInput(dpy,root,wa.event_mask);
	/* checkpoint init */
	checkpoint_init();
	/* key and mouse binding */
	unsigned int mods[] = {0, LockMask, Mod2Mask, LockMask|Mod2Mask};
	KeyCode code;
	XUngrabKey(dpy,AnyKey,AnyModifier,root);
	int i,j;
	for (i = 0; i < sizeof(keys)/sizeof(keys[0]); i++)
		if ( (code=XKeysymToKeycode(dpy,keys[i].keysym)) ) for (j = 0; j < 4; j++)
			XGrabKey(dpy,code,keys[i].mod|mods[j],root,True,
				GrabModeAsync,GrabModeAsync);
	for (i = 0; i < sizeof(buttons)/sizeof(buttons[0]); i++) for (j = 0; j < 4; j++)
		if (buttons[i].mod)
	    XGrabButton(dpy,buttons[i].button,buttons[i].mod,root,True,ButtonPressMask,
			GrabModeAsync,GrabModeAsync,None,None);
	/* main loop */
	draw(clients);
    XEvent ev;
	int xfd, sfd;
	fd_set fds;
	sfd = fileno(inpipe);
	xfd = ConnectionNumber(dpy);
	char *line = (char *) calloc(max_status_line+1,sizeof(char));
	while (running) {
		FD_ZERO(&fds);
		FD_SET(sfd,&fds);
		FD_SET(xfd,&fds);
		select(xfd+1,&fds,0,0,NULL);
		if (FD_ISSET(xfd,&fds)) while (XPending(dpy)) {
			XNextEvent(dpy,&ev);
			if (handler[ev.type]) handler[ev.type](&ev);
		}
		if (FD_ISSET(sfd,&fds)) {
			if (fgets(line,max_status_line,inpipe))
				status(line);
		}
	}
	/* clean up */
	Checkpoint *cp = checks;
	while (checks) {
		cp = checks;
		checks = checks->next;
		free(cp);
	}
	free(line);
	XFreeFontInfo(NULL,fontstruct,1);
	XUnloadFont(dpy,val.font);
	return 0;
}

// vim: ts=4

