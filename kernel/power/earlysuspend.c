/* kernel/power/earlysuspend.c
 *
 * Copyright (C) 2005-2008 Google, Inc.
 * Copyright (c) 2012, Code Aurora Forum. All rights reserved.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/earlysuspend.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/rtc.h>
#include <linux/wakelock.h>
#include <linux/workqueue.h>

#ifdef CONFIG_MSM_SM_EVENT
#include <linux/sm_event_log.h>
#include <linux/sm_event.h>
#endif

#include "power.h"

enum {
	DEBUG_USER_STATE = 1U << 0,
	DEBUG_SUSPEND = 1U << 2,
	DEBUG_VERBOSE = 1U << 3,
};
static int debug_mask = DEBUG_USER_STATE | DEBUG_SUSPEND | DEBUG_VERBOSE;
module_param_named(debug_mask, debug_mask, int, S_IRUGO | S_IWUSR | S_IWGRP);

static DEFINE_MUTEX(early_suspend_lock);
static LIST_HEAD(early_suspend_handlers);
static void early_suspend(struct work_struct *work);
static void late_resume(struct work_struct *work);
static DECLARE_WORK(early_suspend_work, early_suspend);
static DECLARE_WORK(late_resume_work, late_resume);
static DEFINE_SPINLOCK(state_lock);
enum {
	SUSPEND_REQUESTED = 0x1,
	SUSPENDED = 0x2,
	SUSPEND_REQUESTED_AND_SUSPENDED = SUSPEND_REQUESTED | SUSPENDED,
};
static int state;

void register_early_suspend(struct early_suspend *handler)
{
	struct list_head *pos;

	mutex_lock(&early_suspend_lock);
	list_for_each(pos, &early_suspend_handlers) {
		struct early_suspend *e;
		e = list_entry(pos, struct early_suspend, link);
		if (e->level > handler->level)
			break;
	}
	list_add_tail(&handler->link, pos);
	if ((state & SUSPENDED) && handler->suspend)
		handler->suspend(handler);
	mutex_unlock(&early_suspend_lock);
}
EXPORT_SYMBOL(register_early_suspend);

void unregister_early_suspend(struct early_suspend *handler)
{
	mutex_lock(&early_suspend_lock);
	list_del(&handler->link);
	mutex_unlock(&early_suspend_lock);
}
EXPORT_SYMBOL(unregister_early_suspend);

/* yangjq, 20121127, Add to show consumed time, START */
static void dpm_show_time(ktime_t starttime, char *info)
{
	ktime_t calltime;
	u64 usecs64;
	int usecs;

	calltime = ktime_get();
	usecs64 = ktime_to_ns(ktime_sub(calltime, starttime));
	do_div(usecs64, NSEC_PER_USEC);
	usecs = usecs64;
	if (usecs == 0)
		usecs = 1;
	pr_info("PM: %s of devices complete after %ld.%03ld msecs\n",
		info ?: "", 
		usecs / USEC_PER_MSEC, usecs % USEC_PER_MSEC);
}
/* yangjq, 20121127, Add to show consumed time, END */

static void early_suspend(struct work_struct *work)
{
	struct early_suspend *pos;
	unsigned long irqflags;
	int abort = 0;
	/* yangjq, 20121127, Add to show consumed time */
	ktime_t starttime = ktime_get();

	mutex_lock(&early_suspend_lock);
#ifdef CONFIG_MSM_SM_EVENT
	sm_set_system_state (SM_STATE_EARLYSUSPEND);
	sm_add_event(SM_POWER_EVENT | SM_POWER_EVENT_EARLY_SUSPEND, SM_EVENT_START, 0, NULL, 0);
	add_active_wakelock_event();
#endif
	spin_lock_irqsave(&state_lock, irqflags);
	if (state == SUSPEND_REQUESTED)
	{
		pr_info("%s: state %d\n",__func__, state);
		state |= SUSPENDED;
	}
	else
		abort = 1;
	spin_unlock_irqrestore(&state_lock, irqflags);

	if (abort) {
		if (debug_mask & DEBUG_SUSPEND)
			pr_info("early_suspend: abort, state %d\n", state);
		mutex_unlock(&early_suspend_lock);
		goto abort;
	}

	if (debug_mask & DEBUG_SUSPEND)
		pr_info("early_suspend: call handlers\n");

	list_for_each_entry(pos, &early_suspend_handlers, link) {
		if (pos->suspend != NULL) {
			if (debug_mask & DEBUG_VERBOSE)
				pr_info("early_suspend: calling %pf\n", pos->suspend);
			pos->suspend(pos);
		}
	}
	mutex_unlock(&early_suspend_lock);

	suspend_sys_sync_queue();
abort:
	spin_lock_irqsave(&state_lock, irqflags);
#ifdef CONFIG_MSM_SM_EVENT
	sm_add_event(SM_POWER_EVENT | SM_POWER_EVENT_EARLY_SUSPEND, SM_EVENT_END, 0, NULL, 0);
#endif
	if (state == SUSPEND_REQUESTED_AND_SUSPENDED)
		wake_unlock(&main_wake_lock);
	spin_unlock_irqrestore(&state_lock, irqflags);

	/* yangjq, 20121127, Add to show consumed time */
	dpm_show_time(starttime, "early suspend");
}

static void late_resume(struct work_struct *work)
{
	struct early_suspend *pos;
	unsigned long irqflags;
	int abort = 0;
	/* yangjq, 20121127, Add to show consumed time */
	ktime_t starttime = ktime_get();

	mutex_lock(&early_suspend_lock);
#ifdef CONFIG_MSM_SM_EVENT
	sm_set_system_state (SM_STATE_LATERESUME);
	sm_add_event(SM_POWER_EVENT | SM_POWER_EVENT_LATE_RESUME, SM_EVENT_START, 0, NULL, 0);
#endif
	spin_lock_irqsave(&state_lock, irqflags);
	if (state == SUSPENDED)
	{
		pr_info("%s: state %d\n",__func__, state);
		state &= ~SUSPENDED;
	}
	else
		abort = 1;
	spin_unlock_irqrestore(&state_lock, irqflags);

	if (abort) {
		if (debug_mask & DEBUG_SUSPEND)
			pr_info("late_resume: abort, state %d\n", state);
		goto abort;
	}
	if (debug_mask & DEBUG_SUSPEND)
		pr_info("late_resume: call handlers\n");
	list_for_each_entry_reverse(pos, &early_suspend_handlers, link) {
		if (pos->resume != NULL) {
			if (debug_mask & DEBUG_VERBOSE)
				pr_info("late_resume: calling %pf\n", pos->resume);

			pos->resume(pos);
		}
	}
	if (debug_mask & DEBUG_SUSPEND)
		pr_info("late_resume: done\n");
abort:
#ifdef CONFIG_MSM_SM_EVENT
	sm_set_system_state (SM_STATE_RUNNING);
	sm_add_event(SM_POWER_EVENT | SM_POWER_EVENT_LATE_RESUME, SM_EVENT_END, 0, NULL, 0);
#endif
	mutex_unlock(&early_suspend_lock);

	/* yangjq, 20121127, Add to show consumed time */
	dpm_show_time(starttime, "late resume");
}

/* yangjq, 20121127, Add to show consumed time */
ktime_t power_key_starttime;
void request_suspend_state(suspend_state_t new_state)
{
	unsigned long irqflags;
	int old_sleep;

	mutex_lock(&early_suspend_lock);
	spin_lock_irqsave(&state_lock, irqflags);
	old_sleep = state & SUSPEND_REQUESTED;
	if (debug_mask & DEBUG_USER_STATE) {
		struct timespec ts;
		struct rtc_time tm;
		getnstimeofday(&ts);
		rtc_time_to_tm(ts.tv_sec, &tm);
		pr_info("request_suspend_state: %s (%d->%d) at %lld "
			"(%d-%02d-%02d %02d:%02d:%02d.%09lu UTC)\n",
			new_state != PM_SUSPEND_ON ? "sleep" : "wakeup",
			requested_suspend_state, new_state,
			ktime_to_ns(ktime_get()),
			tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
			tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec);
	}
	if (!old_sleep && new_state != PM_SUSPEND_ON) {
		state |= SUSPEND_REQUESTED;
		pr_info("%s: goto late resume.state %d\n",__func__, state);
		queue_work(suspend_work_queue, &early_suspend_work);
		/* yangjq, 20121127, Add to show consumed time */
		power_key_starttime.tv64 = 0;
	} else if (old_sleep && new_state == PM_SUSPEND_ON) {
		state &= ~SUSPEND_REQUESTED;
		pr_info("%s: goto late resume.state %d\n",__func__, state);
		wake_lock(&main_wake_lock);
		queue_work(suspend_work_queue, &late_resume_work);
		/* yangjq, 20121127, Add to show consumed time, START */
		if (power_key_starttime.tv64 != 0)
			dpm_show_time(power_key_starttime, "power key");
		/* yangjq, 20121127, Add to show consumed time, END */
	}
	requested_suspend_state = new_state;
	pr_info("%s: end of function. new_state %d, old_state %d, requested_suspend_state %d\n",__func__, new_state, old_sleep, requested_suspend_state);
	spin_unlock_irqrestore(&state_lock, irqflags);
	mutex_unlock(&early_suspend_lock);
}

suspend_state_t get_suspend_state(void)
{
	return requested_suspend_state;
}
