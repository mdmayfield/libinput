/*
 * Copyright © 2014-2015 Red Hat, Inc.
 * Copyright © 2018 Matt Mayfield
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "config.h"

#include <math.h>
#include <stdbool.h>
#include <limits.h>

#include "evdev-mt-touchpad.h"

#define PINCH_THRESHOLD 2.0 /* mm movement before "not a pinch" */
#define SCROLL_MM_X 35
#define SCROLL_MM_Y 25

static inline const char*
thumb_state_to_str(enum tp_thumb_state state)
{
	switch(state) {
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
tp_thumb_hw_says_finger(const struct tp_dispatch *tp, const struct tp_touch *t)
{
	if (tp->thumb.use_size &&
	    t->major <= tp->thumb.size_threshold &&
	    t->minor <= tp->thumb.size_threshold)
		return true;

	if (tp->thumb.use_pressure &&
	    t->pressure <= tp->thumb.pressure_threshold)
		return true;

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
	if ((t->point.y < tp->thumb.upper_thumb_line) ||
	    (t->speed.exceeded_count == 10) ||
	    (tp_thumb_hw_says_finger(tp, t)))
		return true;
	else
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

bool
tp_thumb_gesture_active(const struct tp_touch *t)
{
	return (t->thumb.state == THUMB_STATE_LIVE ||
		t->thumb.state == THUMB_STATE_GESTURE ||
		t->thumb.state == THUMB_STATE_REVIVED);
}


// called in tp_process_state inside tp_for_each_touch. Do we even need *tp?
void
tp_thumb_update(struct tp_dispatch *tp, struct tp_touch *t)
{

	if (!tp->thumb.detect_thumbs)
		return;

	switch (t->thumb.state) {
	case THUMB_STATE_NEW:
		t->thumb.initial = t->point;

		if (tp_thumb_needs_jail(tp, t))
			tp_thumb_set_state(tp, t, THUMB_STATE_JAILED);
		else
			tp_thumb_set_state(tp, t, THUMB_STATE_LIVE);
		return;			

	case THUMB_STATE_JAILED:
		if (tp_thumb_escaped_jail(tp, t))
			tp_thumb_set_state(tp, t, THUMB_STATE_LIVE);
		return;

	case THUMB_STATE_REV_JAILED:
		if (tp_thumb_escaped_jail(tp, t))
			tp_thumb_set_state(tp, t, THUMB_STATE_REVIVED);
		return;

	case THUMB_STATE_SUPPRESSED:
	case THUMB_STATE_GESTURE:
	if (tp->nfingers_down == 1) {
		t->thumb.initial = t->point;

		if (tp_thumb_needs_jail(tp, t))
			tp_thumb_set_state(tp, t, THUMB_STATE_REV_JAILED);
		else
			tp_thumb_set_state(tp, t, THUMB_STATE_REVIVED);
		return;
	}

	/* LIVE: do nothing; will be updated by context
	 * REVIVED: do nothing; will be updated by context
	 * DEAD: do nothing; "it's dead, Jim"
	 */
	default:
		return;
	}
}

void
tp_thumb_update_by_context(struct tp_dispatch *tp)
{
	struct tp_touch *t;
	struct tp_touch *first = NULL,
			*second = NULL,
			*newest = NULL;
	struct device_coords distance;
	struct phys_coords mm;
	unsigned int speed_exceeded_count = 0;

	/* Get the first and second bottom-most touches, the max speed exceeded
	 * count overall, and the newest touch (or one of them, if more).
	 */
	tp_for_each_touch(tp, t) {
		if (t->state == TOUCH_NONE ||
		    t->state == TOUCH_HOVERING)
			continue;

		if (t->state == TOUCH_BEGIN)
			newest = t;

		speed_exceeded_count = max(speed_exceeded_count,
		                           t->speed.exceeded_count);

		if (!first) {
			first = t;
			continue;
		}

		if (t->point.y > first->point.y) {
			second = first;
			first = t;
			continue;
		}

		if (!second || t->point.y > second->point.y ) {
			second = t;
		}
	}

	// find 1st/2nd bottom-most touches, max speed, newest
	// detect thumb by speed (SUPPRESSED if detect_thumbs, else DEAD)
	// if !detect_thumbs, return

	assert(first);
	assert(second);

	distance.x = abs(first->point.x - second->point.x);
	distance.y = abs(first->point.y - second->point.y);
	mm = evdev_device_unit_delta_to_mm(tp->device, &distance);

	/* If there's a new touch, and an existing touch is moving while
	 * (2fg scrolling is disabled OR the touches are far apart), the new
	 * touch is a thumb.
	 */
	if (newest &&
	    tp->nfingers_down == 2 &&
	    speed_exceeded_count > 5 &&
	     (tp->scroll.method != LIBINPUT_CONFIG_SCROLL_2FG ||
		  (mm.x > SCROLL_MM_X && mm.y > SCROLL_MM_Y))) {
		evdev_log_debug(tp->device,
				"touch %d is speed-based thumb\n",
				newest->index);
		if (tp->thumb.detect_thumbs)
			tp_thumb_set_state(tp, newest, THUMB_STATE_SUPPRESSED);
		else
			tp_thumb_set_state(tp, newest, THUMB_STATE_DEAD);
	}

	/* Don't use other thumb detection if not enabled for the device */
	if (!tp->thumb.detect_thumbs)
		return;

	/* Enable responsive 2+ finger swipes/scrolls from the bottom of the
	 * touchpad: if a new touch appears, and the first AND second bottom-
	 * most touches are below the upper_thumb_line and close to each other,
	 * set newest, first, and second to LIVE. (Two of these will be the
	 * same touch if nfingers_down == 2; that's OK)
	 */
	if (newest &&
	    tp->nfingers_down >=2 &&
	    first->point.y > tp->thumb.upper_thumb_line &&
	    second->point.y > tp->thumb.upper_thumb_line &&
	    mm.x <= SCROLL_MM_X && mm.y <= SCROLL_MM_Y) {
		tp_thumb_set_state(tp, newest, THUMB_STATE_LIVE);
		tp_thumb_set_state(tp, first, THUMB_STATE_LIVE);
		tp_thumb_set_state(tp, second, THUMB_STATE_LIVE);
		return;
	}

	switch(first->thumb.state) {
	case THUMB_STATE_LIVE:
	case THUMB_STATE_JAILED:
	/* If touches are close together, probably a swipe or scroll */
		if (mm.x <= SCROLL_MM_X && mm.y <= SCROLL_MM_Y)
			break;

		distance.x = abs(first->point.x - first->thumb.initial.x);
		distance.y = abs(first->point.y - first->thumb.initial.y);
		mm = evdev_device_unit_delta_to_mm(tp->device, &distance);

		if (hypot(mm.x, mm.y) < PINCH_THRESHOLD)
			tp_thumb_set_state(tp, first, THUMB_STATE_GESTURE);
		else
			tp_thumb_set_state(tp, first, THUMB_STATE_SUPPRESSED);
		break;

	case THUMB_STATE_REVIVED:
	case THUMB_STATE_REV_JAILED:
	/* If touches are close together, probably a swipe or scroll */
		if (mm.x <= SCROLL_MM_X && mm.y <= SCROLL_MM_Y)
			break;

		tp_thumb_set_state(tp, first, THUMB_STATE_DEAD);
		break;

	default:
		break;
	}

}

void
tp_thumb_update_in_gesture(struct tp_dispatch *tp)
{
	struct tp_touch *left = tp->gesture.touches[0];
	struct tp_touch *right = tp->gesture.touches[1];
	struct tp_touch *lowest;
	double left_moved,
	       right_moved;
	struct device_coords temp_dist;
	struct phys_coords temp_mm;

	lowest = (left->gesture.initial.y > right->gesture.initial.y) ?
		left : right;

	/* Gesture thumb handling: If either of the significant gesture
	 * touches exceeds the speed threshold, while the other touch has
	 * not moved more than 2mm, the lowest touch becomes a thumb and
	 * the gesture is cancelled.
	 */

	temp_dist.x = abs(left->point.x - left->gesture.initial.x);
	temp_dist.y = abs(left->point.y - left->gesture.initial.y);
	temp_mm = evdev_device_unit_delta_to_mm(tp->device, &temp_dist);
	left_moved = hypot(temp_mm.x, temp_mm.y);

	temp_dist.x = abs(right->point.x - right->gesture.initial.x);
	temp_dist.y = abs(right->point.y - right->gesture.initial.y);
	temp_mm = evdev_device_unit_delta_to_mm(tp->device, &temp_dist);
	right_moved = hypot(temp_mm.x, temp_mm.y);

	if ((left_moved <= 2.0 && right_moved > 2.0 &&
	     right->speed.exceeded_count > 5) ||
	    (right_moved <= 2.0 && left_moved > 2.0 &&
	     left->speed.exceeded_count > 5))
		tp_thumb_set_state(tp, lowest, THUMB_STATE_SUPPRESSED);
	return;
}
