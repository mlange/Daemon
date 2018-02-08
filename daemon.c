#include <moca.h>

#include <stdio.h>
#include <stdlib.h>

#include <oslib.h>


long Daemonize(void)
{
    PROCESS_INFORMATION procInfo;
    STARTUPINFO         startupInfo;

    startupInfo.cb          = sizeof startupInfo;
    startupInfo.lpReserved  = NULL;
    startupInfo.lpDesktop   = NULL;
    startupInfo.lpTitle     = NULL;
    startupInfo.dwFlags     = STARTF_USESTDHANDLES;
    startupInfo.cbReserved2 = 0;
    startupInfo.lpReserved2 = NULL;

    printf("Creating daemon process...\n"); fflush(stdout);

    if (!CreateProcess(NULL, "daemon", NULL, NULL, TRUE, 0L,
		       NULL, NULL, &startupInfo, &procInfo))
    {
        fprintf(stderr, "CreateProcess: %s\n", osError( ));
	exit(1);
    }

    printf("MAIN: Sleeping for 60 seconds...\n"); fflush(stdout);
    osSleep(60, 0);

    return eOK;
}

int main(int argc, char *argv[])
{
    int ii;

    Daemonize( );

    for (ii = 0; ii < 100; ii++)
    {
        printf("DAEMON: Sleeping for 1 second...\n"); fflush(stdout);
        osSleep(1, 0);
    }

    exit(0);
}
