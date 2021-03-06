#include <syslog.h>

#ifndef _libio_h_
#define _libio_h_

#ifdef __cplusplus
extern "C" {
#endif

/* SYSLOG */
extern void elog(int prio, int errnum, const char *fmt, ...)
	__attribute__((format(printf,3,4)));

/* core loop */
extern int libio_wait(void);

/* GENERIC */
extern void register_applet(const char *name, int (*fn)(int, char *[]));

/* malloc & zero */
extern void *zalloc(unsigned int size);

/*
 * string table lookups,
 * returns the unique index of @str in @table,
 * or -1 when @str is not unique, or not found.
 */
extern int strlookup(const char *str, const char *const table[]);

/*
 * Given a string like 'name=horse,color=gray,tall',
 * mygetsubopt returns 'name', 'color', 'tall'
 * and puts 'horse', 'gray', NULL in *value
 */
extern char *mygetsubopt(char *key);
extern char *mygetsuboptvalue(void);

/* sysfs (or any other file) iface */
extern const char *attr_reads(const char *fmt, ...)
	__attribute__((format(printf,1,2)));
extern int attr_read(int default_value, const char *fmt, ...)
	__attribute__((format(printf,2,3)));
extern int attr_write(int value, const char *fmt, ...)
	__attribute__((format(printf,2,3)));

/* find file with shell wildcards
 * returns NULL when not matched
 * the result is malloc'd and should be freed by caller
 */
extern char *findfile(const char *pattern);

/* set single event with itimer */
extern int schedule_itimer(double value);

struct iopar;

/* parameter API */
extern int create_iopar(const char *str);
extern int create_ioparf(const char *fmt, ...)
	__attribute__((format(printf,1,2)));
extern void destroy_iopar(int iopar);
extern double get_iopar(int iopar);
extern int set_iopar(int iopar, double value);

/* return true when iopar is dirty */
extern int iopar_dirty(int iopar);
/* return true when iopar is lost */
extern int iopar_present(int iopar);
/* return the name used during construction */
extern const char *iopar_name(int iopar);

/* fetch with constant from /etc/libio-const.conf */
extern const char *libio_strconst(const char *name);
extern double libio_const(const char *name);
/* external iterator */
extern const char *libio_next_const(const char *name);

/* netio: publish local parameter via this socket */
extern int libio_bind_net(const char *uri);

/* netio: probe for remote socket (send a *probe packet) */
extern int netio_probe_remote(const char *uri);

/* parse uri into a sockaddr */
extern int netio_strtosockname(const char *uri, void *paddr, int family);

/* netio messages */
extern int netio_send_msg(const char *uri, const char *signal);
/* ack current received message */
extern int netio_ack_msg(const char *msg);
extern int netio_msg_pending(void);
extern const char *netio_recv_msg(void);
/* call after netio_recv_msg() */
extern unsigned int netio_msg_id(void);

/* long-press-detection */
extern int new_longdet(void); /* default delay */
extern int new_longdet1(double delay);
extern void set_longdet(int id, double value);

#define SHORTPRESS	1
#define LONGPRESS	3
extern int longdet_state(int id);
#define get_longdet longdet_state
/* longdet_edge() returns get_longdet when it just changed */
extern int longdet_edge(int id);

/* resource control */
extern int libio_take_resource(const char *uri, const char *cid, double value);

/*
 * libio_flush
 * called at each main cycle, clears dirty flags
 */
extern void libio_flush(void);
/*
 * libio_run_notifiers
 * called at each main cycle, before processing new data
 * It calls internal notifiers for iopar dependencies
 */
extern void libio_run_notifiers(void);

/* set verbosity of libio */
extern void libio_set_trace(int value);

#ifdef __cplusplus
}
#endif
#endif
