#ifndef BOX_H
#define BOX_H
#include <stdbool.h>

struct box {
	int x, y;
	int width, height;
};

bool box_contains_point(const struct box *box, double x, double y);
bool box_empty(const struct box *box);

#endif /* BOX_H */
