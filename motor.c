#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>

#include "lib/libt.h"
#include "_libio.h"

/* MOTOR TYPES */
static const char *const motortypes[] = {
	"updown",
		/* UPDOWN: out1 for up, out2 for down */
		#define TYPE_UPDOWN	0
	"godir",
		/* GODIR: out1 to drive, out2 for direction */
		#define TYPE_GODIR	1
	NULL,
};

static int lookup_motor_type(const char *str)
{
	int len = strlen(str);
	const char *const *table;

	if (!len)
		return 0; /* default type */
	for (table = motortypes; *table; ++table) {
		if (!strncmp(*table, str, len))
			return table - motortypes;
	}
	return -1;
}

/* MOTOR struct */
struct motor {
	/* usage counter of this struct */
	int refcnt;
	int state;
		#define ST_IDLE		0
		#define ST_BUSY		1
		#define ST_WAIT		2 /* implement cooldown period */
		#define ST_POST		3
	#define COOLDOWN_TIME	1
	int ctrltype;
		#define CTRL_NONE	0
		#define CTRL_POS	1 /* control position */
		#define CTRL_DIR	2 /* control speed/dir */
	/* how to combine 2 outputs to 1 motor */
	int type;
	/* backend gpio/pwm's */
	int out1, out2;
	int flags;
		#define INV2	2 /* invert out2 driving, for 'godir' */
		#define EOL0	4 /* wait ST_POST near 0 */
		#define EOL1	8 /* wait ST_POST near 1 */

	/* direction control */
	struct iopar dirpar;
	/* requested, but postponed, direction */
	int reqspeed;

	/* position control */
	struct iopar pospar;
	/* time of last sample */
	double lasttime;
		#define UPDINT	0.5
	/* (calibrated) maximum that equals 1.0 */
	double maxval;
	/* requested position */
	double setpoint;

	char *poweruri;
	char *powerid;
	double powerval;
};

/* find motor struct */
static inline struct motor *pospar2motor(struct iopar *iopar)
{
	return (void *)(((char *)iopar) - offsetof(struct motor, pospar));
}
static inline struct motor *dirpar2motor(struct iopar *iopar)
{
	return (void *)(((char *)iopar) - offsetof(struct motor, dirpar));
}

/* generic control */
static inline double motor_curr_speed(struct motor *mot)
{
	return mot->dirpar.value;
}

static inline double motor_curr_position(struct motor *mot)
{
	return mot->pospar.value;
}

static inline int motor_moving(struct motor *mot)
{
	return fpclassify(motor_curr_speed(mot)) != FP_ZERO;
}

/* get value */
static void motor_update_position(struct motor *mot)
{
	double currtime;

	currtime = libt_now();
	if (motor_curr_speed(mot) != 0) {
		mot->pospar.value +=
			(motor_curr_speed(mot) * (currtime - mot->lasttime)) /
			mot->maxval;
		if (mot->pospar.value < 0)
			mot->pospar.value = 0;
		else if (mot->pospar.value > 1)
			mot->pospar.value = 1;
		iopar_set_dirty(&mot->pospar);
	}
	mot->lasttime = currtime;
}

static void keep_motor_power(void *dat)
{
	struct motor *mot = dat;

	if (labs(mot->dirpar.value) < 0.001)
		/* stopp polling */
		return;

	if (libio_take_resource(mot->poweruri, mot->powerid, mot->powerval) >= 0) {
		/* schedule next poll */
		libt_add_timeout(0.5, keep_motor_power, mot);
		return;
	}

	libio_take_resource(mot->poweruri, mot->powerid, 0);
	/* keep power failed! */
	elog(LOG_WARNING, 0, "%s#%s: lost power", mot->poweruri, mot->powerid);
	/* stop motor */
	set_iopar(mot->out1, NAN);
	set_iopar(mot->out2, NAN);

	motor_update_position(mot);
	mot->dirpar.value = 0;
	iopar_set_dirty(&mot->dirpar);
}

/* actually set the GPIO's for the motor. caller should deal with timeouts */
static int change_motor_speed(struct motor *mot, double speed)
{
	if (speed == 0) {
		/* writing NAN should not fail */
		set_iopar(mot->out1, NAN);
		set_iopar(mot->out2, NAN);
		libt_remove_timeout(keep_motor_power, mot);
		libio_take_resource(mot->poweruri, mot->powerid, 0);
	} else if (speed > 0) {
		if (libio_take_resource(mot->poweruri, mot->powerid, mot->powerval) < 0)
			return change_motor_speed(mot, 0);
		if (set_iopar(mot->out2, (mot->flags & INV2) ? 1 : 0) < 0)
			goto fail_2;
		if (set_iopar(mot->out1, speed) < 0)
			goto fail_21;
		libt_add_timeout(0.5, keep_motor_power, mot);
	} else if (speed < 0) {
		if (libio_take_resource(mot->poweruri, mot->powerid, mot->powerval) < 0)
			return change_motor_speed(mot, 0);
		if (mot->type == TYPE_GODIR) {
			if (set_iopar(mot->out2, (mot->flags & INV2) ? 0 : 1) < 0)
				goto fail_2;
			if (set_iopar(mot->out1, -speed) < 0)
				goto fail_21;
		} else {
			if (set_iopar(mot->out1, 0) < 0)
				goto fail_1;
			if (set_iopar(mot->out2, -speed) < 0)
				goto fail_12;
		}
		libt_add_timeout(0.5, keep_motor_power, mot);
	}
	motor_update_position(mot);
	mot->dirpar.value = speed;
	iopar_set_dirty(&mot->dirpar);
	return 0;
fail_21:
	/* writing NAN should not fail */
	set_iopar(mot->out2, NAN);
fail_2:
	libio_take_resource(mot->poweruri, mot->powerid, 0);
	return -1;
fail_12:
	set_iopar(mot->out1, NAN);
fail_1:
	libio_take_resource(mot->poweruri, mot->powerid, 0);
	return -1;
}

static inline double next_wakeup(struct motor *mot)
{
	double result;
	double endpoint;

	if (mot->state == ST_WAIT)
		return COOLDOWN_TIME;
	else if (mot->state == ST_POST)
		return mot->maxval * 0.10;

	if (mot->ctrltype == CTRL_POS)
		endpoint = mot->setpoint;
	else
		endpoint = (motor_curr_speed(mot) < 0) ? 0 : 1;

	result = mot->lasttime + fabs(endpoint - motor_curr_position(mot))*mot->maxval - libt_now();
	/* optimization */
	if (result <= UPDINT)
		return result;
	else if (result < UPDINT*2)
		return result/2;
	else
		return UPDINT;
}

static void motor_handler(void *dat)
{
	struct motor *mot = dat;
	double oldspeed;

	motor_update_position(mot);
	if (mot->state == ST_WAIT)
		mot->state = ST_IDLE;

	/* test for end-of-course statuses */
	if (mot->state == ST_POST) {
		/* go into cooldown state for a bit */
		if (change_motor_speed(mot, 0) < 0)
			goto repeat;
		mot->state = ST_WAIT;
	} else if ((motor_curr_speed(mot) < 0) && (motor_curr_position(mot) <= 0.01)) {
		mot->reqspeed = 0;
		if (mot->flags & EOL0)
			/* run 10% in 'post' mode */
			mot->state = ST_POST;
		else {
			if (change_motor_speed(mot, 0) < 0)
				goto repeat;
			mot->state = ST_WAIT;
		}
	} else if ((motor_curr_speed(mot) > 0) && (motor_curr_position(mot) >= 0.99)) {
		mot->reqspeed = 0;
		if (mot->flags & EOL1)
			/* run 10% in 'post' mode */
			mot->state = ST_POST;
		else {
			if (change_motor_speed(mot, 0) < 0)
				goto repeat;
			mot->state = ST_WAIT;
		}
	} else if (mot->ctrltype == CTRL_POS) {
		if (motor_curr_position(mot) < (mot->setpoint - 0.01)) {
			if (motor_curr_speed(mot) < 0) {
				if (change_motor_speed(mot, 0) < 0)
					goto repeat;
				mot->state = ST_WAIT;
			} else if (motor_curr_speed(mot) == 0) {
				if (change_motor_speed(mot, 1) < 0)
					goto repeat;
				mot->state = ST_BUSY;
			}
		} else if (motor_curr_position(mot) > (mot->setpoint + 0.01)) {
			if (motor_curr_speed(mot) > 0) {
				if (change_motor_speed(mot, 0) < 0)
					goto repeat;
				mot->state = ST_WAIT;
			} else if (motor_curr_speed(mot) == 0) {
				if (change_motor_speed(mot, -1) < 0)
					goto repeat;
				mot->state = ST_BUSY;
			}
		} else if (mot->state != ST_IDLE) {
			if (change_motor_speed(mot, 0) < 0)
				goto repeat;
			mot->state = ST_WAIT;
		}
	} else {
		/* DIR control */
		oldspeed = motor_curr_speed(mot);
		if (fabs(oldspeed - mot->reqspeed) > 0.01) {
			/* speed must change */
			if ((fabs(oldspeed) > 0.01) || (fabs(mot->reqspeed) < 0.01)) {
				/* stop motor */
				if (change_motor_speed(mot, 0) < 0)
					goto repeat;
				mot->state = ST_WAIT;
			} else {
				if (change_motor_speed(mot, mot->reqspeed) < 0)
					goto repeat;
				mot->state = ST_BUSY;
			}
		}
	}
	libt_add_timeout(next_wakeup(mot), motor_handler, mot);
	return;
repeat:
	/* schedule next attempt */
	libt_add_timeout(0.1, motor_handler, mot);
}

static inline void call_motor_handler(struct motor *mot)
{
	if ((mot->state != ST_WAIT) && (mot->state != ST_POST))
		motor_handler(mot);
}

/* iopar methods */
static int set_motor_pos(struct iopar *iopar, double newvalue)
{
	struct motor *mot = pospar2motor(iopar);

	if (mot->ctrltype == CTRL_NONE)
		mot->ctrltype = CTRL_POS;
	else if ((mot->ctrltype == CTRL_DIR) && (motor_curr_speed(mot) == 0))
		/* revert to position control */
		mot->ctrltype = CTRL_POS;
	mot->setpoint = newvalue;
	call_motor_handler(mot);
	return 0;
}

static int set_motor_dir(struct iopar *iopar, double newvalue)
{
	struct motor *mot = dirpar2motor(iopar);

	if (isnan(newvalue)) {
		/* revert to position control */
		mot->ctrltype = CTRL_POS;
		call_motor_handler(mot);
		return 0;
	}

	mot->ctrltype = CTRL_DIR;
	/* test for end-of-course positions */
	if (((newvalue > 0) && (motor_curr_position(mot) >= 1)) ||
			((newvalue < 0) && (motor_curr_position(mot) <= 0))) {
		/* trigger update with old value */
		iopar_set_dirty(&mot->dirpar);
		return -1;
	}
	mot->reqspeed = newvalue;
	call_motor_handler(mot);
	return 0;
}

static void cleanup_motorpar(struct motor *mot)
{
	/* stop motor */
	change_motor_speed(mot, 0);
	/* free power resources */
	if (mot->poweruri)
		free(mot->poweruri);
	if (mot->powerid)
		free(mot->powerid);

	/* cleanup child iopar's */
	destroy_iopar(mot->out1);
	destroy_iopar(mot->out2);

	/* cleanup myself */
	cleanup_libiopar(&mot->pospar);
	cleanup_libiopar(&mot->dirpar);
	free(mot);
}

static void del_motor_dir(struct iopar *iopar)
{
	struct motor *mot = dirpar2motor(iopar);

	if (mot->pospar.id)
		/* clear my id already */
		iopar->id = 0;
	else
		cleanup_motorpar(mot);
}

static void del_motor_pos(struct iopar *iopar)
{
	struct motor *mot = pospar2motor(iopar);

	if (mot->dirpar.id)
		/* clear my id already */
		iopar->id = 0;
	else
		cleanup_motorpar(mot);
}

/*
 * constructors:
 * mkmotordir is supposed to be called first,
 * and mkmotorpos is cached to be returned directly after that.
 */
static struct iopar *next_pospar;

struct iopar *mkmotorpos(char *cstr)
{
	struct iopar *result;

	(void)cstr;
	result = next_pospar;
	next_pospar = NULL;
	return result;
}

struct iopar *mkmotordir(char *str)
{
	struct motor *mot;
	char *tok, *saved;
	int ntok;

	mot = zalloc(sizeof(*mot));
	mot->flags = EOL0 | EOL1;
	mot->dirpar.del = del_motor_dir;
	mot->dirpar.set = set_motor_dir;
	mot->dirpar.value = 0;

	mot->pospar.del = del_motor_pos;
	mot->pospar.set = set_motor_pos;
	mot->pospar.value = 0;

	for (ntok = 0, tok = strtok_r(str, "+", &saved); tok && (ntok < 4); ++ntok, tok = strtok_r(NULL, "+", &saved))
	switch (ntok) {
	case 0:
		mot->type = lookup_motor_type(tok);
		if (mot->type < 0) {
			elog(LOG_ERR, 0, "%s: bad motor type '%s'", __func__, tok);
			goto fail_config;
		}
		break;
	case 1:
		mot->out1 = create_iopar(tok);
		if (mot->out1 <= 0) {
			elog(LOG_ERR, 0, "%s: bad output '%s'", __func__, tok);
			goto fail_config;
		}
		break;
	case 2:
		if (*tok == '/') {
			mot->flags |= INV2;
			++tok;
		}
		mot->out2 = create_iopar(tok);
		if (mot->out2 <= 0) {
			elog(LOG_ERR, 0, "%s: bad output '%s'", __func__, tok);
			goto fail_config;
		}
		break;
	case 3:
		mot->maxval = strtod(tok, NULL);
		break;
	}

	if (ntok < 4) {
		elog(LOG_ERR, 0, "%s: need arguments \"[MOTORTYPE(updown|godir)]+OUT1+[/]OUT2+MAXVAL[+eol0,eol1,noeol,power=URI#CID:VALUE]\"", __func__);
		goto fail_config;
	}

	/* parse the final ,-seperated options */
	for (tok = strtok_r(tok, ",", &saved); tok; tok = strtok_r(NULL, ",", &saved))
	if (!strcmp("eol0", tok))
		mot->flags = (mot->flags & ~EOL1) | EOL0;
	else if (!strcmp("eol1", tok))
		mot->flags = (mot->flags & ~EOL0) | EOL1;
	else if (!strcmp("noeol", tok))
		mot->flags &= ~(EOL0| EOL1);
	else if (!strncmp("power=", tok, 6)) {
		tok += 6;
		mot->poweruri = strdup(strtok(tok, "#"));
		mot->powerid = strdup(strtok(NULL, ":") ?: "motor");
		mot->powerval = strtod(strtok(NULL, ":") ?: "1", NULL);
	}

	/* fixups */
	if (mot->type == TYPE_UPDOWN)
		/* INV2 makes no sense for updown control */
		mot->flags &= ~INV2;

	/* final */
	iopar_set_present(&mot->pospar);
	iopar_set_present(&mot->dirpar);
	/* save next pospar */
	next_pospar = &mot->pospar;
	return &mot->dirpar;

fail_config:
	if (mot->out2)
		destroy_iopar(mot->out2);
	if (mot->out1)
		destroy_iopar(mot->out1);
	free(mot);
	return NULL;
}
