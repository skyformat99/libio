#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netdb.h>

#include "lib/libe.h"
#include "lib/libt.h"
#include "_libio.h"

#define netiomsg_ignored "##ignored"

union sockaddrs {
	struct sockaddr sa;
	struct sockaddr_un un;
	struct sockaddr_in in;
	struct sockaddr_in6 in6;
};

struct iosocket;
struct ioremote;

struct sockparam {
	struct iopar iopar;
	struct ioremote *remote;
	struct sockparam *next;
	double newvalue;
	int state;
		#define ST_WRITABLE	0x01
		#define ST_WAITING	0x02 /* waiting for transmission, ... */
		#define ST_NEW		0x04 /* newly created: transmit without dirty ... */

	char name[2];
};

struct ioremote {
	struct ioremote *next;
	struct iosocket *sock;
	struct sockparam *params;
	union sockaddrs name;
	socklen_t namelen;
	int flags;
		#define FL_SENDTO	0x01
		#define FL_RECVFROM	0x02
	time_t last_recvfrom_time;
};

struct iosocket {
	int fd;
	struct ioremote *remotes;
	int flags;
		/*
		 * socket for publishing, not subscribing
		 * The goal is to have socket do only 1 function:
		 * a) publish parameters => fixed address
		 * b) subscribe to parameters => we make those
		 *    a dynamic address, so that program restarts
		 *    results in a new socket address.
		 */
		#define FL_MYPUBLIC_SOCK	0x01
};

struct netiomsg {
	struct netiomsg *next;
	unsigned int id;
	union sockaddrs name;
	socklen_t namelen;
	char txt[2];
};

#define NETIO_MTU	1500
#define NETIO_PINGTIME	1

#define NIOSOCKETS PF_MAX
static struct iosocket *iosockets[PF_MAX];
static struct iosocket *pubsockets[PF_MAX];
static int netio_dirty;
static struct sockparam *localparams;
/* netiomsg queue (first & last) */
static struct netiomsg *netiomsgq, *netiomsgqlast;
static struct netiomsg *netiomsgp;
static unsigned int netiomsgid;
static int netiomsg_acked;

/* locally used buffer */
static char pktbuf[NETIO_MTU+1];

/* list management */
static void add_sockparam(struct sockparam *par, struct ioremote *rem)
{
	struct sockparam **ppar = rem ? &rem->params : &localparams;

	par->next = *ppar;
	*ppar = par;

	par->remote = rem;
}

static void del_sockparam(struct sockparam *par)
{
	struct sockparam **ppar =
		par->remote ? &par->remote->params : &localparams;

	for (; *ppar; ppar = &(*ppar)->next) {
		if (*ppar == par) {
			*ppar = par->next;
			break;
		}
	}
}

static void add_ioremote(struct ioremote *rem, struct iosocket *sock)
{
	rem->next = sock->remotes;
	sock->remotes = rem;
	rem->sock = sock;
}

static void del_ioremote(struct ioremote *rem)
{
	struct ioremote **prem;

	for (prem = &rem->sock->remotes; *prem; prem = &(*prem)->next) {
		if (*prem == rem) {
			*prem = rem->next;
			break;
		}
	}
}

/* network address translation, returns addr_len */
int netio_strtosockname(const char *uri, void *paddr, int family)
{
	int len;
	char *pstr, uribuf[strlen(uri ?: "") +1], *luri = uribuf;

	strcpy(luri, uri ?: "");
	pstr = strpbrk(luri, "?#");
	if (pstr)
		/* cut anchor */
		*pstr = 0;

	if (!family) {
		/* try to autodetect */
		if (*luri == '@' || strchr(luri, '/'))
			family = AF_UNIX;
		else if (*luri == '[')
			family = AF_INET6;
		else if (strchr(luri, '.'))
			family = AF_INET;
	}
	if (family == PF_UNIX) {
		struct sockaddr_un *uaddr = (void *)paddr;

		memset(uaddr, 0, sizeof(*uaddr));
		uaddr->sun_family = AF_UNIX;
		if (luri && strlen(luri))
			strncpy(uaddr->sun_path, luri, sizeof(uaddr->sun_path));
		else
			sprintf(uaddr->sun_path, "@netio-%i", getpid());
		len = SUN_LEN(uaddr);
		if ('@' == uaddr->sun_path[0])
			/* abstract namespace */
			uaddr->sun_path[0] = 0;
		return len;
	} else {
		/* inet? */
		char *strport = NULL;

		struct addrinfo *ai, hints = {
			.ai_family = family,
			.ai_socktype = SOCK_DGRAM,
			.ai_protocol = 0,
			.ai_flags = 0,
		};

		if (!uri)
			return -1;

		if (*luri == '[') {
			/* inet6 ip addr */
			pstr = strchr(luri, ']');
			if (pstr) {
				++luri;
				*pstr++ = 0;
				if (*pstr == ':')
					strport = pstr+1;
			}
		} else {
			strport = strrchr(luri, ':');
			if (strport)
				*strport++ = 0;
		}

#ifdef AI_NUMERICSERV
		hints.ai_flags |= AI_NUMERICSERV;
#endif
		/* resolve host to IP */
		if (getaddrinfo(luri, strport, &hints, &ai) < 0) {
			elog(LOG_WARNING, errno, "getaddrinfo '%s'", uri);
			return -1;
		}

		len = ai->ai_addrlen;
		memcpy(paddr, ai->ai_addr, len);
		freeaddrinfo(ai);
		/* TODO: export ai_protocol */
		return len;
	}
}

/* timers */
static void netio_keepalive(void *dat)
{
	static const char pktmst[] = "*keepalive\n";
	static const char pktslv[] = "*subscribe\n";
	int j;
	struct ioremote *remote;

	/* loop over remotes to send update to */
	for (j = 0; j < NIOSOCKETS; ++j) {
		if (!pubsockets[j])
			continue;
		for (remote = pubsockets[j]->remotes; remote; remote = remote->next) {
			sendto(pubsockets[j]->fd, pktmst, sizeof(pktmst), 0,
						&remote->name.sa, remote->namelen);
		}
	}
	for (j = 0; j < NIOSOCKETS; ++j) {
		if (!iosockets[j])
			continue;
		for (remote = iosockets[j]->remotes; remote; remote = remote->next) {
			sendto(iosockets[j]->fd, pktslv, sizeof(pktslv), 0,
						&remote->name.sa, remote->namelen);
		}
	}
	libt_add_timeout(NETIO_PINGTIME, netio_keepalive, dat);
}

static void netio_schedule_keepalive(void)
{
	static int netio_keepalive_scheduled;

	if (!netio_keepalive_scheduled)
		libt_add_timeout(NETIO_PINGTIME, netio_keepalive, NULL);
	netio_keepalive_scheduled = 1;
}

static void netio_lost_remote(void *param)
{
	struct ioremote *remote = param;

	if (!(remote->flags & FL_SENDTO)) {
		/*
		 * I subscribe to this remote's parameters
		 * Keep the parameters alive and keep subscribing,
		 * the remote may come back some day
		 */
		struct sockparam *par;

		for (par = remote->params; par; par = par->next)
			iopar_clr_present(&par->iopar);
	} else {
		/* no parameters to receive, just drop the remote */
		while (remote->params) {
			struct sockparam *par = remote->params;

			del_sockparam(par);
			/* set parameter lost + dirty */
			iopar_clr_present(&par->iopar);
		}
		del_ioremote(remote);
		free(remote);
	}
}

/* Device */
static struct sockparam *find_param(const char *name, struct sockparam *list)
{
	for (; list; list = list->next) {
		if (!strcmp(list->name, name))
			return list;
	}
	return NULL;
}

static void read_iosocket(int fd, void *data)
{
	struct iosocket *sk = data;
	struct ioremote *remote;
	struct sockparam *par;
	socklen_t namelen;
	int ret, recvlen, saved_remote_flags;
	char *tok, *dat, *savedstr;
	union sockaddrs name;

	/* fetch packet */
	namelen = sizeof(name);
	recvlen = ret = recvfrom(fd, pktbuf, NETIO_MTU, 0, &name.sa, &namelen);
	if (ret < 0) {
		libe_remove_fd(fd);
		close(fd);
		/* TODO: proper cleanup */
		return;
	}
	pktbuf[recvlen] = 0;

	/* find remote */
	for (remote = sk->remotes; remote; remote = remote->next) {
		if ((namelen == remote->namelen) &&
				!memcmp(&remote->name, &name, namelen))
			break;
	}
	if (!remote) {
		/* create remote? */
		remote = zalloc(sizeof(*remote));
		memcpy(&remote->name, &name, namelen);
		remote->namelen = namelen;
		add_ioremote(remote, sk);
		libt_add_timeout(2*NETIO_PINGTIME, netio_lost_remote, remote);
	}

	/* parse packet */
	saved_remote_flags = remote->flags;
	for (tok = strtok_r(pktbuf, "\n", &savedstr); tok; tok = strtok_r(NULL, "\n", &savedstr)) {
		if (*tok == '*') {
			/* special command */
			if (!strncmp(tok, "*ping", 5)) {
				sendto(fd, "*pong", 5, 0, &name.sa, namelen);
			} else if (!strncmp(tok, "*pong", 5)) {
				/* ignore */
			} else if (!strncmp(tok, "*keepalive", 8)) {
				if (!(sk->flags & FL_MYPUBLIC_SOCK))
					/* postpone destruction for the remotes
					   on the subscribing side */
					libt_add_timeout(2*NETIO_PINGTIME,
							netio_lost_remote, remote);
			} else if (!strncmp(tok, "*subscribe", 8)) {
				if (!(sk->flags & FL_MYPUBLIC_SOCK)) {
					elog(LOG_WARNING, 0, "subscriber via client socket");
					continue;
				}
				libt_add_timeout(2*NETIO_PINGTIME,
						netio_lost_remote, remote);
				/* mark this as consumer (= send data) */
				remote->flags |= FL_SENDTO;
			} else if (!strncmp(tok, "*msg ", 5) || !strncmp(tok, "*ack ", 5)) {
				struct netiomsg *msg;
				int id;
                                
				id = strtoul(tok+5, &tok, 0);
				if (*tok == ' ')
					++tok;
				/* fill new netiomsg */
				msg = zalloc(sizeof(*msg) + strlen(tok));
				msg->id = id;
				msg->name = name;
				msg->namelen = namelen;
				strcpy(msg->txt, tok);
                                
				/* queue netiomsg */
				if (netiomsgqlast)
					netiomsgqlast->next = msg;
				else
					netiomsgq = msg;
				netiomsgqlast = msg;
			}
			continue;
		}
		dat = strpbrk(tok, "=>");
		if (!dat)
			continue;
		switch (*dat) {
		case '=':
			/* assign for remote parameter */
			if (sk->flags & FL_MYPUBLIC_SOCK) {
				elog(LOG_WARNING, 0, "assign %s via server socket", tok);
				break;
			}
			/* assign */
			*dat++ = 0;
			par = find_param(tok, remote->params);
			if (!par)
				/* TODO: auto-create */
				break;
			par->iopar.value = strtod(dat, NULL);
			iopar_set_dirty(&par->iopar);
			iopar_set_present(&par->iopar);
			if (libio_trace >= 3)
				fprintf(stderr, "netio:%s %s\n", par->name, dat);
			break;
		case '>':
			if (!(sk->flags & FL_MYPUBLIC_SOCK)) {
				elog(LOG_WARNING, 0, "write %s via client socket", tok);
				break;
			}
			/* write request for local parameter */
			*dat++ = 0;
			par = find_param(tok, localparams);
			if (!par)
				break;
			if (!(par->state & ST_WRITABLE)) {
				/* write-protect readonly parameters */
				elog(LOG_WARNING, 0, "remote writes %s, refused!", par->name);
				break;
			}
			/* trigger broadcast */
			netio_dirty = 1;
			/* set parameter */
			par->iopar.value = strtod(dat, NULL);
			iopar_set_dirty(&par->iopar);
			if (libio_trace >= 3)
				fprintf(stderr, "netio:%s %s\n", par->name, dat);
			break;
		}
	}
	/* actions for remote */
	if ((remote->flags ^ saved_remote_flags) & FL_SENDTO) {
		int len = 0;
		/* new consumer, emit all params */
		len += snprintf(pktbuf+len, NETIO_MTU-len, "*initial\n");
		for (len = 0, par = localparams; par; par = par->next) {
			len += snprintf(pktbuf+len, NETIO_MTU-len, "%s=%lf\n",
					par->name, par->iopar.value);
		}
		if (sendto(fd, pktbuf, len, 0, &remote->name.sa, remote->namelen) < 0) {
			elog(LOG_WARNING, errno, "send initial packet");
			/* clear flag */
			remote->flags &= ~FL_SENDTO;
		}
	}
}

/* socket creation */
static int netio_autobind(int family)
{
	struct iosocket *iosock;
	int ret, sk, namelen;
	union sockaddrs name;

	if (iosockets[family])
		return 0;

	ret = sk = socket(family, SOCK_DGRAM/* | SOCK_CLOEXEC*/, 0);
	if (ret < 0) {
		elog(LOG_WARNING, errno, "socket %i dgram 0", name.sa.sa_family);
		return -1;
	}
	fcntl(sk, F_SETFD, fcntl(sk, F_GETFD) | FD_CLOEXEC);

	namelen = netio_strtosockname(NULL, &name.sa, family);
	if (namelen > 0) {
		ret = bind(sk, &name.sa, namelen);
		if (ret < 0) {
			elog(LOG_WARNING, errno, "bind '%i'", family);
			close(sk);
			return -1;
		}
	}

	/* manage socket */
	iosock = zalloc(sizeof(*iosock));
	iosock->fd = sk;
	libe_add_fd(sk, read_iosocket, iosock);
	iosockets[name.sa.sa_family] = iosock;
	netio_schedule_keepalive();
	return sk;
}

/* local socket binding for parameter publishing */
int libio_bind_net(const char *uri)
{
	struct iosocket *iosock;
	int ret, family, sk, namelen, saved_umask;
	char *namestr;
	union sockaddrs name;

	/* find name */
	if (!strncmp(uri, "unix:", 5))
		family = PF_UNIX;
	else if (!strncmp(uri, "udp:", 4))
		/* is ipv4 default? */
		family = PF_INET;
	else if (!strncmp(uri, "udp4:", 5))
		family = PF_INET;
	else if (!strncmp(uri, "udp6:", 5))
		family = PF_INET6;
	else
		family = 0;

	namestr = strchr(uri, ':');
	if (!namestr) {
		elog(LOG_WARNING, 0, "no family in %s", uri);
		return -1;
	}

	namelen = ret = netio_strtosockname(namestr+1, &name.sa, family);
	if (ret < 0)
		goto fail_sockname;

	/* test for duplicate */
	if (pubsockets[name.sa.sa_family])
		elog(LOG_CRIT, 0, "duplicate family socket '%s'", uri);

	/* socket creation */
	ret = sk = socket(name.sa.sa_family, SOCK_DGRAM/* | SOCK_CLOEXEC*/, 0);
	if (ret < 0) {
		elog(LOG_WARNING, errno, "socket %i dgram 0", name.sa.sa_family);
		goto fail_socket;
	}
	fcntl(sk, F_SETFD, fcntl(sk, F_GETFD) | FD_CLOEXEC);

	saved_umask = umask(0);
	ret = bind(sk, &name.sa, namelen);
	umask(saved_umask);
	if (ret < 0) {
		elog(LOG_WARNING, errno, "bind '%s'", uri);
		goto fail_bind;
	}

	/* manage socket */
	iosock = zalloc(sizeof(*iosock));
	iosock->fd = sk;
	iosock->flags |= FL_MYPUBLIC_SOCK;
	libe_add_fd(sk, read_iosocket, iosock);
	pubsockets[name.sa.sa_family] = iosock;
	netio_schedule_keepalive();
	return sk;

fail_bind:
	close(sk);
fail_socket:
fail_sockname:
	return -1;
}

/* local sockparams */
static int set_sockparam(struct iopar *iopar, double value)
{
	struct sockparam *par = (void *)iopar;

	if (par->remote) {
		par->newvalue = value;
		par->state |= ST_WAITING;
	} else {
		iopar_set_present(iopar);
		par->iopar.value = value;
	}
	netio_dirty = 1;
	return 0;
}

static void del_sockparam_hook(struct iopar *iopar)
{
	struct sockparam *par = (void *)iopar;

	del_sockparam(par);
	cleanup_libiopar(&par->iopar);
	free(par);
}

struct iopar *mknetiolocal(char *name)
{
	struct sockparam *par;

	par = zalloc(sizeof(*par) + strlen(name));
	if (*name == '+') {
		par->state |= ST_WRITABLE;
		++name;
	}
	strcpy(par->name, name);
	par->iopar.del = del_sockparam_hook;
	par->iopar.set = set_sockparam;
	par->iopar.value = NAN;
	/* trigger initial transmission */
	par->state |= ST_NEW;
	netio_dirty = 1;

	/* register sockparam */
	add_sockparam(par, NULL);
	return &par->iopar;
}

static struct iopar *mknetioremote(const char *uri, int family)
{
	struct sockparam *par;
	struct ioremote *remote;
	struct iosocket *sock;
	char *parname;
	union sockaddrs name;
	int namelen, ret;

	parname = strchr(uri, '#') + 1;
	if (parname == (char *)1) {
		elog(LOG_WARNING, 0, "no parameter name for '%s'", uri);
		goto fail_noname;
	}
	if (!iosockets[family] && (netio_autobind(family) < 0)) {
		elog(LOG_WARNING, 0, "no socket for '%s'", uri);
		goto fail_family;
	}

	par = zalloc(sizeof(*par) + strlen(parname));
	strcpy(par->name, parname);
	par->iopar.del = del_sockparam_hook;
	par->iopar.set = set_sockparam;
	par->iopar.value = 0;

	/* register sockparam */
	sock = iosockets[family];
	/* lookup remote */
	namelen = netio_strtosockname(uri, &name.sa, family);
	if (namelen < 0)
		goto fail_sockname;
	for (remote = sock->remotes; remote; remote = remote->next) {
		if (remote->namelen == namelen &&
				!memcmp(&remote->name, &name, namelen))
			break;
	}

	if (!remote) {
		remote = zalloc(sizeof(*remote));
		remote->namelen = namelen;
		memcpy(&remote->name, &name, namelen);
		add_ioremote(remote, sock);
		ret = sendto(sock->fd, "*subscribe\n", 10, 0, &name.sa, namelen);
		if ((ret < 0) && (errno != ECONNREFUSED)) {
			elog(LOG_WARNING, errno, "subscribe failed");
			del_ioremote(remote);
			free(remote);
			goto fail_subscribe;
		}
		libt_add_timeout(2*NETIO_PINGTIME, netio_lost_remote, remote);
	}

	add_sockparam(par, remote);
	return &par->iopar;

fail_subscribe:
fail_sockname:
	free(par);
fail_family:
fail_noname:
	return NULL;
}

struct iopar *mknetiounix(char *uri)
{
	return mknetioremote(uri, PF_UNIX);
}
struct iopar *mknetioudp4(char *uri)
{
	return mknetioremote(uri, PF_INET);
}
struct iopar *mknetioudp6(char *uri)
{
	return mknetioremote(uri, PF_INET6);
}

/* hook into iolib */
void netio_sync(void)
{
	struct ioremote *remote;
	struct sockparam *par;
	int len, j;

	/* flush netiomsg queue */
	while (netio_recv_msg()) ;

	if (!netio_dirty)
		return;
	/* prepare local parameters update packet */
	for (len = 0, par = localparams; par; par = par->next) {
		if (par->state & ST_NEW) {
			len += snprintf(pktbuf+len, NETIO_MTU-len, "%s=%lf\n",
					par->name, par->iopar.value);
			par->state &= ~ST_NEW;

		} else if (par->iopar.state & ST_DIRTY)
			len += snprintf(pktbuf+len, NETIO_MTU-len, "%s=%lf\n",
					par->name, par->iopar.value);
	}

	for (j = 0; j < NIOSOCKETS; ++j) {
		if (!pubsockets[j])
			continue;
		for (remote = pubsockets[j]->remotes; remote; remote = remote->next) {
			/* test if we need to send */
			if (sendto(pubsockets[j]->fd, pktbuf, len, 0, &remote->name.sa, remote->namelen) < 0)
				if (errno != ECONNREFUSED)
					elog(LOG_WARNING, errno, "netio_sync public");
		}
	}

	/* loop over remotes to send update to */
	for (j = 0; j < NIOSOCKETS; ++j) {
		if (!iosockets[j])
			continue;
		for (remote = iosockets[j]->remotes; remote; remote = remote->next) {
			/* add remote waiting parameters */
			len = 0;
			for (par = remote->params; par; par = par->next) {
				if (par->state & ST_WAITING) {
					len += snprintf(pktbuf+len, NETIO_MTU-len, "%s>%lf\n",
							par->name, par->newvalue);
					par->state &= ~ST_WAITING;
				}
			}
			/* test if we need to send */
			if (!len)
				continue;
			if (sendto(iosockets[j]->fd, pktbuf, len, 0, &remote->name.sa, remote->namelen) < 0)
				elog(LOG_WARNING, errno, "netio_sync client");
		}
	}
	netio_dirty = 0;
}

/* netio tools */
static int netio_send_direct(const char *uri, const char *pkt, int connmayfail)
{
	union sockaddrs name;
	int namelen, family = 0;
	int ret;

	do {
		if (!uri)
			return -1;
		if (!strncmp(uri, "unix:", 5)) {
			family = AF_UNIX;
			uri += 5;
		} else if (!strncmp(uri, "udp6:", 5)) {
			family = AF_INET6;
			uri += 5;
		} else if (!strncmp(uri, "udp4:", 5)) {
			family = AF_INET;
			uri += 5;
		} else if (!strncmp(uri, "udp:", 5)) {
			family = AF_INET;
			uri += 4;
		} else
			/* find uri through iopar preset */
			uri = libio_strconst(uri);
	} while (!family);

	/* autobind client socket */
	if (!iosockets[family] && (netio_autobind(family) < 0))
		goto fail_family;

	/* don't create a remote, just find a peername for use in sendto */
	namelen = netio_strtosockname(uri, &name.sa, family);
	if (namelen < 0)
		goto fail_sock;

	ret = sendto(iosockets[family]->fd, pkt, strlen(pkt), 0, &name.sa, namelen);
	if (ret < 0) {
		if (!connmayfail || (errno != ECONNREFUSED))
			elog(LOG_WARNING, errno, "netio_send_msg");
	}
	return ret;

fail_sock:
fail_family:
	return -1;
}

int netio_probe_remote(const char *uri)
{
	return netio_send_direct(uri, "*ping", 1);
}

/* netio messages */
int netio_send_msg(const char *uri, const char *msg)
{
	char *pkt;

	pkt = alloca(strlen(msg ?: "") + 32);
	sprintf(pkt, "*msg %u %s", ++netiomsgid, msg);
	if (netio_send_direct(uri, pkt, 0) < 0)
		return -1;
	return netiomsgid;
}

int netio_ack_msg(const char *msg)
{
	char *pkt;
	int family;

	if (netiomsg_acked++)
		return -1;
	if (!netiomsgp)
		return -1;
	family = netiomsgp->name.sa.sa_family;

	/* autobind client socket */
	if (!pubsockets[family])
		return -1;

	pkt = alloca(strlen(msg ?: "") + 32);
	sprintf(pkt, "*ack %u %s", netiomsgp->id, msg);
	if (sendto(pubsockets[netiomsgp->name.sa.sa_family]->fd, pkt, strlen(pkt), 0,
				&netiomsgp->name.sa, netiomsgp->namelen) >= 0)
		return netiomsgp->id;
	if (errno != ECONNREFUSED)
		elog(LOG_WARNING, errno, "netio_ack_msg");
	return -1;
}

int netio_msg_pending(void)
{
	return netiomsgq ? 1 : 0;
}

unsigned int netio_msg_id(void)
{
	return netiomsgp ? netiomsgp->id : 0;
}

const char *netio_recv_msg(void)
{
	netio_ack_msg(netiomsg_ignored);
	if (netiomsgp)
		free(netiomsgp);
	/* shift queue */
	netiomsgp = netiomsgq;
	if (!netiomsgp)
		return NULL;

	netiomsgq = netiomsgq->next;
	if (!netiomsgq)
		netiomsgqlast = NULL;
	netiomsg_acked = 0;

	/* unlink current entry */
	netiomsgp->next = NULL;
	return netiomsgp->txt;
}
