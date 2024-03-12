// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021-2024 Cedric Bellegarde <cedric.bellegarde@adishatz.org>.
 * Copyright (C) 2018-2019 Sultan Alsawaf <sultan@kerneltoast.com>.
 */

#define pr_fmt(fmt) "power_saver: " fmt

#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/input.h>
#include <linux/kthread.h>
#include <linux/msm_drm_notify.h>
#include <linux/slab.h>
#include <linux/version.h>

#include <power/power_saver.h>

/* The sched_param struct is located elsewhere in newer kernels */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0)
#include <uapi/linux/sched/types.h>
#endif

static unsigned int power_saver_min_little __read_mostly =
    CONFIG_POWER_SAVER_CPU_MIN_FREQ_LITTLE;
static unsigned int power_saver_min_big __read_mostly =
    CONFIG_POWER_SAVER_CPU_MIN_FREQ_BIG;
static unsigned int power_saver_min_prime __read_mostly =
    CONFIG_POWER_SAVER_CPU_MIN_FREQ_PRIME;

static unsigned int power_saver_max_little __read_mostly =
    CONFIG_POWER_SAVER_CPU_MAX_FREQ_LITTLE;
static unsigned int power_saver_max_big __read_mostly =
    CONFIG_POWER_SAVER_CPU_MAX_FREQ_BIG;
static unsigned int power_saver_max_prime __read_mostly =
    CONFIG_POWER_SAVER_CPU_MAX_FREQ_PRIME;

static unsigned int power_saver_max_snd_little __read_mostly =
    CONFIG_POWER_SAVER_CPU_MAX_FREQ_SND_LITTLE;
static unsigned int power_saver_max_snd_big __read_mostly =
    CONFIG_POWER_SAVER_CPU_MAX_FREQ_SND_BIG;
static unsigned int power_saver_max_snd_prime __read_mostly =
    CONFIG_POWER_SAVER_CPU_MAX_FREQ_SND_PRIME;

static void sound_enabled(void)
{
	power_saver.streams += 1;
	wake_up(&power_saver.update_waitq);
}

static void sound_disabled(void)
{
	if (power_saver.streams > 0)
		power_saver.streams -= 1;
	wake_up(&power_saver.update_waitq);
}

static unsigned int get_max_freq(struct cpufreq_policy *policy)
{
	unsigned int freq;

	if (cpumask_test_cpu(policy->cpu, cpu_lp_mask)) {
		if (power_saver.streams > 0) {
			freq = power_saver_max_snd_little;
		} else {
			freq = power_saver_max_little;
		}
	} else if (cpumask_test_cpu(policy->cpu, cpu_perf_mask)) {
		if (power_saver.streams > 0) {
			freq = power_saver_max_snd_big;
		} else {
			freq = power_saver_max_prime;
		}
	} else {
		if (power_saver.streams > 0) {
			freq = power_saver_max_snd_prime;
		} else {
			freq = power_saver_max_prime;
		}
	}
	return max(policy->cpuinfo.min_freq,
		   min(freq, policy->cpuinfo.max_freq));
}

static unsigned int get_min_freq(struct cpufreq_policy *policy)
{
	unsigned int freq;

	if (cpumask_test_cpu(policy->cpu, cpu_lp_mask))
		freq = power_saver_min_little;
	else if (cpumask_test_cpu(policy->cpu, cpu_perf_mask))
		freq = power_saver_min_big;
	else
		freq = power_saver_min_prime;

	return max(freq, policy->cpuinfo.min_freq);
}

static void update_online_cpu_policy(void)
{
	unsigned int cpu;

	/* Only one CPU from each cluster needs to be updated */
	get_online_cpus();
	if (power_saver_min_little != 0 || power_saver_max_little != 0) {
		cpu = cpumask_first_and(cpu_lp_mask, cpu_online_mask);
		cpufreq_update_policy(cpu);
	}
	if (power_saver_min_big != 0 || power_saver_max_big != 0) {
		cpu = cpumask_first_and(cpu_perf_mask, cpu_online_mask);
		cpufreq_update_policy(cpu);
	}
	if (power_saver_min_prime != 0 || power_saver_max_prime != 0) {
		cpu = cpumask_first_and(cpu_prime_mask, cpu_online_mask);
		cpufreq_update_policy(cpu);
	}
	put_online_cpus();
}

static int cpu_update_thread(void *data)
{
	static const struct sched_param sched_max_rt_prio = {
		.sched_priority = MAX_RT_PRIO - 1
	};
	struct power_saver_drv *drv = data;
	bool screen_on = false;
	unsigned int streams = 0;

	sched_setscheduler_nocheck(current, SCHED_FIFO, &sched_max_rt_prio);

	while (1) {
		bool _screen_on;
		unsigned int _streams = streams;
		bool should_stop = false;

		wait_event(drv->update_waitq,
			   (_screen_on = READ_ONCE(drv->screen_on)) != screen_on ||
			   (_streams = READ_ONCE(drv->streams)) != streams ||
			   (should_stop = kthread_should_stop()));

		if (should_stop)
			break;

		screen_on = _screen_on;
		streams = _streams;
		update_online_cpu_policy();
	}

	return 0;
}

static int cpu_notifier_cb(struct notifier_block *nb, unsigned long action,
			   void *data)
{
	struct power_saver_drv *drv = container_of(nb, typeof(*drv), cpu_notif);
	struct cpufreq_policy *policy = data;

	if (action != CPUFREQ_ADJUST)
		return NOTIFY_OK;

	/* When screen is on, use upper frequency */
	if (drv->screen_on == true) {
		policy->min = get_min_freq(policy);
		policy->max = policy->cpuinfo.max_freq;
		return NOTIFY_OK;
	}
	policy->max = get_max_freq(policy);
	policy->min = policy->cpuinfo.min_freq;
	return NOTIFY_OK;
}

static int msm_drm_notifier_cb(struct notifier_block *nb, unsigned long action,
			       void *data)
{
	struct power_saver_drv *drv =
	    container_of(nb, typeof(*drv), msm_drm_notif);
	struct msm_drm_notifier *evdata = data;
	int *blank = evdata->data;

	/* Parse framebuffer blank events as soon as they occur */
	if (action != MSM_DRM_EARLY_EVENT_BLANK)
		return NOTIFY_OK;

	if (*blank == MSM_DRM_BLANK_UNBLANK) {
		drv->screen_on = true;
	} else if (*blank == MSM_DRM_BLANK_POWERDOWN) {
		drv->screen_on = false;
	}

	wake_up(&drv->update_waitq);
	return NOTIFY_OK;
}

struct power_saver_drv power_saver __read_mostly = {
	.update_waitq = __WAIT_QUEUE_HEAD_INITIALIZER(power_saver.update_waitq),
	.sound_enabled = sound_enabled,
	.sound_disabled = sound_disabled
};

EXPORT_SYMBOL(power_saver);

static int __init power_saver_init(void)
{
	struct power_saver_drv *drv = &power_saver;
	struct task_struct *thread;
	int ret;

	drv->screen_on = true;
	drv->streams = 0;

	drv->cpu_notif.notifier_call = cpu_notifier_cb;
	ret =
	    cpufreq_register_notifier(&drv->cpu_notif, CPUFREQ_POLICY_NOTIFIER);
	if (ret) {
		pr_err("Failed to register cpufreq notifier, err: %d\n", ret);
		return ret;
	}

	drv->msm_drm_notif.notifier_call = msm_drm_notifier_cb;
	drv->msm_drm_notif.priority = INT_MAX;
	ret = msm_drm_register_client(&drv->msm_drm_notif);
	if (ret) {
		pr_err("Failed to register msm_drm notifier, err: %d\n", ret);
		goto unregister_cpu_notif;
	}

	thread =
	    kthread_run_perf_critical(cpu_perf_mask, cpu_update_thread, drv,
				      "cpu_updated");
	if (IS_ERR(thread)) {
		ret = PTR_ERR(thread);
		pr_err("Failed to start CPU update thread, err: %d\n", ret);
		goto unregister_fb_notif;
	}

	return 0;

 unregister_fb_notif:
	msm_drm_unregister_client(&drv->msm_drm_notif);
 unregister_cpu_notif:
	cpufreq_unregister_notifier(&drv->cpu_notif, CPUFREQ_POLICY_NOTIFIER);
	return ret;
}

subsys_initcall(power_saver_init);
