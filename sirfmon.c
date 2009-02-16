/* $Id$ */
/*
 * GPS packet monitor, originally by Rob Janssen, PE1CHL.
 * Heavily hacked by Eric S. Raymond for use with the gpsd project.
 *
 * Useful commands:
 *	l -- toggle packet logging
 *	b -- change baud rate.
 *	s -- send hex bytes to device.
 *	q -- quit, leaving device in binary mode.
 *      Ctrl-S -- freeze display.
 *      Ctrl-Q -- unfreeze display.
 *
 * There may be chipset-specific commands as well.
 */
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#ifndef S_SPLINT_S
#include <unistd.h>
#endif /* S_SPLINT_S */
#include <assert.h>
#include <signal.h>
#include <setjmp.h>
/* Cygwin has only _timezone and not timezone unless the following is set */
#if defined(__CYGWIN__)
#define timezonevar
#endif /* defined(__CYGWIN__) */
#include <time.h>
#include <termios.h>
#include <fcntl.h>	/* for O_RDWR */
#include <stdarg.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/ioctl.h>	/* for O_RDWR */

#include "gpsd_config.h"
#ifdef HAVE_NCURSES_H
#include <ncurses.h>
#else
#include <curses.h>
#endif /* HAVE_NCURSES_H */
#include "gpsd.h"

#include "bits.h"

#if defined(HAVE_SYS_TIME_H)
#include <sys/time.h>
#endif
#if defined (HAVE_SYS_SELECT_H)
#include <sys/select.h>
#endif

#ifdef S_SPLINT_S
extern struct tm *localtime_r(const time_t *,/*@out@*/struct tm *tp)/*@modifies tp@*/;
#endif /* S_SPLINT_S */

extern int netlib_connectsock(const char *, const char *, const char *);

#define BUFLEN		2048

static int controlfd = -1;
static int gmt_offset;
static bool serial, curses_active;
static int debuglevel = 0;

static struct gps_context_t	context;
static struct gps_device_t	session;

static WINDOW *statwin, *cmdwin, *debugwin;
static FILE *logfile;

#define display	(void)mvwprintw

/* these things will go in gpsmon.h someday */

#define COMMAND_TERMINATE	-1
#define COMMAND_MATCH		1
#define COMMAND_UNKNOWN		0

struct mdevice_t {
    /* capability table for device monitoring */
    void (*probe)(void);
    void (*analyze)(unsigned char [], size_t);
    bool (*windows)(void);
    void (*repaint)(bool);
    int (*command)(char[]);
    int min_y, min_x;
    const struct gps_type_t *driver;
};

// SiRF-specific code will need this.
static bool monitor_control_send(/*@in@*/unsigned char *buf, size_t len);

/*****************************************************************************
 *****************************************************************************
 *
 * SiRF-dependent stuff begins here
 *
 * These are the SiRF-specific commands:
 *	a -- toggle receipt of 50BPS subframe data.
 *	c -- set or clear static navigation mode
 *	n -- switch device to NMEA at current speed and exit.
 *	t -- toggle navigation-parameter display mode
 *
 * Initialiation takes a SiRF chip in NMEA mode to binary mode, if needed.
 *
 *****************************************************************************
 *****************************************************************************/

extern const struct gps_type_t sirf_binary;

static WINDOW *mid2win, *mid4win, *mid6win, *mid7win, *mid9win, *mid13win;
static WINDOW *mid19win, *mid27win;
static bool dispmode = false, subframe_enabled = false;;
static int nfix,fix[20];

/*@ -nullassign @*/
static char *verbpat[] =
{
    "#Time:",
    "@R Time:",
    "CSTD: New almanac for",
    "NOTICE: DOP Q Boost",
    "RTC not set",
    "numOfSVs = 0",
    "rtcaj tow ",
    NULL
};
/*@ +nullassign @*/

static char *dgpsvec[] =
{
    "None",
    "SBAS",
    "Serial",
    "Beacon",
    "Software",
};

/*****************************************************************************
 *
 * SiRF packet-decoding routines
 *
 *****************************************************************************/

static void decode_time(int week, int tow)
{
    int day = tow / 8640000;
    int tod = tow % 8640000;
    int h = tod / 360000;
    int m = tod % 360000;
    int s = m % 6000;

    m = (m - s) / 6000;

    (void)wmove(mid2win, 3,7);
    (void)wprintw(mid2win, "%4d+%9.2f", week, (double)tow/100);
    (void)wmove(mid2win, 3, 29);
    (void)wprintw(mid2win, "%d %02d:%02d:%05.2f", day, h,m,(double)s/100);
    (void)wmove(mid2win, 4, 8);
    (void)wprintw(mid2win, "%f", timestamp()-gpstime_to_unix(week,tow/100.0));
    (void)wmove(mid2win, 4, 29);
    (void)wprintw(mid2win, "%d", gmt_offset);
}

static void decode_ecef(double x, double y, double z,
			double vx, double vy, double vz)
{
    const double a = 6378137;
    const double f = 1 / 298.257223563;
    const double b = a * (1 - f);
    const double e2 = (a*a - b*b) / (a*a);
    const double e_2 = (a*a - b*b) / (b*b);
    double lambda,p,theta,phi,n,h,vnorth,veast,vup,speed,heading;

    lambda = atan2(y,x);
    /*@ -evalorder @*/
    p = sqrt(pow(x,2) + pow(y,2));
    theta = atan2(z*a,p*b);
    phi = atan2(z + e_2*b*pow(sin(theta),3),p - e2*a*pow(cos(theta),3));
    n = a / sqrt(1.0 - e2*pow(sin(phi),2));
    h = p / cos(phi) - n;
    h -= wgs84_separation((double)(RAD_2_DEG*phi),(double)(RAD_2_DEG*lambda));
    vnorth = -vx*sin(phi)*cos(lambda)-vy*sin(phi)*sin(lambda)+vz*cos(phi);
    veast = -vx*sin(lambda)+vy*cos(lambda);
    vup = vx*cos(phi)*cos(lambda)+vy*cos(phi)*sin(lambda)+vz*sin(phi);
    speed = sqrt(pow(vnorth,2) + pow(veast,2));
    heading = atan2(veast,vnorth);
    /*@ +evalorder @*/
    if (heading < 0)
	heading += 2 * GPS_PI;

    (void)wmove(mid2win, 1,40);
    (void)wprintw(mid2win, "%9.5f %9.5f",(double)(RAD_2_DEG*phi),
				   (double)(RAD_2_DEG*lambda));
    (void)mvwaddch(mid2win, 1, 49, ACS_DEGREE);
    (void)mvwaddch(mid2win, 1, 59, ACS_DEGREE);
    (void)wmove(mid2win, 1,61);
    (void)wprintw(mid2win, "%8d",(int)h);

    (void)wmove(mid2win, 2,40);
    (void)wprintw(mid2win, "%9.1f %9.1f",vnorth,veast);
    (void)wmove(mid2win, 2,61);
    (void)wprintw(mid2win, "%8.1f",vup);

    (void)wmove(mid2win, 3,54);
    (void)wprintw(mid2win, "%5.1f",(double)(RAD_2_DEG*heading));
    (void)mvwaddch(mid2win, 3, 59, ACS_DEGREE);
    (void)wmove(mid2win, 3,61);
    (void)wprintw(mid2win, "%8.1f",speed);
}

static void sirf_analyze(unsigned char buf[], size_t len)
{
    int i,j,ch,off,cn;

    assert(mid27win != NULL);
    buf += 4;
    len -= 8;
    switch (buf[0])
    {
    case 0x02:		/* Measured Navigation Data */
	(void)wmove(mid2win, 1,6);	/* ECEF position */
	(void)wprintw(mid2win, "%8d %8d %8d",getbesl(buf, 1),getbesl(buf, 5),getbesl(buf, 9));
	(void)wmove(mid2win, 2,6);	/* ECEF velocity */
	(void)wprintw(mid2win, "%8.1f %8.1f %8.1f",
		(double)getbesw(buf, 13)/8,(double)getbesw(buf, 15)/8,(double)getbesw(buf, 17)/8);
	decode_ecef((double)getbesl(buf, 1),(double)getbesl(buf, 5),(double)getbesl(buf, 9),
		(double)getbesw(buf, 13)/8,(double)getbesw(buf, 15)/8,(double)getbesw(buf, 17)/8);
	decode_time((int)getbeuw(buf, 22),getbesl(buf, 24));
	/* line 4 */
	(void)wmove(mid2win, 4,49);
	(void)wprintw(mid2win, "%4.1f",(double)getub(buf, 20)/5);	/* HDOP */
	(void)wmove(mid2win, 4,58);
	(void)wprintw(mid2win, "%02x",getub(buf, 19));		/* Mode 1 */
	(void)wmove(mid2win, 4,70);
	(void)wprintw(mid2win, "%02x",getub(buf, 21));		/* Mode 2 */
	(void)wmove(mid2win, 5,7);
	nfix = (int)getub(buf, 28);
	(void)wprintw(mid2win, "%d = ",nfix);		/* SVs in fix */
	for (i = 0; i < SIRF_CHANNELS; i++) {	/* SV list */
	    if (i < nfix)
		(void)wprintw(mid2win, "%3d",fix[i] = (int)getub(buf, 29+i));
	    else
		(void)wprintw(mid2win, "   ");
	}
	(void)wprintw(debugwin, "MND 0x02=");
	break;

    case 0x04:		/* Measured Tracking Data */
	decode_time((int)getbeuw(buf, 1),getbesl(buf, 3));
	ch = (int)getub(buf, 7);
	for (i = 0; i < ch; i++) {
	    int sv,st;
	    
	    off = 8 + 15 * i;
	    (void)wmove(mid4win, i+2, 3);
	    sv = (int)getub(buf, off);
	    (void)wprintw(mid4win, " %3d",sv);

	    (void)wprintw(mid4win, " %3d%3d %04x",((int)getub(buf, off+1)*3)/2,(int)getub(buf, off+2)/2,(int)getbesw(buf, off+3));

	    st = ' ';
	    if ((int)getbeuw(buf, off+3) == 0xbf)
		st = 'T';
	    for (j = 0; j < nfix; j++)
		if (sv == fix[j]) {
		    st = 'N';
		    break;
		}

	    cn = 0;

	    for (j = 0; j < 10; j++)
		cn += (int)getub(buf, off+5+j);

	    (void)wprintw(mid4win, "%5.1f %c",(double)cn/10,st);

	    if (sv == 0)			/* not tracking? */
		(void)wprintw(mid4win, "   ");	/* clear other info */
	}
	(void)wprintw(debugwin, "MTD 0x04=");
    	break;

#ifdef __UNUSED__
    case 0x05:		/* raw track data */
	for (off = 1; off < len; off += 51) {
	    ch = getbeul(buf, off);
	    (void)wmove(mid4win, ch+2, 19);
	    cn = 0;

	    for (j = 0; j < 10; j++)
		cn += getub(buf, off+34+j);

	    printw("%5.1f",(double)cn/10);

	    printw("%9d%3d%5d",getbeul(buf, off+8),(int)getbeuw(buf, off+12),(int)getbeuw(buf, off+14));
	    printw("%8.5f %10.5f",
	    	(double)getbeul(buf, off+16)/65536,(double)getbeul(buf, off+20)/1024);
	}
	(void)wprintw(debugwin, "RTD 0x05=");
    	break;
#endif /* __UNUSED */

    case 0x06:		/* firmware version */
	display(mid6win, 1, 10, "%s",buf + 1);
	(void)wprintw(debugwin, "FV  0x06=");
    	break;

    case 0x07:		/* Response - Clock Status Data */
	decode_time((int)getbeuw(buf, 1),getbesl(buf, 3));
	display(mid7win, 1, 5,  "%2d", getub(buf, 7));	/* SVs */
	display(mid7win, 1, 16, "%lu", getbeul(buf, 8));	/* Clock drift */
	display(mid7win, 1, 29, "%lu", getbeul(buf, 12));	/* Clock Bias */
	display(mid7win, 2, 21, "%lu", getbeul(buf, 16));	/* Estimated Time */
	(void)wprintw(debugwin, "CSD 0x07=");
	break;

    case 0x08:		/* 50 BPS data */
	ch = (int)getub(buf, 1);
	display(mid4win, ch+2, 27, "Y");
	(void)wprintw(debugwin, "50B 0x08=");
	subframe_enabled = true;
    	break;

    case 0x09:		/* Throughput */
	display(mid9win, 1, 6,  "%.3f",(double)getbeuw(buf, 1)/186);	/*SegStatMax*/
	display(mid9win, 1, 18, "%.3f",(double)getbeuw(buf, 3)/186);	/*SegStatLat*/
	display(mid9win, 1, 31, "%.3f",(double)getbeuw(buf, 5)/186);	/*SegStatTime*/
	display(mid9win, 1, 42, "%3d",(int)getbeuw(buf, 7));	/* Last Millisecond */
	(void)wprintw(debugwin, "THR 0x09=");
    	break;

    case 0x0b:		/* Command Acknowledgement */
	(void)wprintw(debugwin, "ACK 0x0b=");
    	break;

    case 0x0c:		/* Command NAcknowledgement */
	(void)wprintw(debugwin, "NAK 0x0c=");
    	break;

    case 0x0d:		/* Visible List */
	display(mid13win, 1, 6, "%d",getub(buf, 1));
	(void)wmove(mid13win, 1, 10);
	for (i = 0; i < SIRF_CHANNELS; i++) {
	    if (i < (int)getub(buf, 1))
		(void)wprintw(mid13win, " %2d",getub(buf, 2 + 5 * i));
	    else
		(void)wprintw(mid13win, "   ");

	}
	(void)wprintw(debugwin, "VL  0x0d=");
    	break;

    case 0x13:
#define YESNO(n)	(((int)getub(buf, n) != 0)?'Y':'N')
	display(mid19win, 1, 20, "%d", getub(buf, 5));	/* Alt. hold mode */
	display(mid19win, 2, 20, "%d", getub(buf, 6));	/* Alt. hold source*/
	display(mid19win, 3, 20, "%dm", (int)getbeuw(buf, 7));	/* Alt. source input */
	if (getub(buf, 9) != (unsigned char)'\0')
	    display(mid19win, 4, 20, "%dsec", getub(buf, 10));	/* Degraded timeout*/
	else
	    display(mid19win, 4, 20, "N/A   ");
	display(mid19win, 5, 20, "%dsec",getub(buf, 11));	/* DR timeout*/
	display(mid19win, 6, 20, "%c", YESNO(12));/* Track smooth mode*/
	display(mid19win, 7, 20, "%c", YESNO(13)); /* Static Nav.*/
	display(mid19win, 8, 20, "0x%x", getub(buf, 14));	/* 3SV Least Squares*/
	display(mid19win, 9 ,20, "0x%x", getub(buf, 19));	/* DOP Mask mode*/
	display(mid19win, 10,20, "0x%x", (int)getbeuw(buf, 20));	/* Nav. Elev. mask*/
	display(mid19win, 11,20, "0x%x", getub(buf, 22));	/* Nav. Power mask*/
	display(mid19win, 12,20, "0x%x", getub(buf, 27));	/* DGPS Source*/
	display(mid19win, 13,20, "0x%x", getub(buf, 28));	/* DGPS Mode*/
	display(mid19win, 14,20, "%dsec",getub(buf, 29));	/* DGPS Timeout*/
	display(mid19win, 1, 42, "%c", YESNO(34));/* LP Push-to-Fix */
	display(mid19win, 2, 42, "%dms", getbeul(buf, 35));	/* LP On Time */
	display(mid19win, 3, 42, "%d", getbeul(buf, 39));	/* LP Interval */
	display(mid19win, 4, 42, "%c", YESNO(43));/* User Tasks enabled */
	display(mid19win, 5, 42, "%d", getbeul(buf, 44));	/* User Task Interval */
	display(mid19win, 6, 42, "%c", YESNO(48));/* LP Power Cycling Enabled */
	display(mid19win, 7, 42, "%d", getbeul(buf, 49));/* LP Max Acq Search Time */
	display(mid19win, 8, 42, "%d", getbeul(buf, 53));/* LP Max Off Time */
	display(mid19win, 9, 42, "%c", YESNO(57));/* APM Enabled */
	display(mid19win,10, 42, "%d", (int)getbeuw(buf, 58));/* # of fixes */
	display(mid19win,11, 42, "%d", (int)getbeuw(buf, 60));/* Time Between fixes */
	display(mid19win,12, 42, "%d", getub(buf, 62));/* H/V Error Max */
	display(mid19win,13, 42, "%d", getub(buf, 63));/* Response Time Max */
	display(mid19win,14, 42, "%d", getub(buf, 64));/* Time/Accu & Duty Cycle Priority */
#undef YESNO
	break;

    case 0x1b:
	/******************************************************************
	 Not actually documented in any published materials.
	 Here is what Chris Kuethe got from the SiRF folks,
	 (plus some corrections from the GpsPaSsion forums):

	Start of message
	----------------
	Message ID          1 byte    27
	Correction Source   1 byte    0=None, 1=SBAS, 2=Serial, 3=Beacon,
	4=Software

	total:              2 bytes

	Middle part of message varies if using beacon or other:
	-------------------------------------------------------
	If Beacon:
	Receiver Freq Hz    4 bytes
	Bit rate BPS        1 byte
	Status bit map      1 byte    01=Signal Valid,
				      02=Auto frequency detect
				      04=Auto bit rate detect
	Signal Magnitude    4 bytes   Note: in internal units
	Signal Strength dB  2 bytes   derived from Signal Magnitude
	SNR  dB             2 bytes

	total:             14 bytes

	If Not Beacon:
	Correction Age[12]  1 byte x 12  Age in seconds in same order as follows
	Reserved            2 bytes

	total:             14 bytes

	End of Message
	--------------
	Repeated 12 times (pad with 0 if less than 12 SV corrections):
	SVID                1 byte
	Correction (cm)     2 bytes (signed short)

	total               3 x 12 = 36 bytes
	******************************************************************/
	display(mid27win, 1, 14, "%d (%s)", 
		getub(buf, 1), dgpsvec[(int)getub(buf, 1)]);
	/*@ -type @*/
	//(void) wmove(mid27win, 2, 0);
	for (i = j = 0; i < 12; i++) {
	    if (getub(buf, 16+3*i) != '\0') {
		//(void)wprintw(mid27win, " %d=%d", getub(buf, 16+3*i), getbesw(buf, 16+3*i+1));
		j++;
	    }
	}
	/*@ +type @*/
	display(mid27win, 1, 44, "%d", j);
	(void)wprintw(debugwin, "DST 0x1b=");
	break;

    case 0x1C:	/* NL Measurement Data */
    case 0x1D:	/* DGPS Data */
    case 0x1E:	/* SV State Data */
    case 0x1F:	/* NL Initialized Data */
	subframe_enabled = true;
	break;
    case 0x29:	/* Geodetic Navigation Message */
	(void)wprintw(debugwin, "GNM 0x29=");
	break;
    case 0x32:	/* SBAS Parameters */
	(void)wprintw(debugwin, "SBP 0x32=");
	break;
    case 0x34:	/* PPS Time */
	(void)wprintw(debugwin, "PPS 0x34=");
	break;

#ifdef __UNUSED__
    case 0x62:
	attrset(A_BOLD);
	move(2,40);
	printw("%9.5f %9.5f",(double)(RAD_2_DEG*1e8*getbesl(buf, 1)),
			     (double)(RAD_2_DEG*1e8*getbesl(buf, 5)));
	move(2,63);
	printw("%8d",getbesl(buf, 9)/1000);

	move(3,63);

	printw("%8.1f",(double)getbesl(buf, 17)/1000);

	move(4,54);
	if (getbeul(buf, 13) > 50) {
	    double heading = RAD_2_DEG*1e8*getbesl(buf, 21);
	    if (heading < 0)
		heading += 360;
	    printw("%5.1f",heading);
	} else
	    printw("  0.0");

	move(4,63);
	printw("%8.1f",(double)getbesl(buf, 13)/1000);
	attrset(A_NORMAL);

	move(5,13);
	printw("%04d-%02d-%02d %02d:%02d:%02d.%02d",
		(int)getbeuw(buf, 26),getub(buf, 28),getub(buf, 29),getub(buf, 30),getub(buf, 31),
		(unsigned short)getbeuw(buf, 32)/1000,
		((unsigned short)getbeuw(buf, 32)%1000)/10);
	{
	    struct timeval clk,gps;
	    struct tm tm;

	    gettimeofday(&clk,NULL);

	    memset(&tm,0,sizeof(tm));
	    tm.tm_sec = (unsigned short)getbeuw(buf, 32)/1000;
	    tm.tm_min = (int)getub(buf, 31);
	    tm.tm_hour = (int)getub(buf, 30);
	    tm.tm_mday = (int)getub(buf, 29);
	    tm.tm_mon = (int)getub(buf, 28) - 1;
	    tm.tm_year = (int)getbeuw(buf, 26) - 1900;

	    gps.tv_sec = mkgmtime(&tm);
	    gps.tv_usec = (((unsigned short)getbeuw(buf, 32)%1000)/10) * 10000;

	    move(5,2);
	    printw("           ");
	    move(5,2);
#if 1	    
	    printw("%ld",(gps.tv_usec - clk.tv_usec) +
	    		 ((gps.tv_sec - clk.tv_sec) % 3600) * 1000000);
#else
	    printw("%ld %ld %ld %ld",gps.tv_sec % 3600,gps.tv_usec,
	    			     clk.tv_sec % 3600,clk.tv_usec);
#endif
	}
		(void)wprintw(debugwin, "??? 0x62=");
    	break;
#endif /* __UNUSED__ */

    case 0xff:		/* Development Data */
	/*@ +ignoresigns @*/
	while (len > 0 && buf[len-1] == '\n')
	    len--;
	while (len > 0 && buf[len-1] == ' ')
	    len--;
	/*@ -ignoresigns @*/
	buf[len] = '\0';
	j = 1;
	for (i = 0; verbpat[i] != NULL; i++)
	    if (strncmp((char *)(buf+1),verbpat[i],strlen(verbpat[i])) == 0) {
		j = 0;
		break;
	    }
	if (j != 0)
	    (void)wprintw(debugwin, "%s\n",buf+1);
	(void)wprintw(debugwin, "DD  0xff=");
	break;

    default:
	(void)wprintw(debugwin, "    0x%02x=", buf[4]);
	break;
    }

    buf -= 4;
    len += 8;
    (void)wprintw(debugwin, "(%d) ", len);
    for (i = 0; i < (int)len; i++)
	(void)wprintw(debugwin, "%02x",buf[i]);
    (void)wprintw(debugwin, "\n");
}

/*@ -nullpass -globstate @*/
static void refresh_rightpanel1(void)
{
    (void)touchwin(mid6win);
    (void)touchwin(mid7win);
    (void)touchwin(mid9win);
    (void)touchwin(mid13win);
    (void)touchwin(mid27win);
    (void)wrefresh(mid6win);
    (void)wrefresh(mid7win);
    (void)wrefresh(mid9win);
    (void)wrefresh(mid13win);
    (void)wrefresh(mid27win);
}
/*@ +nullpass +globstate @*/

static bool sirf_windows(void)
{
    unsigned int i;

    /*@ -onlytrans @*/
    mid2win   = newwin(7,  80,  1, 0);
    mid4win   = newwin(15, 30,  8, 0);
    mid6win   = newwin(3,  50,  8, 30);
    mid7win   = newwin(4,  50, 11, 30);
    mid9win   = newwin(3,  50, 15, 30);
    mid13win  = newwin(3,  50, 18, 30);
    mid19win  = newwin(16, 50,  8, 30);
    mid27win  = newwin(3,  50, 21, 30);
    if (mid2win==NULL || mid4win==NULL || mid6win==NULL || mid9win==NULL
	|| mid13win==NULL || mid19win==NULL || mid27win==NULL)
	return false;

    /*@ -nullpass @*/
    (void)wborder(mid2win, 0, 0, 0, 0, 0, 0, 0, 0),
    (void)wattrset(mid2win, A_BOLD);
    (void)wmove(mid2win, 0,1);
    display(mid2win, 0, 12, " X "); 
    display(mid2win, 0, 21, " Y "); 
    display(mid2win, 0, 30, " Z "); 
    display(mid2win, 0, 43, " North "); 
    display(mid2win, 0, 54, " East "); 
    display(mid2win, 0, 65, " Alt "); 

    (void)wmove(mid2win, 1,1);
    (void)wprintw(mid2win, "Pos:                            m                                    m");
    (void)wmove(mid2win, 2,1);
    (void)wprintw(mid2win, "Vel:                            m/s                                  climb m/s");
    (void)wmove(mid2win, 3,1);
    (void)wprintw(mid2win, "Time:                  GPS:                Heading:                  speed m/s");
    (void)wmove(mid2win, 4,1);
    (void)wprintw(mid2win, "Skew:                   TZ:                HDOP:      M1:        M2:    ");
    (void)wmove(mid2win, 5,1);
    (void)wprintw(mid2win, "Fix:");
    display(mid2win, 6, 24, " Packet type 2 (0x02) ");
    (void)wattrset(mid2win, A_NORMAL);

    (void)wborder(mid4win, 0, 0, 0, 0, 0, 0, 0, 0),
    (void)wattrset(mid4win, A_BOLD);
    display(mid4win, 1, 1, " Ch SV  Az El Stat  C/N ? A");
    for (i = 0; i < SIRF_CHANNELS; i++) {
	display(mid4win, (int)(i+2), 1, "%2d",i);
    }
    display(mid4win, 14, 4, " Packet Type 4 (0x04) ");
    (void)wattrset(mid4win, A_NORMAL);

    (void)wborder(mid19win, 0, 0, 0, 0, 0, 0, 0, 0),
    (void)wattrset(mid19win, A_BOLD);
    display(mid19win, 1, 1, "Alt. hold mode:");
    display(mid19win, 2, 1, "Alt. hold source:");
    display(mid19win, 3, 1, "Alt. source input:");
    display(mid19win, 4, 1, "Degraded timeout:");
    display(mid19win, 5, 1, "DR timeout:");
    display(mid19win, 6, 1, "Track smooth mode:");
    display(mid19win, 7, 1, "Static Navigation:");
    display(mid19win, 8, 1, "3SV Least Squares:");
    display(mid19win, 9 ,1, "DOP Mask mode:");
    display(mid19win, 10,1, "Nav. Elev. mask:");
    display(mid19win, 11,1, "Nav. Power mask:");
    display(mid19win, 12,1, "DGPS Source:");
    display(mid19win, 13,1, "DGPS Mode:");
    display(mid19win, 14,1, "DGPS Timeout:");
    display(mid19win, 1, 26,"LP Push-to-Fix:");
    display(mid19win, 2, 26,"LP On Time:");
    display(mid19win, 3, 26,"LP Interval:");
    display(mid19win, 4, 26,"U. Tasks Enab.:");
    display(mid19win, 5, 26,"U. Task Inter.:");
    display(mid19win, 6, 26,"LP Pwr Cyc En:");
    display(mid19win, 7, 26,"LP Max Acq Srch:");
    display(mid19win, 8, 26,"LP Max Off Time:");
    display(mid19win, 9, 26,"APM enabled:");
    display(mid19win,10, 26,"# of Fixes:");
    display(mid19win,11, 26,"Time btw Fixes:");
    display(mid19win,12, 26,"H/V Error Max:");
    display(mid19win,13, 26,"Rsp Time Max:");
    display(mid19win,14, 26,"Time/Accu:");

    display(mid19win, 15, 8, " Packet type 19 (0x13) ");
    (void)wattrset(mid19win, A_NORMAL);

    (void)wborder(mid6win, 0, 0, 0, 0, 0, 0, 0, 0),
    (void)wattrset(mid6win, A_BOLD);
    display(mid6win, 1, 1, "Version:");
    display(mid6win, 2, 8, " Packet Type 6 (0x06) ");
    (void)wattrset(mid6win, A_NORMAL);

    (void)wborder(mid7win, 0, 0, 0, 0, 0, 0, 0, 0),
    (void)wattrset(mid7win, A_BOLD);
    display(mid7win, 1, 1,  "SVs: ");
    display(mid7win, 1, 9,  "Drift: ");
    display(mid7win, 1, 23, "Bias: ");
    display(mid7win, 2, 1,  "Estimated GPS Time: ");
    display(mid7win, 3, 8, " Packet type 7 (0x07) ");
    (void)wattrset(mid7win, A_NORMAL);

    (void)wborder(mid9win, 0, 0, 0, 0, 0, 0, 0, 0),
    (void)wattrset(mid9win, A_BOLD);
    display(mid9win, 1, 1,  "Max: ");
    display(mid9win, 1, 13, "Lat: ");
    display(mid9win, 1, 25, "Time: ");
    display(mid9win, 1, 39, "MS: ");
    display(mid9win, 2, 8, " Packet type 9 (0x09) ");
    (void)wattrset(mid9win, A_NORMAL);

    (void)wborder(mid13win, 0, 0, 0, 0, 0, 0, 0, 0),
    (void)wattrset(mid13win, A_BOLD);
    display(mid13win, 1, 1, "SVs: ");
    display(mid13win, 1, 9, "=");
    display(mid13win, 2, 8, " Packet type 13 (0x0D) ");
    (void)wattrset(mid13win, A_NORMAL);

    (void)wborder(mid27win, 0, 0, 0, 0, 0, 0, 0, 0),
    (void)wattrset(mid27win, A_BOLD);
    display(mid27win, 1, 1, "DGPS source: ");
    display(mid27win, 1, 31, "Corrections: ");
    display(mid27win, 2, 8, " Packet type 27 (0x1B) ");
    (void)wattrset(mid27win, A_NORMAL);
    /*@ +nullpass @*/
    /*@ -onlytrans @*/

    return true;
}

static void sirf_probe(void)
{
    /* probe for version */
    /*@ -compdef @*/
    (void)monitor_control_send((unsigned char *)"\x84\x00", 2);
    /*@ +compdef @*/
}

/*@ -globstate */
static void sirf_refresh(bool inloop)
{
    /* refresh navigation parameters */
    if (dispmode && (time(NULL) % 10 == 0)){
	(void)monitor_control_send((unsigned char *)"\x98\x00", 2);
    }

    (void)wrefresh(mid2win);
    (void)wrefresh(mid4win);
    /*@ -nullpass -nullderef @*/
    if (!dispmode) {
	refresh_rightpanel1();
    } else {
	(void)touchwin(mid19win);
	(void)wrefresh(mid19win);
	(void)redrawwin(mid19win);
    }
    if (inloop)
	(void)wrefresh(mid19win);
    /*@ +nullpass -nullderef @*/
}
/*@ +globstate */

static int sirf_command(char line[])
{
    unsigned char buf[BUFSIZ];
    int v;

    switch (line[0]) 
    {
    case 'a':			/* toggle 50bps subframe data */
	(void)memset(buf, '\0', sizeof(buf));
	putbyte(buf, 0, 0x80);
	putbyte(buf, 23, 12);
	putbyte(buf, 24, subframe_enabled ? 0x00 : 0x10);
	(void)monitor_control_send(buf, 25);
	return COMMAND_MATCH;

    case 'c':			/* static navigation */
	putbyte(buf, 0,0x8f);			/* id */
	putbyte(buf, 1, atoi(line+1));
	(void)monitor_control_send(buf, 2);
	return COMMAND_MATCH;

    case 'd':		/* MID 4 rate change -- not documented */
	v = atoi(line+1);
	if (v > 30)
	    return COMMAND_MATCH;
	putbyte(buf, 0,0xa6);
	putbyte(buf, 1,0);
	putbyte(buf, 2, 4);	/* satellite picture */
	putbyte(buf, 3, v);
	putbyte(buf, 4, 0);
	putbyte(buf, 5, 0);
	putbyte(buf, 6, 0);
	putbyte(buf, 7, 0);
	(void)monitor_control_send(buf, 8);
	return COMMAND_MATCH;

    case 'n':			/* switch to NMEA */
	putbyte(buf, 0,  0x81);			/* id */
	putbyte(buf, 1,  0x02);			/* mode */
	putbyte(buf, 2,  0x01);			/* GGA */
	putbyte(buf, 3,  0x01);
	putbyte(buf, 4,  0x01);			/* GLL */
	putbyte(buf, 5,  0x01);
	putbyte(buf, 6, 0x01);		  	/* GSA */
	putbyte(buf, 7, 0x01);
	putbyte(buf, 8, 0x05);			/* GSV */
	putbyte(buf, 9, 0x01);
	putbyte(buf, 10, 0x01);			/* RNC */
	putbyte(buf, 11, 0x01);
	putbyte(buf, 12, 0x01);			/* VTG */
	putbyte(buf, 13, 0x01);
	putbyte(buf, 14, 0x00);			/* unused fields */
	putbyte(buf, 15, 0x01);
	putbyte(buf, 16, 0x00);
	putbyte(buf, 17, 0x01);
	putbyte(buf, 18, 0x00);
	putbyte(buf, 19, 0x01);
	putbyte(buf, 20, 0x00);
	putbyte(buf, 21, 0x01);
	putbeword(buf, 22, gpsd_get_speed(&session.ttyset));
	(void)monitor_control_send(buf, 24);
	return COMMAND_TERMINATE;		/* terminate */

    case 't':			/* poll navigation params */
	dispmode = !dispmode;
	return COMMAND_MATCH;
    }

    return COMMAND_UNKNOWN;	/* no match */
}

static const struct mdevice_t sirf = {
    .probe = sirf_probe,
    .analyze = sirf_analyze,
    .windows = sirf_windows,
    .repaint = sirf_refresh,
    .command = sirf_command,
    .min_y = 23, .min_x = 80,
    .driver = &sirf_binary,
};

/*****************************************************************************
 *****************************************************************************
 *
 * SiRF-dependent stuff ends here (someday)
 *
 *****************************************************************************
 *****************************************************************************/

/******************************************************************************
 *
 * Device-independent I/O routines
 *
 ******************************************************************************/

void gpsd_report(int errlevel UNUSED, const char *fmt, ... )
/* our version of the logger */
{
    if (errlevel <= debuglevel) {
	va_list ap;
	va_start(ap, fmt);
	if (!curses_active)
	    (void)vprintf(fmt, ap);
	else
	    (void)wprintw(debugwin, fmt, ap);
	va_end(ap);
    }
}

/*@ -globstate @*/
static ssize_t readpkt(void)
{
    /*@ -type -shiftnegative -compdef -nullpass @*/
    struct timeval timeval;
    fd_set select_set;
    gps_mask_t changed;

    FD_ZERO(&select_set);
    FD_SET(session.gpsdata.gps_fd,&select_set);
    if (controlfd < -1)
	FD_SET(controlfd,&select_set);
    timeval.tv_sec = 0;
    timeval.tv_usec = 500000;
    if (select(session.gpsdata.gps_fd + 1,&select_set,NULL,NULL,&timeval) < 0)
	return EOF;

    if (!FD_ISSET(session.gpsdata.gps_fd,&select_set))
	return EOF;

    (void)usleep(100000);

    changed = gpsd_poll(&session);
    if (changed & ERROR_SET)
	return EOF;

    if (logfile != NULL) {
	/*@ -shiftimplementation -sefparams +charint @*/
	assert(fwrite(session.packet.outbuffer, 
		      sizeof(char), session.packet.outbuflen, 
		      logfile) >= 1);
	/*@ +shiftimplementation +sefparams -charint @*/
    }
    return session.packet.outbuflen;
    /*@ +type +shiftnegative +compdef +nullpass @*/
}
/*@ +globstate @*/

static void monitor_dump_send(void)
{
    size_t i;

    (void)wprintw(debugwin, ">>>");
    if (session.msgbuf[0] == '$') {
	for (i = 0; i < session.msgbuflen; i++)
	    if (isprint(session.msgbuf[i]))
		(void)waddch(debugwin, (chtype)session.msgbuf[i]);
	    else
		(void)wprintw(debugwin, "\\x%02x", (unsigned char)session.msgbuf[i]);
    }else {
	for (i = 0; i < session.msgbuflen; i++)
	    (void)wprintw(debugwin, " %02x", (unsigned char)session.msgbuf[i]);
    }
    (void)wprintw(debugwin, "\n");
}

static bool monitor_control_send(/*@in@*/unsigned char *buf, size_t len)
{
    monitor_dump_send();

    if (controlfd == -1) 
	return false;
    else {
	int savefd;
	ssize_t st;

	if (!serial) {
	    /*@ -sefparams @*/
	    assert(write(controlfd, "!", 1) != -1);
	    assert(write(controlfd, session.gpsdata.gps_device, strlen(session.gpsdata.gps_device)) != -1);
	    assert(write(controlfd, "=", 1) != -1);
	    /*@ +sefparams @*/
	    /*
	     * Ugh...temporarily con the libgpsd layer into using the
	     * socket descriptor.
	     */
	    savefd = session.gpsdata.gps_fd;
	    session.gpsdata.gps_fd = controlfd;
	}

	st = sirf.driver->control_send(&session, (char *)buf, len);

	if (!serial) {
	    /* stop pretending now */
	    session.gpsdata.gps_fd = controlfd;
	    /* enough room for "ERROR\r\n\0" */
	    /*@ -sefparams @*/
	    assert(read(controlfd, buf, 8) != -1);
	    /*@ +sefparams @*/
	}
	return ((size_t)st == len);
    }
}

/*****************************************************************************
 *
 * Main sequence and display machinery
 *
 *****************************************************************************/

static long tzoffset(void)
{
    time_t now = time(NULL);
    struct tm tm;
    long res = 0;

    tzset();
#ifdef HAVE_TIMEZONE
    res = timezone;
#else
    res = localtime_r(&now, &tm)->tm_gmtoff;
#endif
#ifdef HAVE_DAYLIGHT
    if (daylight != 0 && localtime_r(&now, &tm)->tm_isdst != 0)
	res -= 3600;
#else
    if (localtime_r(&now, &tm)->tm_isdst)
	res -= 3600;
#endif
    return res;
}

static void command(char buf[], size_t len, const char *fmt, ... )
/* assemble command in printf(3) style, use stderr or syslog */
{
    va_list ap;
    ssize_t n;

    va_start(ap, fmt) ;
    (void)vsnprintf(buf, len, fmt, ap);
    va_end(ap);

    /*@i1@*/assert(write(session.gpsdata.gps_fd, buf, strlen(buf)) != -1);
    n = read(session.gpsdata.gps_fd, buf, len);
    if (n >= 0) {
	buf[n] = '\0';
	while (isspace(buf[strlen(buf)-1]))
	    buf[strlen(buf)-1] = '\0';
    }
}

static void error_and_pause(const char *fmt, ...) 
{
    va_list ap;
    va_start(ap, fmt);
    (void)wmove(cmdwin, 0, 5);
    (void)wclrtoeol(cmdwin);
    (void)wattrset(cmdwin, A_BOLD | A_BLINK);
    (void)wprintw(cmdwin, fmt, ap);
    (void)wattrset(cmdwin, A_NORMAL);
    (void)wrefresh(cmdwin);
    (void)wgetch(cmdwin);
    va_end(ap);
}

/*@ -noret @*/
static gps_mask_t get_packet(struct gps_device_t *session)
/* try to get a well-formed packet from the GPS */
{
    gps_mask_t fieldmask;

    for (;;) {
	int waiting = 0;
	(void)ioctl(session->gpsdata.gps_fd, FIONREAD, &waiting);
	if (waiting == 0) {
	    (void)usleep(300);
	    continue;
	}
	fieldmask = gpsd_poll(session);
	if ((fieldmask &~ ONLINE_SET)!=0)
	    return fieldmask;
    }
}
/*@ +noret @*/

static jmp_buf assertbuf;

static void onsig(int sig UNUSED)
{
    longjmp(assertbuf, 1);
}

int main (int argc, char **argv)
{
    unsigned int v;
    int option, status;
    ssize_t len;
    char *p, *arg = NULL, *colon1 = NULL, *colon2 = NULL, *slash = NULL;
    char *server=NULL, *port = DEFAULT_GPSD_PORT, *device = NULL;
    char *controlsock = "/var/run/gpsd.sock";
    fd_set select_set;
    unsigned char buf[BUFLEN];
    char line[80];

    gmt_offset = (int)tzoffset();

    /*@ -branchstate @*/
    while ((option = getopt(argc, argv, "D:F:Vh")) != -1) {
	switch (option) {
	case 'D':
	    debuglevel = atoi(optarg);
	    break;
	case 'F':
	    controlsock = optarg;
	    break;
	case 'V':
	    (void)printf("sirfmon %s\n", VERSION);
	    exit(0);
	case 'h': case '?': default:
	    (void)fputs("usage:  sirfmon [-?hv] [-F controlsock] [server[:port:[device]]]\n", stderr);
	    exit(1);
	}
    }
    /*@ +branchstate @*/

    /*@ -nullpass -branchstate -compdef @*/
    if (optind < argc) {
	arg = strdup(argv[optind]);
	colon1 = strchr(arg, ':');
	slash = strchr(arg, '/');
	server = arg;
	if (colon1 != NULL) {
	    if (colon1 == arg)
		server = NULL;
	    else
		*colon1 = '\0';
	    port = colon1 + 1;
	    colon2 = strchr(port, ':');
	    if (colon2 != NULL) {
		if (colon2 == port)
		    port = NULL;
	        else
		    *colon2 = '\0';
		device = colon2 + 1;
	    }
	}
    }

    gpsd_init(&session, &context, NULL);

    /*@ -boolops */
    if (!arg || (arg && !slash) || (arg && colon1 && slash)) {	
	if (!server)
	    server = "127.0.0.1";
	if (!port)
	    port = DEFAULT_GPSD_PORT;
	session.gpsdata.gps_fd = netlib_connectsock(server, port, "tcp");
	if (session.gpsdata.gps_fd < 0) {
	    (void)fprintf(stderr, 
			  "%s: connection failure on %s:%s, error %d.\n", 
			  argv[0], server, port, session.gpsdata.gps_fd);
	    exit(1);
	}
	controlfd = open(controlsock, O_RDWR);
	/*@ -compdef @*/
	if (device)
	    command((char *)buf, sizeof(buf), "F=%s\r\n", device);
	else
	    command((char *)buf, sizeof(buf), "O\r\n");	/* force device allocation */
	command((char *)buf, sizeof(buf), "F\r\n");
	(void)strlcpy(session.gpsdata.gps_device, (char *)buf+7, PATH_MAX);
	command((char *)buf, sizeof(buf), "R=2\r\n");
	/*@ +compdef @*/
	serial = false;
    } else {
	int seq;

	(void)strlcpy(session.gpsdata.gps_device, arg, PATH_MAX);
	if (gpsd_activate(&session, false) == -1) {
	    gpsd_report(LOG_ERROR,
			  "activation of device %s failed, errno=%d\n",
			  session.gpsdata.gps_device, errno);
	    exit(2);
	}
	/* hunt for packet type and serial parameters */
	for (seq = 1; session.device_type == NULL; seq++) {
	    gps_mask_t valid = get_packet(&session);

	    if (valid & ERROR_SET) {
		gpsd_report(LOG_ERROR,
			    "autodetection failed.\n");
		exit(2);
	    } else if (valid & ONLINE_SET) {
		gpsd_report(LOG_IO,
			    "autodetection after %d reads.\n", seq);
		break;
	    }
	}
	gpsd_report(LOG_PROG, "%s looks like a %s at %d.\n",
		    session.gpsdata.gps_device, 
		    gpsd_id(&session), session.gpsdata.baudrate);

	/* This will change when we support more types */
	if (session.packet.type == NMEA_PACKET) {
	    sirf_binary.mode_switcher(&session, MODE_BINARY);
	    gpsd_report(LOG_PROG, "switching to SiRF binary mode.\n");
	} else if (session.packet.type != SIRF_PACKET) {
	    gpsd_report(LOG_ERROR, "cannot yet handle packet type %d.\n",
		       session.packet.type);
	    exit(1);
	}

	/* 
	 * If we've identified this as an NMEA device, we have to eat
	 * packets for a while to see if one of our probes elicits an
	 * ID response telling us that it's really a SiRF or
	 * something.  If so, the libgpsd(3) layer will automatically
	 * redispatch to the correct driver type.
	 */
#define REDIRECT_SNIFF 12
	if (strcmp(session.device_type->type_name, "Generic NMEA") == 0) {
	    int dummy;
	    for (dummy = 0; dummy < REDIRECT_SNIFF; dummy++) {
		if ((get_packet(&session) & DEVICEID_SET)!=0)
		    break;
	    }
	}
	gpsd_report(LOG_SHOUT, "%s identified as a %s at %d.\n",
		    session.gpsdata.gps_device, 
		    gpsd_id(&session), session.gpsdata.baudrate);

	controlfd = session.gpsdata.gps_fd;
	serial = true;
    }
    /*@ +boolops */
    /*@ +nullpass +branchstate @*/

    /* quit cleanly if an assertion fails */
    (void)signal(SIGABRT, onsig);
    if (setjmp(assertbuf) > 0) {
	if (logfile)
	    (void)fclose(logfile);
	(void)endwin();
	(void)fputs("sirfmon: assertion failure, probable I/O error\n", stderr);
	exit(1);
    }

    (void)initscr();
    (void)cbreak();
    (void)noecho();
    (void)intrflush(stdscr, FALSE);
    (void)keypad(stdscr, true);
    curses_active = true;

    /*@ -onlytrans @*/
    statwin   = newwin(1,  30, 0, 0);
    cmdwin    = newwin(1,  0,  0, 30);
    if (!sirf.windows() || cmdwin==NULL)
	goto quit;

    debugwin  = newwin(0,   0, sirf.min_y+1, 0);
    (void)scrollok(debugwin, true);
    (void)wsetscrreg(debugwin, 0, LINES-21);
    /*@ +onlytrans @*/

    (void)wmove(debugwin,0, 0);

    FD_ZERO(&select_set);

    sirf.probe();

    for (;;) {
	(void)wattrset(statwin, A_BOLD);
	if (serial)
	    display(statwin, 0, 0, "%s %4d %c %d", 
		    session.gpsdata.gps_device, 
		    gpsd_get_speed(&session.ttyset),
		    session.gpsdata.parity, 
		    session.gpsdata.stopbits);
	else
	    display(statwin, 0, 0, "%s:%s:%s", 
		    server, port, session.gpsdata.gps_device);
	(void)wattrset(statwin, A_NORMAL);
	(void)wrefresh(statwin);
	(void)wmove(cmdwin, 0,0);
	(void)wprintw(cmdwin, "cmd> ");
	(void)wclrtoeol(cmdwin);
	(void)refresh();
	sirf.repaint(false);
	(void)wrefresh(debugwin);
	(void)wrefresh(cmdwin);

	FD_SET(0,&select_set);
	FD_SET(session.gpsdata.gps_fd,&select_set);

	if (select(FD_SETSIZE, &select_set, NULL, NULL, NULL) < 0)
	    break;

	if (FD_ISSET(0,&select_set)) {
	    (void)wmove(cmdwin, 0,5);
	    (void)wrefresh(cmdwin);
	    (void)echo();
	    /*@ -usedef -compdef @*/
	    (void)wgetnstr(cmdwin, line, 80);
	    (void)noecho();
	    //(void)move(0,0);
	    //(void)clrtoeol();
	    //(void)refresh();
	    sirf.repaint(true);
	    (void)wrefresh(debugwin);
	    (void)wrefresh(cmdwin);

	    if ((p = strchr(line,'\r')) != NULL)
		*p = '\0';

	    if (line[0] == '\0')
		continue;
	    /*@ +usedef +compdef @*/

	    p = line;

	    while (*p != '\0' && !isspace(*p))
		p++;
	    while (*p != '\0' && isspace(*p))
		p++;

	    status = sirf.command(line);
	    if (status == COMMAND_TERMINATE)
		goto quit;
	    else if (status == COMMAND_MATCH)
		continue;
	    assert(status == COMMAND_UNKNOWN);
	    switch (line[0])
	    {
	    case 'b':
		monitor_dump_send();
		if (serial) {
		    v = (unsigned)atoi(line+1);
		    /* Ugh...should have a controlfd slot 
		     * in the session structure, really
		     */
		    if (sirf.driver->speed_switcher) {
			int dfd = session.gpsdata.gps_fd;
			session.gpsdata.gps_fd = controlfd;
			(void)sirf.driver->speed_switcher(&session, v);
			/*
			 * See the comment attached to the 'B' command in gpsd.
			 * Allow the control string time to register at the
			 * GPS before we do the baud rate switch, which
			 * effectively trashes the UART's buffer.
			 */
			(void)tcdrain(session.gpsdata.gps_fd);
			(void)usleep(50000);
			session.gpsdata.gps_fd = dfd;
			(void)gpsd_set_speed(&session, v, 
					 session.gpsdata.parity, 
					 session.gpsdata.stopbits);
		    } else
			error_and_pause("Device type has no speed switcher");
		} else {
		    line[0] = 'b';
		    /*@ -sefparams @*/
		    assert(write(session.gpsdata.gps_fd, line, strlen(line)) != -1);
		    /* discard response */
		    assert(read(session.gpsdata.gps_fd, buf, sizeof(buf)) != -1);
		    /*@ +sefparams @*/
		}
		break;

	    case 'l':				/* open logfile */
		if (logfile != NULL) {
		    (void)wprintw(debugwin, ">>> Logging to %s off", logfile);
		    (void)fclose(logfile);
		}

		if ((logfile = fopen(line+1,"a")) != NULL)
		    (void)wprintw(debugwin, ">>> Logging to %s on", logfile);
		break;

	    case 'q':
		goto quit;

	    case 's':
		len = 0;
		/*@ -compdef @*/
		while (*p != '\0')
		{
		    (void)sscanf(p,"%x",&v);
		    putbyte(buf, len, v);
		    len++;
		    while (*p != '\0' && !isspace(*p))
			p++;
		    while (*p != '\0' && isspace(*p))
			p++;
		}
		if (sirf.driver->control_send != NULL)
		    (void)monitor_control_send(buf, (size_t)len);
		else
		    error_and_pause("Device type has no control-send method.");
		/*@ +compdef @*/
		break;
	    }
	}

	if ((len = readpkt()) > 0 && session.packet.outbuflen > 0) {
	    sirf.analyze(session.packet.outbuffer,session.packet.outbuflen);
	}
    }
    /*@ +nullpass @*/

 quit:
    if (logfile)
	(void)fclose(logfile);
    (void)endwin();
    exit(0);
}

/* sirfmon.c ends here */
