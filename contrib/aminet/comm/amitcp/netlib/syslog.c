/*
 * syslog.c --- syslog function stubs for the AmiTCP/IP
 *
 * Author: jraja <Jarno.Rajahalme@hut.fi>
 *
 * Copyright � 1994 AmiTCP/IP Group, <amitcp-group@hut.fi>
 *                  Helsinki University of Technology, Finland.
 *                  All rights reserved.
 *
 * Created      : Sat Jan 22 12:19:02 1994 jraja
 * Last modified: Sat Feb 26 19:52:42 1994 jraja
 *
 */

#include <sys/param.h>
#include <sys/syslog.h>

#include <bsdsocket.h>
#include <proto/socket.h>
#include <amitcp/socketbasetags.h>

/****** net.lib/syslog *********************************************
*
*   NAME   
*       openlog(), closelog(), setlogmask() -- syslog utility functions
*
*   SYNOPSIS
*       #include <syslog.h>
*       
*       openlog(ident, logopt, facility);
*
*       void openlog(const char *, int, int);
*
*       closelog();
*
*       void closelog(void);
*
*       oldmask = setlogmask(maskpri);
*       
*       int setlogmask(int);
*       
*   FUNCTION
*       openlog() can be called to initialize the log file, if special
*       processing is needed.  ident is a string that precedes every
*       message.  logopt is a mask of bits, logically OR'ed together,
*       indicating logging options.  The values for logopt are:
*       
*            LOG_PID             Log the process ID with each message;
*                                useful for identifying instantiations
*                                of daemons.
*
*            LOG_CONS            Force writing messages to the console
*                                if unable to send it to syslogd.
*                                This option is safe to use in daemon
*                                processes that have no controlling
*                                terminal because syslog() forks
*                                before opening the console.
*
*            LOG_NDELAY          Open the connection to syslogd
*                                immediately.  Normally, the open is
*                                delayed until the first message is
*                                logged.  This is useful for programs
*                                that need to manage the order in
*                                which file descriptors are allocated.
*
*            LOG_NOWAIT          Do not wait for children forked to
*                                log messages on the console. Obsolete
*                                in AmiTCP/IP, since AmiTCP/IP does
*                                not fork.
*
*       facility encodes a default facility to be assigned to all
*       messages written subsequently by syslog() with no explicit
*       facility encoded. The facility codes are:
*
*            LOG_KERN            Messages generated by the kernel.
*                                These cannot be generated by any user
*                                processes.
*
*            LOG_USER            Messages generated by random user
*                                processes.  This is the default
*                                facility identifier if none is
*                                specified.
*
*            LOG_MAIL            The mail system.
*
*            LOG_DAEMON          System daemons, such as inetd, ftpd,
*                                etc.
*
*            LOG_AUTH            The authorization system: login, su,
*                                getty, etc.
*
*            LOG_LPR             The line printer spooling system: lp,
*                                lpsched, etc.
*
*            LOG_LOCAL0          Reserved for local use. Similarly for
*                                LOG_LOCAL1 through LOG_LOCAL7.
*
*
*       closelog() closes the log file.
*
*       setlogmask() sets the log priority mask to maskpri and returns
*       the previous mask.  Calls to syslog() with a priority not set
*       in maskpri are rejected.  The mask for an individual priority
*       pri is calculated by the macro LOG_MASK(pri); the mask for all
*       priorities up to and including toppri is given by the macro
*       LOG_UPTO(toppri).  By default, all priorities are logged.
*
*   EXAMPLES
*       who logs a message regarding some sort of unexpected and
*       serious error:
*
*           syslog(LOG_ALERT, "who: internal error 23");
*
*       ftpd uses openlog() to arrange to log its process ID, to log
*       to the console if necessary, and to log in the name of the
*       daemon facility:
*
*           openlog("ftpd", LOG_PID|LOG_CONS, LOG_DAEMON);
*
*       Arrange to log messages only at levels LOG_ERR and lower:
*
*           setlogmask(LOG_UPTO(LOG_ERR));
*
*       Typical usage of syslog() to log a connection:
*
*           syslog(LOG_INFO, "Connection from host %d", CallingHost);
*
*       If the facility has not been set with openlog(), it defaults
*       to LOG_USER.
*
*       Explicitly set the facility for this message:
*
*           syslog(LOG_INFO|LOG_LOCAL2, "foobar error: %m");
*       
*   NOTES
*       openlog() does not copy and store the ident string internally;
*       it stores only the character pointer.  Therefore it is the
*       responsibility of the programmer to make sure that the ident
*       argument points to the correct string while syslog() is being
*       called. 
*
*   BUGS
*       Most of the logopt and facility codes are currently being
*       ignored by AmiTCP/IP, but they should be used for future
*       compatibility.
*
*       The autoinit module of the net.lib tells the program name 
*       to the AmiTCP/IP at program startup, so use of the openlog()
*       for that purpose only is not necessary.
*
*   AUTHOR
*       syslog() was developed by the University of California,
*       Berkeley.
*
*   SEE ALSO
*       bsdsocket.library/syslog(), bsdsocket.library/SocketBaseTagList()
*****************************************************************************
*
*/
void
openlog(const char *ident, int logstat, int logfac)
{
  /*
   * Note: SocketBaseTags() does value checking for the arguments
   */
  SocketBaseTags(SBTM_SETVAL(SBTC_LOGTAGPTR), ident,
		 SBTM_SETVAL(SBTC_LOGSTAT), logstat,
		 SBTM_SETVAL(SBTC_LOGFACILITY), logfac,
		 TAG_END);
}

void
closelog(void)
{
  SocketBaseTags(SBTM_SETVAL(SBTC_LOGTAGPTR), NULL,
		 TAG_END);
}

/* setlogmask -- set the log mask level */
int
setlogmask(int pmask)
{
  ULONG taglist[5];

  taglist[0] = SBTM_GETVAL(SBTC_LOGMASK);
  taglist[2] = SBTM_SETVAL(SBTC_LOGMASK);
  taglist[3] = pmask;
  taglist[4] = TAG_END;

  SocketBaseTagList((struct TagItem *)taglist);
  return (int)taglist[1];
}
