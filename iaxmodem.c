/*
 * Copyright (C) 2005-2006 Lee Howard <faxguy@howardsilvan.com>
 * Copyright (C) 2006 Julien BLACHE / Linbox FAS <julien.blache@linbox.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * The GNU General Public License can be found at
 * http://www.gnu.org/licenses/gpl.html.
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <string.h>
#include <strings.h>

#if !defined(__OpenBSD__) && !defined(__FreeBSD__)
# ifndef USE_UNIX98_PTY
#  include <pty.h>
# endif /* !USE_UNIX98_PTY */
# include <termios.h>
#else
#if !defined(__FreeBSD__)
# include <util.h>
#endif
#endif

#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <pwd.h>
#include <grp.h>
#include <ctype.h>

#include <arpa/inet.h>

#include <math.h>

#include <stdint.h>
#include <tiffio.h>

#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif

#define SPANDSP_EXPOSE_INTERNAL_STRUCTURES 
#include <spandsp.h>
#ifdef STATICLIBS
#include <iax-client.h>
#else
#include <iax/iax-client.h>
#endif

#ifndef MODEMVER
#define MODEMVER "0.0.0"
#endif
#ifndef DSPVER
#define DSPVER "0.0.0"
#endif
#ifndef IAXVER
#define IAXVER "0.0.0"
#endif

/* Forward declaration - defined in iax.c from libiax2 */
extern void iax_disable_jitterbuffer(void);

#define MODEM_ONHOOK		0
#define MODEM_OFFHOOK		1
#define MODEM_RINGING		2
#define MODEM_CALLING		3
#define MODEM_CONNECTED		4

#define PHONE_FREED		0
#define PHONE_CALLACCEPTED	1
#define PHONE_RINGING		2
#define PHONE_ANSWERED		3
#define PHONE_CONNECTED		4

#define UNREGISTERED		0
#define REGISTERED		1
#define PENDING			2

/* Packet length is 20 ms.  At 8000 Hz that's 160 samples. */
#define VOIP_PACKET_LENGTH	20000
#define VOIP_PACKET_SIZE	160

#define CODEC_SUPPORT		AST_FORMAT_SLINEAR|AST_FORMAT_ULAW|AST_FORMAT_ALAW

#define DSP_BUFSIZE		T31_TX_BUF_LEN

static int amaster, aslave;
static int modemstate = MODEM_ONHOOK;
static int phonestate = PHONE_FREED;
static int regstate = UNREGISTERED;
static struct iax_session *session[2];	/* one for calls, the other for registration */
static int dspaudiofd = -1, iaxaudiofd = -1;
static char dspaudiofile[50];
static char iaxaudiofile[50];
static char dspnowaudiofile[50];
static char iaxnowaudiofile[50];
static struct timeval now, lasthangup, nextaudio;	/* target time for next audio */
static int window = 1000;		/* u-sec prior to target that we will let audio go */
static int codec = AST_FORMAT_SLINEAR;	/* negotiated codec */

static int port = 4569;
static int refreshreq = 300;		/* requested refresh */
static int refresh = 300;		/* negotiated refresh, initialize equal to refreshreq */
static int codecreq = AST_FORMAT_SLINEAR; /* requested codec */
static char devlink[64];
static char devowner[64];		/* owner:group for the slave tty */
static char devmode[64];		/* mode for the slave tty */
static char server[64];
static char regpeer[64];
static char regsecret[64];
static char cidname[64];
static char cidnumber[64];
static char call_time[16];
static char call_date[16];
static char *dialextra = NULL;
static int record = 0;
static int replay = 0;
static int nojitterbuffer = 0;
static int iax2debug = 0;
static int dspdebug = 0;
static int nodaemon = 0;
static int commalen = 2;
static int defskew = 0;
static int skew;
static int audiobuflen = 100;

struct modem {
    struct modem *next;
    int pid;
    char *config;
    time_t last_started;
};

static int numchild;			/* number of children */
static int gothup;			/* got SIGHUP ? */
static struct modem *modems;		/* linked list of children */

/* Mode for the log files */
int logmode = S_IRUSR | S_IWUSR | S_IRGRP;

#define LOG_ERROR   stderr
#define LOG_INFO    stdout

#ifdef SOLARIS
#include "compat/daemon.c"
#include "compat/timers.c"
#include "compat/strings.c"
#include "compat/headers.h"
#include "sys/stropts.h"
#endif

#ifdef __FreeBSD__
#include "compat/strndup-freebsd.c"
#endif

void
printlog(FILE *fp, char *fmt, ...)
{
  va_list ap;
  char buf[32];
  struct timeval tv;

  gettimeofday(&tv, NULL);
  time_t tt = tv.tv_sec;
  strftime(buf, sizeof(buf), "[%Y-%m-%d %H:%M:%S", localtime(&tt));
  fprintf(fp, "%s.%.6ld] ", buf, tv.tv_usec);

  va_start(ap, fmt);
  vfprintf(fp, fmt, ap);
  va_end(ap);
  fflush(fp);
}

void
checklog(char* filename)
{
    struct stat fs;
    if (stat(filename, &fs) == 0) {
	if (fs.st_size > 0x40000000) {
	    char oname[256];
	    snprintf(oname, sizeof(oname), "%s.old", filename);
	    rename(filename, oname);
	}
    }
}

void
cleanup(int sig)
{
    if (refreshreq && regstate == REGISTERED) {
	struct iax_event *iaxevent = 0;
	session[1] = iax_session_new();
	iax_unregister(session[1], server, regpeer, regsecret, "Exiting");
	while (!(iaxevent = iax_get_event(1)));
	iax_event_free(iaxevent);
	iax_destroy(session[1]);
	iax_destroy(session[0]);
    }

    /* Escalate privileges */
    seteuid(0);
    setegid(0);

    unlink(devlink);
    close(amaster);
    close(aslave);
    if (record) {
	if (dspaudiofd > 0) {
	    close(dspaudiofd);
	    rename(dspnowaudiofile, dspaudiofile);
	}
	if (iaxaudiofd > 0) {
	    close(iaxaudiofd);
	    rename(iaxnowaudiofile, iaxaudiofile);
	}
    } else if (replay) {
	close(dspaudiofd);
	close(iaxaudiofd);
    }
    _exit(sig);
}

void
sighandler(int sig)
{
    signal(SIGINT, NULL);
    signal(SIGTERM, NULL);

    printlog(LOG_ERROR, "Terminating on signal %d...\n", sig);

    cleanup(sig);
}

void
sighandler_hup(int sig)
{
    if (phonestate != PHONE_FREED || modemstate != MODEM_ONHOOK) {
	gothup = 1;
    } else {
	sighandler(sig);
    }
}

void
sighandler_alarm(int sig)
{
    int other = 0;
    char dtmf[2];
    dtmf[1] = '\0';
    while (dialextra && *dialextra != '\0' && *dialextra != ',') {
	dtmf[0] = *dialextra;
	printlog(LOG_INFO, "Dialing DTMF '%s'\n", dtmf);
	iax_send_dtmf(session[0], *dialextra);
	dialextra++;
    }
    while (dialextra && *dialextra == ',') {
	other += commalen;
	dialextra++;
    }
    if (other) {
	printlog(LOG_INFO, "Dialing pause %d second(s)\n", other);
	alarm(other);
    } else dialextra = NULL;
}

void
orderbytes(int16_t *buf, int len)
{
    int i = 0;
    for (; i < len; i++) {
	buf[i] = ntohs(buf[i]);
    }
}

int
timediff(struct timeval a, struct timeval b)
{
    struct timeval tv;
    timersub(&a, &b, &tv);
    /* The return value, in microseconds, will overflow
       a signed 32-bit integer every 36 minutes.  As we don't 
       need to account for timings that long anyway we cap the
       timer at 5-minutes. */
    return (tv.tv_sec > 300 ? 300000000 : tv.tv_sec * 1000000 + tv.tv_usec);
}

void
printtime()
{
    char buf[32];
    gettimeofday(&now, NULL);
    strftime(buf, sizeof(buf), "%h %d %T", localtime((time_t*) &now.tv_sec));
    printf("%s", buf);

    printf(".%02ld: ", now.tv_usec/10000);
}


void
setconfigline(char *line)
{
    if (strchr(line, '%')) return;	/* don't allow troublesome "%" in configs */
    /* trim comments */
    if (strchr(line, ';')) line[strchr(line, ';') - line] = '\0';
    /* trim trailing whitespace */
    while (line[strlen(line) - 1] == ' ' || line[strlen(line) - 1] == '\t') line[strlen(line) - 1] = '\0';
    if (strncasecmp(line, "device", 6) == 0) {
	line += 6;
	while (*line == '\t' || *line == ' ') line++;
	strncpy(devlink, line, 64);
	printlog(LOG_INFO, "Setting device = '%s'\n", devlink);
    }
    if (strncasecmp(line, "owner", 5) == 0) {
	line += 5;
	while (*line == '\t' || *line == ' ') line++;
	strncpy(devowner, line, 64);
	printlog(LOG_INFO, "Setting owner = '%s'\n", devowner);
    }
    if (strncasecmp(line, "mode", 4) == 0) {
	line += 4;
	while (*line == '\t' || *line == ' ') line++;
	strncpy(devmode, line, 64);
	printlog(LOG_INFO, "Setting mode = '%s'\n", devmode);
    }
    if (strncasecmp(line, "port", 4) == 0) {
	line += 4;
	while (*line == '\t' || *line == ' ') line++;
	port = atoi(line);
	printlog(LOG_INFO, "Setting port = %d\n", port);
    }
    if (strncasecmp(line, "refresh", 7) == 0) {
	line += 7;
	while (*line == '\t' || *line == ' ') line++;
	refreshreq = atoi(line);
	printlog(LOG_INFO, "Setting refresh = %d\n", refreshreq);
    }
    if (strncasecmp(line, "server", 6) == 0) {
	line += 6;
	while (*line == '\t' || *line == ' ') line++;
	strncpy(server, line, 64);
	printlog(LOG_INFO, "Setting server = '%s'\n", server);
    }
    if (strncasecmp(line, "peername", 8) == 0) {
	line += 8;
	while (*line == '\t' || *line == ' ') line++;
	strncpy(regpeer, line, 64);
	printlog(LOG_INFO, "Setting peername = '%s'\n", regpeer);
    }
    if (strncasecmp(line, "secret", 6) == 0) {
	line += 6;
	while (*line == '\t' || *line == ' ') line++;
	strncpy(regsecret, line, 64);
	printlog(LOG_INFO, "Setting secret = '%s'\n", regsecret);
    }
    if (strncasecmp(line, "cidname", 7) == 0) {
	line += 7;
	while (*line == '\t' || *line == ' ') line++;
	strncpy(cidname, line, 64);
	printlog(LOG_INFO, "Setting cidname = '%s'\n", cidname);
    }
    if (strncasecmp(line, "cidnumber", 9) == 0) {
	line += 9;
	while (*line == '\t' || *line == ' ') line++;
	strncpy(cidnumber, line, 64);
	printlog(LOG_INFO, "Setting cidnumber = '%s'\n", cidnumber);
    }
    if (strncasecmp(line, "codec", 5) == 0) {
	line += 5;
	while (*line == '\t' || *line == ' ') line++;
	if (strcasecmp(line, "slinear") == 0) {
	    codecreq = AST_FORMAT_SLINEAR;
	    printlog(LOG_INFO, "Setting codec = slinear\n");
	} else if (strcasecmp(line, "ulaw") == 0) {
	    codecreq = AST_FORMAT_ULAW;
	    printlog(LOG_INFO, "Setting codec = ulaw\n");
	} else if (strcasecmp(line, "alaw") == 0) {
	    codecreq = AST_FORMAT_ALAW;
	    printlog(LOG_INFO, "Setting codec = alaw\n");
	}
    }
    if (strncasecmp(line, "record", 6) == 0) {
	record = 1;
	printlog(LOG_INFO, "Enabling record\n");
    }
    if (strncasecmp(line, "replay", 6) == 0) {
	replay = 1;
	printlog(LOG_INFO, "Enabling replay\n");
    }
    if (strncasecmp(line, "nojitterbuffer", 12) == 0) {
	nojitterbuffer = 1;
	printlog(LOG_INFO, "Disabling jitterbuffer\n");
    }
    if (strncasecmp(line, "iax2debug", 9) == 0) {
	iax2debug = 1;
	printlog(LOG_INFO, "Enabling IAX2 debugging\n");
    }
    if (strncasecmp(line, "dspdebug", 8) == 0) {
	dspdebug = 1;
	printlog(LOG_INFO, "Enabling DSP debugging\n");
    }
    if (strncasecmp(line, "nodaemon", 8) == 0) {
	nodaemon = 1;
	printlog(LOG_INFO, "This modem is exempt from daemon use.\n");
    }
    if (strncasecmp(line, "skew", 4) == 0) {
	line += 4;
	while (*line == '\t' || *line == ' ') line++;
	defskew = atoi(line);
	printlog(LOG_INFO, "Setting skew = %d\n", defskew);
    }
    if (strncasecmp(line, "audiobuflen", 11) == 0) {
	line += 11;
	while (*line == '\t' || *line == ' ') line++;
	audiobuflen = atoi(line);
	printlog(LOG_INFO, "Setting audiobuflen = %d\n", audiobuflen);
    }
}

void
readconfig(const char *name)
{
    if (!strchr(name, '%')) {
	char filename[256];
	snprintf(filename, sizeof(filename), "/etc/iaxmodem/%s", name);
	int fd = open(filename, O_RDONLY);
	if (fd < 0) {
	    printlog(LOG_ERROR, "Cannot open %s.\n", filename);
	    cleanup(-1);
	}
	int cc, pos = 0;
	char line[1024];
	memset(line, 0, 1024);
	while ((cc = read(fd, &line[pos], 1))) {
	    if (line[pos] == '\n' || pos == 1023) {
		line[pos] = '\0';
		setconfigline(line);
		memset(line, 0, 1024);
		pos = 0;
	    } else pos++;
	}
	if (strlen(line)) {
	    setconfigline(line);
	}
	if (replay && record) {
	    record = 0;
	    printlog(LOG_INFO, "Disabling record\n");
	}
	close(fd);
    }
}

int
at_tx_handler(at_state_t *s, void *user_data, const uint8_t *buf, size_t len)
{
    ssize_t wrote = write(amaster, buf, len);
    if (wrote != len) {
	printlog(LOG_ERROR, "Unable to pass the full buffer onto the device file. %zd bytes of %d written: %s\n",
		 wrote, len, strerror(errno));
	if (wrote == -1) wrote = 0;	/* nothing was written */
	/*
	  The pty has a limited buffer size. (On my system it's 4095 bytes.)
	  Being here tells us that the pty buffer is full, and that generally
	  indicates that nothing is reading the tty on the other end.  In
	  order to make room for future writes we need to flush the pty
	  buffer.
	*/
	if (tcflush(amaster, TCOFLUSH)) {
	    printlog(LOG_ERROR, "Unable to flush pty master buffer: %s\n", strerror(errno));
	} else if (tcflush(aslave, TCOFLUSH)) {
	    printlog(LOG_ERROR, "Unable to flush pty slave buffer: %s\n", strerror(errno));
	} else {
	    printlog(LOG_INFO, "Successfully flushed pty buffer\n");
	}
    }
    return wrote;
}

static int
t31_call_control_handler(t31_state_t *s, void *user_data, int op, const char *num)
{
    int info = 0;
    switch (op) {
	case AT_MODEM_CONTROL_CALL:
	    /* Dialing */
	    if (modemstate == MODEM_CONNECTED && num[0] == '\0') {	// play CNG for an off-hook call
		t31_call_event(s, AT_CALL_EVENT_CONNECTED);
		break;
	    }
	    if (modemstate != MODEM_ONHOOK && modemstate != MODEM_OFFHOOK) return -1;
	    if (!strchr(num, '%')) {
		session[0] = iax_session_new();
		char *nnum = strndup(num, 256);
		char *nnump = strchr(nnum, ',');
		if (nnump) {
		    dialextra = strndup(nnump, 64);
		    nnump[0] = '\0';
		}
		printlog(LOG_INFO, "Dialing '%s'\n", nnum);
		char ich[256];
		snprintf(ich, sizeof(ich), "%s:%s@%s/%s", regpeer, regsecret, server, nnum);
		iax_call(session[0], cidnumber, cidname, ich, NULL, 0, codecreq, CODEC_SUPPORT);
		modemstate = MODEM_CALLING;
	    }
	    break;
	case AT_MODEM_CONTROL_OFFHOOK:
	    /* Take receiver off-hook (busy-out) */
	    if (modemstate == MODEM_ONHOOK) {
		printlog(LOG_INFO, "Taking receiver off-hook.\n");
		modemstate = MODEM_OFFHOOK;
		break;
	    } else if (modemstate != MODEM_RINGING) {
		return -1;
	    }
	    info = 1;		// indicator to not play CED
	    /* pass a ringing modem through... */
	case AT_MODEM_CONTROL_ANSWER:
	    /* Answering */
	    if (modemstate == MODEM_CONNECTED) {	// play CED for an off-hook call
		t31_call_event(s, AT_CALL_EVENT_ANSWERED);
		break;
	    }
	    if (modemstate != MODEM_RINGING) return -1;
	    printlog(LOG_INFO, "Answering\n");

	    /* Unset V.24 Circuit 125, "ring indicator". */
	    int tioflags = 0;
	    ioctl(aslave, TIOCMGET, &tioflags);
	    tioflags &= ~TIOCM_RI;
	    ioctl(aslave, TIOCMSET, &tioflags);

	    iax_answer(session[0]);
	    if (!info) t31_call_event(s, AT_CALL_EVENT_ANSWERED);
	    modemstate = MODEM_CONNECTED;
	    gettimeofday(&nextaudio, NULL);
	    nextaudio.tv_usec += (VOIP_PACKET_LENGTH + skew);
	    if (nextaudio.tv_usec >= 1000000) {
		nextaudio.tv_sec += 1;
		nextaudio.tv_usec -= 1000000;
	    }
	    if (record) {
		if (dspaudiofd > 0) {
		    close(dspaudiofd);
		    rename(dspnowaudiofile, dspaudiofile);
		}
		if (iaxaudiofd > 0) {
		    close(iaxaudiofd);
		    rename(iaxnowaudiofile, iaxaudiofile);
		}
		dspaudiofd = open(dspnowaudiofile, O_WRONLY|O_CREAT, 00660);
		iaxaudiofd = open(iaxnowaudiofile, O_WRONLY|O_CREAT, 00660);
	    } else if (replay) {
		if (dspaudiofd > 0) close(dspaudiofd);
		if (iaxaudiofd > 0) close(iaxaudiofd);
		dspaudiofd = open(dspaudiofile, O_RDONLY);
		iaxaudiofd = open(iaxaudiofile, O_RDONLY);
	    }
	    break;
	case AT_MODEM_CONTROL_HANGUP:
	    /* Hang up */
	    printlog(LOG_INFO, "Hanging Up\n");
	    if (record) {
		if (dspaudiofd > 0) {
		    close(dspaudiofd);
		    rename(dspnowaudiofile, dspaudiofile);
		}
		if (iaxaudiofd > 0) {
		    close(iaxaudiofd);
		    rename(iaxnowaudiofile, iaxaudiofile);
		}
	    } else if (replay) {
		if (dspaudiofd > 0) close(dspaudiofd);
		if (iaxaudiofd > 0) close(iaxaudiofd);
	    }
	    dspaudiofd = -1;
	    iaxaudiofd = -1;
	    gettimeofday(&lasthangup, NULL);
	    if (phonestate != PHONE_FREED) {
		if (modemstate == MODEM_RINGING && phonestate == PHONE_CONNECTED ) {
		    /* Asterisk gets a little confused if we hangup an IAX call that  */
		    /* we never answered, resulting in congestion, rather than a      */
		    /* hangup, on the asterisk side.                                  */
		    iax_answer(session[0]);
		    sleep(1);	// apparently necessary or the answer can be ignored
		}
		iax_hangup(session[0], "Normal disconnect");
		iax_destroy(session[0]);
		phonestate = PHONE_FREED;
		if (gothup) sighandler(SIGHUP);
	    }
	    modemstate = MODEM_ONHOOK;
	    break;
	case AT_MODEM_CONTROL_CTS:
	    {
		if (num) {
		    u_char xon[1];
		    xon[0] = 0x11;
		    at_tx_handler(&s->at_state, NULL, xon, 1);
		    // printtime(); printf("XON\n");
		} else {
		    u_char xoff[1];
		    xoff[0] = 0x13;
		    at_tx_handler(&s->at_state, NULL, xoff, 1);
		    // printtime(); printf("XOFF\n");
		}
	    }
	    break;
	case AT_MODEM_CONTROL_SETID:
	    /* Set Caller*ID */
	    {
		char id[65];
		strncpy(id, num, 64);
		id[64] = '\0';
		char *marker = (strstr(id, "\",\""));
		if (!marker) {
		    cidname[0] = '\0';
		    strcpy(cidnumber, id);
		} else {
		    char *fquote = strchr(id, '"');
		    char *lquote = strrchr(id, '"');
		    int len = marker - fquote - 1;
		    if (len < 0) len = 0;
		    strncpy(cidname, fquote + 1, len);
		    cidname[len] = '\0';
		    len = lquote - marker - 3;
		    if (len < 0) len = 0;
		    strncpy(cidnumber, marker + 3, len);
		    cidnumber[len] = '\0';
		}
	    }
	    break;
	case AT_MODEM_CONTROL_RNG:
	    /* Ring indicator, ignore it */
	    break;
	case AT_MODEM_CONTROL_ONHOOK:
	    /* Hangup indicator, ignore it */
	    break;
	default:
	    printlog(LOG_ERROR, "Unknown DSP control handler: %d\n", op);
	    break;
    }
    return 0;
}

void
simulateHangup(t31_state_t *s)
{
    /*
     * End-of-file was reached.  Simulate hangup.
     */
    printlog(LOG_INFO, "Reached end of recording to replay.  Simulating hangup.\n");
    phonestate = PHONE_FREED;
    modemstate = MODEM_ONHOOK;
    t31_call_event(s, AT_CALL_EVENT_HANGUP);
    iax_hangup(session[0], "Normal disconnect");
    if (gothup) sighandler(SIGHUP);
}

void
iaxmodem(const char *config, int nondaemon)
{
    /*
     * IAXmodem is started as root, but drops privileges once the
     * required superuser tasks are performed.
     */
    uid_t modem_uid;
    gid_t modem_gid;
    struct passwd *pwent;
    int fd;
    char logfile[256];

    printlog(LOG_ERROR, "Modem started\n");

    pwent = getpwnam("uucp");

    if (pwent == NULL) {
	printlog(LOG_ERROR, "Fatal error: uucp user not found in passwd file\n");
	_exit(-1);
    }

    modem_uid = pwent->pw_uid;
    modem_gid = pwent->pw_gid;

    strcpy(devlink, "/dev/iaxmodem");
    strcpy(server, "127.0.0.1");
    strcpy(regpeer, "");
    strcpy(cidname, "IAXmodem");
    strcpy(cidnumber, "");

    if (!nondaemon) {
	/* We don't read it, so close stdin. */
	close(STDIN_FILENO);

	/* Redirect stdout to /dev/null. */
	fd = open("/dev/null", O_WRONLY);
	if (fd < 0) {
	    printlog(LOG_ERROR, "Error: could not open /dev/null: %s\n", strerror(errno));
	} else {
	    dup2(fd, STDOUT_FILENO);
	}
	close(fd);

	/* Redirect stderr to the log file */
	snprintf(logfile, sizeof(logfile), "/var/log/iaxmodem/%s", config);
	checklog(logfile);
	fd = open(logfile, O_WRONLY | O_APPEND | O_CREAT | O_LARGEFILE, logmode);
	if (fd < 0) {
	    printlog(LOG_ERROR, "Error: could not open %s: %s\n", logfile, strerror(errno));
	} else {
	    dup2(fd, STDERR_FILENO);
	}
	close(fd);
    }

    readconfig(config);

    if (!nondaemon && nodaemon) {
	/* This modem is exempted from daemon use. */
	_exit(0);
    }

    gothup = 0;
    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);
    signal(SIGHUP, sighandler_hup);
    signal(SIGALRM, sighandler_alarm);

    int iaxnetfd;
    struct iax_event *iaxevent = 0;
    session[0] = NULL;
    session[1] = NULL;
    struct timeval lastreg, lastring, lastdtedata;
    gettimeofday(&lastdtedata, NULL);
    lastdtedata.tv_sec -= 60;
    gettimeofday(&lastreg, NULL);
    lastreg.tv_sec -= (refreshreq > refresh ? (refreshreq+1) : (refresh+1));
    gettimeofday(&lastring, NULL);
    lastring.tv_sec -= 60;
    gettimeofday(&lasthangup, NULL);
    lasthangup.tv_sec -= 5;
    char modembuf[DSP_BUFSIZE];
    uint8_t dspbuf[sizeof(int16_t)*VOIP_PACKET_SIZE];
    int16_t iaxbuf[VOIP_PACKET_SIZE];
    static t31_state_t t31_state;
    int t31buflen;
    int tioflags = 0;
    struct group *grent;
    char *devgroup;
    char *pmode;
    uid_t devuid = 0;
    gid_t devgid = 0;
    int dmode = 0;
    int i;
    unsigned int last_ts = 0;

    int16_t audiobuf[VOIP_PACKET_SIZE*audiobuflen];
    int16_t* audiobufpos = audiobuf;
    memset(audiobuf, (int16_t) 0, sizeof(int16_t)*VOIP_PACKET_SIZE*audiobuflen);

    snprintf(dspnowaudiofile, sizeof(dspnowaudiofile), "/tmp/%s-dsp.raw.recording", regpeer);
    snprintf(iaxnowaudiofile, sizeof(iaxnowaudiofile), "/tmp/%s-iax.raw.recording", regpeer);
    snprintf(dspaudiofile, sizeof(dspaudiofile), "/tmp/%s-dsp.raw", regpeer);
    snprintf(iaxaudiofile, sizeof(iaxaudiofile), "/tmp/%s-iax.raw", regpeer);

    /* Get the uid/gid corresponding to devowner */
    devgroup = strchr(devowner, ':');
    if (devgroup != NULL) {
	*devgroup = '\0';
	devgroup++;
	grent = getgrnam(devgroup);
	if (grent == NULL) {
	    printlog(LOG_ERROR, "Error: %s group not found in group file, using root instead\n", devgroup);
	} else {
	    devgid = grent->gr_gid;
	}
    } else {
	printlog(LOG_ERROR, "Error: group unspecified, using root instead\n");
    }
    pwent = getpwnam(devowner);
    if (pwent == NULL) {
	printlog(LOG_ERROR, "Error: %s user not found in passwd file, using root instead\n", devowner);
    } else {
	devuid = pwent->pw_uid;
    }

    /* Compute the mode */
    pmode = NULL;
    if (strlen(devmode) != 4) {
	if (strlen(devmode) != 3) {
	    printlog(LOG_ERROR, "Error: invalid mode string (%s) ? Leaving default modes on %s\n", devmode, devlink);
	} else {
	    pmode = devmode;
	}
    } else {
	/* If mode is specified as 0664 for instance, skip the first value (special modes) */
	pmode = devmode + 1;
    }

    if (pmode != NULL) {
	for (i = 0; i < 3; i++) {
	    if ((!isdigit(pmode[i])) || (pmode[i] > '6')) {
		dmode = 0;
		printlog(LOG_ERROR, "Error: invalid mode string (%s), leaving default modes on %s\n", devmode, devlink);
		break;
	    }
	    /* Mode is specified as an octal string representing the bitfield, like 664 */
	    /*      convert to numeric   shift 3 bits */
	    dmode |= (pmode[i] - '0') << (3 * (2 - i));
	}
    }

#ifndef USE_UNIX98_PTY
    if (openpty(&amaster, &aslave, NULL, NULL, NULL)) {
        printlog(LOG_ERROR, "Fatal error: failed to initialize pty\n");
        cleanup(-1);
    }

    char *stty = ttyname(aslave);
    if (stty == NULL) {
      printlog(LOG_ERROR, "Fatal error: failed to obtain slave pty filename\n");
      cleanup(-1);
    }
    stty = strdup(stty);

#else /* USE_UNIX98_PTY */
#ifdef SOLARIS
    amaster = open("/dev/ptmx", O_RDWR);
#else
    amaster = posix_openpt(O_RDWR | O_NOCTTY);
#endif
    if (amaster < 0) {
      printlog(LOG_ERROR, "Fatal error: failed to initialize UNIX98 master pty\n");
      cleanup(-1);
    }

    if (grantpt(amaster) < 0) {
      printlog(LOG_ERROR, "Fatal error: failed to grant access to slave pty\n");
      cleanup(-1);
    }

    if (unlockpt(amaster) < 0) {
      printlog(LOG_ERROR, "Fatal error: failed to unlock slave pty\n");
      cleanup(-1);
    }

    char *stty = ptsname(amaster);
    if (stty == NULL) {
      printlog(LOG_ERROR, "Fatal error: failed to obtain slave pty filename\n");
      cleanup(-1);
    }
    stty = strdup(stty);

    aslave = open(stty, O_RDWR);
    if (aslave < 0) {
      printlog(LOG_ERROR, "Fatal error: failed to open slave pty %s\n", stty);
      cleanup(-1);
    }
#ifdef SOLARIS
    ioctl(aslave, I_PUSH, "ptem");	/* push ptem */
    ioctl(aslave, I_PUSH, "ldterm");	/* push ldterm*/
#endif
#endif /* !USE_UNIX98_PTY */

    printlog(LOG_INFO, "Opened pty, slave device: %s\n", stty);
    if (!unlink(devlink)) {
	printlog(LOG_ERROR, "Removed old %s\n", devlink);
    }

    int ret = symlink(stty, devlink);
    free(stty);
    if (ret < 0) {
	printlog(LOG_ERROR, "Fatal error: failed to create %s symbolic link\n", devlink);
	cleanup(-1);
    }
    printlog(LOG_INFO, "Created %s symbolic link\n", devlink);

    if (dmode == 0) {
	printlog(LOG_ERROR, "Error: mode is 0, leaving default permissions\n");
    } else {
	if (fchown(aslave, devuid, devgid)) {
	    printlog(LOG_ERROR, "Error: cannot set tty owner to %s:%s: %s\n", devowner, devgroup, strerror(errno));
	}
    }

    if (fchmod(aslave, dmode)) {
	printlog(LOG_ERROR, "Error: cannot set tty mode to %s: %s\n", devmode, strerror(errno));
    }

    if (fcntl(amaster, F_SETFL, fcntl(amaster, F_GETFL, 0) | O_NONBLOCK)) {
	printlog(LOG_ERROR, "Cannot set up non-blocking read on %s\n", ttyname(amaster));
	cleanup(-1);
    }

    /* Root privileges not needed anymore, drop privs. */
    setegid(modem_gid);
    seteuid(modem_uid);
    
    if ((port = iax_init(port) < 0)) {
	printlog(LOG_ERROR, "Fatal error: failed to initialize iax with port %d\n", port);
	cleanup(-1);
    }
    iaxnetfd = iax_get_fd();

    /*
     * Disabling the jitterbuffer shouldn't be necessary, and in fact it probably
     * would be even useful if it worked right, but alas, enabling the jitterbuffer
     * seems to only make things worse.
     */
    iax_disable_jitterbuffer();

    /*
     * We disable debugging because the screen writes can slow things down.
     */
    if (iax2debug) {
	iax_enable_debug();
    } else {
	iax_disable_debug();
    }

    if (t31_init(&t31_state, at_tx_handler, NULL, t31_call_control_handler, NULL, NULL, NULL) < 0) {
	printlog(LOG_ERROR, "Cannot initialize the T.31 modem\n");
	cleanup(-1);
    }

    /*
     * Again, debugging should be minimal for normal use as the writes slow things too much.
     */
    if (dspdebug) {
	int loglevel = SPAN_LOG_SEVERITY_MASK | SPAN_LOG_SHOW_DATE | SPAN_LOG_SHOW_SEVERITY | 
	    SPAN_LOG_SHOW_PROTOCOL | SPAN_LOG_SHOW_VARIANT | SPAN_LOG_SHOW_TAG;
	t31_state.logging.level = loglevel;
	t31_state.at_state.logging.level = loglevel;
	t31_state.audio.modems.fast_modems.v17_rx.logging.level = loglevel;
	t31_state.audio.modems.fast_modems.v29_rx.logging.level = loglevel;
	t31_state.audio.modems.fast_modems.v27ter_rx.logging.level = loglevel;
    }

    int selectfd, selectretval, selectblock, avail, audiobalance = 0;
    skew = defskew;
    struct timeval tv;
    fd_set select_rfds;
    for (;;) {
	gettimeofday(&now, NULL);
	/*
	 * Here we stop and wait for some socket or device activity or for some needed
	 * action such as DSP audio transmission, a follow-up RING message, a registration
	 * renewal, or an expected IAX event.  (The last item here because sometimes
	 * iax_get_event returns data even when no socket activity occurred.)
	 *
	 * When we're in a connected state so much activity occurs on the device node
	 * and on the IAX socket that we'd loop too fast (using too much CPU) in trying
	 * to address it all.  As we loop "rather rapidly" anyway (every 20 ms) during
	 * the connected state, and as that's fast enough to address their needs,
	 * we ignore that activity during the connected state and rather focus on the
	 * audio frame timings instead.
	 *
	 * The transmisison of audio frames must be based on a reliable clock, and ideally
	 * that clock would be the same between the two IAX2 endpoints (the same system).
	 * However, even then the "window" value can tend to make audio transmission happen
	 * ever so slightly faster than audio reception.  Also, resource consumption and
	 * other things can slow us down.  Thus, the "clocking" needs to be slightly
	 * dynamic and must adjust quickly in time to try to keep the transmission and
	 * reception of audio frames at a 1-to-1 ratio.  We do this with the "skew", aiming
	 * to keep the "audiobalance" value somewhere between -5 and 5 (+/- 100 ms), 
	 * ideally at zero (which would mean a perfect 1-to-1 ratio of rx vs tx frames).
	 */
	selectfd = 1, selectretval = -1, selectblock = 0;
	FD_ZERO(&select_rfds);
	if (modemstate != MODEM_CONNECTED) {
	    FD_SET(amaster, &select_rfds);
	    FD_SET(iaxnetfd, &select_rfds);
	    selectfd = iaxnetfd > amaster ? iaxnetfd + 1 : amaster + 1;
	}
	if (modemstate == MODEM_CONNECTED) {
	    tv.tv_sec = 0;
	    tv.tv_usec = timediff(nextaudio, now) - window;
	} else if (modemstate == MODEM_RINGING) {
	    tv.tv_sec = 5; tv.tv_usec = 0;	/* ring every 5 seconds */
	    timeradd(&lastring, &tv, &tv);
	    timersub(&tv, &now, &tv);
	    if (tv.tv_sec > 100 || tv.tv_sec < -100) {
		/* This is a silly result.  The clock must have skipped. */
		printlog(LOG_INFO, "Clock skip detected in ringing (%d seconds).  Compensating.\n", tv.tv_sec);
		lastring = now;
		tv.tv_sec = 5;
		tv.tv_usec = 0;
	    }
	} else if (modemstate == MODEM_CALLING) {
	    tv.tv_sec = (t31_state.at_state.p.s_regs[7] + 5); 	/* give up after S7+5 seconds */
	    tv.tv_usec = 0;
	    timeradd(&lastdtedata, &tv, &tv);
	    timersub(&tv, &now, &tv);
	    if (tv.tv_sec > 500 || tv.tv_sec < -500) {
		/* This is a silly result.  The clock must have skipped. */
		lastdtedata = now;
		printlog(LOG_INFO, "Clock skip detected in DTE communication (%d seconds).  Compensating.\n", tv.tv_sec);
		tv.tv_sec = (t31_state.at_state.p.s_regs[7] + 5);
		tv.tv_usec = 0;
	    }
	} else if (refreshreq) {
	    /* Handle maintaining registration every refresh - 5 seconds. */
	    tv.tv_sec = refresh - 5; tv.tv_usec = 0;
	    timeradd(&lastreg, &tv, &tv);
	    timersub(&tv, &now, &tv);
	    if (tv.tv_sec > 10000 || tv.tv_sec < -10000) {
		/* This is a silly result.  The clock must have skipped. */
		printlog(LOG_INFO, "Clock skip detected in IAX2 registration (%d seconds).  Compensating.\n", tv.tv_sec);
		lastreg = now;
		tv.tv_sec = refresh - 5;
		tv.tv_usec = 0;
	    }
	} else if (iax_time_to_next_event() >= 0) {
	    /* The actual calculations will be done later. */
	    tv.tv_sec = 60; tv.tv_usec = 0;
	} else {
	    /* select may block, no time-based activity */
	    tv.tv_sec = 0; tv.tv_usec = 0;
	    selectblock = 1;
	}
	/*
	 * Ending up here with a slightly negative tv during a connected call will
	 * not be uncommon.  We have to send voice frames rather rapidly (20 ms
	 * apart); if the processing of this loop took that much time or
	 * slightly more to perform, then we'll end up with a negative tv
	 * here.  As long as most loops do not lag like this then there is not
	 * a problem as the "drift" is corrected in the following iterations 
	 * by maintaining a focus on the fixed schedule of sending voice frames.
	 *
	 * The select call here is intended to "throttle" our looping.  However,
	 * in cases where tv is zero or negative then there is no throttle.  This
	 * presumes, then, that in those cases the rest of the loop has served as
	 * the throttle for that iteration.
	 */
	if ((tv.tv_sec == 0 && tv.tv_usec > 0) || (tv.tv_sec > 0 && tv.tv_sec < 3600)) {
	    int nexteventms = iax_time_to_next_event();
	    /*
	     * iax_time_to_next_event will return 0 when a response is "pending",
	     * like an ACK.  These responses are not extremely time sensitive in
	     * comparison to the rest of the things we do, so in order to prevent
	     * fast looping solely for these "pending" events we prevent nexteventms
	     * from causing fast looping.  During a call, we pace our looping with
	     * tv, so our "sluggishness" here shouldn't affect audio.
	     *
	     * iax_time_to_next_event will return -1 when there are no pending events,
	     * thus nexteventms less than zero are ignored.
	     */
	    if (nexteventms >= 0 && nexteventms < 2) nexteventms = 2;
	    if (nexteventms > 0 && nexteventms < tv.tv_sec * 1000 + tv.tv_usec / 1000) {
		/* Handle expected IAX events perhaps not triggered by IAX socket activity. */
		tv.tv_sec = nexteventms / 1000;
		tv.tv_usec = (nexteventms % 1000) * 1000;
	    }

	    selectretval = select(selectfd, &select_rfds, NULL, NULL, &tv);
	} else if (selectblock) {
	    /*
	     * There is no time-based activity to worry about.  Just watch the fds.
	     */
	    selectretval = select(selectfd, &select_rfds, NULL, NULL, NULL);
	} else {
	    /*
	     * We're behind schedule, but in testing this doesn't seem to be a problem.
	     *
	     * We still need to run select in order to flag the select_rfds.
	     */
	    tv.tv_sec = 0; tv.tv_usec = 0;
	    selectretval = select(selectfd, &select_rfds, NULL, NULL, &tv);
	}
	gettimeofday(&now, NULL);

	/*
	 * Is it time to send more audio?  (This comes first for a reason, as it's our priority.)
	 */
	if (modemstate == MODEM_CONNECTED && timediff(nextaudio, now) <= window) {
	    nextaudio.tv_usec += (VOIP_PACKET_LENGTH + skew);
	    if (nextaudio.tv_usec >= 1000000) {
		nextaudio.tv_sec += 1;
		nextaudio.tv_usec -= 1000000;
	    }
	    memset(dspbuf, (int16_t) 0, sizeof(int16_t)*VOIP_PACKET_SIZE);
	    t31buflen = 0;
	    int gotlen = 0;
	    if (!dialextra) {
		do {
		    /*
		     * If t31_tx returns less than VOIP_PACKET_SIZE but not zero then it means we're switching
		     * carriers.  If it returns zero, then it means we're silent.
		     */
		    gotlen = t31_tx(&t31_state, (int16_t *) (dspbuf + sizeof(int16_t)*t31buflen), VOIP_PACKET_SIZE - t31buflen);
		    t31buflen += gotlen;
		} while (t31buflen < VOIP_PACKET_SIZE && gotlen > 0 && modemstate == MODEM_CONNECTED);
	    }
	    if (record) write(dspaudiofd, (uint8_t *) dspbuf, VOIP_PACKET_SIZE*sizeof(int16_t));
	    if (replay) {
		if (read(dspaudiofd, (uint8_t *) dspbuf, VOIP_PACKET_SIZE*sizeof(int16_t)) == 0) {
		    simulateHangup(&t31_state);
		}
	    }
	    if (modemstate == MODEM_CONNECTED) {	/* t31_tx can change modemstate */
		audiobalance++;
		if (audiobalance > 5) {
		    /*
		     * We seem to be getting ahead in our audio transmissions, relative
		     * to the audio receptions.  So we lengthen the time between transmissions
		     * (by increasing our perception of VOIP_PACKET_LENGTH), hoping to improve the ratio.
		     *
		     * We are deliberately less-sensitive to getting ahead than we are to getting behind
		     * because having a little bit of buffer in-front of us is better than not.
		     */
		    audiobalance = 0;	/* don't try to make up */
		    skew += 10;
		    printlog(LOG_INFO, "Adjusting skew to %d.\n", skew);
		}
		if (codec == AST_FORMAT_SLINEAR) {
		    orderbytes((int16_t *) dspbuf, VOIP_PACKET_SIZE);
		    iax_send_voice(session[0], AST_FORMAT_SLINEAR, (uint8_t *) dspbuf, VOIP_PACKET_SIZE*(sizeof(int16_t)), VOIP_PACKET_SIZE);
		} else {
		    unsigned char convertedbuf[VOIP_PACKET_SIZE];
		    int i = 0;
		    if (codec == AST_FORMAT_ULAW)
			for (; i < VOIP_PACKET_SIZE; i++) convertedbuf[i] = linear_to_ulaw(((int16_t *) dspbuf)[i]);
		    else
			for (; i < VOIP_PACKET_SIZE; i++) convertedbuf[i] = linear_to_alaw(((int16_t *) dspbuf)[i]);
		    iax_send_voice(session[0], codec, convertedbuf, VOIP_PACKET_SIZE, VOIP_PACKET_SIZE);
		}
	    }
	}
	/*
	 * Is there tty data coming in from the DTE?
	 *
	 * We don't overfill the buffer...
	 */
	avail = DSP_BUFSIZE - t31_state.tx.in_bytes + t31_state.tx.out_bytes - 1;
	if (avail < 0) {
	    avail = 0;
	    printlog(LOG_INFO, "strange... tx.in_bytes: %d, tx.out_bytes: %d, DSP_BUFSIZE: %d\n", t31_state.tx.in_bytes, t31_state.tx.out_bytes, DSP_BUFSIZE);
	} else if (avail > DSP_BUFSIZE) {
	    avail = DSP_BUFSIZE;
	    printlog(LOG_INFO, "strange... tx.in_bytes: %d, tx.out_bytes: %d, DSP_BUFSIZE: %d\n", t31_state.tx.in_bytes, t31_state.tx.out_bytes, DSP_BUFSIZE);
	}
	if ((modemstate != MODEM_CONNECTED && selectretval && FD_ISSET(amaster, &select_rfds)) ||
	    (modemstate == MODEM_CONNECTED && !t31_state.tx.holding && avail)) {
	    ssize_t len;
	    do {
		len = read(amaster, modembuf, 1);
		if (len > 0) {
		    int taken = t31_at_rx(&t31_state, modembuf, len);
		    if (taken != len) {
			/* As we checked the available buffer beforehand and only
			   read and sent that number of bytes, this should not
			   happen, and if it does will cause data loss and possibly
			   timing problems. */
			printlog(LOG_ERROR, "Unexpected modem buffering. Sent %zd bytes, modem buffered %d.\n", len, taken);
		    }
		    avail -= taken;
		    lastdtedata = now;
		}
	    } while (len > 0 && avail > 0);
	}
	/*
	 * Is it time to send another RING message to the DTE?
	 */
	if (modemstate == MODEM_RINGING && (lastring.tv_sec + 5 < now.tv_sec || (lastring.tv_sec + 5 == now.tv_sec && lastring.tv_usec <= now.tv_sec))) {
	    t31_call_event(&t31_state, AT_CALL_EVENT_ALERTING);
	    lastring = now;
	}
	/*
	 * Did the DCE timeout in sending CONNECT response?
	 * spandsp has an S-register (7) that is employed there.
	 * We merely offer a fail-over 5 seconds later if that 
	 * doesn't work
	 *
	 */
	if (modemstate == MODEM_CALLING && timediff(now, lastdtedata) >= (t31_state.at_state.p.s_regs[7] + 5) * 1000000) {
	    t31_call_event(&t31_state, AT_CALL_EVENT_NO_ANSWER);
	    /*
	     * One would think that this would be appropriate here:
	     *
	     * iax_hangup(session[0], "Give up");
	     *
	     * Yet, it is sometimes not.  It's probably a bug in libiax2, and is 
	     * triggered by an incoming call occurring simultaneous with an outgoing 
	     * one.  In any case, omitting it seems harmless, as iax_destroy seems
	     * to do the necessary job.
	     */
	    iax_destroy(session[0]);
	    phonestate = PHONE_FREED;
	    modemstate = MODEM_ONHOOK;
	    if (gothup) sighandler(SIGHUP);
	}
	/*
	 * Is there any IAX event that we should handle?
	 */
	if ((modemstate != MODEM_CONNECTED && selectretval && FD_ISSET(iaxnetfd, &select_rfds)) ||
	    modemstate == MODEM_CONNECTED || !iax_time_to_next_event()) {
	    int times = 0;	// avoid a bug in libiax2 where iax_get_event() always retrieves voice data even after a call ends
	    while (times++ < 100 && (iaxevent = iax_get_event(0))) {
		switch (iaxevent->etype) {
#ifdef IAX_EVENT_NULL
		    case IAX_EVENT_NULL:
			/* Just ignore it, per lib/libiax2/src/iax-client.h */
			break;
#endif
		    case IAX_EVENT_REGACK:
			printlog(LOG_INFO, "Registration completed successfully.\n");
			if (iaxevent->ies.refresh > 0) refresh = iaxevent->ies.refresh;
			regstate = REGISTERED;
			iax_destroy(session[1]);
			break;
		    case IAX_EVENT_REGREJ:
			printlog(LOG_ERROR, "Registration failed.\n");
			/* To prevent fast looping with registration-attempts, we leave regstate PENDING. */
			//regstate = UNREGISTERED;
			iax_destroy(session[1]);
			break;
		    case IAX_EVENT_TIMEOUT:
			if (regstate == PENDING) {
			    printlog(LOG_ERROR, "Registration timed out.\n");
			}
			break;
		    case IAX_EVENT_ACCEPT:
			phonestate = PHONE_CALLACCEPTED;
			printlog(LOG_INFO, "Call accepted.\n");
			codec = iaxevent->ies.format;
			last_ts = 0;
			break;
		    case IAX_EVENT_RINGA:
			phonestate = PHONE_RINGING;	/* meaning the server detected ringing on the channel */
			printlog(LOG_INFO, "Ringing heard.\n");
			break;
		    case IAX_EVENT_PONG:
			/* informative only */
			break;
		    case IAX_EVENT_ANSWER:
			/* the other side answered our call */
			phonestate = PHONE_ANSWERED;
			printlog(LOG_INFO, "Remote answered.\n");
			t31_call_event(&t31_state, AT_CALL_EVENT_CONNECTED);
			gettimeofday(&nextaudio, NULL);
			nextaudio.tv_usec += (VOIP_PACKET_LENGTH + skew);
			if (nextaudio.tv_usec >= 1000000) {
			    nextaudio.tv_sec += 1;
			    nextaudio.tv_usec -= 1000000;
			}
			modemstate = MODEM_CONNECTED;
			audiobalance = 0;
			audiobufpos = audiobuf;
			skew = defskew;
			if (record) {
			    if (dspaudiofd > 0) {
				close(dspaudiofd);
				rename(dspnowaudiofile, dspaudiofile);
			    }
			    if (iaxaudiofd > 0) {
				close(iaxaudiofd);
				rename(iaxnowaudiofile, iaxaudiofile);
			    }
			    dspaudiofd = open(dspnowaudiofile, O_WRONLY|O_CREAT, 00660);
			    iaxaudiofd = open(iaxnowaudiofile, O_WRONLY|O_CREAT, 00660);
			} else if (replay) {
			    if (dspaudiofd > 0) close(dspaudiofd);
			    if (iaxaudiofd > 0) close(iaxaudiofd);
			    dspaudiofd = open(dspaudiofile, O_RDONLY);
			    iaxaudiofd = open(iaxaudiofile, O_RDONLY);
			}
			commalen = t31_state.at_state.p.s_regs[8];	// initialize per modem's S-register
			sighandler_alarm(0);				// process dialextra
			break;
		    case IAX_EVENT_CONNECT:
			/* incoming call detected */
			if (phonestate != PHONE_FREED || modemstate != MODEM_ONHOOK || timediff(now, lasthangup) < 5000000) {
			    /* we can only handle one call at a time, enforce automatic post-hangup busy-out */
			    iax_reject(iaxevent->session, "Busy");
			    break;
			}
			codec = CODEC_SUPPORT;
			codec &= iaxevent->ies.format;
			if (!codec) {
			    codec = codecreq & iaxevent->ies.capability;
			    if (!codec) {
				codec = CODEC_SUPPORT;
				codec &= iaxevent->ies.capability;
				if (codec) {
				    /* we have some matching codec support, so just pick one */
				    if (codec & AST_FORMAT_SLINEAR) codec = AST_FORMAT_SLINEAR;
				    else if (codec & AST_FORMAT_ALAW) codec = AST_FORMAT_ALAW;
				    else codec = AST_FORMAT_ULAW;
				} else {
				    /* we cannot support the audio format */
				    iax_reject(iaxevent->session, "No matching codec support");
				    break;
				}
			    }
			}
			audiobufpos = audiobuf;
			audiobalance = 0;
			skew = defskew;
			last_ts = 0;
			phonestate = PHONE_CONNECTED;
			modemstate = MODEM_RINGING;
			lastring = now;
			session[0] = iaxevent->session;
			iax_accept(session[0], codec);
			iax_ring_announce(session[0]);

			/* Set V.24 Circuit 125, "ring indicator". */
			ioctl(aslave, TIOCMGET, &tioflags);
			tioflags |= TIOCM_RI;
			ioctl(aslave, TIOCMSET, &tioflags);
			printlog(LOG_INFO, "Incoming call connected %s, %s, %s.\n", iaxevent->ies.called_number, iaxevent->ies.calling_number, iaxevent->ies.calling_name);
			strftime(call_date, sizeof(call_date), "%m%d", localtime((time_t*) &now.tv_sec));
			strftime(call_time, sizeof(call_time), "%H%M", localtime((time_t*) &now.tv_sec));
			at_reset_call_info(&t31_state.at_state);
			at_set_call_info(&t31_state.at_state, "DATE", call_date);
			at_set_call_info(&t31_state.at_state, "TIME", call_time);
			at_set_call_info(&t31_state.at_state, "NAME", iaxevent->ies.calling_name);
			at_set_call_info(&t31_state.at_state, "NMBR", iaxevent->ies.calling_number);
			at_set_call_info(&t31_state.at_state, "ANID", iaxevent->ies.calling_ani);
			at_set_call_info(&t31_state.at_state, "USER", iaxevent->ies.username);
			at_set_call_info(&t31_state.at_state, "PASS", iaxevent->ies.password);
			at_set_call_info(&t31_state.at_state, "CDID", iaxevent->ies.called_context);
			at_set_call_info(&t31_state.at_state, "NDID", iaxevent->ies.called_number);
			t31_call_event(&t31_state, AT_CALL_EVENT_ALERTING);
			break;
		    case IAX_EVENT_BUSY:
			phonestate = PHONE_FREED;
			modemstate = MODEM_ONHOOK;
			t31_call_event(&t31_state, AT_CALL_EVENT_BUSY);
			iax_hangup(session[0], "Normal disconnect");
			if (gothup) sighandler(SIGHUP);
			break;
		    case IAX_EVENT_HANGUP:
			printlog(LOG_INFO, "Remote hangup. Cause: \"%s\" Code: %u\n", iaxevent->ies.cause ? iaxevent->ies.cause : "", iaxevent->ies.causecode);
			if (modemstate != MODEM_ONHOOK) {
			    if (modemstate == MODEM_CALLING)
				t31_call_event(&t31_state, AT_CALL_EVENT_BUSY);
			    t31_call_event(&t31_state, AT_CALL_EVENT_HANGUP);
			    modemstate = MODEM_ONHOOK;
			}
			phonestate = PHONE_FREED;
			gettimeofday(&lasthangup, NULL);
			if (record) {
			    if (dspaudiofd > 0) {
				close(dspaudiofd);
				rename(dspnowaudiofile, dspaudiofile);
			    }
			    if (iaxaudiofd > 0) {
				close(iaxaudiofd);
				rename(iaxnowaudiofile, iaxaudiofile);
			    }
			} else if (replay) {
			    if (dspaudiofd > 0) close(dspaudiofd);
			    if (iaxaudiofd > 0) close(iaxaudiofd);
			}
			dspaudiofd = -1;
			iaxaudiofd = -1;
			if (gothup) sighandler(SIGHUP);
			break;
		    case IAX_EVENT_CNG:
			/* pseudo-silence */
			memset(dspbuf, (int16_t) 0, sizeof(int16_t)*VOIP_PACKET_SIZE);
			if (replay) {
			    if (read(iaxaudiofd, (uint8_t *) dspbuf, VOIP_PACKET_SIZE*sizeof(int16_t)) == 0) {
				simulateHangup(&t31_state);
			    }
			}
			if (!dialextra && modemstate == MODEM_CONNECTED && t31_rx(&t31_state, (int16_t *) dspbuf, VOIP_PACKET_SIZE)) {
			    printlog(LOG_ERROR, "Error sending silence to DSP.\n");
			}
			if (record) write(iaxaudiofd, (uint8_t *) dspbuf, VOIP_PACKET_SIZE*sizeof(int16_t));
			break;
		    case IAX_EVENT_VOICE:
			/*
			 * Watch for IAX2 jitter... the 32-bit timestamp shouldn't ever "wrap" 
			 * unless we expect a single call to last more than a month.
			 */
			if (!nojitterbuffer && last_ts && abs((int)(iaxevent->ts - last_ts)) > 300  ) {
			    printlog(LOG_ERROR, "IAX2 jitter gap too big, syncing - last_ts: %u, ts: %u\n", last_ts, iaxevent->ts);
			    last_ts = iaxevent->ts;
			    break;
			}
			if (!nojitterbuffer && last_ts && iaxevent->ts <= last_ts) {
			    /*
			     * We've already sent audio corresponding to this time.
			     */
			    break;
			}
			if (!nojitterbuffer && last_ts && iaxevent->ts != last_ts + 20) {
			    printlog(LOG_ERROR, "IAX2 jitter - last_ts: %u, ts: %u\n", last_ts, iaxevent->ts);
			    int16_t fillbuf[VOIP_PACKET_SIZE];
			    memcpy(fillbuf, iaxbuf, VOIP_PACKET_SIZE*sizeof(int16_t));
			    while (last_ts + 20 < iaxevent->ts) {
				/*
				 * This audio packet skips ahead.  Oops, audio was lost.
				 * Compensate by repeating the last audio packet enough
				 * to fill in the missing time.
				 *
				 * It is important to make sure that we do not replace a
				 * period of non-silence with silence and that silence is
				 * not replaced with non-silence... in order to keep as
				 * syncrhonous with the remote as possible.
				 */
				if (replay) {
				    if (read(iaxaudiofd, fillbuf, VOIP_PACKET_SIZE*sizeof(int16_t)) == 0) {
					simulateHangup(&t31_state);
				    }
				}
				if (!dialextra && modemstate == MODEM_CONNECTED && t31_rx(&t31_state, fillbuf, VOIP_PACKET_SIZE)) {
				    printlog(LOG_ERROR, "Error sending %d samples of IAX make-up audio to DSP.\n", VOIP_PACKET_SIZE);
				}
				if (record) write(iaxaudiofd, fillbuf, VOIP_PACKET_SIZE*sizeof(int16_t));
				last_ts += 20;
				audiobalance--;
			    }
			}
			last_ts = iaxevent->ts;

			if (modemstate == MODEM_CONNECTED) {
			    audiobalance--;
			    if (audiobalance < -1) {
				/*
				 * We seem to be getting behind in our audio transmissions, relative
				 * to the audio receptions.  So we decrease our perception of VOIP_PACKET_LENGTH,
				 * hoping to improve the ratio.
				 *
				 * We are deliberately very sensitive to getting behind because in many cases
				 * this will result in missing audio (possibly an unintended carrier drop).
				 */
				audiobalance = 0;	/* don't try to make up */
				skew -= 10;
				printlog(LOG_INFO, "Adjusting skew to %d.\n", skew);
			    }
			}
			/* Here's an audio frame from the IAX server, send it to the DSP. */
			{
			    int samples = iaxevent->datalen;
			    int16_t convertedbuf[iaxevent->datalen];
			    int16_t* audiodata;
			    int i = 0;
			    switch (codec) {
				case AST_FORMAT_SLINEAR:
				    samples = iaxevent->datalen/sizeof(int16_t);
				    orderbytes((int16_t *) iaxevent->data, samples);
				    audiodata = (int16_t*) iaxevent->data;
				    break;
				case AST_FORMAT_ALAW:
				    for (; i < iaxevent->datalen; i++) convertedbuf[i] = alaw_to_linear((iaxevent->data)[i]);
				    audiodata = convertedbuf;
				    break;
				case AST_FORMAT_ULAW:
				    for (; i < iaxevent->datalen; i++) convertedbuf[i] = ulaw_to_linear((iaxevent->data)[i]);
				    audiodata = convertedbuf;
				    break;
				default:
				    samples = 0;
				    memset(convertedbuf, 0, iaxevent->datalen);
				    audiodata = convertedbuf;
				    printlog(LOG_ERROR, "Unknown codec!\n");
				    break;
			    }

			    /* Save this for our use in with jitter-fill. */
			    if (!nojitterbuffer)
				memcpy(iaxbuf, audiodata, samples >= VOIP_PACKET_SIZE ? VOIP_PACKET_SIZE*sizeof(int16_t) : samples*sizeof(int16_t));

			    /*
			     * During a fax session (the entire time the modem is off-hook) the modem should
			     * only be in command-mode for extemely brief periods of time (between commands).
			     * Whatever audio we send to the DSP while the modem is in command-mode is lost 
			     * and wasted.  Sometimes the DTE is slow between one command's result and the 
			     * sending of the subsequent command.  Buffer this audio to some extent and "replay" 
			     * it later to the DSP once we leave command-mode.
			     */
			    if (t31_state.at_state.at_rx_mode == AT_MODE_OFFHOOK_COMMAND ||
				t31_state.at_state.at_rx_mode == AT_MODE_ONHOOK_COMMAND) {
				/* Save this audio for later. */
				if ((audiobufpos + samples) > (audiobuf + VOIP_PACKET_SIZE*audiobuflen)) {
				    int16_t bufsamples = audiobufpos - (audiobuf + samples);
				    memmove(audiobuf, audiobuf+samples, bufsamples*sizeof(int16_t));
				    audiobufpos = audiobuf + bufsamples;
				}
				memcpy(audiobufpos, audiodata, samples*sizeof(int16_t));
				audiobufpos += samples;
				/* Use silence instead. */
				memset(audiodata, (int16_t) 0, samples*sizeof(int16_t));
			    } else {
				if (audiobufpos != audiobuf && !dialextra && modemstate == MODEM_CONNECTED) {
				    /* Deliver audio that was buffered while the modem was in command-mode. */
				    printlog(LOG_INFO, "DTE was slow.  Playing %ld samples of buffered audio.\n", audiobufpos-audiobuf);
				    if (replay) {
					if (read(iaxaudiofd, audiobuf, audiobufpos-audiobuf) == 0) {
					    simulateHangup(&t31_state);
					}
				    }
				    if (t31_rx(&t31_state, audiobuf, (audiobufpos-audiobuf)/sizeof(int16_t))) {
					printlog(LOG_ERROR, "Error sending %ld samples of buffered IAX audio to DSP.\n", audiobufpos-audiobuf);
				    }
				    if (record) write(iaxaudiofd, audiobuf, audiobufpos-audiobuf);
				    audiobufpos = audiobuf;
				}
				if (replay) {
				    if (read(iaxaudiofd, audiodata, samples*sizeof(int16_t)) == 0) {
					simulateHangup(&t31_state);
				    }
				}
				if (record) write(iaxaudiofd, audiodata, samples*sizeof(int16_t));
			    }
			    if (!dialextra && modemstate == MODEM_CONNECTED && t31_rx(&t31_state, audiodata, samples)) {
				printlog(LOG_ERROR, "Error sending %d samples of IAX audio to DSP.\n", samples);
			    }
			}
			break;
		    case IAX_EVENT_TRANSFER:
			last_ts = 0;
			audiobalance = 0;
			skew = defskew;
			printlog(LOG_INFO, "Call transfer occurred.\n");
			session[0] = iaxevent->session;
			break;
		    case IAX_EVENT_REJECT:
			printlog(LOG_INFO, "Rejected call.\n");
			if (modemstate != MODEM_ONHOOK) {
			    t31_call_event(&t31_state, AT_CALL_EVENT_NO_DIALTONE);
			    modemstate = MODEM_ONHOOK;
			}
			phonestate = PHONE_FREED;
			if (gothup) sighandler(SIGHUP);
			break;
		    case IAX_EVENT_DTMF:
			/* only report DTMF if the modem is in command-mode */
			if (t31_state.at_state.at_rx_mode == AT_MODE_OFFHOOK_COMMAND ||
			    t31_state.at_state.at_rx_mode == AT_MODE_ONHOOK_COMMAND) {
			    char dtmf[16];
			    snprintf(dtmf, sizeof(dtmf), "\r\nDTMF=%d\r\n", iaxevent->subclass - 48);
			    at_tx_handler(&t31_state.at_state, NULL, (u_char*) dtmf, strlen(dtmf));
			}
			printlog(LOG_INFO, "Received DTMF '%d'\n", iaxevent->subclass - 48);
			break;
		    default:
			printlog(LOG_ERROR, "Don't know what to do with IAX event %d.\n", iaxevent->etype);
			break;
		}
		iax_event_free(iaxevent);
	    }
	}
	/*
	 * Maintain registration.  Registrations use a different
	 * session than actual calls.
	 */
	if (refreshreq) {
	    if (now.tv_sec > lastreg.tv_sec + refresh || (now.tv_sec == lastreg.tv_sec + refresh && now.tv_usec > lastreg.tv_usec))
		regstate = UNREGISTERED;
	    if (regstate == UNREGISTERED || now.tv_sec > lastreg.tv_sec + refresh - 5 || (now.tv_sec == lastreg.tv_sec + refresh - 5 && now.tv_usec >= lastreg.tv_usec)) {
		/* refresh our registration */
		session[1] = iax_session_new();
		iax_register(session[1], server, regpeer, regsecret, refreshreq);
		lastreg = now;
		regstate = PENDING;
	    }
	}
    }
} 

struct modem *modem_for_config(const char *config) {
    struct modem *m;

    for (m = modems; (m != NULL) && (strcmp(m->config, config) != 0); m = m->next) /* just walking the list... */;
    return m;
}

struct modem *modem_for_pid(pid_t pid) {
    struct modem *m;

    for (m = modems; (m != NULL) && (m->pid != pid); m = m->next) /* just walking the list... */;
    return m;
}

void
add_modem(char *config)
{
    struct modem *nm;

    nm = malloc(sizeof(struct modem));

    nm->pid = 0;
    nm->last_started = 0;
    nm->config = strdup(config);
    nm->next = modems;
    modems = nm;

    numchild++;
}

void
remove_modem(struct modem **m)
{
    struct modem *n = (*m)->next;
    free((*m)->config);
    free(*m);
    *m = n;
    numchild--;
}

void
remove_all_modems()
{
    while (numchild) 
	remove_modem(&modems);
}

void start_modem(struct modem *m) {
    pid_t newpid = fork();
    if (newpid == 0) {
        /* child */
        const char *config=strdup(m->config);
	remove_all_modems();    /* Children don't need info about siblings */
        iaxmodem(config, 0);    /* Fax away */
        _exit(255);		/* shouldn't ever get here */
    } else if (newpid > 0) {
        /* parent */
        printlog(LOG_ERROR, "Process %d started for config %s.\n", newpid, m->config);
        m->pid = newpid;
        m->last_started = time(NULL);
    } else {
        /* failed */
        printlog(LOG_ERROR, "Error: no modem could be started for %s (fork failed: %s)\n", m->config, strerror(errno));
    }
}

void
respawn_modem(struct modem *m)
{
    if (time(NULL)-m->last_started < 5) {
        printlog(LOG_INFO, "Process for config file %s respawning too fast, not restarting.\n", m->config);
    } else {
        start_modem(m);
    }
}

void
wait_for_modems()
{
    pid_t pid;
    int status;

    /* Wait for any child to exit */
    while ((pid = waitpid(-1, &status, 0)) > 0) {
	struct modem *m = modem_for_pid(pid);
	if ((status & 0xff) == 0) {
	    printlog(LOG_ERROR, "iaxmodem process %d terminated on %s.\n", pid, strsignal(status>>8));
	} else {
	    printlog(LOG_ERROR, "iaxmodem process %d ended, status 0x%X.\n", pid, status);
	}
	if (m == NULL) {
	    printlog(LOG_ERROR, "forgot all about iaxmomdem process %d.\n", pid);
	} else {
	    printlog(LOG_ERROR, "respawning process for config: %s.\n", m->config);
	    respawn_modem(modem_for_pid(pid));
	}
    }
    printlog(LOG_ERROR, "waitpid returned: %d\n", pid);
}

void
terminate_modems()
{
    struct modem *m;

    for (m = modems; m != NULL; m = m->next)
	kill(m->pid, SIGTERM);
}

void
spawn_modems(void)
{
    DIR *cfdir;
    struct dirent *cf;
    struct stat st;
    int len;
    char filename[256];
    struct modem *m, **mp;

    /* purge existing list */
    mp = &modems;
    while (*mp != NULL) {
	m = *mp;
	snprintf(filename, sizeof(filename), "/etc/iaxmodem/%s", m->config);
        if (stat(filename, &st) < 0)
            printlog(LOG_INFO, "Config file %s disappeared (%s), forgetting about it.\n", filename, strerror(errno));
        else if (!S_ISREG(st.st_mode))
            printlog(LOG_INFO, "Config file %s was replaced by a non-regular file, forgetting about it.\n", filename);
        else {
	    mp = &(m->next);
	    continue;
	}
        if (m->pid != 0) kill(m->pid, SIGTERM);
        remove_modem(mp);
    }
    
    /* List configuration files */
    cfdir = opendir("/etc/iaxmodem");

    if (cfdir == NULL) {
      printlog(LOG_ERROR, "Error: could not open configuration directory: %s\n", strerror(errno));
      return;
    }

    while ((cf = readdir(cfdir)) != NULL) {
	/* Skip dotfiles and backup files */
	len = strlen(cf->d_name);
	if ((cf->d_name[0] == '.') || (cf->d_name[0] == '#') || (cf->d_name[len - 1] == '~'))
            continue;

	/* Skip anything that isn't a file */
	snprintf(filename, sizeof(filename), "/etc/iaxmodem/%s", cf->d_name);
	if (stat(filename, &st) < 0) {
            printlog(LOG_INFO, "Could not stat configuration file %s: %s\n", cf->d_name, strerror(errno));
	    continue;
        }
	if (!S_ISREG(st.st_mode)) {
	    printlog(LOG_INFO, "%s: not a regular file, skipping.\n");
	    continue;
        }
        if (modem_for_config(cf->d_name) == NULL)
            add_modem(cf->d_name);
    }
    closedir(cfdir);
    
    for (m = modems; m != NULL; m = m->next) {
        if (m->pid != 0 && kill(m->pid, 0)==0) /* child still running */
            kill(m->pid, SIGHUP);
        else
            start_modem(m);
    }
}

void
ctrl_term_sighandler(int sig)
{
    /* nothing to do... the signal will interrupt waitpid, without gothup == 1
       and that will cause the program to terminate. */
}

void
ctrl_hup_sighandler(int sig)
{
    gothup = 1;
}

int
main(int argc, char** argv)
{
    char config[256];
    int isdaemon = 1;
    int fd;
    FILE *pidfile;
    struct sigaction sa;

    if (geteuid() != 0) {
      printlog(LOG_ERROR, "Error: run iaxmodem as root\n");
      _exit(-1);
    }

    if (argc == 2 && strncmp(argv[1], "-V", 2) == 0) {
	printf("%s\n%s\n%s\n", MODEMVER, DSPVER, IAXVER);
	_exit(0);
    }

    /* If a config is specified, behave in non-daemon mode */
    if (argc == 2) {

	snprintf(config, sizeof(config), "%s", argv[1]);

	if (!strncmp(config, "-F", 2)) {

		isdaemon = 0;

	} else {
      
		iaxmodem(config, 1);
	}
    }

    /* 
     * iaxmodem() never returns, so the code below won't be reached if
     * a config file is specified on the command line
     */

    /* Detach from the console */
    if ((isdaemon) && (daemon(0, 1) != 0)) {
      printlog(LOG_ERROR, "Fatal error: daemon() failed: %s\n", strerror(errno));
      exit(-1);
    }

    /* Write pid to pidfile */
    pidfile = fopen("/var/run/iaxmodem.pid", "w");
    if (pidfile == NULL) {
      printlog(LOG_ERROR, "Fatal error: could not open pidfile /var/run/iaxmodem.pid: %s\n", strerror(errno));
      exit(-1);
    }
    fprintf(pidfile, "%d\n", (int) getpid());
    fclose(pidfile);

    gothup = 1;

    sa.sa_handler = ctrl_term_sighandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sa.sa_handler = ctrl_hup_sighandler;
    sigaction(SIGHUP, &sa, NULL);

    /* No config file specified, be a controlling process for modems */
    numchild = 0;
    modems = NULL;

    /* Close stdin */
    close(STDIN_FILENO);

    /* Redirect stdout to /dev/null */
    fd = open("/dev/null", O_WRONLY);
    if (fd < 0) {
      printlog(LOG_ERROR, "Error: could not open /dev/null: %s\n", strerror(errno));
    } else {
      dup2(fd, STDOUT_FILENO);
    }
    close(fd);

    while (gothup) {
	gothup = 0;

	/* Reopen the log file */
	checklog("/var/log/iaxmodem/iaxmodem");
	fd = open("/var/log/iaxmodem/iaxmodem", O_WRONLY | O_APPEND | O_CREAT | O_LARGEFILE, logmode);
	if (fd < 0) {
	    printlog(LOG_ERROR, "Error: could not open /var/log/iaxmodem/iaxmodem: %s\n", strerror(errno));
	} else {
	    dup2(fd, STDERR_FILENO);
	}
	close(fd);

	/* kill/spawn/restart children */
	spawn_modems();
	if (numchild <= 0)
	    break;
	wait_for_modems();
    }

    printlog(LOG_ERROR, "Terminating...\n");
    terminate_modems();
    unlink("/var/run/iaxmodem.pid");

    exit(0);
}
