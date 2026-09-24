#ifndef PIKO_IRSTORE_H
#define PIKO_IRSTORE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IR_DIR_DEFAULT		"/mnt/card/.zaurus/ir"
#define IR_NAME_MAX		64
#define IR_MAX_EDGES		1024
#define IR_MAX_DEVICES		64
#define IR_MAX_BUTTONS		128

struct ir_button {
	char		key[IR_NAME_MAX];
	char		label[IR_NAME_MAX];
	unsigned int	*edges;
	unsigned int	count;
};

struct ir_device {
	char			slug[IR_NAME_MAX];
	char			label[IR_NAME_MAX];
	struct ir_button	*buttons;
	unsigned int		count;
	unsigned int		alloc;
};

struct ir_label {
	const char	*key;
	const char	*label;
	const char	*group;
};

extern const struct ir_label	ir_labels[];
extern const unsigned int	ir_labels_count;

const char	*ir_label_for(const char *key);
void		 ir_slug(const char *label, char *out, size_t n);

const char	*ir_store_dir(void);
void		 ir_store_set_dir(const char *dir);
int		 ir_store_ensure(void);
int		 ir_store_list(char slugs[][IR_NAME_MAX], int max);

struct ir_device *ir_device_new(const char *label);
struct ir_device *ir_device_load(const char *slug);
int		  ir_device_save(const struct ir_device *dev);
int		  ir_device_delete(const char *slug);
void		  ir_device_free(struct ir_device *dev);
int		  ir_device_relabel(struct ir_device *dev, const char *label);

struct ir_button *ir_device_find(struct ir_device *dev, const char *key);
int		  ir_device_add(struct ir_device *dev, const char *key,
				const char *label, const unsigned int *edges,
				unsigned int count);
int		  ir_device_remove(struct ir_device *dev, const char *key);

#ifdef __cplusplus
}
#endif

#endif
