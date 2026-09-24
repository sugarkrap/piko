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

const struct ir_label ir_labels[] = {
	{ "KEY_POWER",		"Power",	"power" },
	{ "KEY_MUTE",		"Mute",		"power" },
	{ "KEY_SLEEP",		"Sleep",	"power" },

	{ "KEY_VOLUMEUP",	"Vol +",	"volume" },
	{ "KEY_VOLUMEDOWN",	"Vol -",	"volume" },
	{ "KEY_CHANNELUP",	"Ch +",		"volume" },
	{ "KEY_CHANNELDOWN",	"Ch -",		"volume" },

	{ "KEY_1",		"1",		"digits" },
	{ "KEY_2",		"2",		"digits" },
	{ "KEY_3",		"3",		"digits" },
	{ "KEY_4",		"4",		"digits" },
	{ "KEY_5",		"5",		"digits" },
	{ "KEY_6",		"6",		"digits" },
	{ "KEY_7",		"7",		"digits" },
	{ "KEY_8",		"8",		"digits" },
	{ "KEY_9",		"9",		"digits" },
	{ "KEY_0",		"0",		"digits" },

	{ "KEY_PREVIOUSSONG",	"Prev",		"transport" },
	{ "KEY_REWIND",		"Rew",		"transport" },
	{ "KEY_PLAY",		"Play",		"transport" },
	{ "KEY_PAUSE",		"Pause",	"transport" },
	{ "KEY_STOP",		"Stop",		"transport" },
	{ "KEY_FASTFORWARD",	"FFwd",		"transport" },
	{ "KEY_NEXTSONG",	"Next",		"transport" },
	{ "KEY_RECORD",		"Rec",		"transport" },
	{ "KEY_EJECTCD",	"Eject",	"transport" },

	{ "KEY_CD",		"CD",		"source" },
	{ "KEY_TUNER",		"Tuner",	"source" },
	{ "KEY_TAPE",		"Tape",		"source" },
	{ "KEY_AUX",		"Aux",		"source" },
	{ "KEY_TV",		"TV",		"source" },
	{ "KEY_VIDEO",		"Video",	"source" },
	{ "KEY_RADIO",		"Radio",	"source" },

	{ "KEY_UP",		"Up",		"navigate" },
	{ "KEY_DOWN",		"Down",		"navigate" },
	{ "KEY_LEFT",		"Left",		"navigate" },
	{ "KEY_RIGHT",		"Right",	"navigate" },
	{ "KEY_ENTER",		"OK",		"navigate" },
	{ "KEY_MENU",		"Menu",		"navigate" },
	{ "KEY_BACK",		"Back",		"navigate" },
	{ "KEY_EXIT",		"Exit",		"navigate" },
	{ "KEY_INFO",		"Info",		"navigate" },
	{ "KEY_HOME",		"Home",		"navigate" },
};

const unsigned int ir_labels_count = sizeof(ir_labels) / sizeof(ir_labels[0]);

static char store_dir[512];

const char *ir_label_for(const char *key)
{
	unsigned int i;

	for (i = 0; i < ir_labels_count; i++) {
		if (!strcmp(ir_labels[i].key, key))
			return ir_labels[i].label;
	}

	return key;
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

	b->edges = malloc(count * sizeof(*b->edges));
	if (!b->edges) {
		b->count = 0;
		return -1;
	}

	memcpy(b->edges, edges, count * sizeof(*b->edges));
	b->count = count;

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
		   const unsigned int *edges, unsigned int *count)
{
	unsigned int n = *count;

	if (key[0] && n) {
		if (!(n % 2))
			n--;
		if (n)
			ir_device_add(dev, key, label, edges, n);
	}

	key[0] = '\0';
	*count = 0;
}

struct ir_device *ir_device_load(const char *slug)
{
	char path[600];
	char line[512];
	char pending[IR_NAME_MAX];
	char key[IR_NAME_MAX];
	char label[IR_NAME_MAX];
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
	key[0] = '\0';
	label[0] = '\0';

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
		if (*p == '#' || !*p)
			continue;

		if (!strncmp(p, "begin raw_codes", 15)) {
			in_raw = 1;
			pending[0] = '\0';
			continue;
		}
		if (!strncmp(p, "end raw_codes", 13)
		    || !strncmp(p, "end remote", 10)) {
			commit(dev, key, label, edges, &count);
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

			commit(dev, key, label, edges, &count);
			snprintf(key, sizeof(key), "%s", v);
			snprintf(label, sizeof(label), "%s",
				 pending[0] ? pending : ir_label_for(v));
			pending[0] = '\0';

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

	commit(dev, key, label, edges, &count);

	free(edges);
	fclose(f);

	return dev;
}
