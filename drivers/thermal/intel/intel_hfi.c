// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hardware Feedback Interface Driver
 *
 * Copyright (c) 2021, Intel Corporation.
 *
 * Authors: Aubrey Li <aubrey.li@linux.intel.com>
 *          Ricardo Neri <ricardo.neri-calderon@linux.intel.com>
 *
 *
 * The Hardware Feedback Interface provides a performance and energy efficiency
 * capability information for each CPU in the system. Depending on the processor
 * model, hardware may periodically update these capabilities as a result of
 * changes in the operating conditions (e.g., power limits or thermal
 * constraints). On other processor models, there is a single HFI update
 * at boot.
 *
 * This file provides functionality to process HFI updates and relay these
 * updates to userspace.
 */

#define pr_fmt(fmt)  "intel-hfi: " fmt

#include <linux/bitops.h>
#include <linux/cpufeature.h>
#include <linux/cpumask.h>
#include <linux/gfp.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/math.h>
#include <linux/mutex.h>
#include <linux/percpu.h>
#include <linux/percpu-defs.h>
#include <linux/printk.h>
#include <linux/processor.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/topology.h>
#include <linux/workqueue.h>

#include <asm/msr.h>
#include <asm/intel-family.h>

#include "../thermal_core.h"
#include "intel_hfi.h"

#define THERM_STATUS_CLEAR_PKG_MASK (BIT(1) | BIT(3) | BIT(5) | BIT(7) | \
				     BIT(9) | BIT(11) | BIT(26))

/* Hardware Feedback Interface MSR configuration bits */
#define HW_FEEDBACK_PTR_VALID_BIT		BIT(0)
#define HW_FEEDBACK_CONFIG_HFI_ENABLE_BIT	BIT(0)
#define HW_FEEDBACK_CONFIG_ITD_ENABLE_BIT	BIT(1)
#define HW_FEEDBACK_THREAD_CONFIG_ENABLE_BIT	BIT(0)

/* CPUID detection and enumeration definitions for HFI */

#define CPUID_HFI_LEAF 6

union hfi_capabilities {
	struct {
		u8	performance:1;
		u8	energy_efficiency:1;
		u8	__reserved:6;
	} split;
	u8 bits;
};

union cpuid6_edx {
	struct {
		union hfi_capabilities	capabilities;
		u32			table_pages:4;
		u32			__reserved:4;
		s32			index:16;
	} split;
	u32 full;
};

union cpuid6_ecx {
	struct {
		u32	dont_care0:8;
		u32	nr_classes:8;
		u32	dont_care1:16;
	} split;
	u32 full;
};

#ifdef CONFIG_IPC_CLASSES
union hfi_thread_feedback_char_msr {
	struct {
		u64	classid : 8;
		u64	__reserved : 55;
		u64	valid : 1;
	} split;
	u64 full;
};
#endif

/**
 * struct hfi_cpu_data - HFI capabilities per CPU
 * @perf_cap:		Performance capability
 * @ee_cap:		Energy efficiency capability
 *
 * Capabilities of a logical processor in the HFI table. These capabilities are
 * unitless and specific to each HFI class.
 */
struct hfi_cpu_data {
	u8	perf_cap;
	u8	ee_cap;
} __packed;

/**
 * struct hfi_hdr - Header of the HFI table
 * @perf_updated:	Hardware updated performance capabilities
 * @ee_updated:		Hardware updated energy efficiency capabilities
 *
 * Properties of the data in an HFI table. There exists one header per each
 * HFI class.
 */
struct hfi_hdr {
	u8	perf_updated;
	u8	ee_updated;
} __packed;

/**
 * struct hfi_instance - Representation of an HFI instance (i.e., a table)
 * @local_table:	Base of the local copy of the HFI table
 * @timestamp:		Timestamp of the last update of the local table.
 *			Located at the base of the local table.
 * @hdr:		Base address of the header of the local table
 * @data:		Base address of the data of the local table
 * @cpus:		CPUs represented in this HFI table instance
 * @hw_table:		Pointer to the HFI table of this instance
 * @update_work:	Delayed work to process HFI updates
 * @table_lock:		Lock to protect acceses to the table of this instance
 * @event_lock:		Lock to process HFI interrupts
 *
 * A set of parameters to parse and navigate a specific HFI table.
 */
struct hfi_instance {
	union {
		void			*local_table;
		u64			*timestamp;
	};
	void			*hdr;
	void			*data;
	cpumask_var_t		cpus;
	void			*hw_table;
	struct delayed_work	update_work;
	raw_spinlock_t		table_lock;
	raw_spinlock_t		event_lock;
};

/**
 * struct hfi_features - Supported HFI features
 * @nr_classes:		Number of classes supported
 * @nr_table_pages:	Size of the HFI table in 4KB pages
 * @cpu_stride:		Stride size to locate the capability data of a logical
 *			processor within the table (i.e., row stride)
 * @hdr_size:		Size of the table header
 * @class_stride:	Stride size to locate a class within the capability
 *			data of a logical processor or the HFI table header
 * Parameters and supported features that are common to all HFI instances
 */
struct hfi_features {
	unsigned int	nr_classes;
	unsigned int	nr_table_pages;
	unsigned int	cpu_stride;
	unsigned int	class_stride;
	unsigned int	hdr_size;
};

/**
 * struct hfi_cpu_info - Per-CPU attributes to consume HFI data
 * @index:		Row of this CPU in its HFI table
 * @hfi_instance:	Attributes of the HFI table to which this CPU belongs
 *
 * Parameters to link a logical processor to an HFI table and a row within it.
 */
struct hfi_cpu_info {
	s16			index;
	struct hfi_instance	*hfi_instance;
};

static DEFINE_PER_CPU(struct hfi_cpu_info, hfi_cpu_info) = { .index = -1 };

static int max_hfi_instances;
static struct hfi_instance *hfi_instances;

static struct hfi_features hfi_features;
static DEFINE_MUTEX(hfi_instance_lock);

static struct workqueue_struct *hfi_updates_wq;
#define HFI_UPDATE_INTERVAL		HZ
#define HFI_MAX_THERM_NOTIFY_COUNT	16

#ifdef CONFIG_IPC_CLASSES
static int __percpu *hfi_ipcc_scores;

/*
 * A task may be unclassified if it has been recently created, spend most of
 * its lifetime sleeping, or hardware has not provided a classification.
 *
 * Most tasks will be classified as scheduler's IPC class 1 (HFI class 0)
 * eventually. Meanwhile, the scheduler will place classes of tasks with higher
 * IPC scores on higher-performance CPUs.
 *
 * IPC class 1 is a reasonable choice. It matches the performance capability
 * of the legacy, classless, HFI table.
 */
#define HFI_UNCLASSIFIED_DEFAULT 1

#define CLASS_DEBOUNCER_SKIPS 4

/**
 * debounce_and_update_class() - Process and update a task's classification
 *
 * @p:		The task of which the classification will be updated
 * @new_ipcc:	The new IPC classification
 *
 * Update the classification of @p with the new value that hardware provides.
 * Only update the classification of @p if it has been the same during
 * CLASS_DEBOUNCER_SKIPS consecutive ticks.
 */
static void debounce_and_update_class(struct task_struct *p, u8 new_ipcc)
{
	u16 debounce_skip;

	/* The class of @p changed. Only restart the debounce counter. */
	if (p->ipcc_tmp != new_ipcc) {
		p->ipcc_cntr = 1;
		goto out;
	}

	/*
	 * The class of @p did not change. Update it if it has been the same
	 * for CLASS_DEBOUNCER_SKIPS user ticks.
	 */
	debounce_skip = p->ipcc_cntr + 1;
	if (debounce_skip < CLASS_DEBOUNCER_SKIPS)
		p->ipcc_cntr++;
	else
		p->ipcc = new_ipcc;

out:
	p->ipcc_tmp = new_ipcc;
}

static bool classification_is_accurate(u8 hfi_class, bool smt_siblings_idle)
{
	switch (boot_cpu_data.x86_model) {
	case INTEL_FAM6_ALDERLAKE:
	case INTEL_FAM6_ALDERLAKE_L:
	case INTEL_FAM6_RAPTORLAKE:
	case INTEL_FAM6_RAPTORLAKE_P:
	case INTEL_FAM6_RAPTORLAKE_S:
		if (hfi_class == 3 || hfi_class == 2 || smt_siblings_idle)
			return true;

		return false;

	default:
		return true;
	}
}

void intel_hfi_update_ipcc(struct task_struct *curr)
{
	union hfi_thread_feedback_char_msr msr;
	bool idle;

	/* We should not be here if ITD is not supported. */
	if (!cpu_feature_enabled(X86_FEATURE_ITD)) {
		pr_warn_once("task classification requested but not supported!");
		return;
	}

	rdmsrl(MSR_IA32_HW_FEEDBACK_CHAR, msr.full);
	if (!msr.split.valid)
		return;

	/*
	 * 0 is a valid classification for Intel Thread Director. A scheduler
	 * IPCC class of 0 means that the task is unclassified. Adjust.
	 */
	idle = sched_smt_siblings_idle(task_cpu(curr));
	if (classification_is_accurate(msr.split.classid, idle))
		debounce_and_update_class(curr, msr.split.classid + 1);
}

unsigned long intel_hfi_get_ipcc_score(unsigned short ipcc, int cpu)
{
	unsigned short hfi_class;
	int *scores;

	if (cpu < 0 || cpu >= nr_cpu_ids)
		return -EINVAL;

	if (ipcc == IPC_CLASS_UNCLASSIFIED)
		ipcc = HFI_UNCLASSIFIED_DEFAULT;

	/*
	 * Scheduler IPC classes start at 1. HFI classes start at 0.
	 * See note intel_hfi_update_ipcc().
	 */
	hfi_class = ipcc - 1;

	if (hfi_class >= hfi_features.nr_classes)
		return -EINVAL;

	scores = per_cpu_ptr(hfi_ipcc_scores, cpu);
	if (!scores)
		return -ENODEV;

	return READ_ONCE(scores[hfi_class]);
}

static int alloc_hfi_ipcc_scores(void)
{
	if (!cpu_feature_enabled(X86_FEATURE_ITD))
		return 0;

	hfi_ipcc_scores = __alloc_percpu(sizeof(*hfi_ipcc_scores) *
					 hfi_features.nr_classes,
					 sizeof(*hfi_ipcc_scores));

	return !hfi_ipcc_scores;
}

static void set_hfi_ipcc_score(void *caps, int cpu)
{
	int i, *hfi_class;

	if (!cpu_feature_enabled(X86_FEATURE_ITD))
		return;

	hfi_class = per_cpu_ptr(hfi_ipcc_scores, cpu);

	for (i = 0;  i < hfi_features.nr_classes; i++) {
		struct hfi_cpu_data *class_caps;

		class_caps = caps + i * hfi_features.class_stride;
		WRITE_ONCE(hfi_class[i], class_caps->perf_cap);
	}
}

#else
static int alloc_hfi_ipcc_scores(void) { return 0; }
static void set_hfi_ipcc_score(void *caps, int cpu) { }
#endif /* CONFIG_IPC_CLASSES */

static void get_hfi_caps(struct hfi_instance *hfi_instance,
			 struct thermal_genl_cpu_caps *cpu_caps)
{
	int cpu, i = 0;

	raw_spin_lock_irq(&hfi_instance->table_lock);
	for_each_cpu(cpu, hfi_instance->cpus) {
		struct hfi_cpu_data *caps;
		s16 index;

		index = per_cpu(hfi_cpu_info, cpu).index;
		caps = hfi_instance->data + index * hfi_features.cpu_stride;
		cpu_caps[i].cpu = cpu;

		/*
		 * Scale performance and energy efficiency to
		 * the [0, 1023] interval that thermal netlink uses.
		 */
		cpu_caps[i].performance = caps->perf_cap << 2;
		cpu_caps[i].efficiency = caps->ee_cap << 2;

		++i;

		set_hfi_ipcc_score(caps, cpu);
	}
	raw_spin_unlock_irq(&hfi_instance->table_lock);
}

/*
 * Call update_capabilities() when there are changes in the HFI table.
 */
static void update_capabilities(struct hfi_instance *hfi_instance)
{
	struct thermal_genl_cpu_caps *cpu_caps;
	int i = 0, cpu_count;

	/* CPUs may come online/offline while processing an HFI update. */
	mutex_lock(&hfi_instance_lock);

	cpu_count = cpumask_weight(hfi_instance->cpus);

	/* No CPUs to report in this hfi_instance. */
	if (!cpu_count)
		goto out;

	cpu_caps = kcalloc(cpu_count, sizeof(*cpu_caps), GFP_KERNEL);
	if (!cpu_caps)
		goto out;

	get_hfi_caps(hfi_instance, cpu_caps);

	if (cpu_count < HFI_MAX_THERM_NOTIFY_COUNT)
		goto last_cmd;

	/* Process complete chunks of HFI_MAX_THERM_NOTIFY_COUNT capabilities. */
	for (i = 0;
	     (i + HFI_MAX_THERM_NOTIFY_COUNT) <= cpu_count;
	     i += HFI_MAX_THERM_NOTIFY_COUNT)
		thermal_genl_cpu_capability_event(HFI_MAX_THERM_NOTIFY_COUNT,
						  &cpu_caps[i]);

	cpu_count = cpu_count - i;

last_cmd:
	/* Process the remaining capabilities if any. */
	if (cpu_count)
		thermal_genl_cpu_capability_event(cpu_count, &cpu_caps[i]);

	kfree(cpu_caps);
out:
	mutex_unlock(&hfi_instance_lock);
}

static void hfi_update_work_fn(struct work_struct *work)
{
	struct hfi_instance *hfi_instance;

	hfi_instance = container_of(to_delayed_work(work), struct hfi_instance,
				    update_work);

	update_capabilities(hfi_instance);
}

void intel_hfi_process_event(__u64 pkg_therm_status_msr_val)
{
	struct hfi_instance *hfi_instance;
	int cpu = smp_processor_id();
	struct hfi_cpu_info *info;
	u64 new_timestamp;

	if (!pkg_therm_status_msr_val)
		return;

	info = &per_cpu(hfi_cpu_info, cpu);
	if (!info)
		return;

	/*
	 * A CPU is linked to its HFI instance before the thermal vector in the
	 * local APIC is unmasked. Hence, info->hfi_instance cannot be NULL
	 * when receiving an HFI event.
	 */
	hfi_instance = info->hfi_instance;
	if (unlikely(!hfi_instance)) {
		pr_debug("Received event on CPU %d but instance was null", cpu);
		return;
	}

	/*
	 * On most systems, all CPUs in the package receive a package-level
	 * thermal interrupt when there is an HFI update. It is sufficient to
	 * let a single CPU to acknowledge the update and queue work to
	 * process it. The remaining CPUs can resume their work.
	 */
	if (!raw_spin_trylock(&hfi_instance->event_lock))
		return;

	/* Skip duplicated updates. */
	new_timestamp = *(u64 *)hfi_instance->hw_table;
	if (*hfi_instance->timestamp == new_timestamp) {
		raw_spin_unlock(&hfi_instance->event_lock);
		return;
	}

	raw_spin_lock(&hfi_instance->table_lock);

	/*
	 * Copy the updated table into our local copy. This includes the new
	 * timestamp.
	 */
	memcpy(hfi_instance->local_table, hfi_instance->hw_table,
	       hfi_features.nr_table_pages << PAGE_SHIFT);

	raw_spin_unlock(&hfi_instance->table_lock);
	raw_spin_unlock(&hfi_instance->event_lock);

	/*
	 * Let hardware know that we are done reading the HFI table and it is
	 * free to update it again.
	 */
	pkg_therm_status_msr_val &= THERM_STATUS_CLEAR_PKG_MASK &
				    ~PACKAGE_THERM_STATUS_HFI_UPDATED;
	wrmsrl(MSR_IA32_PACKAGE_THERM_STATUS, pkg_therm_status_msr_val);

	queue_delayed_work(hfi_updates_wq, &hfi_instance->update_work,
			   HFI_UPDATE_INTERVAL);
}

static void init_hfi_cpu_index(struct hfi_cpu_info *info)
{
	union cpuid6_edx edx;

	/* Do not re-read @cpu's index if it has already been initialized. */
	if (info->index > -1)
		return;

	edx.full = cpuid_edx(CPUID_HFI_LEAF);
	info->index = edx.split.index;
}

/*
 * The format of the HFI table depends on the number of capabilities and classes
 * that the hardware supports. Keep a data structure to navigate the table.
 */
static void init_hfi_instance(struct hfi_instance *hfi_instance)
{
	/* The HFI header is below the time-stamp. */
	hfi_instance->hdr = hfi_instance->local_table +
			    sizeof(*hfi_instance->timestamp);

	/* The HFI data starts below the header. */
	hfi_instance->data = hfi_instance->hdr + hfi_features.hdr_size;
}

/**
 * intel_hfi_online() - Enable HFI on @cpu
 * @cpu:	CPU in which the HFI will be enabled
 *
 * Enable the HFI to be used in @cpu. The HFI is enabled at the die/package
 * level. The first CPU in the die/package to come online does the full HFI
 * initialization. Subsequent CPUs will just link themselves to the HFI
 * instance of their die/package.
 *
 * This function is called before enabling the thermal vector in the local APIC
 * in order to ensure that @cpu has an associated HFI instance when it receives
 * an HFI event.
 */
void intel_hfi_online(unsigned int cpu)
{
	struct hfi_instance *hfi_instance;
	struct hfi_cpu_info *info;
	phys_addr_t hw_table_pa;
	u64 msr_val;
	u16 die_id;

	/* Nothing to do if hfi_instances are missing. */
	if (!hfi_instances)
		return;

	/*
	 * Link @cpu to the HFI instance of its package/die. It does not
	 * matter whether the instance has been initialized.
	 */
	info = &per_cpu(hfi_cpu_info, cpu);
	die_id = topology_logical_die_id(cpu);
	hfi_instance = info->hfi_instance;
	if (!hfi_instance) {
		if (die_id < 0 || die_id >= max_hfi_instances)
			return;

		hfi_instance = &hfi_instances[die_id];
		info->hfi_instance = hfi_instance;
	}

	init_hfi_cpu_index(info);

	if (cpu_feature_enabled(X86_FEATURE_ITD)) {
		msr_val = HW_FEEDBACK_THREAD_CONFIG_ENABLE_BIT;
		wrmsrl(MSR_IA32_HW_FEEDBACK_THREAD_CONFIG, msr_val);
	}

	/*
	 * Now check if the HFI instance of the package/die of @cpu has been
	 * initialized (by checking its header). In such case, all we have to
	 * do is to add @cpu to this instance's cpumask.
	 */
	mutex_lock(&hfi_instance_lock);
	if (hfi_instance->hdr) {
		cpumask_set_cpu(cpu, hfi_instance->cpus);
		goto unlock;
	}

	/*
	 * Hardware is programmed with the physical address of the first page
	 * frame of the table. Hence, the allocated memory must be page-aligned.
	 */
	hfi_instance->hw_table = alloc_pages_exact(hfi_features.nr_table_pages,
						   GFP_KERNEL | __GFP_ZERO);
	if (!hfi_instance->hw_table)
		goto unlock;

	hw_table_pa = virt_to_phys(hfi_instance->hw_table);

	/*
	 * Allocate memory to keep a local copy of the table that
	 * hardware generates.
	 */
	hfi_instance->local_table = kzalloc(hfi_features.nr_table_pages << PAGE_SHIFT,
					    GFP_KERNEL);
	if (!hfi_instance->local_table)
		goto free_hw_table;

	/*
	 * Program the address of the feedback table of this die/package. On
	 * some processors, hardware remembers the old address of the HFI table
	 * even after having been reprogrammed and re-enabled. Thus, do not free
	 * the pages allocated for the table or reprogram the hardware with a
	 * new base address. Namely, program the hardware only once.
	 */
	msr_val = hw_table_pa | HW_FEEDBACK_PTR_VALID_BIT;
	wrmsrl(MSR_IA32_HW_FEEDBACK_PTR, msr_val);

	init_hfi_instance(hfi_instance);

	INIT_DELAYED_WORK(&hfi_instance->update_work, hfi_update_work_fn);
	raw_spin_lock_init(&hfi_instance->table_lock);
	raw_spin_lock_init(&hfi_instance->event_lock);

	cpumask_set_cpu(cpu, hfi_instance->cpus);

	/*
	 * Enable the hardware feedback interface and never disable it. See
	 * comment on programming the address of the table.
	 */
	rdmsrl(MSR_IA32_HW_FEEDBACK_CONFIG, msr_val);
	msr_val |= HW_FEEDBACK_CONFIG_HFI_ENABLE_BIT;

	if (cpu_feature_enabled(X86_FEATURE_ITD))
		msr_val |= HW_FEEDBACK_CONFIG_ITD_ENABLE_BIT;

	wrmsrl(MSR_IA32_HW_FEEDBACK_CONFIG, msr_val);

	/*
	 * We have all we need to support IPC classes. Task classification is
	 * now working.
	 *
	 * All class scores are zero until after the first HFI update. That is
	 * OK. The scheduler queries these scores at every load balance.
	 */
	if (cpu_feature_enabled(X86_FEATURE_ITD))
		sched_enable_ipc_classes();

unlock:
	mutex_unlock(&hfi_instance_lock);
	return;

free_hw_table:
	free_pages_exact(hfi_instance->hw_table, hfi_features.nr_table_pages);
	goto unlock;
}

/**
 * intel_hfi_offline() - Disable HFI on @cpu
 * @cpu:	CPU in which the HFI will be disabled
 *
 * Remove @cpu from those covered by its HFI instance.
 *
 * On some processors, hardware remembers previous programming settings even
 * after being reprogrammed. Thus, keep HFI enabled even if all CPUs in the
 * die/package of @cpu are offline. See note in intel_hfi_online().
 */
void intel_hfi_offline(unsigned int cpu)
{
	struct hfi_cpu_info *info = &per_cpu(hfi_cpu_info, cpu);
	struct hfi_instance *hfi_instance;

	/*
	 * Check if @cpu as an associated, initialized (i.e., with a non-NULL
	 * header). Also, HFI instances are only initialized if X86_FEATURE_HFI
	 * is present.
	 */
	hfi_instance = info->hfi_instance;
	if (!hfi_instance)
		return;

	if (!hfi_instance->hdr)
		return;

	mutex_lock(&hfi_instance_lock);
	cpumask_clear_cpu(cpu, hfi_instance->cpus);
	mutex_unlock(&hfi_instance_lock);
}

static __init int hfi_parse_features(void)
{
	unsigned int nr_capabilities;
	union cpuid6_edx edx;

	if (!boot_cpu_has(X86_FEATURE_HFI))
		return -ENODEV;

	/*
	 * If we are here we know that CPUID_HFI_LEAF exists. Parse the
	 * supported capabilities and the size of the HFI table.
	 */
	edx.full = cpuid_edx(CPUID_HFI_LEAF);

	if (!edx.split.capabilities.split.performance) {
		pr_debug("Performance reporting not supported! Not using HFI\n");
		return -ENODEV;
	}

	/*
	 * The number of supported capabilities determines the number of
	 * columns in the HFI table. Exclude the reserved bits.
	 */
	edx.split.capabilities.split.__reserved = 0;
	nr_capabilities = hweight8(edx.split.capabilities.bits);

	/* The number of 4KB pages required by the table */
	hfi_features.nr_table_pages = edx.split.table_pages + 1;

	/*
	 * Capability fields of an HFI class are grouped together. Classes are
	 * contiguous in memory.  Hence, use the number of supported features to
	 * locate a specific class.
	 */
	hfi_features.class_stride = nr_capabilities;

	if (cpu_feature_enabled(X86_FEATURE_ITD)) {
		union cpuid6_ecx ecx;

		ecx.full = cpuid_ecx(CPUID_HFI_LEAF);
		hfi_features.nr_classes = ecx.split.nr_classes;
	} else {
		hfi_features.nr_classes = 1;
	}

	/*
	 * The header contains change indications for each supported feature.
	 * The size of the table header is rounded up to be a multiple of 8
	 * bytes.
	 */
	hfi_features.hdr_size = DIV_ROUND_UP(nr_capabilities *
					     hfi_features.nr_classes, 8) * 8;

	/*
	 * Data of each logical processor is also rounded up to be a multiple
	 * of 8 bytes.
	 */
	hfi_features.cpu_stride = DIV_ROUND_UP(nr_capabilities *
					       hfi_features.nr_classes, 8) * 8;

	return 0;
}

void __init intel_hfi_init(void)
{
	struct hfi_instance *hfi_instance;
	int i, j;

	if (hfi_parse_features())
		return;

	/* There is one HFI instance per die/package. */
	max_hfi_instances = topology_max_packages() *
			    topology_max_die_per_package();

	/*
	 * This allocation may fail. CPU hotplug callbacks must check
	 * for a null pointer.
	 */
	hfi_instances = kcalloc(max_hfi_instances, sizeof(*hfi_instances),
				GFP_KERNEL);
	if (!hfi_instances)
		return;

	for (i = 0; i < max_hfi_instances; i++) {
		hfi_instance = &hfi_instances[i];
		if (!zalloc_cpumask_var(&hfi_instance->cpus, GFP_KERNEL))
			goto err_nomem;
	}

	hfi_updates_wq = create_singlethread_workqueue("hfi-updates");
	if (!hfi_updates_wq)
		goto err_nomem;

	if (alloc_hfi_ipcc_scores())
		goto err_ipcc;

	return;

err_ipcc:
	destroy_workqueue(hfi_updates_wq);

err_nomem:
	for (j = 0; j < i; ++j) {
		hfi_instance = &hfi_instances[j];
		free_cpumask_var(hfi_instance->cpus);
	}

	kfree(hfi_instances);
	hfi_instances = NULL;
}
