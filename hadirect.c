#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <error.h>
#include <getopt.h>

#include <libevt.h>
#include "_libio.h"

/* ARGUMENTS */
static const char help_msg[] =
	NAME ": dumb control of N ouputs with N inputs\n"
	"Usage: " NAME " [OPTIONS] NAME IN OUT [NAME2 IN2 OUT2 ...]\n"
	"\n"
	"Options:\n"
	" -V, --version		Show version\n"
	" -v, --verbose		Be more verbose\n"
	" -l, --listen=SPEC	Listen on SPEC\n"
	;

#ifdef _GNU_SOURCE
static const struct option long_opts[] = {
	{ "help", no_argument, NULL, '?', },
	{ "version", no_argument, NULL, 'V', },
	{ "verbose", no_argument, NULL, 'v', },
	{ "listen", required_argument, NULL, 'l', },
	{ },
};

#else
#define getopt_long(argc, argv, optstring, longopts, longindex) \
	getopt((argc), (argv), (optstring))
#endif

static const char optstring[] = "?Vvl:";

struct link {
	struct link *next;
	/* local params */
	int in, out;
	/* public params */
	int pub;
};

static struct args {
	int verbose;
	struct link *links;
} s;

static int hadirect(int argc, char *argv[])
{
	int opt;
	struct link *lnk;

	while ((opt = getopt_long(argc, argv, optstring, long_opts, NULL)) != -1)
	switch (opt) {
	case 'V':
		fprintf(stderr, "%s %s\n", NAME, LOCALVERSION);
		return 0;
	case 'v':
		++s.verbose;
		break;
	case 'l':
		if (libio_bind_net(optarg) < 0)
			error(1, 0, "bind %s failed", optarg);
		break;

	case '?':
	default:
		fputs(help_msg, stderr);
		exit(1);
		break;
	}
	libio_set_trace(s.verbose);

	if (optind >= argc) {
		fputs(help_msg, stderr);
		exit(1);
	}

	/* create common input */
	for (; (optind + 3) <= argc; optind += 3) {
		lnk = zalloc(sizeof(*lnk));
		lnk->pub = create_iopar_type("netio", argv[optind]);
		lnk->in = create_iopar(argv[optind+1]);
		lnk->out = create_iopar(argv[optind+2]);
		lnk->next = s.links;
		s.links = lnk;
	}

	/* main ... */
	while (1) {
		for (lnk = s.links; lnk; lnk = lnk->next) {
			/* PER-link control */
			if (iopar_dirty(lnk->pub)) {
				/* remote command received, process first */
				set_iopar(lnk->out, get_iopar(lnk->pub, 0));
				set_iopar(lnk->pub, get_iopar(lnk->out, 0));
			}
			if (iopar_dirty(lnk->in) && (get_iopar(lnk->in, 0) > 0.5)) {
				/* local button input pressed, toggle */
				set_iopar(lnk->out, !(int)get_iopar(lnk->out, 0));
				set_iopar(lnk->pub, get_iopar(lnk->out, 0));
			}
		}

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
	register_applet(NAME, hadirect);
}