#include "panel.h"
#include <assert.h>
#include <wlr/util/log.h>

void
plugin_clock_create(struct panel *panel)
{
	struct clock *clock = znew(*clock);
	clock->base.type = WIDGET_CLOCK;
	wl_list_insert(panel->widgets.prev, &clock->base.link);
}
