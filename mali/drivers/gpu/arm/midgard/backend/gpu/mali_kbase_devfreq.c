/*
 *
 * (C) COPYRIGHT 2014-2016 ARM Limited. All rights reserved.
 *
 * This program is free software and is provided to you under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation, and any use by you of this program is subject to the terms
 * of such GNU licence.
 *
 * A copy of the licence is included with the program, and can also be obtained
 * from Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 *
 */



#include <mali_kbase.h>
#include <mali_kbase_tlstream.h>
#include <mali_kbase_config_defaults.h>
#include <backend/gpu/mali_kbase_pm_internal.h>
#ifdef CONFIG_DEVFREQ_THERMAL
#include <backend/gpu/mali_kbase_power_model_simple.h>
#endif

#include <linux/clk.h>
#include <linux/devfreq.h>
#ifdef CONFIG_DEVFREQ_THERMAL
#include <linux/devfreq_cooling.h>
#endif

#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 13, 0)
#include <linux/pm_opp.h>
#else /* Linux >= 3.13 */
/* In 3.13 the OPP include header file, types, and functions were all
 * renamed. Use the old filename for the include, and define the new names to
 * the old, when an old kernel is detected.
 */
#include <linux/opp.h>
#define dev_pm_opp opp
#define dev_pm_opp_get_voltage opp_get_voltage
#define dev_pm_opp_get_opp_count opp_get_opp_count
#define dev_pm_opp_find_freq_ceil opp_find_freq_ceil
#endif /* Linux >= 3.13 */


static unsigned long gpu_max_freq = 0;
static unsigned long gpu_min_freq = -1UL;
static u32 pre_fault_count = 0;

static int
kbase_devfreq_target(struct device *dev, unsigned long *target_freq, u32 flags)
{
	struct kbase_device *kbdev = dev_get_drvdata(dev);
	struct dev_pm_opp *opp;
	unsigned long freq = 0;
	unsigned long voltage;
	int err;
    unsigned long maxFreq = gpu_max_freq;

	freq = *target_freq;

	rcu_read_lock();
    if (kbdev->job_fault_count) {
        unsigned long factors = 20000000; //20 Mhz
        maxFreq = maxFreq - ((kbdev->job_fault_count > 1) ? (2 * factors) : (1 * factors));
    }

    //printk(KERN_ALERT " devfreq max_freq %ld\n", kbdev->devfreq->max_freq);
    //printk(KERN_ALERT " devfreq min_freq %ld\n", kbdev->devfreq->min_freq);
    if (maxFreq) {
        if (!kbdev->devfreq->max_freq)
            kbdev->devfreq->max_freq = maxFreq;
        else if (maxFreq < kbdev->devfreq->max_freq)
            kbdev->devfreq->max_freq = maxFreq;
    }
    if (gpu_min_freq != -1UL) {
        if (gpu_min_freq > kbdev->devfreq->min_freq)
            kbdev->devfreq->min_freq = gpu_min_freq;
    }
	opp = devfreq_recommended_opp(dev, &freq, flags);
	voltage = dev_pm_opp_get_voltage(opp);

	if (kbdev->job_fault_count != pre_fault_count) {
		dev_err(dev, "GPU Job fault freq from %ld to %ld\n", kbdev->current_freq, freq);
		pre_fault_count = kbdev->job_fault_count;
	}
	rcu_read_unlock();

	if (IS_ERR_OR_NULL(opp)) {
		dev_err(dev, "Failed to get opp (%ld)\n", PTR_ERR(opp));
		return PTR_ERR(opp);
	}

	/*
	 * Only update if there is a change of frequency
	 */
	if (kbdev->current_freq == freq) {
		*target_freq = freq;
		return 0;
	}

#ifdef CONFIG_REGULATOR
	if (kbdev->regulator && kbdev->current_voltage != voltage
			&& kbdev->current_freq < freq) {
		err = regulator_set_voltage(kbdev->regulator, voltage, voltage);
		if (err) {
			dev_err(dev, "Failed to increase voltage (%d)\n", err);
			return err;
		}
	}
#endif

    err = clk_set_rate(kbdev->clock, freq);

	if (err) {
		dev_err(dev, "Failed to set clock %lu (target %lu)\n",
				freq, *target_freq);
		return err;
	}

#ifdef CONFIG_REGULATOR
	if (kbdev->regulator && kbdev->current_voltage != voltage
			&& kbdev->current_freq > freq) {
		err = regulator_set_voltage(kbdev->regulator, voltage, voltage);
		if (err) {
			dev_err(dev, "Failed to decrease voltage (%d)\n", err);
			return err;
		}
	}
#endif

	*target_freq = freq;
	kbdev->current_voltage = voltage;
	kbdev->current_freq = freq;

	KBASE_TLSTREAM_AUX_DEVFREQ_TARGET((u64)freq);

	kbase_pm_reset_dvfs_utilisation(kbdev);

	return err;
}

static int
kbase_devfreq_cur_freq(struct device *dev, unsigned long *freq)
{
	struct kbase_device *kbdev = dev_get_drvdata(dev);

	*freq = kbdev->current_freq;

	return 0;
}

static int
kbase_devfreq_status(struct device *dev, struct devfreq_dev_status *stat)
{
	struct kbase_device *kbdev = dev_get_drvdata(dev);

	stat->current_frequency = kbdev->current_freq;

	kbase_pm_get_dvfs_utilisation(kbdev,
			&stat->total_time, &stat->busy_time);

	stat->private_data = NULL;

	return 0;
}

static int kbase_devfreq_init_freq_table(struct kbase_device *kbdev,
		struct devfreq_dev_profile *dp)
{
	int count;
	int i = 0;
	unsigned long freq = 0;
	struct dev_pm_opp *opp;

	rcu_read_lock();
	count = dev_pm_opp_get_opp_count(kbdev->dev);
	if (count < 0) {
		rcu_read_unlock();
		return count;
	}
	rcu_read_unlock();

	dp->freq_table = kmalloc_array(count, sizeof(dp->freq_table[0]),
				GFP_KERNEL);
	if (!dp->freq_table)
		return -ENOMEM;

	rcu_read_lock();
	for (i = 0; i < count; i++, freq++) {
		opp = dev_pm_opp_find_freq_ceil(kbdev->dev, &freq);
		if (IS_ERR(opp))
			break;

		dp->freq_table[i] = freq;
        if (gpu_max_freq < freq)
            gpu_max_freq = freq;
        if (gpu_min_freq > freq)
            gpu_min_freq = freq;
    }
    //printk("[GPU] gpu_max_freq %ld\n", gpu_max_freq);
    //printk("[GPU] gpu_min_freq %ld\n", gpu_min_freq);
	rcu_read_unlock();

	if (count != i)
		dev_warn(kbdev->dev, "Unable to enumerate all OPPs (%d!=%d\n",
				count, i);

	dp->max_state = i;

	return 0;
}

static void kbase_devfreq_term_freq_table(struct kbase_device *kbdev)
{
	struct devfreq_dev_profile *dp = kbdev->devfreq->profile;

	kfree(dp->freq_table);
}

static void kbase_devfreq_exit(struct device *dev)
{
	struct kbase_device *kbdev = dev_get_drvdata(dev);

	kbase_devfreq_term_freq_table(kbdev);
}

int kbase_devfreq_init(struct kbase_device *kbdev)
{
	struct devfreq_dev_profile *dp;
	int err;
	unsigned long freq;

	if (!kbdev->clock)
		return -ENODEV;

	freq = 520000000;
	err = clk_set_rate(kbdev->clock, freq);
	if (err) {
		dev_err(kbdev->dev, "Failed to set clock %lu\n", freq);
		return err;
	}

	kbdev->current_freq = clk_get_rate(kbdev->clock);

	dp = &kbdev->devfreq_profile;

	dp->initial_freq = kbdev->current_freq;
	dp->polling_ms = 100;
	dp->target = kbase_devfreq_target;
	dp->get_dev_status = kbase_devfreq_status;
	dp->get_cur_freq = kbase_devfreq_cur_freq;
	dp->exit = kbase_devfreq_exit;

	if (kbase_devfreq_init_freq_table(kbdev, dp))
		return -EFAULT;

	kbdev->devfreq = devfreq_add_device(kbdev->dev, dp,
				"simple_ondemand", NULL);
	if (IS_ERR(kbdev->devfreq)) {
		kbase_devfreq_term_freq_table(kbdev);
		return PTR_ERR(kbdev->devfreq);
	}

	err = devfreq_register_opp_notifier(kbdev->dev, kbdev->devfreq);
	if (err) {
		dev_err(kbdev->dev,
			"Failed to register OPP notifier (%d)\n", err);
		goto opp_notifier_failed;
	}

#ifdef CONFIG_DEVFREQ_THERMAL

#ifdef CONFIG_ENABLE_POWER_MODEL
    err = kbase_power_model_simple_init(kbdev);
	if (err && err != -ENODEV && err != -EPROBE_DEFER) {
		dev_err(kbdev->dev,
			"Failed to initialize simple power model (%d)\n",
			err);
		goto cooling_failed;
	}
	if (err == -EPROBE_DEFER)
		goto cooling_failed;
#endif
	if (err != -ENODEV) {
#ifdef CONFIG_ENABLE_POWER_MODEL
		kbdev->devfreq_cooling = of_devfreq_cooling_register_power(
				kbdev->dev->of_node,
				kbdev->devfreq,
				&power_model_simple_ops);
#else
        kbdev->devfreq_cooling = of_devfreq_cooling_register(
            kbdev->dev->of_node,
            kbdev->devfreq);
#endif
		if (IS_ERR_OR_NULL(kbdev->devfreq_cooling)) {
			err = PTR_ERR(kbdev->devfreq_cooling);
			dev_err(kbdev->dev,
				"Failed to register cooling device (%d)\n",
				err);
			goto cooling_failed;
		}
	} else {
		err = 0;
	}
#endif

	return 0;

#ifdef CONFIG_DEVFREQ_THERMAL
cooling_failed:
	devfreq_unregister_opp_notifier(kbdev->dev, kbdev->devfreq);
#endif /* CONFIG_DEVFREQ_THERMAL */
opp_notifier_failed:
	if (devfreq_remove_device(kbdev->devfreq))
		dev_err(kbdev->dev, "Failed to terminate devfreq (%d)\n", err);
	else
		kbdev->devfreq = NULL;

	return err;
}

void kbase_devfreq_term(struct kbase_device *kbdev)
{
	int err;

	dev_dbg(kbdev->dev, "Term Mali devfreq\n");

#ifdef CONFIG_DEVFREQ_THERMAL
	if (kbdev->devfreq_cooling)
		devfreq_cooling_unregister(kbdev->devfreq_cooling);
#endif

	devfreq_unregister_opp_notifier(kbdev->dev, kbdev->devfreq);

	err = devfreq_remove_device(kbdev->devfreq);
	if (err)
		dev_err(kbdev->dev, "Failed to terminate devfreq (%d)\n", err);
	else
		kbdev->devfreq = NULL;
}
