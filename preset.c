#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <error.h>

#include "_libio.h"

struct lookup {
	struct lookup *next, *prev;

	char *key;
	char *value;
	char buf[2];

};

static struct {
	struct lookup *first, *last;
	int level;
	int loaded;
} s;

/* load presets of 1 named file */
static void load_presets_file(const char *file)
{
	FILE *fp;
	int ret;
	char *line = NULL, *key, *value;
	size_t linesize = 0;
	struct lookup *ptr;

	fp = fopen(file, "r");
	if (!fp) {
		if (errno != ENOENT)
			error(0, errno, "open %s", file);
		return;
	}
	while (!feof(fp)) {
		ret = getline(&line, &linesize, fp);
		if (ret <= 0)
			break;
		line[ret] = 0;
		/* strip trailing newline */
		if (line[ret-1] == '\n')
			line[--ret] = 0;
		/* test for comments or empty lines */
		if (strchr("#\n", *line))
			continue;
		key = strtok(line, "\t");
		value = strtok(NULL, "\t");
		if (!strcmp(key, "include")) {
			load_presets_file(value);
			continue;
		}
		/* create entry */
		ptr = zalloc(sizeof(*ptr) + ret);
		strcpy(ptr->buf, key);
		ptr->key = ptr->buf;
		ptr->value = ptr->key + strlen(ptr->key) + 1;
		strcpy(ptr->value, value);
		/* add in linked list */
		if (s.last) {
			ptr->prev = s.last;
			ptr->prev->next = ptr;
		}
		s.last = ptr;
		if (!s.first)
			s.first = ptr;
	}
	fclose(fp);
	return;
}

/* load all system-wide & user definded presets */
static void load_presets(void)
{
	load_presets_file(".libio-presets");
	load_presets_file("/etc/libio-presets.conf");
	s.loaded = 1;
}

__attribute__((destructor))
static void free_presets(void)
{
	struct lookup *ptr;

	while (s.first) {
		ptr = s.first;
		s.first = ptr->next;
		free(ptr);
	}
}

struct iopar *mkpreset(const char *str)
{
	struct lookup *lp;

	if (!s.loaded)
		load_presets();
	if (s.level++ > 10)
		error(1, 0, "%s: max. nesting reached, are you looping?", __func__);
	for (lp = s.first; lp; lp = lp->next) {
		if (!strcmp(lp->key, str)) {
			--s.level;
			return create_libiopar(lp->value);
		}
	}
	--s.level;
	error(0, 0, "preset %s not found", str);
	return NULL;
}