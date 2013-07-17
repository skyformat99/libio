#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>

#include <unistd.h>
#include <error.h>
#include <getopt.h>
#include <sys/wait.h>

#include <libevt.h>
#include "libio.h"

/* ARGUMENTS */
static const char help_msg[] =
	NAME ": spawn process on input\n"
	"Usage: " NAME " [OPTIONS] <PARAM> PROGRAM [ARGS]\n"
	"\n"
	"Options:\n"
	" -V, --version		Show version\n"
	" -v, --verbose		Be more verbose\n"

	" -d, --delay=SECS	detect long press\n"
	;

#ifdef _GNU_SOURCE
static const struct option long_opts[] = {
	{ "help", no_argument, NULL, '?', },
	{ "version", no_argument, NULL, 'V', },
	{ "verbose", no_argument, NULL, 'v', },

	{ "delay", required_argument, NULL, 'd', },
	{ },
};

#else
#define getopt_long(argc, argv, optstring, longopts, longindex) \
	getopt((argc), (argv), (optstring))
#endif

static const char optstring[] = "+?Vvd:";

static struct args {
	int verbose;
	double delay;
} s = {
	.delay = 0.5,
};

static void sigchld(int sig)
{
	int pid, status;

	while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
		if (s.verbose)
			error(0, 0, "pid %u exited", pid);

	signal(sig, sigchld);
}

static void starttorun(char **argv)
{
	int pid;

	pid = fork();
	if (!pid) {
		if (s.verbose)
			error(0, 0, "forked %u", getpid());
		execvp(*argv, argv);
		error(1, errno, "execvp %s ...", *argv);
	} else if (pid < 0)
		error(1, errno, "fork");
}

static int haspawn(int argc, char *argv[])
{
	int opt, param, ldid;

	while ((opt = getopt_long(argc, argv, optstring, long_opts, NULL)) != -1)
	switch (opt) {
	case 'V':
		fprintf(stderr, "%s %s\n", NAME, LOCALVERSION);
		return 0;
	case 'v':
		++s.verbose;
		break;
	case 'd':
		s.delay = strtod(optarg, NULL);
		break;
	case '?':
	default:
		fputs(help_msg, stderr);
		exit(1);
		break;
	}
	libio_set_trace(s.verbose);

	if (optind +2 > argc) {
		fputs(help_msg, stderr);
		exit(1);
	}

	param = create_iopar(argv[optind++]);
	ldid = new_longdet1(s.delay);

	/* main ... */
	signal(SIGCHLD, sigchld);
	while (1) {
		set_longdet(ldid, get_iopar(param, 0));
		if (longdet_edge(ldid) && (longdet_state(ldid) == s.type))
			starttorun(argv+optind);

		/* enter sleep */
		libio_flush();
		if (evt_loop(-1) < 0) {
			if (errno == EINTR)
				continue;
			error(0, errno, "evt_loop");
			break;
		}
	}
	return 0;
}

__attribute__((constructor))
static void init(void)
{
	register_applet(NAME, haspawn);
}
