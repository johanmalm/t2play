#include "common/box.h"

bool
box_empty(const struct box *box)
{
	return !box || box->width <= 0 || box->height <= 0;
}

bool box_contains_point(const struct box *box, double x, double y) {
	if (box_empty(box)) {
		return false;
	} else {
		return x >= box->x && x < box->x + box->width &&
			y >= box->y && y < box->y + box->height;
	}
}
