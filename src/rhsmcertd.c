/*
* Copyright (c) 2010 Red Hat, Inc.
*
* Authors: Jeff Ortel <jortel@redhat.com>
*
* This software is licensed to you under the GNU General Public License,
* version 2 (GPLv2). There is NO WARRANTY for this software, express or
* implied, including the implied warranties of MERCHANTABILITY or FITNESS
* FOR A PARTICULAR PURPOSE. You should have received a copy of GPLv2
* along with this software; if not, see
* http://www.gnu.org/licenses/old-licenses/gpl-2.0.txt.
*
* Red Hat trademarks are not licensed under GPLv2. No permission is
* granted to use or replicate Red Hat trademarks that are incorporated
* in this software or its documentation.
*/

#include <sys/file.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <wait.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>

#define LOGFILE "/var/log/rhsm/rhsmcertd.log"
#define LOCKFILE "/var/lock/subsys/rhsmcertd"
#define UPDATEFILE "/var/run/rhsm/update"
#define CERT_INTERVAL 240	/*4 hours */
#define HEAL_INTERVAL 1440	/*24 hours */
#define RETRY 10		/*10 min */
#define BUF_MAX 256

static FILE *log = 0;

void
printUsage ()
{
	printf ("usage: rhsmcertd <certinterval> <healinterval>");
}

char *
ts ()
{
	time_t tm = time (0);
	char *ts = asctime (localtime (&tm));
	char *p = ts;
	while (*p) {
		p++;
		if (*p == '\n') {
			*p = 0;
		}
	}
	return ts;
}

void
logUpdate (int delay)
{
	time_t update = time (NULL);
	struct tm update_tm = *localtime (&update);
	char buf[BUF_MAX];

	update_tm.tm_min += delay;
	strftime (buf, BUF_MAX, "%s", &update_tm);

	FILE *updatefile = fopen (UPDATEFILE, "w");
	if (updatefile == NULL) {
		fprintf (log, "%s: error opening %s to write timestamp: %s\n",
			 ts (), UPDATEFILE, strerror (errno));
		fflush (log);
	} else {
		fprintf (updatefile, "%s", buf);
		fclose (updatefile);
	}
}

int
run (int interval, bool heal)
{
	int status = 0;
	fprintf (log, "%s: started: interval = %d minutes\n", ts (), interval);
	fflush (log);

	while (1) {
		int pid = fork ();
		if (pid < 0) {
			fprintf (log, "%s: fork failed\n", ts ());
			fflush (log);
			return EXIT_FAILURE;
		}
		if (pid == 0) {
			if (heal) {
				execl ("/usr/bin/python", "python",
				       "/usr/share/rhsm/subscription_manager/certmgr.py",
				       "--autoheal", NULL);
			} else {
				execl ("/usr/bin/python", "python",
				       "/usr/share/rhsm/subscription_manager/certmgr.py",
				       NULL);
			}

		}
		int delay = interval;
		waitpid (pid, &status, 0);
		status = WEXITSTATUS (status);
		if (status == 0) {
			fprintf (log, "%s: certificates updated\n", ts ());
			fflush (log);
		} else {
			if (delay > RETRY)
				delay = RETRY;
			fprintf (log,
				 "%s: update failed (%d), retry in %d minutes\n",
				 ts (), status, delay);
			fflush (log);
		}

		logUpdate (delay);
		sleep (delay * 60);
	}

	return status;
}

int
get_lock ()
{
	int fdlock;
	struct flock fl;

	fl.l_type = F_WRLCK;
	fl.l_whence = SEEK_SET;
	fl.l_start = 0;
	fl.l_len = 1;

	if ((fdlock = open (LOCKFILE, O_WRONLY | O_CREAT, 0640)) == -1)
		return 1;

	if (flock (fdlock, LOCK_EX | LOCK_NB) == -1)
		return 1;

	return 0;
}

void
run_parts (int cert_interval, int heal_interval)
{
	// TODO: This will become a more robust event loop. I need to verify that
	// it's OK for sub-mgr to depend on glib
	int pid = fork ();
	if (pid < 0) {
		fprintf (log, "%s: fork failed\n", ts ());
		fflush (log);
		exit (EXIT_FAILURE);
	}
	if (pid == 0) {
		run (cert_interval, false);	//cert
	} else {
		run (heal_interval, true);	//heal
	}
}

int
main (int argc, char *argv[])
{
	log = fopen (LOGFILE, "a+");
	if (log == 0) {
		return EXIT_FAILURE;
	}
	if (argc < 3) {
		printUsage ();
		return EXIT_FAILURE;
	}

	int cert_interval = atoi (argv[1]);
	int heal_interval = atoi (argv[2]);

	if (cert_interval < 1) {
		cert_interval = CERT_INTERVAL;
	}
	if (heal_interval < 1) {
		heal_interval = HEAL_INTERVAL;
	}

	int pid = fork ();
	if (pid == 0) {
		daemon (0, 0);

		if (get_lock () != 0) {
			fprintf (log, "%s: unable to get lock, exiting\n",
				 ts ());
			fflush (log);
			return EXIT_FAILURE;
		}

		run_parts (cert_interval, heal_interval);
	}
	fclose (log);

	return EXIT_SUCCESS;
}
