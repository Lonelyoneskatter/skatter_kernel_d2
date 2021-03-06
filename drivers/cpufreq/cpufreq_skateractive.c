/*
 * drivers/cpufreq/cpufreq_skateractive.c
 *
 * Copyright (C) 2010 Google, Inc.
 *
 * Copyright (C) 2017 Lonelyoneskatter <threesixoh.skater@yahoo.com>
 *
 * Credits to imoseyon for max freq screen off
 *
 * Credits to franciscofranco for timer_rate tweak
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
 * Author: Mike Chan (mike@android.com)
 *
 */

#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/cpufreq.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/rwsem.h>
#include <linux/sched.h>
#include <linux/tick.h>
#include <linux/time.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/kernel_stat.h>
#include <asm/cputime.h>

#define CREATE_TRACE_POINTS
#include <trace/events/cpufreq_skateractive.h>

extern bool earphones_connected;
extern bool screen_on;
extern bool bluetooth_on;
extern void msm_pm_sleep_mode_enable(bool enable);
extern void msm_pm_retention_mode_enable(bool enable);

struct cpufreq_skateractive_cpuinfo {
	struct timer_list cpu_timer;
	struct timer_list cpu_slack_timer;
	spinlock_t load_lock; /* protects the next 4 fields */
	u64 time_in_idle;
	u64 time_in_idle_timestamp;
	u64 cputime_speedadj;
	u64 cputime_speedadj_timestamp;
	u64 last_evaluated_jiffy;
	struct cpufreq_policy *policy;
	struct cpufreq_frequency_table *freq_table;
	spinlock_t target_freq_lock; /*protects target freq */
	unsigned int target_freq;
	unsigned int floor_freq;
	unsigned int max_freq;
	u64 floor_validate_time;
	u64 hispeed_validate_time; /* cluster hispeed_validate_time */
	u64 local_hvtime; /* per-cpu hispeed_validate_time */
	u64 max_freq_idle_start_time;
	struct rw_semaphore enable_sem;
	bool reject_notification;
	int governor_enabled;
	struct cpufreq_skateractive_tunables *cached_tunables;
	int first_cpu;
};

static DEFINE_PER_CPU(struct cpufreq_skateractive_cpuinfo, cpuinfo);

/* realtime thread handles frequency scaling */
static struct task_struct *speedchange_task;
static cpumask_t speedchange_cpumask;
static spinlock_t speedchange_cpumask_lock;
static struct mutex gov_lock;

/* Target load.  Lower values result in higher CPU speeds. */
#define DEFAULT_TARGET_LOAD 85
static unsigned int default_target_loads[] = {DEFAULT_TARGET_LOAD};

#define DEFAULT_ABOVE_HISPEED_DELAY 25000
static unsigned int default_above_hispeed_delay[] = {
	DEFAULT_ABOVE_HISPEED_DELAY };

#define DEFAULT_SCREEN_OFF_MAX 702000
static unsigned long screen_off_max = DEFAULT_SCREEN_OFF_MAX;
static unsigned long screen_off_max_prev = DEFAULT_SCREEN_OFF_MAX;

/* Max frequency to limit while earphones are in & screen is off. */
#define DEFAULT_EARPHONES_MAX_FREQ_SCREEN_OFF 1080000
static unsigned long earphones_maxfreq = DEFAULT_EARPHONES_MAX_FREQ_SCREEN_OFF;

/* Max frequency to limit while bluetooth is on & screen is off. */
#define DEFAULT_BT_MAX_FREQ_SCREEN_OFF 1242000
static unsigned long bluetooth_maxfreq = DEFAULT_BT_MAX_FREQ_SCREEN_OFF;

struct cpufreq_skateractive_tunables {
	int usage_count;

	/* Hi speed to bump to from lo speed when load burst (default max) */
#define DEFAULT_HISPEED_FREQ 384000
	unsigned int hispeed_freq;
	unsigned int hispeed_freq_prev;

	/* Go to hi speed when CPU load at or above this value. */
#define DEFAULT_GO_HISPEED_LOAD 99
	unsigned long go_hispeed_load;

	/* Target load. Lower values result in higher CPU speeds. */
	spinlock_t target_loads_lock;
	unsigned int *target_loads;
	int ntarget_loads;

	/*
	 * The minimum amount of time to spend at a frequency before we can ramp
	 * down.
	 */
#define DEFAULT_MIN_SAMPLE_TIME (80 * USEC_PER_MSEC)
	unsigned long min_sample_time;

	/*
	 * The sample rate of the timer used to increase frequency
	 */
#define DEFAULT_TIMER_RATE 60000
	unsigned long timer_rate;

	/*
	 * Save previous value of timer_rate
	 */
	unsigned long timer_rate_prev;
	/*
	 * The number of times to multiply timer_rate on screen off
	 */
#define DEFAULT_TIMER_RATE_MULTIPLIER 2
	unsigned long timer_rate_multiplier;

	/*
	 * Wait this long before raising speed above hispeed, by default a
	 * single timer interval.
	 */
	spinlock_t above_hispeed_delay_lock;
	unsigned int *above_hispeed_delay;
	int nabove_hispeed_delay;

	/*
	 * Max additional time to wait in idle, beyond timer_rate, at speeds
	 * above minimum before wakeup to reduce speed, or -1 if unnecessary.
	 */
#define DEFAULT_TIMER_SLACK 80000
	int timer_slack_val;

	/*
	 * Whether to align timer windows across all CPUs. When
	 * use_sched_load is true, this flag is ignored and windows
	 * will always be aligned.
	 */
	bool align_windows;

	/*
	 * Stay at max freq for at least sampling_down_factor before dropping
	 * frequency.
	 */
	bool sampling_down_factor;
};

/* For cases where we have single governor instance for system */
static struct cpufreq_skateractive_tunables *common_tunables;

static struct attribute_group *get_sysfs_attr(void);

/* Round to starting jiffy of next evaluation window */
static u64 round_to_nw_start(u64 jif,
			     struct cpufreq_skateractive_tunables *tunables)
{
	unsigned long step = usecs_to_jiffies(tunables->timer_rate);
	u64 ret;

	if (tunables->align_windows) {
		do_div(jif, step);
		ret = (jif + 1) * step;
	} else {
		ret = jiffies + usecs_to_jiffies(tunables->timer_rate);
	}

	return ret;
}

static void cpufreq_skateractive_timer_resched(unsigned long cpu)
{
	struct cpufreq_skateractive_cpuinfo *pcpu = &per_cpu(cpuinfo, cpu);
	struct cpufreq_skateractive_tunables *tunables =
		pcpu->policy->governor_data;
	u64 expires;
	unsigned long flags;

	spin_lock_irqsave(&pcpu->load_lock, flags);
	pcpu->time_in_idle =
		get_cpu_idle_time(smp_processor_id(),
				  &pcpu->time_in_idle_timestamp, 0);
	pcpu->cputime_speedadj = 0;
	pcpu->cputime_speedadj_timestamp = pcpu->time_in_idle_timestamp;
	expires = round_to_nw_start(pcpu->last_evaluated_jiffy, tunables);
	del_timer(&pcpu->cpu_timer);
	pcpu->cpu_timer.expires = expires;
	add_timer_on(&pcpu->cpu_timer, cpu);

	if ((likely(screen_on)) && tunables->timer_slack_val >= 0 &&
	    pcpu->target_freq > pcpu->policy->min) {
		expires += usecs_to_jiffies(tunables->timer_slack_val);
		del_timer(&pcpu->cpu_slack_timer);
		pcpu->cpu_slack_timer.expires = expires;
		add_timer_on(&pcpu->cpu_slack_timer, cpu);
	}

	spin_unlock_irqrestore(&pcpu->load_lock, flags);
}

/* The caller shall take enable_sem write semaphore to avoid any timer race.
 * The cpu_timer and cpu_slack_timer must be deactivated when calling this
 * function.
 */
static void cpufreq_skateractive_timer_start(
	struct cpufreq_skateractive_tunables *tunables, int cpu)
{
	struct cpufreq_skateractive_cpuinfo *pcpu = &per_cpu(cpuinfo, cpu);
	u64 expires = round_to_nw_start(pcpu->last_evaluated_jiffy, tunables);
	unsigned long flags;

	spin_lock_irqsave(&pcpu->load_lock, flags);
	pcpu->cpu_timer.expires = expires;
	add_timer_on(&pcpu->cpu_timer, cpu);
	if ((likely(screen_on)) && tunables->timer_slack_val >= 0 &&
	    pcpu->target_freq > pcpu->policy->min) {
		expires += usecs_to_jiffies(tunables->timer_slack_val);
		pcpu->cpu_slack_timer.expires = expires;
		add_timer_on(&pcpu->cpu_slack_timer, cpu);
	}

	pcpu->time_in_idle =
		get_cpu_idle_time(cpu, &pcpu->time_in_idle_timestamp, 0);
	pcpu->cputime_speedadj = 0;
	pcpu->cputime_speedadj_timestamp = pcpu->time_in_idle_timestamp;
	spin_unlock_irqrestore(&pcpu->load_lock, flags);
}

static unsigned int freq_to_above_hispeed_delay(
	struct cpufreq_skateractive_tunables *tunables,
	unsigned int freq)
{
	int i;
	unsigned int ret;
	unsigned long flags;

	spin_lock_irqsave(&tunables->above_hispeed_delay_lock, flags);

	for (i = 0; i < tunables->nabove_hispeed_delay - 1 &&
			freq >= tunables->above_hispeed_delay[i+1]; i += 2)
		;

	ret = tunables->above_hispeed_delay[i];
	spin_unlock_irqrestore(&tunables->above_hispeed_delay_lock, flags);
	return ret;
}

static unsigned int freq_to_targetload(
	struct cpufreq_skateractive_tunables *tunables, unsigned int freq)
{
	int i;
	unsigned int ret;
	unsigned long flags;

	spin_lock_irqsave(&tunables->target_loads_lock, flags);

	for (i = 0; i < tunables->ntarget_loads - 1 &&
		    freq >= tunables->target_loads[i+1]; i += 2)
		;

	ret = tunables->target_loads[i];
	spin_unlock_irqrestore(&tunables->target_loads_lock, flags);
	return ret;
}

/*
 * If increasing frequencies never map to a lower target load then
 * choose_freq() will find the minimum frequency that does not exceed its
 * target load given the current load.
 */
static unsigned int choose_freq(struct cpufreq_skateractive_cpuinfo *pcpu,
		unsigned int loadadjfreq)
{
	unsigned int freq = pcpu->policy->cur;
	unsigned int prevfreq, freqmin, freqmax;
	unsigned int tl;
	int index;

	freqmin = 0;
	freqmax = UINT_MAX;

	do {
		prevfreq = freq;
		tl = freq_to_targetload(pcpu->policy->governor_data, freq);

		/*
		 * Find the lowest frequency where the computed load is less
		 * than or equal to the target load.
		 */

		if (cpufreq_frequency_table_target(
			    pcpu->policy, pcpu->freq_table, loadadjfreq / tl,
			    CPUFREQ_RELATION_L, &index))
			break;
		freq = pcpu->freq_table[index].frequency;

		if (freq > prevfreq) {
			/* The previous frequency is too low. */
			freqmin = prevfreq;

			if (freq >= freqmax) {
				/*
				 * Find the highest frequency that is less
				 * than freqmax.
				 */
				if (cpufreq_frequency_table_target(
					    pcpu->policy, pcpu->freq_table,
					    freqmax - 1, CPUFREQ_RELATION_H,
					    &index))
					break;
				freq = pcpu->freq_table[index].frequency;

				if (freq == freqmin) {
					/*
					 * The first frequency below freqmax
					 * has already been found to be too
					 * low.  freqmax is the lowest speed
					 * we found that is fast enough.
					 */
					freq = freqmax;
					break;
				}
			}
		} else if (freq < prevfreq) {
			/* The previous frequency is high enough. */
			freqmax = prevfreq;

			if (freq <= freqmin) {
				/*
				 * Find the lowest frequency that is higher
				 * than freqmin.
				 */
				if (cpufreq_frequency_table_target(
					    pcpu->policy, pcpu->freq_table,
					    freqmin + 1, CPUFREQ_RELATION_L,
					    &index))
					break;
				freq = pcpu->freq_table[index].frequency;

				/*
				 * If freqmax is the first frequency above
				 * freqmin then we have already found that
				 * this speed is fast enough.
				 */
				if (freq == freqmax)
					break;
			}
		}

		/* If same frequency chosen as previous then done. */
	} while (freq != prevfreq);

	return freq;
}

static u64 update_load(int cpu)
{
	struct cpufreq_skateractive_cpuinfo *pcpu = &per_cpu(cpuinfo, cpu);

	u64 now;
	u64 now_idle;
	u64 delta_idle;
	u64 delta_time;
	u64 active_time;

	now_idle = get_cpu_idle_time(cpu, &now, 0);
	delta_idle = (now_idle - pcpu->time_in_idle);
	delta_time = (now - pcpu->time_in_idle_timestamp);

	if (delta_time <= delta_idle)
		active_time = 0;
	else
		active_time = delta_time - delta_idle;

	pcpu->cputime_speedadj += active_time * pcpu->policy->cur;

	pcpu->time_in_idle = now_idle;
	pcpu->time_in_idle_timestamp = now;
	return now;
}

static void cpufreq_skateractive_timer(unsigned long data)
{
	u64 now;
	unsigned int delta_time;
	u64 cputime_speedadj;
	int cpu_load;
	struct cpufreq_skateractive_cpuinfo *pcpu =
		&per_cpu(cpuinfo, data);
	struct cpufreq_skateractive_tunables *tunables =
		pcpu->policy->governor_data;
	unsigned int new_freq;
	unsigned int loadadjfreq;
	unsigned int index;
	unsigned long flags;
	struct cpufreq_govinfo int_info;

	if (!down_read_trylock(&pcpu->enable_sem))
		return;
	if (!pcpu->governor_enabled)
		goto exit;

	if (pcpu->policy->min == pcpu->policy->max)
		goto rearm;

	spin_lock_irqsave(&pcpu->load_lock, flags);
	pcpu->last_evaluated_jiffy = get_jiffies_64();
	now = update_load(data);

		delta_time = (unsigned int)
				(now - pcpu->cputime_speedadj_timestamp);
		cputime_speedadj = pcpu->cputime_speedadj;
		spin_unlock_irqrestore(&pcpu->load_lock, flags);

	/* Increase timer rate if suspended */
	if ((likely(screen_on)) && tunables->timer_rate != tunables->timer_rate_prev)
		tunables->timer_rate = tunables->timer_rate_prev;
	else if ((unlikely(!screen_on)) && tunables->timer_rate == tunables->timer_rate_prev) {
		tunables->timer_rate_prev = tunables->timer_rate;
		tunables->timer_rate = tunables->timer_rate * tunables->timer_rate_multiplier;
	}

		if (WARN_ON_ONCE(!delta_time))
			goto rearm;
		do_div(cputime_speedadj, delta_time);

	loadadjfreq = (unsigned int)cputime_speedadj * 100;

	int_info.cpu = data;
	int_info.load = loadadjfreq / pcpu->policy->max;
	int_info.sampling_rate_us = tunables->timer_rate;
	atomic_notifier_call_chain(&cpufreq_govinfo_notifier_list,
					CPUFREQ_LOAD_CHANGE, &int_info);

	spin_lock_irqsave(&pcpu->target_freq_lock, flags);
	cpu_load = loadadjfreq / pcpu->policy->cur;


	if (unlikely(!screen_on)) {
		if (tunables->hispeed_freq != 54000)
			tunables->hispeed_freq = 54000;
	} else {
		if (tunables->hispeed_freq != tunables->hispeed_freq_prev)
			tunables->hispeed_freq = tunables->hispeed_freq_prev;
	}

	if (cpu_load >= tunables->go_hispeed_load) {
		if (pcpu->policy->cur < tunables->hispeed_freq) {
			new_freq = tunables->hispeed_freq;
		} else {
			new_freq = choose_freq(pcpu, loadadjfreq);

			if (new_freq < tunables->hispeed_freq)
				new_freq = tunables->hispeed_freq;
		}
	} else {
		new_freq = choose_freq(pcpu, loadadjfreq);
		if (new_freq > tunables->hispeed_freq &&
				pcpu->target_freq < tunables->hispeed_freq)
			new_freq = tunables->hispeed_freq;
	}

	if (pcpu->policy->cur >= tunables->hispeed_freq &&
	    new_freq > pcpu->policy->cur &&
	    now - pcpu->hispeed_validate_time <
	    freq_to_above_hispeed_delay(tunables, pcpu->policy->cur)) {
		trace_cpufreq_skateractive_notyet(
			data, cpu_load, pcpu->target_freq,
			pcpu->policy->cur, new_freq);
		spin_unlock_irqrestore(&pcpu->target_freq_lock, flags);
		goto rearm;
	}

	pcpu->local_hvtime = now;

	if (cpufreq_frequency_table_target(pcpu->policy, pcpu->freq_table,
					   new_freq, CPUFREQ_RELATION_L,
					   &index)) {
		spin_unlock_irqrestore(&pcpu->target_freq_lock, flags);
		goto rearm;
	}

	new_freq = pcpu->freq_table[index].frequency;

	if (pcpu->target_freq >= pcpu->policy->max
	    && new_freq < pcpu->target_freq
	    && now - pcpu->max_freq_idle_start_time < 0) {
		trace_cpufreq_skateractive_notyet(data, cpu_load,
			pcpu->target_freq, pcpu->policy->cur, new_freq);
		spin_unlock_irqrestore(&pcpu->target_freq_lock, flags);
		goto rearm;
	}

	/*
	 * Do not scale below floor_freq unless we have been at or above the
	 * floor frequency for the minimum sample time since last validated.
	 */
	if (new_freq < pcpu->floor_freq) {
		if (now - pcpu->floor_validate_time <
				tunables->min_sample_time) {
			trace_cpufreq_skateractive_notyet(
				data, cpu_load, pcpu->target_freq,
				pcpu->policy->cur, new_freq);
			spin_unlock_irqrestore(&pcpu->target_freq_lock, flags);
			goto rearm;
		}
	}

	/*
	 * Update the timestamp for checking whether speed has been held at
	 * or above the selected frequency for a minimum of min_sample_time,
	 * if not boosted to hispeed_freq.  If boosted to hispeed_freq then we
	 * allow the speed to drop as soon as the boostpulse duration expires
	 * (or the indefinite boost is turned off).
	 */

	if (new_freq > tunables->hispeed_freq) {
		pcpu->floor_freq = new_freq;
		pcpu->floor_validate_time = now;
	}

	if (pcpu->target_freq == new_freq) {
		trace_cpufreq_skateractive_already(
			data, cpu_load, pcpu->target_freq,
			pcpu->policy->cur, new_freq);
		spin_unlock_irqrestore(&pcpu->target_freq_lock, flags);
		goto rearm_if_notmax;
	}

	trace_cpufreq_skateractive_target(data, cpu_load, pcpu->target_freq,
					 pcpu->policy->cur, new_freq);

	pcpu->target_freq = new_freq;
	spin_unlock_irqrestore(&pcpu->target_freq_lock, flags);
	spin_lock_irqsave(&speedchange_cpumask_lock, flags);
	cpumask_set_cpu(data, &speedchange_cpumask);
	spin_unlock_irqrestore(&speedchange_cpumask_lock, flags);
	wake_up_process_no_notif(speedchange_task);

rearm_if_notmax:
	/*
	 * Already set max speed and don't see a need to change that,
	 * wait until next idle to re-evaluate, don't need timer.
	 */
	if (pcpu->target_freq == pcpu->policy->max)
		goto exit;

rearm:
	if (!timer_pending(&pcpu->cpu_timer))
		cpufreq_skateractive_timer_resched(data);

exit:
	up_read(&pcpu->enable_sem);
	return;
}

static void cpufreq_skateractive_idle_start(void)
{
	struct cpufreq_skateractive_cpuinfo *pcpu =
		&per_cpu(cpuinfo, smp_processor_id());
	int pending;
	struct cpufreq_skateractive_tunables *tunables;
	unsigned long flags;
	u64 now;

	if (!down_read_trylock(&pcpu->enable_sem))
		return;
	if (!pcpu->governor_enabled) {
		up_read(&pcpu->enable_sem);
		return;
	}

	pending = timer_pending(&pcpu->cpu_timer);

	/* Enable retention mode on screen off. */
	if (unlikely(!screen_on)) {
		msm_pm_sleep_mode_enable(0);
		msm_pm_retention_mode_enable(0);
	} else {
		msm_pm_sleep_mode_enable(1);
		msm_pm_retention_mode_enable(1);
	}

	if (pcpu->target_freq != pcpu->policy->min) {
		/*
		 * Entering idle while not at lowest speed.  On some
		 * platforms this can hold the other CPU(s) at that speed
		 * even though the CPU is idle. Set a timer to re-evaluate
		 * speed so this idle CPU doesn't hold the other CPUs above
		 * min indefinitely.  This should probably be a quirk of
		 * the CPUFreq driver.
		 */
		if (!pending) {
			pcpu->last_evaluated_jiffy = get_jiffies_64();
			cpufreq_skateractive_timer_resched(smp_processor_id());

			/*
			 * If timer is cancelled because CPU is running at
			 * policy->max, record the time CPU first goes to
			 * idle.
			 */
			now = ktime_to_us(ktime_get());
			tunables = pcpu->policy->governor_data;
			if (tunables->sampling_down_factor) {
				spin_lock_irqsave(&pcpu->target_freq_lock,
						  flags);
				pcpu->max_freq_idle_start_time = now;
				spin_unlock_irqrestore(&pcpu->target_freq_lock,
						       flags);
			}
		}
	}
	up_read(&pcpu->enable_sem);
}

static void cpufreq_skateractive_idle_end(void)
{
	struct cpufreq_skateractive_cpuinfo *pcpu =
		&per_cpu(cpuinfo, smp_processor_id());

	if (!down_read_trylock(&pcpu->enable_sem))
		return;
	if (!pcpu->governor_enabled) {
		up_read(&pcpu->enable_sem);
		return;
	}

	/* Arm the timer for 1-2 ticks later if not already. */
	if (!timer_pending(&pcpu->cpu_timer)) {
		cpufreq_skateractive_timer_resched(smp_processor_id());
	} else if (time_after_eq(jiffies, pcpu->cpu_timer.expires)) {
		del_timer(&pcpu->cpu_timer);
		del_timer(&pcpu->cpu_slack_timer);
		cpufreq_skateractive_timer(smp_processor_id());
	}

	up_read(&pcpu->enable_sem);
}

static int cpufreq_skateractive_speedchange_task(void *data)
{
	unsigned int cpu;
	cpumask_t tmp_mask;
	unsigned long flags;
	struct cpufreq_skateractive_cpuinfo *pcpu;

	while (1) {
		set_current_state(TASK_INTERRUPTIBLE);
		spin_lock_irqsave(&speedchange_cpumask_lock, flags);

		if (cpumask_empty(&speedchange_cpumask)) {
			spin_unlock_irqrestore(&speedchange_cpumask_lock,
					       flags);
			schedule();

			if (kthread_should_stop())
				break;

			spin_lock_irqsave(&speedchange_cpumask_lock, flags);
		}

		set_current_state(TASK_RUNNING);
		tmp_mask = speedchange_cpumask;
		cpumask_clear(&speedchange_cpumask);
		spin_unlock_irqrestore(&speedchange_cpumask_lock, flags);

		for_each_cpu(cpu, &tmp_mask) {
			unsigned int j;
			unsigned int max_freq = 0;
			struct cpufreq_skateractive_cpuinfo *pjcpu;
			u64 hvt = 0;

			pcpu = &per_cpu(cpuinfo, cpu);
			if (!down_read_trylock(&pcpu->enable_sem))
				continue;
			if (!pcpu->governor_enabled) {
				up_read(&pcpu->enable_sem);
				continue;
			}

			for_each_cpu(j, pcpu->policy->cpus) {
				pjcpu = &per_cpu(cpuinfo, j);

				if (pjcpu->target_freq > max_freq) {
					max_freq = pjcpu->target_freq;
					hvt = pjcpu->local_hvtime;
				} else if (pjcpu->target_freq == max_freq) {
					hvt = min(hvt, pjcpu->local_hvtime);
				}
			}

			/* On screen on, return correct value */
			if (likely(screen_on)) {
				screen_off_max = screen_off_max_prev;
			/* If earphone are plugged in & screen is off set to headset frequency settings */
			} else if (earphones_connected && unlikely(!screen_on)) {
				if (max_freq > earphones_maxfreq)
					max_freq = earphones_maxfreq;
			/* Put earphones before bluetooth just in case there plugged in while BT is on */
			} else if (bluetooth_on && unlikely(!screen_on)) {
				if (max_freq > bluetooth_maxfreq)
					max_freq = bluetooth_maxfreq;
			/* If number of online cores is over 1, set to regular screen off frequency */
			} else if (unlikely(!screen_on)) {
				if (max_freq > screen_off_max)
					max_freq = screen_off_max;
			}

			if (max_freq != pcpu->policy->cur) {
				__cpufreq_driver_target(pcpu->policy,
							max_freq,
							CPUFREQ_RELATION_H);
				for_each_cpu(j, pcpu->policy->cpus) {
					pjcpu = &per_cpu(cpuinfo, j);
					pjcpu->hispeed_validate_time = hvt;
				}
			}
			trace_cpufreq_skateractive_setspeed(cpu,
						     pcpu->target_freq,
						     pcpu->policy->cur);

			up_read(&pcpu->enable_sem);
		}
	}

	return 0;
}

static int cpufreq_skateractive_notifier(
	struct notifier_block *nb, unsigned long val, void *data)
{
	struct cpufreq_freqs *freq = data;
	struct cpufreq_skateractive_cpuinfo *pcpu;
	int cpu;
	unsigned long flags;

	if (val == CPUFREQ_POSTCHANGE) {
		pcpu = &per_cpu(cpuinfo, freq->cpu);
		if (!down_read_trylock(&pcpu->enable_sem))
			return 0;
		if (!pcpu->governor_enabled) {
			up_read(&pcpu->enable_sem);
			return 0;
		}

		for_each_cpu(cpu, pcpu->policy->cpus) {
			struct cpufreq_skateractive_cpuinfo *pjcpu =
				&per_cpu(cpuinfo, cpu);
			if (cpu != freq->cpu) {
				if (!down_read_trylock(&pjcpu->enable_sem))
					continue;
				if (!pjcpu->governor_enabled) {
					up_read(&pjcpu->enable_sem);
					continue;
				}
			}
			spin_lock_irqsave(&pjcpu->load_lock, flags);
			update_load(cpu);
			spin_unlock_irqrestore(&pjcpu->load_lock, flags);
			if (cpu != freq->cpu)
				up_read(&pjcpu->enable_sem);
		}

		up_read(&pcpu->enable_sem);
	}
	return 0;
}

static struct notifier_block cpufreq_notifier_block = {
	.notifier_call = cpufreq_skateractive_notifier,
};

static unsigned int *get_tokenized_data(const char *buf, int *num_tokens)
{
	const char *cp;
	int i;
	int ntokens = 1;
	unsigned int *tokenized_data;
	int err = -EINVAL;

	cp = buf;
	while ((cp = strpbrk(cp + 1, " :")))
		ntokens++;

	if (!(ntokens & 0x1))
		goto err;

	tokenized_data = kmalloc(ntokens * sizeof(unsigned int), GFP_KERNEL);
	if (!tokenized_data) {
		err = -ENOMEM;
		goto err;
	}

	cp = buf;
	i = 0;
	while (i < ntokens) {
		if (sscanf(cp, "%u", &tokenized_data[i++]) != 1)
			goto err_kfree;

		cp = strpbrk(cp, " :");
		if (!cp)
			break;
		cp++;
	}

	if (i != ntokens)
		goto err_kfree;

	*num_tokens = ntokens;
	return tokenized_data;

err_kfree:
	kfree(tokenized_data);
err:
	return ERR_PTR(err);
}

static ssize_t show_target_loads(
	struct cpufreq_skateractive_tunables *tunables,
	char *buf)
{
	int i;
	ssize_t ret = 0;
	unsigned long flags;

	spin_lock_irqsave(&tunables->target_loads_lock, flags);

	for (i = 0; i < tunables->ntarget_loads; i++)
		ret += sprintf(buf + ret, "%u%s", tunables->target_loads[i],
			       i & 0x1 ? ":" : " ");

	sprintf(buf + ret - 1, "\n");
	spin_unlock_irqrestore(&tunables->target_loads_lock, flags);
	return ret;
}

static ssize_t store_target_loads(
	struct cpufreq_skateractive_tunables *tunables,
	const char *buf, size_t count)
{
	int ntokens;
	unsigned int *new_target_loads = NULL;
	unsigned long flags;

	new_target_loads = get_tokenized_data(buf, &ntokens);
	if (IS_ERR(new_target_loads))
		return PTR_RET(new_target_loads);

	spin_lock_irqsave(&tunables->target_loads_lock, flags);
	if (tunables->target_loads != default_target_loads)
		kfree(tunables->target_loads);
	tunables->target_loads = new_target_loads;
	tunables->ntarget_loads = ntokens;
	spin_unlock_irqrestore(&tunables->target_loads_lock, flags);
	return count;
}

static ssize_t show_above_hispeed_delay(
	struct cpufreq_skateractive_tunables *tunables, char *buf)
{
	int i;
	ssize_t ret = 0;
	unsigned long flags;

	spin_lock_irqsave(&tunables->above_hispeed_delay_lock, flags);

	for (i = 0; i < tunables->nabove_hispeed_delay; i++)
		ret += sprintf(buf + ret, "%u%s",
			       tunables->above_hispeed_delay[i],
			       i & 0x1 ? ":" : " ");

	sprintf(buf + ret - 1, "\n");
	spin_unlock_irqrestore(&tunables->above_hispeed_delay_lock, flags);
	return ret;
}

static ssize_t store_above_hispeed_delay(
	struct cpufreq_skateractive_tunables *tunables,
	const char *buf, size_t count)
{
	int ntokens;
	unsigned int *new_above_hispeed_delay = NULL;
	unsigned long flags;

	new_above_hispeed_delay = get_tokenized_data(buf, &ntokens);
	if (IS_ERR(new_above_hispeed_delay))
		return PTR_RET(new_above_hispeed_delay);

	spin_lock_irqsave(&tunables->above_hispeed_delay_lock, flags);
	if (tunables->above_hispeed_delay != default_above_hispeed_delay)
		kfree(tunables->above_hispeed_delay);
	tunables->above_hispeed_delay = new_above_hispeed_delay;
	tunables->nabove_hispeed_delay = ntokens;
	spin_unlock_irqrestore(&tunables->above_hispeed_delay_lock, flags);
	return count;

}

static ssize_t show_hispeed_freq(struct cpufreq_skateractive_tunables *tunables,
		char *buf)
{
	return sprintf(buf, "%u\n", tunables->hispeed_freq);
}

static ssize_t store_hispeed_freq(struct cpufreq_skateractive_tunables *tunables,
		const char *buf, size_t count)
{
	int ret;
	long unsigned int val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	tunables->hispeed_freq = val;
	tunables->hispeed_freq_prev = val;

	return count;
}

static ssize_t show_go_hispeed_load(struct cpufreq_skateractive_tunables
		*tunables, char *buf)
{
	return sprintf(buf, "%lu\n", tunables->go_hispeed_load);
}

static ssize_t store_go_hispeed_load(struct cpufreq_skateractive_tunables
		*tunables, const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	tunables->go_hispeed_load = val;
	return count;
}

static ssize_t show_timer_rate(struct cpufreq_skateractive_tunables *tunables,
		char *buf)
{
	return sprintf(buf, "%lu\n", tunables->timer_rate);
}

static ssize_t store_timer_rate(struct cpufreq_skateractive_tunables *tunables,
		const char *buf, size_t count)
{
	int ret;
	unsigned long val, val_round;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	val_round = jiffies_to_usecs(usecs_to_jiffies(val));
	if (val != val_round)
		pr_warn("timer_rate not aligned to jiffy. Rounded up to %lu\n",
			val_round);

	tunables->timer_rate = val_round;
	tunables->timer_rate_prev = val_round;

	return count;
}

static ssize_t show_timer_rate_multiplier(
		struct cpufreq_skateractive_tunables *tunables, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%lu\n", tunables->timer_rate_multiplier);
}

static ssize_t store_timer_rate_multiplier(
			struct cpufreq_skateractive_tunables *tunables,
			const char *buf, size_t count)
{
	int ret = 0;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret)
		return ret;

	if (val < 1 || val > 10)
		return -EINVAL;

	tunables->timer_rate_multiplier = val;

	return count;
}

static ssize_t show_screen_off_maxfreq(struct cpufreq_skateractive_tunables *tunables,
		char *buf)
{
	return sprintf(buf, "%lu\n", screen_off_max);
}

static ssize_t store_screen_off_maxfreq(struct cpufreq_skateractive_tunables *tunables,
		const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	if (val < 0)
		screen_off_max = DEFAULT_SCREEN_OFF_MAX;

	screen_off_max = val;
	screen_off_max_prev = val;

	return count;
}

static ssize_t show_earphones_maxfreq(struct cpufreq_skateractive_tunables *tunables,
		char *buf)
{
	return sprintf(buf, "%lu\n", earphones_maxfreq);
}

static ssize_t store_earphones_maxfreq(struct cpufreq_skateractive_tunables *tunables,
		const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	/* Return min freq limit if it reaches below this */
	if (val < 594000)
		val = 594000;

	earphones_maxfreq = val;

	return count;
}

static ssize_t show_bluetooth_maxfreq(struct cpufreq_skateractive_tunables *tunables,
		char *buf)
{
	return sprintf(buf, "%lu\n", bluetooth_maxfreq);
}

static ssize_t store_bluetooth_maxfreq(struct cpufreq_skateractive_tunables *tunables,
		const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	/* Return min freq limit if it reaches below this */
	if (val < 648000)
		val = 648000;

	bluetooth_maxfreq = val;

	return count;
}

/*
 * Create show/store routines
 * - sys: One governor instance for complete SYSTEM
 * - pol: One governor instance per struct cpufreq_policy
 */
#define show_gov_pol_sys(file_name)					\
static ssize_t show_##file_name##_gov_sys				\
(struct kobject *kobj, struct attribute *attr, char *buf)		\
{									\
	return show_##file_name(common_tunables, buf);			\
}									\
									\
static ssize_t show_##file_name##_gov_pol				\
(struct cpufreq_policy *policy, char *buf)				\
{									\
	return show_##file_name(policy->governor_data, buf);		\
}

#define store_gov_pol_sys(file_name)					\
static ssize_t store_##file_name##_gov_sys				\
(struct kobject *kobj, struct attribute *attr, const char *buf,		\
	size_t count)							\
{									\
	return store_##file_name(common_tunables, buf, count);		\
}									\
									\
static ssize_t store_##file_name##_gov_pol				\
(struct cpufreq_policy *policy, const char *buf, size_t count)		\
{									\
	return store_##file_name(policy->governor_data, buf, count);	\
}

#define show_store_gov_pol_sys(file_name)				\
show_gov_pol_sys(file_name);						\
store_gov_pol_sys(file_name)

show_store_gov_pol_sys(target_loads);
show_store_gov_pol_sys(above_hispeed_delay);
show_store_gov_pol_sys(hispeed_freq);
show_store_gov_pol_sys(go_hispeed_load);
show_store_gov_pol_sys(timer_rate);
show_store_gov_pol_sys(timer_rate_multiplier);
show_store_gov_pol_sys(screen_off_maxfreq);
show_store_gov_pol_sys(earphones_maxfreq);
show_store_gov_pol_sys(bluetooth_maxfreq);

#define gov_sys_attr_rw(_name)						\
static struct global_attr _name##_gov_sys =				\
__ATTR(_name, 0644, show_##_name##_gov_sys, store_##_name##_gov_sys)

#define gov_pol_attr_rw(_name)						\
static struct freq_attr _name##_gov_pol =				\
__ATTR(_name, 0644, show_##_name##_gov_pol, store_##_name##_gov_pol)

#define gov_sys_pol_attr_rw(_name)					\
	gov_sys_attr_rw(_name);						\
	gov_pol_attr_rw(_name)

gov_sys_pol_attr_rw(target_loads);
gov_sys_pol_attr_rw(above_hispeed_delay);
gov_sys_pol_attr_rw(hispeed_freq);
gov_sys_pol_attr_rw(go_hispeed_load);
gov_sys_pol_attr_rw(timer_rate);
gov_sys_pol_attr_rw(timer_rate_multiplier);
gov_sys_pol_attr_rw(screen_off_maxfreq);
gov_sys_pol_attr_rw(earphones_maxfreq);
gov_sys_pol_attr_rw(bluetooth_maxfreq);

/* One Governor instance for entire system */
static struct attribute *skateractive_attributes_gov_sys[] = {
	&target_loads_gov_sys.attr,
	&above_hispeed_delay_gov_sys.attr,
	&hispeed_freq_gov_sys.attr,
	&go_hispeed_load_gov_sys.attr,
	&timer_rate_gov_sys.attr,
	&timer_rate_multiplier_gov_sys.attr,
	&screen_off_maxfreq_gov_sys.attr,
	&earphones_maxfreq_gov_sys.attr,
	&bluetooth_maxfreq_gov_sys.attr,
	NULL,
};

static struct attribute_group skateractive_attr_group_gov_sys = {
	.attrs = skateractive_attributes_gov_sys,
	.name = "skateractive",
};

/* Per policy governor instance */
static struct attribute *skateractive_attributes_gov_pol[] = {
	&target_loads_gov_pol.attr,
	&above_hispeed_delay_gov_pol.attr,
	&hispeed_freq_gov_pol.attr,
	&go_hispeed_load_gov_pol.attr,
	&timer_rate_gov_pol.attr,
	&timer_rate_multiplier_gov_pol.attr,
	&screen_off_maxfreq_gov_pol.attr,
	&earphones_maxfreq_gov_pol.attr,
	&bluetooth_maxfreq_gov_pol.attr,
	NULL,
};

static struct attribute_group skateractive_attr_group_gov_pol = {
	.attrs = skateractive_attributes_gov_pol,
	.name = "skateractive",
};

static struct attribute_group *get_sysfs_attr(void)
{
	if (have_governor_per_policy())
		return &skateractive_attr_group_gov_pol;
	else
		return &skateractive_attr_group_gov_sys;
}

static int cpufreq_skateractive_idle_notifier(struct notifier_block *nb,
					     unsigned long val,
					     void *data)
{
	switch (val) {
	case IDLE_START:
		cpufreq_skateractive_idle_start();
		break;
	case IDLE_END:
		cpufreq_skateractive_idle_end();
		break;
	}

	return 0;
}

static struct notifier_block cpufreq_skateractive_idle_nb = {
	.notifier_call = cpufreq_skateractive_idle_notifier,
};

static void save_tunables(struct cpufreq_policy *policy,
			  struct cpufreq_skateractive_tunables *tunables)
{
	int cpu;
	struct cpufreq_skateractive_cpuinfo *pcpu;

	if (have_governor_per_policy())
		cpu = cpumask_first(policy->related_cpus);
	else
		cpu = 0;

	pcpu = &per_cpu(cpuinfo, cpu);
	WARN_ON(pcpu->cached_tunables && pcpu->cached_tunables != tunables);
	pcpu->cached_tunables = tunables;
}

static struct cpufreq_skateractive_tunables *alloc_tunable(
					struct cpufreq_policy *policy)
{
	struct cpufreq_skateractive_tunables *tunables;

	tunables = kzalloc(sizeof(*tunables), GFP_KERNEL);
	if (!tunables) {
		pr_err("%s: POLICY_INIT: kzalloc failed\n", __func__);
		return ERR_PTR(-ENOMEM);
	}

	tunables->above_hispeed_delay = default_above_hispeed_delay;
	tunables->nabove_hispeed_delay =
		ARRAY_SIZE(default_above_hispeed_delay);
	tunables->go_hispeed_load = DEFAULT_GO_HISPEED_LOAD;
	tunables->target_loads = default_target_loads;
	tunables->ntarget_loads = ARRAY_SIZE(default_target_loads);
	tunables->min_sample_time = DEFAULT_MIN_SAMPLE_TIME;
	tunables->timer_rate = DEFAULT_TIMER_RATE;
	tunables->timer_rate_prev = DEFAULT_TIMER_RATE;
	tunables->timer_rate_multiplier = DEFAULT_TIMER_RATE_MULTIPLIER;
	tunables->timer_slack_val = DEFAULT_TIMER_SLACK;
	tunables->align_windows = true;
	tunables->sampling_down_factor = false;
	tunables->hispeed_freq_prev = DEFAULT_HISPEED_FREQ;

	spin_lock_init(&tunables->target_loads_lock);
	spin_lock_init(&tunables->above_hispeed_delay_lock);

	save_tunables(policy, tunables);
	return tunables;
}

static struct cpufreq_skateractive_tunables *restore_tunables(
						struct cpufreq_policy *policy)
{
	int cpu;

	if (have_governor_per_policy())
		cpu = cpumask_first(policy->related_cpus);
	else
		cpu = 0;

	return per_cpu(cpuinfo, cpu).cached_tunables;
}

static int cpufreq_governor_skateractive(struct cpufreq_policy *policy,
		unsigned int event)
{
	int rc;
	unsigned int j;
	struct cpufreq_skateractive_cpuinfo *pcpu;
	struct cpufreq_frequency_table *freq_table;
	struct cpufreq_skateractive_tunables *tunables;
	unsigned long flags;
	int first_cpu;

	if (have_governor_per_policy())
		tunables = policy->governor_data;
	else
		tunables = common_tunables;

	if (WARN_ON(!tunables && (event != CPUFREQ_GOV_POLICY_INIT)))
		return -EINVAL;

	switch (event) {
	case CPUFREQ_GOV_POLICY_INIT:
		if (have_governor_per_policy()) {
			WARN_ON(tunables);
		} else if (tunables) {
			tunables->usage_count++;
			policy->governor_data = tunables;
			return 0;
		}

		first_cpu = cpumask_first(policy->related_cpus);
		for_each_cpu(j, policy->related_cpus)
			per_cpu(cpuinfo, j).first_cpu = first_cpu;

		tunables = restore_tunables(policy);
		if (!tunables) {
			tunables = alloc_tunable(policy);
			if (IS_ERR(tunables))
				return PTR_ERR(tunables);
		}

		tunables->usage_count = 1;
		policy->governor_data = tunables;
		if (!have_governor_per_policy()) {
			WARN_ON(cpufreq_get_global_kobject());
			common_tunables = tunables;
		}

		rc = sysfs_create_group(get_governor_parent_kobj(policy),
				get_sysfs_attr());
		if (rc) {
			kfree(tunables);
			policy->governor_data = NULL;
			if (!have_governor_per_policy()) {
				common_tunables = NULL;
				cpufreq_put_global_kobject();
			}
			return rc;
		}

		if (!policy->governor->initialized) {
#ifdef CONFIG_INTERACTION_HINTS
			cpufreq_want_interact_hints(1);
#endif
			idle_notifier_register(&cpufreq_skateractive_idle_nb);
			cpufreq_register_notifier(&cpufreq_notifier_block,
					CPUFREQ_TRANSITION_NOTIFIER);
		}

		break;

	case CPUFREQ_GOV_POLICY_EXIT:
		if (!--tunables->usage_count) {
			if (policy->governor->initialized == 1) {
				cpufreq_unregister_notifier(&cpufreq_notifier_block,
						CPUFREQ_TRANSITION_NOTIFIER);
#ifdef CONFIG_INTERACTION_HINTS
				cpufreq_want_interact_hints(0);
#endif
				idle_notifier_unregister(&cpufreq_skateractive_idle_nb);
			}

			sysfs_remove_group(get_governor_parent_kobj(policy),
					get_sysfs_attr());
			if (!have_governor_per_policy())
				cpufreq_put_global_kobject();
			common_tunables = NULL;
		}

		policy->governor_data = NULL;

		break;

	case CPUFREQ_GOV_START:
		mutex_lock(&gov_lock);

		freq_table = cpufreq_frequency_get_table(policy->cpu);

		tunables->hispeed_freq = DEFAULT_HISPEED_FREQ;

		for_each_cpu(j, policy->cpus) {
			pcpu = &per_cpu(cpuinfo, j);
			pcpu->policy = policy;
			pcpu->target_freq = policy->cur;
			pcpu->freq_table = freq_table;
			pcpu->floor_freq = pcpu->target_freq;
			pcpu->floor_validate_time =
				ktime_to_us(ktime_get());
			pcpu->hispeed_validate_time =
				pcpu->floor_validate_time;
			pcpu->local_hvtime = pcpu->floor_validate_time;
			pcpu->max_freq = policy->max;
			pcpu->reject_notification = true;
			down_write(&pcpu->enable_sem);
			del_timer_sync(&pcpu->cpu_timer);
			del_timer_sync(&pcpu->cpu_slack_timer);
			pcpu->last_evaluated_jiffy = get_jiffies_64();
			cpufreq_skateractive_timer_start(tunables, j);
			pcpu->governor_enabled = 1;
			up_write(&pcpu->enable_sem);
			pcpu->reject_notification = false;
		}

		mutex_unlock(&gov_lock);
		break;

	case CPUFREQ_GOV_STOP:
		mutex_lock(&gov_lock);
		for_each_cpu(j, policy->cpus) {
			pcpu = &per_cpu(cpuinfo, j);
			pcpu->reject_notification = true;
			down_write(&pcpu->enable_sem);
			pcpu->governor_enabled = 0;
			pcpu->target_freq = 0;
			del_timer_sync(&pcpu->cpu_timer);
			del_timer_sync(&pcpu->cpu_slack_timer);
			up_write(&pcpu->enable_sem);
			pcpu->reject_notification = false;
		}

		mutex_unlock(&gov_lock);
		break;

	case CPUFREQ_GOV_LIMITS:
		__cpufreq_driver_target(policy,
				policy->cur, CPUFREQ_RELATION_L);
		for_each_cpu(j, policy->cpus) {
			pcpu = &per_cpu(cpuinfo, j);

			down_read(&pcpu->enable_sem);
			if (pcpu->governor_enabled == 0) {
				up_read(&pcpu->enable_sem);
				continue;
			}

			spin_lock_irqsave(&pcpu->target_freq_lock, flags);
			if (policy->max < pcpu->target_freq)
				pcpu->target_freq = policy->max;
			else if (policy->min > pcpu->target_freq)
				pcpu->target_freq = policy->min;

			spin_unlock_irqrestore(&pcpu->target_freq_lock, flags);
			up_read(&pcpu->enable_sem);

			/* Reschedule timer only if policy->max is raised.
			 * Delete the timers, else the timer callback may
			 * return without re-arm the timer when failed
			 * acquire the semaphore. This race may cause timer
			 * stopped unexpectedly.
			 */

			if (policy->max > pcpu->max_freq) {
				pcpu->reject_notification = true;
				down_write(&pcpu->enable_sem);
				del_timer_sync(&pcpu->cpu_timer);
				del_timer_sync(&pcpu->cpu_slack_timer);
				cpufreq_skateractive_timer_resched(j);
				up_write(&pcpu->enable_sem);
				pcpu->reject_notification = false;
			}

			pcpu->max_freq = policy->max;
		}
		break;
	}
	return 0;
}

#ifndef CONFIG_CPU_FREQ_DEFAULT_GOV_SKATERACTIVEX
static
#endif
struct cpufreq_governor cpufreq_gov_skateractive = {
	.name = "skateractive",
	.governor = cpufreq_governor_skateractive,
	.max_transition_latency = 10000000,
	.owner = THIS_MODULE,
};

static void cpufreq_skateractive_nop_timer(unsigned long data)
{
}

static int __init cpufreq_skateractive_init(void)
{
	unsigned int i;
	struct cpufreq_skateractive_cpuinfo *pcpu;
	struct sched_param param = { .sched_priority = MAX_RT_PRIO-1 };
	int ret = 0;

	screen_on = true;
	earphones_connected = false;
	bluetooth_on = false;

	/* Initalize per-cpu timers */
	for_each_possible_cpu(i) {
		pcpu = &per_cpu(cpuinfo, i);
		init_timer_deferrable(&pcpu->cpu_timer);
		pcpu->cpu_timer.function = cpufreq_skateractive_timer;
		pcpu->cpu_timer.data = i;
		init_timer(&pcpu->cpu_slack_timer);
		pcpu->cpu_slack_timer.function = cpufreq_skateractive_nop_timer;
		spin_lock_init(&pcpu->load_lock);
		spin_lock_init(&pcpu->target_freq_lock);
		init_rwsem(&pcpu->enable_sem);
	}

	spin_lock_init(&speedchange_cpumask_lock);
	mutex_init(&gov_lock);
	speedchange_task =
		kthread_create(cpufreq_skateractive_speedchange_task, NULL,
			       "cfskateractive");
	if (IS_ERR(speedchange_task))
		return PTR_ERR(speedchange_task);

	sched_setscheduler_nocheck(speedchange_task, SCHED_FIFO, &param);
	get_task_struct(speedchange_task);

	/* NB: wake up so the thread does not look hung to the freezer */
	wake_up_process_no_notif(speedchange_task);

	ret = cpufreq_register_governor(&cpufreq_gov_skateractive);
	if (ret) {
		kthread_stop(speedchange_task);
		put_task_struct(speedchange_task);
	}
	return ret;
}

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_SKATERACTIVE
fs_initcall(cpufreq_skateractive_init);
#else
module_init(cpufreq_skateractive_init);
#endif

static void __exit cpufreq_skateractive_exit(void)
{
	int cpu;
	struct cpufreq_skateractive_cpuinfo *pcpu;

	cpufreq_unregister_governor(&cpufreq_gov_skateractive);
	kthread_stop(speedchange_task);
	put_task_struct(speedchange_task);

	for_each_possible_cpu(cpu) {
		pcpu = &per_cpu(cpuinfo, cpu);
		kfree(pcpu->cached_tunables);
		pcpu->cached_tunables = NULL;
	}
}

module_exit(cpufreq_skateractive_exit);

MODULE_AUTHOR("Mike Chan <mike@android.com> \
               Lonelyoneskatter <threesixoh.skater@yahoo.com>");
MODULE_DESCRIPTION("'cpufreq_skateractive' a full backport of the 3.10.y governor for 3.4.x kernel"
                   "tuned for d2 devices with the ability to double timer_rate on screen off,"
                   "and set the max frequency to users choice on screen off.");
MODULE_LICENSE("GPL");
