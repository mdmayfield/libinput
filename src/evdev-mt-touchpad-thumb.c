/*
 * Copyright © notice goes here
 *
 * License goes here
 */

#include "config.h"

#include <math.h>
#include <stdbool.h>
#include <limits.h>

#include "evdev-mt-touchpad.h"

static inline const char*
thumb_state_to_str(enum tp_thumb_state state)
{
	switch(state){
	CASE_RETURN_STRING(THUMB_STATE_NEW);
	CASE_RETURN_STRING(THUMB_STATE_LIVE);
	CASE_RETURN_STRING(THUMB_STATE_JAILED);
	CASE_RETURN_STRING(THUMB_STATE_GESTURE);
	CASE_RETURN_STRING(THUMB_STATE_SUPPRESSED);
	CASE_RETURN_STRING(THUMB_STATE_REVIVED);
	CASE_RETURN_STRING(THUMB_STATE_REV_JAILED);
	CASE_RETURN_STRING(THUMB_STATE_DEAD);
	}

	return NULL;
}

static void
tp_thumb_set_state(struct tp_dispatch *tp,
		   struct tp_touch *t,
		   enum tp_thumb_state state)
{
	if (t->thumb.state != state) {
		evdev_log_debug(tp->device,
			"thumb state: touch %d, %s → %s\n",
			t->index,
			thumb_state_to_str(t->thumb.state),
			thumb_state_to_str(state));
	}

	t->thumb.state = state;
}

static bool
tp_thumb_detect_hw_present(const struct tp_dispatch *tp)
{
	return (tp->pressure.use_pressure || tp->touch_size.use_touch_size);
}

static bool
tp_thumb_hw_says_finger(const struct tp_dispatch *tp, const struct tp_touch *t)
{
	// TODO detect with size and/or pressure here, return true if finger
	// return false if there is no HW available to detect
	return false;
}

static bool
tp_thumb_needs_jail(const struct tp_dispatch *tp, const struct tp_touch *t)
{

	if (t->point.y < tp->thumb.upper_thumb_line)
		return false;
	if (t->point.y < tp->thumb.lower_thumb_line &&
	    tp_thumb_hw_says_finger(tp, t))
		return false;

	/* All touches below lower_thumb_line, and touches below the upper_
	 * thumb_line that hardware can't verify are fingers, become JAILED
	 */
	return true;
}

static bool
tp_thumb_escaped_jail(const struct tp_dispatch *tp, const struct tp_touch *t)
{
	// TODO if touch goes over upper_thumb_line, or speed threshold
	// exceeded, return true. Else false
	return false;
}

bool
tp_thumb_edge_scroll_ignore(const struct tp_touch *t)
{
	return (t->thumb.state == THUMB_STATE_GESTURE ||
		t->thumb.state == THUMB_STATE_SUPPRESSED ||
		t->thumb.state == THUMB_STATE_DEAD);
}

bool
tp_thumb_tap_ignore(const struct tp_touch *t)
{
	return (t->thumb.state == THUMB_STATE_GESTURE ||
		t->thumb.state == THUMB_STATE_SUPPRESSED ||
		t->thumb.state == THUMB_STATE_DEAD);
}

bool
tp_thumb_clickfinger_ignore(const struct tp_touch *t)
{
	return (t->thumb.state == THUMB_STATE_SUPPRESSED ||
		t->thumb.state == THUMB_STATE_DEAD);
}

bool
tp_thumb_considered_active(const struct tp_touch *t)
{
	return (t->thumb.state == THUMB_STATE_LIVE ||
		t->thumb.state == THUMB_STATE_REVIVED);
}

// called in tp_process_state inside tp_for_each_touch. Do we even need *tp?
void
tp_thumb_update(struct tp_dispatch *tp, struct tp_touch *t)
{
	switch (t->thumb.state) {
	/* NEW -> LIVE or JAILED, set t->thumb.initial = t->point */
	case THUMB_STATE_NEW:
		t->thumb.initial = t->point;

		if (tp_thumb_needs_jail(tp, t))
			tp_thumb_set_state(tp, t, THUMB_STATE_JAILED);
		else
			tp_thumb_set_state(tp, t, THUMB_STATE_LIVE);
		return;			

	/* JAILED -> LIVE if speed exceeded or y<utl; else handled in context */
	case THUMB_STATE_JAILED:
		if (tp_thumb_escaped_jail(tp, t))
			tp_thumb_set_state(tp, t, THUMB_STATE_LIVE);
		return;

	/* REV_JAILED -> REVIVED if speed or y<utl; else handled in context */
	case THUMB_STATE_REV_JAILED:
		if (tp_thumb_escaped_jail(tp, t))
			tp_thumb_set_state(tp, t, THUMB_STATE_REVIVED);
		return;

	/* LIVE -> do nothing; will be handled in context
	 * GESTURE -> do nothing; tp_gesture_handle_state_unknown takes this
	 * SUPPRESSED -> do nothing; will be handled in context
	 * REVIVED -> do nothing; will be handled in context
	 * DEAD -> do nothing
	 */
	}
}

// need to call this whenever the number of fingers changes (not just on new touch)
void
tp_thumb_detect_by_context(struct tp_dispatch *tp)
{
	// if only one touch exists:
	//  SUPPRESSED -> REVIVED or REV_JAILED; return
	// else do nothing & return

	// find 1st/2nd bottom-most touches, max speed, newest
	// detect thumb by speed (SUPPRESSED if detect_thumbs, else DEAD)
	// if !detect_thumbs, return

	// first (lowest) touch:
	// case LIVE, case JAILED:
	// 	-> GESTURE (if thumb.initial <2mm from point) or
	// 	-> SUPPRESSED otherwise 
	// case GESTURE -> do nothing
	// case SUPPRESSED -> do nothing
	// case REVIVED, case REVIVED_JAILED:
	// 	-> DEAD
}


#if 0
static void
tp_detect_thumb_while_moving(struct tp_dispatch *tp)
{
	struct tp_touch *t;
	struct tp_touch *first = NULL,
			*second = NULL;
	struct device_coords distance;
	struct phys_coords mm;

	tp_for_each_touch(tp, t) {
		if (t->state == TOUCH_NONE ||
		    t->state == TOUCH_HOVERING)
			continue;

		if (t->state != TOUCH_BEGIN)
			first = t;
		else
			second = t;

		if (first && second)
			break;
	}

	assert(first);
	assert(second);

	if (tp->scroll.method == LIBINPUT_CONFIG_SCROLL_2FG) {
		/* If the second finger comes down next to the other one, we
		 * assume this is a scroll motion.
		 */
		distance.x = abs(first->point.x - second->point.x);
		distance.y = abs(first->point.y - second->point.y);
		mm = evdev_device_unit_delta_to_mm(tp->device, &distance);

		if (mm.x <= 25 && mm.y <= 15)
			return;
	}

	/* Finger are too far apart or 2fg scrolling is disabled, mark
	 * second finger as thumb */
	evdev_log_debug(tp->device,
			"touch %d is speed-based thumb\n",
			second->index);
	second->thumb.state = THUMB_STATE_YES;
}
#endif
