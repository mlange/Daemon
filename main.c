/* 
 * Copyright (c) 2001 QUALCOMM Incorporated.  All rights reserved.
 * The file license.txt specifies the terms for use, modification,
 * and redistribution.
 *
 * Revisions:
 *
 *     04/03/01  [rcg]
 *              - Be a little nicer with "-v".
 *
 *     02/13/01  [rcg]
 *              - Don't abort on weird accept() errors.
 *
 *     02/12/01  [rcg]
 *              - HUP now closes and reopens trace file in standalone mode.
 *
 *     12/21/00  [rcg]
 *              - Fixed option recognition.
 *              - Ignore additional accept() errors, per Stevens 5.11
 *                (thanks to Carles Munyoz for reporting this).
 *
 *     12/06/00  [rcg]
 *              - Ensure daemon terminates on signal, by making listen
 *                file descriptor non-blocking and calling select()
 *                before accept(), since accept() does not return on
 *                signals on some platforms.
 *
 *     11/03/00  [rcg]
 *              - Ignore SIGCHLD on non-BSD systems.
 *              - HUP signal now closes and reopens log.
 *              - Log to specified facility.
 *              - Corrected '<strings.h>' to '<string.h>'
 *
 *     10/10/00  [rcg]
 *              - Delete duplicate '#include <sys/wait.h>'
 *
 *     10/13/00  [rcg]
 *              - Added new TLS options (to avoid getopt errors).
 *
 *     09/29/00  [rcg]
 *              - Revert signals to default in child (avoids conflict
 *                with BSDI authentication).
 *
 *     09/20/00  [rcg]
 *              - Modified wait3() calls to avoid looping before quit,
 *                and handle differences in return values.
 *
 *     08/23/00  [rcg]
 *              - Pass all valid options to getopt, to avoid error messages.
 *
 *     08/21/00  [rcg]
 *              - Now becomes a daemon (unless compiled with _DEBUG).
 *
 *     08/17/00  [rcg]
 *              - Modified to eliminate compiler warnings etc.,
 *              - Can now specify IP address and/or port number
 *                as parameter 1, e.g., 'popper 199.46.50.7:8110 -S'
 *                or 'popper 8110 -S -T600'.  If not specified,
 *                IP address defaults to all available.  The default
 *                port is 110 except when _DEBUG is defined, then 8765.
 *              - Better error messages for common situations.
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <sys/errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdarg.h>
#include <signal.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/socket.h> /* this needs to be after other .h files */

#include "config.h"
#include "popper.h"
#include "snprintf.h"
#include "logit.h"

#if HAVE_UNISTD_H
#  include <unistd.h>
#endif /* HAVE_UNISTD_H */

#if HAVE_SYS_UNISTD_H
#  include <sys/unistd.h>
#endif /* HAVE_SYS_UNISTD_H */

#ifdef HAVE_SYS_STAT_H
#  include <sys/stat.h>
#endif /* HAVE_SYS_STAT_H */

#ifdef HAVE_FCNTL_H
#  include <fcntl.h>
#endif /* HAVE_FCNTL_H */

#ifdef HAVE_SYS_FCNTL_H
#  include <sys/fcntl.h>
#endif /* HAVE_SYS_FCNTL_H */


#ifndef  STANDALONE

/*
 * When not in standalone mode, main() is a jacket for qpopper().
 */
int
main ( argc, argv )
int argc;
char *argv[];
{
    if ( argc >= 2 && ( strncmp ( argv[1], "-v",  2 ) == 0 ||
                        strncmp ( argv[1], "--v", 3 ) == 0   ) )
    {
        printf ( "%s%.*s%s version %s (non-standalone)\n",
                 QPOP_NAME,
                 (strlen(BANNERSFX)>0 ? 1 : 0), " ",
                 BANNERSFX,
                 VERSION );
        return 0;
    }
    else
        return qpopper ( argc, argv );
}

#else /* STANDALONE */


#ifdef _DEBUG
#  define SERV_TCP_PORT 8765
#else
#  define SERV_TCP_PORT  110
#endif /* _DEBUG */

#define BAD_ADDR ( (unsigned long) -1 )

/*
 * Be careful using TRACE in an 'if' statement!
 */
#define TRACE if ( debug ) logit


/*
 * System prototypes (functions often missing prototypes in
 * system header files)
 */
void bzero();
pid_t wait3();


/*
 * Local prototypes
 */
int     main ( int argc, char *argv[] );
void    msg      ( WHENCE, const char *format, ... );
void    err_msg  ( WHENCE, const char *format, ... );
void    err_dump ( WHENCE, const char *format, ... );
void    my_perror   ( void );
char   *sys_err_str ( void );
int     reaper  ( SIGPARAM );
int     hupit   ( SIGPARAM );
int     cleanup ( SIGPARAM );
void    roll_it ( void );
void    motherforker ( int newsockfd, int sockfd );


/*
 * Globals
 */
char           *pname       = NULL;
volatile BOOL   bClean      = FALSE;
volatile BOOL   bRollover   = FALSE;
char          **Qargv       = NULL;
int             Qargc       = 0;
BOOL            Qargv_alloc = FALSE;
BOOL            debug       = FALSE;
FILE           *trace_file  = NULL;
char           *trace_name  = NULL;
char            msg_buf [ 2048 ] = "";
FILE           *msg_out     = NULL;
FILE           *err_out     = NULL;


/*
 * In standalone mode, main() is the daemon function.  The port number
 * can be specified as argv[1], e.g., 'popper 110'.
 */
int
main ( int argc, char *argv[] )
{
    int                 sockfd      = -1;
    int                 newsockfd   = -1;
    int                 clilen      =  0;
    int                 i           =  0;
    int                 rslt        =  0;
    struct sockaddr_in  cli_addr;
    struct sockaddr_in  serv_addr;
    char               *ptr         = NULL;
    unsigned short      port        = SERV_TCP_PORT;
    unsigned long       addr        = INADDR_ANY;
    fd_set              fdset_templ;
    fd_set              fdset_read;
    int                 fd_flags    = 0;


    if ( argc >= 2 && ( strncmp ( argv[1], "-v",  2 ) == 0 ||
                        strncmp ( argv[1], "--v", 3 ) == 0   ) )
    {
        printf ( "%s%.*s%s version %s (standalone)\n",
                 QPOP_NAME,
                 ( strlen(BANNERSFX) > 0 ? 1 : 0 ), " ",
                 BANNERSFX,
                 VERSION );
        return 0;
    }

    err_out = msg_out  = fopen ( "/dev/null", "w+" ); /* until we get set up */

    /*
     * Ensure default port & address is in network order
     */
    addr = htonl ( addr );
    port = htons ( port );

    /*
     * Set defaults for Qargc and Qargv
     */
    Qargc = argc;
    Qargv = argv;

    /*
     * Get our own name (if provided)
     */
    pname = argv [ 0 ];
    if ( pname != NULL && *pname != '\0' )
    {
        ptr = strrchr ( pname, '/' );
        if ( ptr != NULL && strlen(ptr) > 1 )
            pname = ptr + 1;
    }

    /*
     * The first specified parameter may be an IP address
     * and/or a port number.  If so, this is what we bind
     * to.  Otherwise we use defaults.
     */
    ptr = argv [ 1 ];
    if ( argc >= 2 && ( *ptr == ':' || isdigit ( (int) *ptr ) ) )
    {
        unsigned long  a = addr;
        unsigned short n = port;
        char           b [ 25 ] = "";
        char          *q = b;

        /*
         * We might have an ip address first
         */
        if ( strchr ( ptr, '.' ) != NULL )
            while ( *ptr == '.' || isdigit ( (int) *ptr ) )
                *q++ = *ptr++;
        
        if ( *b != '\0' )
        {
            a   = inet_addr ( b );
            ptr = strchr ( ptr, ':' );
            if ( ptr != NULL )
                ptr++;
        }
        else
        {
            ptr = argv [ 1 ];
            if ( *ptr == ':' )
                ptr++;
        }

        /*
         * We might have a port number
         */
        if ( ptr != NULL )
            n = atoi ( ptr );

        if ( a == BAD_ADDR || n == 0 || n > USHRT_MAX )
            err_dump ( HERE, "invalid address and/or port: \"%s\"", argv[1] );

        port = htons ( n );
        addr = a;
        
        /*
         * Since we consumed the first specified parameter,
         * create our own argv that omits it, to pass on to
         * Qpopper.
         */
        rslt  = sizeof(char *) * argc;
        Qargv = malloc ( rslt );
        if ( Qargv == NULL )
            err_dump ( HERE, "unable to allocate memory" );
        Qargv_alloc = TRUE;
        bzero ( Qargv, rslt );
        Qargv [ 0 ] = argv [ 0 ];
        for ( rslt = 2; rslt < argc; rslt++ )
            Qargv [ rslt - 1 ] = argv [ rslt ];
        Qargc = argc - 1;
    }

    /*
     * Open the log
     */
#ifdef SYSLOG42
    openlog ( pname, 0 );
#else
    openlog ( pname, POP_LOGOPTS, /*LOG_DAEMON*/ POP_FACILITY );
#endif

    /*
     * See if debug or trace options specified
     */
    i = getopt ( Qargc, Qargv, "b:BcCdD:e:f:FkK:l:L:p:RsSt:T:uUvy:" );
    while ( i != EOF )
    {
        switch ( i )
        {
            case 'd':
                debug = TRUE;
                break;

            case 't':
                debug = TRUE;
                trace_name = strdup ( optarg );
                trace_file = fopen ( optarg, "a" );
                if ( trace_file == NULL )
                    err_dump ( HERE, "Unable to open trace file \"%s\"", optarg );
                TRACE ( trace_file, POP_DEBUG, HERE,
                        "Opened trace file \"%s\" as %d",
                        trace_name, fileno(trace_file) );
                break;

            default:
                break;
        }

        i = getopt ( Qargc, Qargv, "b:BcCdD:e:f:FkK:l:L:p:RsSt:T:uUvy:" );
    }
    optind = 1; /* reset for pop_init */

#ifdef _DEBUG
    msg_out = err_out = fopen ( "/dev/tty", "w+" );
    if ( msg_out == NULL )
    {
        msg_out = stdout;
        err_out = stderr;
    }
#else

    /*
     * Become a daemon
     */

    /*
     * First we `fork()' so our parent can exit; this returns control
     * to the command line or shell that invoked us.  This step is
     * required so that the new process is guaranteed not to be a
     * process group leader. The next step, `setsid()', fails if we're
     * a process group leader.
     */
    rslt = fork();
    if ( rslt == -1 )
        err_dump ( HERE, "fork() failed" );
    if ( rslt > 0 )
    {
        TRACE ( trace_file, POP_DEBUG, HERE, 
                "%s: Server: first fork(); child=%i; exiting",
                pname, rslt );
         exit ( 0 ); /* if we're the parent we just go away */
    }
    TRACE ( trace_file, POP_DEBUG, HERE, 
            "%s: Server: child of first fork(); pid=%i",
            pname, getpid() );

    /*
     * Next call `setsid()' to become a process group and session
     * group leader. Since a controlling terminal is associated with
     * a session, and this new session has not yet acquired a
     * controlling terminal our process now has no controlling
     * terminal, which is a Good Thing for daemons.
     */
    rslt = setsid();       /* Disassociate from controlling terminal */
    if ( rslt == -1 )
        err_dump ( HERE, "setsid() failed" );

    /*
     * Now we `fork()' again so the parent, (the session group leader),
     * can exit.  This means that we, as a non-session group leader,
     * can never regain a controlling terminal.
     */
    rslt = fork();
    if ( rslt == -1 )
        err_dump ( HERE, "fork() failed" );
    if ( rslt > 0 )
    {
        TRACE ( trace_file, POP_DEBUG, HERE,
                "%s: Server: second fork(); child=%i; exiting",
                pname, rslt );
         exit ( 0 ); /* if we're the parent we just go away */
    }
    TRACE ( trace_file, POP_DEBUG, HERE,
            "%s: Server: child of second fork(); pid=%i",
            pname, getpid() );

     /*
      * Now we `chdir("/")' to ensure that our process doesn't keep
      * any directory in use.  Failure to do this could make it so
      * that an administrator couldn't unmount a filesystem, because
      * it was our current directory.
      *
      * (Equivalently, we could change to any directory containing
      * files important to our operation.)
      */
    TRACE ( trace_file, POP_DEBUG, HERE, "calling chdir()" );
    rslt = chdir ( "/" );
    if ( rslt == -1 )
        err_dump ( HERE, "chdir(\"/\") failed" );

    /*
     * Normally, a daemon next calls umask(0)' so that it has complete
     * control over the permissions of anything it writes;  we don't
     * know what umask we may have inherited.  However, Qpopper calls
     * umask as it gets going, for safety (since it runs as root for
     * a while).
     */

    /*
     * Next `close()' fds 0, 1, and 2. This releases the standard in,
     * out, and error we inherited from our parent process.  We have
     * no way of knowing where these fds might have been redirected
     * to.  Note that many daemons use `sysconf()' to determine the
     * limit `_SC_OPEN_MAX'.  `_SC_OPEN_MAX' tells you the maximun
     * open files/process.  Then in a loop, the daemon can close all
     * possible file descriptors.
     */
    TRACE ( trace_file, POP_DEBUG, HERE, "closing file descs %d to 0", sysconf ( _SC_OPEN_MAX )  );
    for ( i = sysconf ( _SC_OPEN_MAX ); i >= 0; i-- )
    {
        if ( debug == FALSE || trace_file == NULL || i != fileno(trace_file) )
            close ( i );
    }

    /*
     * Finally, establish new open descriptors for stdin, stdout and
     * stderr.  Even if we don't plan to use them, it is still a good
     * idea to have them open.  The precise handling of these is a
     * matter of taste; if you have a logfile, for example, you might
     * wish to open it as stdout or stderr, and open `/dev/null' as
     * stdin; alternatively, you could open `/dev/console' as stderr
     * and/or stdout, and `/dev/null' as stdin, or any other combination
     * that makes sense for your particular daemon.
     */
    i    = open ( "/dev/null",    O_RDONLY );           /* stdin  */
    rslt = open ( "/dev/console", O_RDONLY );           /* stdout */
    rslt = open ( "/dev/console", O_RDONLY );           /* stderr */
    if ( rslt > 0 )
        msg_out = err_out = fdopen ( rslt, "w+" );
    if ( msg_out == NULL )
        msg_out = err_out = fopen ( "/dev/null", "w+" );
    
    TRACE ( trace_file, POP_DEBUG, HERE, 
            "opened stdin=%d; stdout=%d stderr=%d; i=%d; rslt=%d; msg_out=%p",
            fileno(stdin), fileno(stdout), fileno(stderr), i, rslt, msg_out );

#endif /* not _DEBUG */

    /*
     * Set up the socket on which we listen
     */
    sockfd = socket ( AF_INET, SOCK_STREAM, 0 );
    if ( sockfd < 0 )
        err_dump ( HERE, "Can't open stream socket" );
    TRACE ( trace_file, POP_DEBUG, HERE, "opened stream socket; sockfd = %d", sockfd );
 
    i = 1;
    rslt = setsockopt ( sockfd, SOL_SOCKET, SO_REUSEADDR, 
                        (char *) &i, sizeof(i) );
    if ( rslt == -1 )
        err_dump ( HERE, "setsockopt(SO_REUSEADDR) failed" );

    if ( debug )
    {
        rslt = setsockopt ( sockfd, SOL_SOCKET, SO_DEBUG,
                            (char *) &i, sizeof(i) );
        if ( rslt == -1 )
            TRACE ( trace_file, POP_DEBUG, HERE, 
                    "%s: Server: setsockopt(SO_DEBUG) failed", pname );
    }

    TRACE ( trace_file, POP_DEBUG, HERE, "set stream socket options; sockfd = %d", sockfd );

    bzero ( (char *) &serv_addr, sizeof(serv_addr) );
    serv_addr.sin_family      = AF_INET;
    serv_addr.sin_addr.s_addr = addr;
    serv_addr.sin_port        = port;

    rslt = bind ( sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr) );
    if ( rslt < 0 )
    {
        if ( errno == EADDRINUSE )
        {
            fprintf ( stderr, "%s:%d in use\n",
                      inet_ntoa ( serv_addr.sin_addr ),
                      ntohs     ( serv_addr.sin_port ) );
            return 1;
        }
        else
            err_dump ( HERE, "Can't bind local address %s:%d",
                       inet_ntoa ( serv_addr.sin_addr ),
                       ntohs     ( serv_addr.sin_port ) );
    }

    TRACE ( trace_file, POP_DEBUG, HERE,
            "did bind on stream socket; sockfd = %d",
            sockfd );


    /*
     * Now we're ready to go
     */
    msg ( HERE, "listening on %s:%d\n",
          inet_ntoa ( serv_addr.sin_addr ),
          ntohs     ( serv_addr.sin_port ) );

    TRACE ( trace_file, POP_DEBUG, HERE, "listening using socket fd %d", sockfd );

    listen ( sockfd, 5 );

    /*
     * Set file descriptor to be non-blocking in case there isn't really a
     * connection available if the select succeeds.  This avoids us
     * blocking there.
     */
    fd_flags = fcntl ( sockfd, F_GETFL, 0 );
    rslt     = fcntl ( sockfd, F_SETFL, O_NONBLOCK | fd_flags );
    if ( rslt == -1 )
        err_dump ( HERE, "Unable to set sockfd(%d) to be non-blocking",
                   sockfd );
    TRACE ( trace_file, POP_DEBUG, HERE, "set fd %d non-blocking (%#x)",
            sockfd, fcntl ( sockfd, F_GETFL, 0 ) );

    /*
     * Build the template descriptor set
     */
    memset ( &fdset_templ, 0, sizeof(fdset_templ) );
    FD_SET ( sockfd, &fdset_templ );

#ifdef    BSD
    signal ( SIGCHLD, VOIDSTAR reaper  );
#else /* not BSD */
    signal ( SIGCHLD, SIG_IGN          ); /* ignore them and they'll go away */
#endif /* BSD */

    signal ( SIGHUP,  VOIDSTAR hupit   );
    signal ( SIGTERM, VOIDSTAR cleanup );

    while ( TRUE ) 
    {
        if ( bClean )
        {
            msg   ( HERE, "cleaning up and exiting normally" );
            close ( sockfd );
            sockfd = -1;
            if ( trace_file != NULL )
            {
                fclose ( trace_file );
                trace_file = NULL;
            }
            exit  ( 0 );
        }

        if ( bRollover )
        {
            roll_it();
            bRollover = FALSE;
            signal ( SIGHUP,  VOIDSTAR hupit   );
        }

        /*
         * Copy the template descriptor set into our read set (since
         * select() modifies the fd set we pass in).
         */
        fdset_read = fdset_templ;

        /*
         * Check for a new connection with select() before calling accept(),
         * since accept() does not return on signals on some platforms.
         */
        rslt = select ( sockfd + 1, &fdset_read, NULL, NULL, NULL );
        if ( rslt == -1 && errno != EINTR )
            err_dump ( HERE, "select() error" );

        if ( rslt == 0 || ( FD_ISSET(sockfd, &fdset_read) == FALSE ) )
            err_dump ( HERE, "unexpected select() result" ); 

        clilen    = sizeof(cli_addr);
        newsockfd = accept ( sockfd, (struct sockaddr *) &cli_addr, &clilen );

        TRACE ( trace_file, POP_DEBUG, HERE, 
                "accept=%d; sockfd=%d; clilen=%d; cli_addr=%s:%d\n",
                newsockfd, sockfd, clilen, 
                inet_ntoa ( cli_addr.sin_addr ),
                ntohs     ( cli_addr.sin_port ) );

        if ( newsockfd > 0 )
            motherforker ( newsockfd, sockfd ); 
        else
            if ( errno != EINTR && errno != EWOULDBLOCK 
                                && errno != EPROTO
                                && errno != ECONNABORTED )
                err_msg ( HERE, "accept() error" );

    } /* main loop */

    return 0;
}


/*
 * Logs a message
 */
void
msg ( WHENCE, const char *format, ... )
{
    va_list  ap;                       /* Pointer into stack to extract
                                        *     parameters */
    size_t   left   = sizeof(msg_buf);
    size_t   len    = 0;
    int      iChunk = 0;


    va_start ( ap, format );

    if ( pname != NULL )
    {
        iChunk = Qsnprintf ( msg_buf + len, left, "%s: ", pname );
        left  -= ( iChunk > 0 ? iChunk : left );
        len   += ( iChunk > 0 ? iChunk : left );
    }

    iChunk = Qsnprintf ( msg_buf + len, left, "Server: " );
    left  -= ( iChunk > 0 ? iChunk : left );
    len   += ( iChunk > 0 ? iChunk : left );

    iChunk = Qvsnprintf ( msg_buf + len, left, format, ap );
    left  -= ( iChunk > 0 ? iChunk : left );
    len   += ( iChunk > 0 ? iChunk : left );

    va_end   ( ap );

    fprintf ( msg_out, "%s\n", msg_buf );
    logit   ( trace_file, POP_DEBUG, fn, ln, "%s", msg_buf );
    fflush  ( msg_out );
}


/*
 * Logs a message, dumps core, terminates.
 */
void
err_dump ( WHENCE, const char *format, ... )
{
    va_list  ap;                       /* Pointer into stack to extract
                                        *     parameters */
    size_t   left   = sizeof(msg_buf);
    size_t   len    = 0;
    int      iChunk = 0;


    va_start ( ap, format );

    if ( pname != NULL )
    {
        iChunk = Qsnprintf ( msg_buf + len, left, "%s: ", pname );
        left  -= ( iChunk > 0 ? iChunk : left );
        len   += ( iChunk > 0 ? iChunk : left );
    }

    iChunk = Qsnprintf ( msg_buf + len, left, "Server: " );
    left  -= ( iChunk > 0 ? iChunk : left );
    len   += ( iChunk > 0 ? iChunk : left );

    iChunk = Qvsnprintf ( msg_buf + len, left, format, ap );
    left  -= ( iChunk > 0 ? iChunk : left );
    len   += ( iChunk > 0 ? iChunk : left );

    iChunk = Qsnprintf ( msg_buf + len, left, ": %s [%s:%d]",
                         sys_err_str(), fn, ln );
    left  -= ( iChunk > 0 ? iChunk : left );
    len   += ( iChunk > 0 ? iChunk : left );

    va_end   ( ap );

    fprintf ( err_out, "%s\n", msg_buf );
    logit   ( trace_file, POP_PRIORITY, fn, ln, "%s", msg_buf );

    if ( Qargv_alloc )
    {
        free ( Qargv );
        Qargv = NULL;
    }

    if ( trace_file != NULL )
    {
        fclose ( trace_file );
        trace_file = NULL;
    }

    fflush ( err_out );

    abort();
    exit ( 1 );
}


/*
 * Logs a message, with error information automatically appended.
 */
void
err_msg ( WHENCE, const char *format, ... )
{
    va_list  ap;                       /* Pointer into stack to extract
                                        *     parameters */

    size_t   left   = sizeof(msg_buf);
    size_t   len    = 0;
    int      iChunk = 0;


    va_start ( ap, format );

    if ( pname != NULL )
    {
        iChunk = Qsnprintf ( msg_buf + len, left, "%s: ", pname );
        left  -= ( iChunk > 0 ? iChunk : left );
        len   += ( iChunk > 0 ? iChunk : left );
    }

    iChunk = Qsnprintf ( msg_buf + len, left, "Server: " );
    left  -= ( iChunk > 0 ? iChunk : left );
    len   += ( iChunk > 0 ? iChunk : left );

    iChunk = Qvsnprintf ( msg_buf + len, left, format, ap );
    left  -= ( iChunk > 0 ? iChunk : left );
    len   += ( iChunk > 0 ? iChunk : left );

    va_end   ( ap );

    iChunk = Qsnprintf ( msg_buf + len, left, ": %s [%s:%d]",
                         sys_err_str(), fn, ln );
    left  -= ( iChunk > 0 ? iChunk : left );
    len   += ( iChunk > 0 ? iChunk : left );

    fprintf ( err_out, "%s\n", msg_buf );
    logit   ( trace_file, POP_PRIORITY, fn, ln, "%s", msg_buf );
    fflush  ( err_out );
}


void
my_perror()
{
    fprintf ( stderr, "%s\n", sys_err_str() );
}


char *
sys_err_str()
{
    static char msgstr [ 200 ];
    if ( errno != 0 )
    {
        if ( errno > 0 && errno < sys_nerr )
            Qsnprintf ( msgstr, sizeof(msgstr), "(%d) %s", 
                        errno, sys_errlist [ errno ] );
        else
            Qsnprintf ( msgstr, sizeof(msgstr), "errno = %d", errno );

        msgstr [ sizeof(msgstr) -1 ] = '\0';
    }
    else
        msgstr[0] = '\0';
    
    return ( msgstr );
}


int
reaper ( SIGPARAM )
{
    int   stts;
    pid_t child_pid = 0;


    TRACE  ( trace_file, POP_DEBUG, HERE, "reaper signal handler called" );

    do
    {
        child_pid = wait3 ( &stts, WNOHANG, (struct rusage *) 0 );
        TRACE  ( trace_file, POP_DEBUG, HERE, 
                 "...wait3() returned %d; error: %s (%d) ",
                 child_pid,
                 ( child_pid > 0 ? "" : STRERROR(errno) ), 
                 ( child_pid > 0 ?  0 : errno ) );
    }
        while  ( child_pid > 0 );

    signal ( SIGCHLD, VOIDSTAR reaper );
    return 0;
}


int
cleanup ( SIGPARAM )
{
    int   stts;
    pid_t child_pid = 0;


    TRACE  ( trace_file, POP_DEBUG, HERE, "cleanup signal handler called" );

    while  ( child_pid > 0 )
    {
        child_pid = wait3 ( &stts, WNOHANG, (struct rusage *) 0 );
        TRACE  ( trace_file, POP_DEBUG, HERE, 
                 "...wait3() returned %d; errno=%d",
                 child_pid, errno );
    }

    bClean = TRUE;
    return 0;
}


int
hupit ( SIGPARAM )
{
    TRACE  ( trace_file, POP_DEBUG, HERE, "hupit signal handler called" );
    signal ( SIGHUP, SIG_IGN );
    bRollover = TRUE;
    return 0;
}


void
roll_it ( void )
{
    TRACE  ( trace_file, POP_DEBUG, HERE, "rolling over log..." );

    closelog();

#ifdef SYSLOG42
    openlog ( pname, 0 );
#else
    openlog ( pname, POP_LOGOPTS, LOG_DAEMON /*POP_FACILITY*/ );
#endif

    if ( trace_file != NULL && trace_name != NULL )
    {
        TRACE  ( trace_file, POP_DEBUG, HERE, "rolling over trace file..." );
        fflush ( trace_file );
        fclose ( trace_file );

        trace_file = fopen ( trace_name, "a" );
        if ( trace_file == NULL )
            err_dump ( HERE, "Unable to open trace file '%s'", trace_name );

        TRACE ( trace_file, POP_DEBUG, HERE, "Opened trace file \"%s\" as %d",
                trace_name, fileno(trace_file) );
    }

}


/*
 * Handles new client connection
 */
void
motherforker ( int newsockfd, int sockfd )
{
    int     childpid    = 0;
    int     fd_flags    = 0;
    int     rslt        = 0;


    TRACE ( trace_file, POP_DEBUG, HERE, "new connection; fd=%d", newsockfd );

#ifndef _DEBUG
    childpid = fork();
    if ( childpid < 0 )
        err_dump ( HERE, "fork() error" );
    
    else if ( childpid == 0 )
    { /* I'm the child */
        TRACE ( trace_file, POP_DEBUG, HERE, "new child for connection" );

        /*
         * Children should not trap signals
         */
        signal ( SIGCHLD, SIG_DFL );
        signal ( SIGTERM, SIG_DFL );
        signal ( SIGHUP,  SIG_DFL );

        /*
         * We don't need sockfd
         */
        close ( sockfd );
        sockfd = -1;

#endif /* not _DEBUG */

        /*
         * Make sure we pass a blocking socket to Qpopper
         */
        fd_flags = fcntl ( newsockfd, F_GETFL, 0 );
        TRACE ( trace_file, POP_DEBUG, HERE, "newsockfd (%d) flags: %#x",
                 newsockfd, fd_flags );
        if ( fd_flags & O_NONBLOCK )
        {
            
            rslt = fcntl ( newsockfd, F_SETFL, fd_flags - O_NONBLOCK );
            if ( rslt == -1 )
                err_dump ( HERE, "Unable to set newsockfd (%d) to be blocking",
                           newsockfd );
            TRACE ( trace_file, POP_DEBUG, HERE, "set fd %d blocking (%#x)",
                    newsockfd, fcntl ( newsockfd, F_GETFL, 0 ) );
        }

        dup2    ( newsockfd, 0 );
        dup2    ( newsockfd, 1 );
        dup2    ( newsockfd, 2 );
        close   ( newsockfd    );
        newsockfd = -1;
        qpopper ( Qargc, Qargv );
            
#ifdef _DEBUG
        close  ( sockfd );
        sockfd = -1;
#endif /* not _DEBUG */

        TRACE ( trace_file, POP_DEBUG, HERE, "exiting after Qpopper returned" );

        if ( Qargv_alloc )
        {
            free ( Qargv );
            Qargv = NULL;
        }

        if ( trace_file != NULL )
        {
            fclose ( trace_file );
            trace_file = NULL;
        }

        _exit ( 0 );

#ifndef _DEBUG
    } /* I'm the child */
    else
    { /* I'm the parent */
        TRACE ( trace_file, POP_DEBUG, HERE, "forked() for new connection; pid=%d",
                childpid );
        close ( newsockfd );
        newsockfd = -1;
    } /* I'm the parent */
#endif /* not _DEBUG */
}


#endif /* STANDALONE */
