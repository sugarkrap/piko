#include "irstore.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

const struct ir_slot ir_slots[] = {
	{ "KEY_POWER",         "Power",  NULL,      "power",     1,   0, 0, 2,   0, 0, 2 },
	{ "KEY_MUTE",          "Mute",   NULL,      "power",     0,   0, 3, 2,   0, 2, 2 },
	{ "KEY_CHANNELDOWN",   "Ch -",   NULL,      "volume",    0,   3, 0, 1,   0, 4, 1 },
	{ "KEY_CHANNELUP",     "Ch +",   NULL,      "volume",    0,   3, 1, 1,   0, 5, 1 },
	{ "KEY_VOLUMEDOWN",    "Vol -",  NULL,      "volume",    0,   3, 3, 1,   0, 6, 1 },
	{ "KEY_VOLUMEUP",      "Vol +",  NULL,      "volume",    0,   3, 4, 1,   0, 7, 1 },
	{ "KEY_1",             "1",      NULL,      "digits",    0,   1, 0, 1,   1, 0, 1 },
	{ "KEY_2",             "2",      NULL,      "digits",    0,   1, 1, 1,   1, 1, 1 },
	{ "KEY_3",             "3",      NULL,      "digits",    0,   1, 2, 1,   1, 2, 1 },
	{ "KEY_4",             "4",      NULL,      "digits",    0,   1, 3, 1,   2, 0, 1 },
	{ "KEY_5",             "5",      NULL,      "digits",    0,   1, 4, 1,   2, 1, 1 },
	{ "KEY_6",             "6",      NULL,      "digits",    0,   2, 0, 1,   2, 2, 1 },
	{ "KEY_7",             "7",      NULL,      "digits",    0,   2, 1, 1,   3, 0, 1 },
	{ "KEY_8",             "8",      NULL,      "digits",    0,   2, 2, 1,   3, 1, 1 },
	{ "KEY_9",             "9",      NULL,      "digits",    0,   2, 3, 1,   3, 2, 1 },
	{ "KEY_0",             "0",      NULL,      "digits",    0,   2, 4, 1,   4, 1, 1 },
	{ "KEY_PREVIOUSSONG",  "Prev",   "@|<",     "transport", 0,   4, 0, 1,   1, 3, 1 },
	{ "KEY_REWIND",        "Rew",    "@<<",     "transport", 0,   4, 1, 1,   1, 4, 1 },
	{ "KEY_PLAY",          "Play",   "@>",      "transport", 0,   4, 2, 1,   1, 5, 1 },
	{ "KEY_FASTFORWARD",   "FFwd",   "@>>",     "transport", 0,   4, 3, 1,   1, 6, 1 },
	{ "KEY_NEXTSONG",      "Next",   "@>|",     "transport", 0,   4, 4, 1,   1, 7, 1 },
	{ "KEY_STOP",          "Stop",   "@square", "transport", 0,   5, 0, 1,   2, 3, 1 },
	{ "KEY_PAUSE",         "Pause",  "@||",     "transport", 0,   5, 1, 1,   2, 4, 1 },
	{ "KEY_RECORD",        "Rec",    "@circle", "transport", 1,   5, 2, 1,   2, 5, 1 },
	{ "KEY_EJECTCD",       "Eject",  NULL,      "transport", 0,   5, 3, 1,   2, 6, 1 },
	{ "KEY_CD",            "CD",     NULL,      "source",    0,   6, 0, 1,   3, 3, 1 },
	{ "KEY_TUNER",         "Tuner",  NULL,      "source",    0,   6, 1, 1,   3, 4, 1 },
	{ "KEY_TAPE",          "Tape",   NULL,      "source",    0,   6, 2, 1,   3, 5, 1 },
	{ "KEY_AUX",           "Aux",    NULL,      "source",    0,   6, 3, 1,   3, 6, 1 },
	{ "KEY_RADIO",         "Radio",  NULL,      "source",    0,   6, 4, 1,   3, 7, 1 },
	{ "KEY_TV",            "TV",     NULL,      "source",    0,   7, 0, 1,   4, 3, 1 },
	{ "KEY_VIDEO",         "Video",  NULL,      "source",    0,   7, 1, 1,   4, 4, 1 },
	{ "KEY_MENU",          "Menu",   NULL,      "navigate",  0,   8, 0, 1,   4, 1, 1 },
	{ "KEY_UP",            "Up",     "@8>",     "navigate",  0,   8, 2, 1,   4, 6, 1 },
	{ "KEY_INFO",          "Info",   NULL,      "navigate",  0,   8, 4, 1,   4, 7, 1 },
	{ "KEY_BACK",          "Back",   NULL,      "navigate",  0,   9, 0, 1,   4, 2, 1 },
	{ "KEY_LEFT",          "Left",   "@4>",     "navigate",  0,   9, 1, 1,   5, 5, 1 },
	{ "KEY_ENTER",         "OK",     NULL,      "navigate",  0,   9, 2, 1,   5, 6, 1 },
	{ "KEY_RIGHT",         "Right",  "@6>",     "navigate",  0,   9, 3, 1,   5, 7, 1 },
	{ "KEY_EXIT",          "Exit",   NULL,      "navigate",  0,   9, 4, 1,   5, 2, 1 },
	{ "KEY_HOME",          "Home",   NULL,      "navigate",  0,  10, 0, 1,   5, 1, 1 },
	{ "KEY_DOWN",          "Down",   "@2>",     "navigate",  0,  10, 2, 1,   6, 6, 1 },
	{ "KEY_SLEEP",         "Sleep",  NULL,      "navigate",  0,  10, 4, 1,   5, 3, 1 },
};

const unsigned int ir_slots_count = sizeof(ir_slots) / sizeof(ir_slots[0]);

static char store_dir[512];

const struct ir_slot *ir_slot_for(const char *key)
{
	unsigned int i;

	for (i = 0; i < ir_slots_count; i++) {
		if (!strcmp(ir_slots[i].key, key))
			return &ir_slots[i];
	}

	return NULL;
}

const char *ir_label_for(const char *key)
{
	const struct ir_slot *s = ir_slot_for(key);

	return s ? s->label : key;
}

const char *ir_glyph_for(const char *key)
{
	const struct ir_slot *s = ir_slot_for(key);

	return s && s->glyph ? s->glyph : "";
}

int ir_major_for(const char *key)
{
	const struct ir_slot *s = ir_slot_for(key);

	return s ? s->major : 0;
}

void ir_slug(const char *label, char *out, size_t n)
{
	size_t w = 0;
	int dash = 0;

	while (*label && w + 1 < n) {
		unsigned char c = (unsigned char)*label++;

		if (isalnum(c)) {
			out[w++] = (char)tolower(c);
			dash = 0;
		} else if (w && !dash) {
			out[w++] = '-';
			dash = 1;
		}
	}

	while (w && out[w - 1] == '-')
		w--;

	if (!w && n > 1) {
		strncpy(out, "device", n - 1);
		out[n - 1] = '\0';
		return;
	}

	out[w] = '\0';
}

const char *ir_store_dir(void)
{
	const char *env;

	if (store_dir[0])
		return store_dir;

	env = getenv("PIKO_IR_DIR");
	if (env && *env)
		snprintf(store_dir, sizeof(store_dir), "%s", env);
	else
		snprintf(store_dir, sizeof(store_dir), "%s", IR_DIR_DEFAULT);

	return store_dir;
}

void ir_store_set_dir(const char *dir)
{
	snprintf(store_dir, sizeof(store_dir), "%s", dir);
}

static int mkdir_p(const char *path)
{
	char buf[512];
	size_t i;

	snprintf(buf, sizeof(buf), "%s", path);

	for (i = 1; buf[i]; i++) {
		if (buf[i] != '/')
			continue;
		buf[i] = '\0';
		if (mkdir(buf, 0755) && errno != EEXIST)
			return -1;
		buf[i] = '/';
	}

	if (mkdir(buf, 0755) && errno != EEXIST)
		return -1;

	return 0;
}

int ir_store_ensure(void)
{
	return mkdir_p(ir_store_dir());
}

static void path_for(const char *slug, char *out, size_t n)
{
	snprintf(out, n, "%s/%s.conf", ir_store_dir(), slug);
}

int ir_store_list(char slugs[][IR_NAME_MAX], int max)
{
	DIR *dir = opendir(ir_store_dir());
	struct dirent *ent;
	int n = 0;

	if (!dir)
		return 0;

	while (n < max && (ent = readdir(dir))) {
		size_t len = strlen(ent->d_name);

		if (len < 6 || strcmp(ent->d_name + len - 5, ".conf"))
			continue;
		if (len - 5 >= IR_NAME_MAX)
			continue;

		memcpy(slugs[n], ent->d_name, len - 5);
		slugs[n][len - 5] = '\0';
		n++;
	}

	closedir(dir);

	return n;
}

struct ir_device *ir_device_new(const char *label)
{
	struct ir_device *dev = calloc(1, sizeof(*dev));

	if (!dev)
		return NULL;

	snprintf(dev->label, sizeof(dev->label), "%s", label);
	ir_slug(label, dev->slug, sizeof(dev->slug));

	return dev;
}

void ir_device_free(struct ir_device *dev)
{
	unsigned int i;

	if (!dev)
		return;

	for (i = 0; i < dev->count; i++)
		free(dev->buttons[i].edges);

	free(dev->buttons);
	free(dev);
}

int ir_device_relabel(struct ir_device *dev, const char *label)
{
	if (!dev || !label || !*label)
		return -1;

	snprintf(dev->label, sizeof(dev->label), "%s", label);

	return 0;
}

struct ir_button *ir_device_find(struct ir_device *dev, const char *key)
{
	unsigned int i;

	if (!dev)
		return NULL;

	for (i = 0; i < dev->count; i++) {
		if (!strcmp(dev->buttons[i].key, key))
			return &dev->buttons[i];
	}

	return NULL;
}

static struct ir_button *grow(struct ir_device *dev)
{
	if (dev->count == dev->alloc) {
		unsigned int want = dev->alloc ? dev->alloc * 2 : 8;
		struct ir_button *next;

		if (want > IR_MAX_BUTTONS)
			want = IR_MAX_BUTTONS;
		if (dev->count == want)
			return NULL;

		next = realloc(dev->buttons, want * sizeof(*next));
		if (!next)
			return NULL;

		dev->buttons = next;
		dev->alloc = want;
	}

	memset(&dev->buttons[dev->count], 0, sizeof(dev->buttons[dev->count]));

	return &dev->buttons[dev->count++];
}

int ir_device_add(struct ir_device *dev, const char *key, const char *label,
		  const unsigned int *edges, unsigned int count)
{
	struct ir_button *b;

	if (!dev || !key || !*key || !count || count > IR_MAX_EDGES)
		return -1;
	if (!(count % 2))
		return -1;

	b = ir_device_find(dev, key);
	if (!b) {
		b = grow(dev);
		if (!b)
			return -1;
	} else {
		free(b->edges);
		b->edges = NULL;
	}

	snprintf(b->key, sizeof(b->key), "%s", key);
	snprintf(b->label, sizeof(b->label), "%s",
		 label && *label ? label : ir_label_for(key));
	snprintf(b->glyph, sizeof(b->glyph), "%s", ir_glyph_for(key));
	b->major = (unsigned char)ir_major_for(key);

	b->edges = malloc(count * sizeof(*b->edges));
	if (!b->edges) {
		b->count = 0;
		return -1;
	}

	memcpy(b->edges, edges, count * sizeof(*b->edges));
	b->count = count;

	return 0;
}

int ir_device_style(struct ir_device *dev, const char *key, const char *glyph,
		    int major)
{
	struct ir_button *b = ir_device_find(dev, key);

	if (!b)
		return -1;

	snprintf(b->glyph, sizeof(b->glyph), "%s", glyph ? glyph : "");
	b->major = major ? 1 : 0;

	return 0;
}

int ir_device_remove(struct ir_device *dev, const char *key)
{
	unsigned int i;

	if (!dev)
		return -1;

	for (i = 0; i < dev->count; i++) {
		if (strcmp(dev->buttons[i].key, key))
			continue;

		free(dev->buttons[i].edges);
		memmove(&dev->buttons[i], &dev->buttons[i + 1],
			(dev->count - i - 1) * sizeof(dev->buttons[0]));
		dev->count--;

		return 0;
	}

	return -1;
}

int ir_device_save(const struct ir_device *dev)
{
	char path[600];
	char tmp[608];
	FILE *f;
	unsigned int i, j;

	if (!dev || !dev->slug[0])
		return -1;
	if (ir_store_ensure())
		return -1;

	path_for(dev->slug, path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.new", path);

	f = fopen(tmp, "w");
	if (!f)
		return -1;

	fprintf(f, "# piko-label: %s\n\n", dev->label);
	fprintf(f, "begin remote\n");
	fprintf(f, "  name  %s\n", dev->slug);
	fprintf(f, "  flags RAW_CODES\n");
	fprintf(f, "  eps   30\n");
	fprintf(f, "  aeps  100\n");
	fprintf(f, "  gap   110000\n");
	fprintf(f, "\n  begin raw_codes\n");

	for (i = 0; i < dev->count; i++) {
		const struct ir_button *b = &dev->buttons[i];

		fprintf(f, "\n    # piko-label: %s\n", b->label);
		if (b->glyph[0])
			fprintf(f, "    # piko-glyph: %s\n", b->glyph);
		if (b->major)
			fprintf(f, "    # piko-style: major\n");
		fprintf(f, "    name %s", b->key);

		for (j = 0; j < b->count; j++) {
			if (!(j % 6))
				fprintf(f, "\n     ");
			fprintf(f, " %7u", b->edges[j]);
		}
		fprintf(f, "\n");
	}

	fprintf(f, "\n  end raw_codes\nend remote\n");

	if (fflush(f) || fsync(fileno(f))) {
		fclose(f);
		unlink(tmp);
		return -1;
	}
	if (fclose(f)) {
		unlink(tmp);
		return -1;
	}
	if (rename(tmp, path)) {
		unlink(tmp);
		return -1;
	}

	return 0;
}

int ir_device_delete(const char *slug)
{
	char path[600];

	if (!slug || !*slug)
		return -1;

	path_for(slug, path, sizeof(path));

	return unlink(path);
}

static char *trim(char *s)
{
	char *e;

	while (*s == ' ' || *s == '\t')
		s++;

	e = s + strlen(s);
	while (e > s && (e[-1] == '\n' || e[-1] == '\r'
			 || e[-1] == ' ' || e[-1] == '\t'))
		*--e = '\0';

	return s;
}

static void commit(struct ir_device *dev, char *key, const char *label,
		   const char *glyph, int major, const unsigned int *edges,
		   unsigned int *count)
{
	unsigned int n = *count;

	if (key[0] && n) {
		if (!(n % 2))
			n--;
		if (n && !ir_device_add(dev, key, label, edges, n)) {
			if (glyph && *glyph)
				ir_device_style(dev, key, glyph, major);
			else if (major)
				ir_device_style(dev, key,
						ir_glyph_for(key), 1);
		}
	}

	key[0] = '\0';
	*count = 0;
}

struct ir_device *ir_device_load(const char *slug)
{
	char path[600];
	char line[512];
	char pending[IR_NAME_MAX];
	char pglyph[IR_GLYPH_MAX];
	char key[IR_NAME_MAX];
	char label[IR_NAME_MAX];
	char glyph[IR_GLYPH_MAX];
	int pmajor = 0;
	int major = 0;
	unsigned int *edges;
	unsigned int count = 0;
	int in_raw = 0;
	struct ir_device *dev;
	FILE *f;

	if (!slug || !*slug)
		return NULL;

	path_for(slug, path, sizeof(path));
	f = fopen(path, "r");
	if (!f)
		return NULL;

	dev = calloc(1, sizeof(*dev));
	edges = malloc(IR_MAX_EDGES * sizeof(*edges));
	if (!dev || !edges) {
		free(dev);
		free(edges);
		fclose(f);
		return NULL;
	}

	snprintf(dev->slug, sizeof(dev->slug), "%s", slug);
	snprintf(dev->label, sizeof(dev->label), "%s", slug);
	pending[0] = '\0';
	pglyph[0] = '\0';
	key[0] = '\0';
	label[0] = '\0';
	glyph[0] = '\0';

	while (fgets(line, sizeof(line), f)) {
		char *p = trim(line);

		if (!strncmp(p, "# piko-label:", 13)) {
			char *v = trim(p + 13);

			if (in_raw)
				snprintf(pending, sizeof(pending), "%s", v);
			else
				snprintf(dev->label, sizeof(dev->label), "%s", v);
			continue;
		}
		if (!strncmp(p, "# piko-glyph:", 13)) {
			snprintf(pglyph, sizeof(pglyph), "%s", trim(p + 13));
			continue;
		}
		if (!strncmp(p, "# piko-style:", 13)) {
			pmajor = !strcmp(trim(p + 13), "major");
			continue;
		}
		if (*p == '#' || !*p)
			continue;

		if (!strncmp(p, "begin raw_codes", 15)) {
			in_raw = 1;
			pending[0] = '\0';
			pglyph[0] = '\0';
			pmajor = 0;
			continue;
		}
		if (!strncmp(p, "end raw_codes", 13)
		    || !strncmp(p, "end remote", 10)) {
			commit(dev, key, label, glyph, major, edges, &count);
			in_raw = 0;
			continue;
		}
		if (!strncmp(p, "begin remote", 12))
			continue;

		if (!strncmp(p, "name", 4) && (p[4] == ' ' || p[4] == '\t')) {
			char *v = trim(p + 4);
			char *rest = strpbrk(v, " \t");

			if (!in_raw)
				continue;

			if (rest)
				*rest++ = '\0';

			commit(dev, key, label, glyph, major, edges, &count);
			snprintf(key, sizeof(key), "%s", v);
			snprintf(label, sizeof(label), "%s",
				 pending[0] ? pending : ir_label_for(v));
			snprintf(glyph, sizeof(glyph), "%s",
				 pglyph[0] ? pglyph : ir_glyph_for(v));
			major = pmajor ? 1 : ir_major_for(v);
			pending[0] = '\0';
			pglyph[0] = '\0';
			pmajor = 0;

			if (rest) {
				char *q = trim(rest);

				while (*q) {
					unsigned long us;
					char *end;

					us = strtoul(q, &end, 10);
					if (end == q)
						break;
					if (us && count < IR_MAX_EDGES)
						edges[count++] = (unsigned int)us;
					q = end;
					while (*q == ' ' || *q == '\t')
						q++;
				}
			}
			continue;
		}

		if (in_raw && key[0] && isdigit((unsigned char)*p)) {
			char *q = p;

			while (*q) {
				unsigned long us;
				char *end;

				us = strtoul(q, &end, 10);
				if (end == q)
					break;
				if (us && count < IR_MAX_EDGES)
					edges[count++] = (unsigned int)us;
				q = end;
				while (*q == ' ' || *q == '\t')
					q++;
			}
			continue;
		}
	}

	commit(dev, key, label, glyph, major, edges, &count);

	free(edges);
	fclose(f);

	return dev;
}
