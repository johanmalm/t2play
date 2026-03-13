// SPDX-License-Identifier: GPL-2.0-only
#include "panel.h"
#include <wlr/util/log.h>

void
plugin_clock_create(struct panel *panel)
{
	struct clock *clock = znew(*clock);
	clock->base.type = WIDGET_CLOCK;
	wl_list_insert(panel->widgets.prev, &clock->base.link);
}
