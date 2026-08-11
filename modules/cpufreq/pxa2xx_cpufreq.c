
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/init.h>
#include <linux/cpufreq.h>
#include <linux/err.h>
#include <linux/regulator/consumer.h>
#include <linux/soc/pxa/cpu.h>
#include <linux/io.h>

#ifdef DEBUG
static unsigned int freq_debug;
module_param(freq_debug, uint, 0);
MODULE_PARM_DESC(freq_debug, "Set the debug messages to on=1/off=0");
#else
#define freq_debug  0
#endif

static struct regulator *vcc_core;

static unsigned int pxa27x_maxfreq;
module_param(pxa27x_maxfreq, uint, 0);
MODULE_PARM_DESC(pxa27x_maxfreq, "Set the pxa27x maxfreq in MHz"
		 "(typically 624=>pxa270, 416=>pxa271, 520=>pxa272)");

struct pxa_cpufreq_data {
	struct clk *clk_core;
};
static struct pxa_cpufreq_data  pxa_cpufreq_data;

struct pxa_freqs {
	unsigned int khz;
	int vmin;
	int vmax;
};

static const struct pxa_freqs pxa255_run_freqs[] =
{
	{ 99533, -1, -1},
	{199066, -1, -1},
	{298599, -1, -1},
	{398132, -1, -1},
	{471860, -1, -1},
	{530842, -1, -1},
	{589824, -1, -1},
	{663552, -1, -1},
};

static const struct pxa_freqs pxa255_turbo_freqs[] =
{
	{ 99500, -1, -1},
	{199100, -1, -1},
	{298500, -1, -1},
	{298600, -1, -1},
	{398100, -1, -1},
};

#define NUM_PXA25x_RUN_FREQS ARRAY_SIZE(pxa255_run_freqs)
#define NUM_PXA25x_TURBO_FREQS ARRAY_SIZE(pxa255_turbo_freqs)

static struct cpufreq_frequency_table
	pxa255_run_freq_table[NUM_PXA25x_RUN_FREQS+1];
static struct cpufreq_frequency_table
	pxa255_turbo_freq_table[NUM_PXA25x_TURBO_FREQS+1];

static unsigned int pxa255_turbo_table;
module_param(pxa255_turbo_table, uint, 0);
MODULE_PARM_DESC(pxa255_turbo_table, "Selects the frequency table (0 = run table, !0 = turbo table)");

#define PXA255_STOCK_MAXFREQ_MHZ	398

static unsigned int pxa255_maxfreq;
static unsigned int pxa255_max_mhz = PXA255_STOCK_MAXFREQ_MHZ;
module_param(pxa255_maxfreq, uint, 0444);
MODULE_PARM_DESC(pxa255_maxfreq, "Set the pxa255 maxfreq in MHz "
		 "(default/rated 398; overclock steps 471, 530, 589, 663)");

static struct pxa_freqs pxa27x_freqs[] = {
	{104000,  900000, 1705000 },
	{156000, 1000000, 1705000 },
	{208000, 1180000, 1705000 },
	{312000, 1250000, 1705000 },
	{416000, 1350000, 1705000 },
	{520000, 1450000, 1705000 },
	{624000, 1550000, 1705000 }
};

#define NUM_PXA27x_FREQS ARRAY_SIZE(pxa27x_freqs)
static struct cpufreq_frequency_table
	pxa27x_freq_table[NUM_PXA27x_FREQS+1];

#ifdef CONFIG_REGULATOR

static int pxa_cpufreq_change_voltage(const struct pxa_freqs *pxa_freq)
{
	int ret = 0;
	int vmin, vmax;

	if (!cpu_is_pxa27x())
		return 0;

	vmin = pxa_freq->vmin;
	vmax = pxa_freq->vmax;
	if ((vmin == -1) || (vmax == -1))
		return 0;

	ret = regulator_set_voltage(vcc_core, vmin, vmax);
	if (ret)
		pr_err("Failed to set vcc_core in [%dmV..%dmV]\n", vmin, vmax);
	return ret;
}

static void pxa_cpufreq_init_voltages(void)
{
	vcc_core = regulator_get(NULL, "vcc_core");
	if (IS_ERR(vcc_core)) {
		pr_info("Didn't find vcc_core regulator\n");
		vcc_core = NULL;
	} else {
		pr_info("Found vcc_core regulator\n");
	}
}
#else
static int pxa_cpufreq_change_voltage(const struct pxa_freqs *pxa_freq)
{
	return 0;
}

static void pxa_cpufreq_init_voltages(void) { }
#endif

static void find_freq_tables(struct cpufreq_frequency_table **freq_table,
			     const struct pxa_freqs **pxa_freqs)
{
	if (cpu_is_pxa25x()) {
		if (!pxa255_turbo_table) {
			*pxa_freqs = pxa255_run_freqs;
			*freq_table = pxa255_run_freq_table;
		} else {
			*pxa_freqs = pxa255_turbo_freqs;
			*freq_table = pxa255_turbo_freq_table;
		}
	} else if (cpu_is_pxa27x()) {
		*pxa_freqs = pxa27x_freqs;
		*freq_table = pxa27x_freq_table;
	} else {
		BUG();
	}
}

static void pxa27x_guess_max_freq(void)
{
	if (!pxa27x_maxfreq) {
		pxa27x_maxfreq = 416000;
		pr_info("PXA CPU 27x max frequency not defined (pxa27x_maxfreq), assuming pxa271 with %dkHz maxfreq\n",
			pxa27x_maxfreq);
	} else {
		pxa27x_maxfreq *= 1000;
	}
}

static unsigned int pxa_cpufreq_get(unsigned int cpu)
{
	struct pxa_cpufreq_data *data = cpufreq_get_driver_data();

	return (unsigned int) clk_get_rate(data->clk_core) / 1000;
}

static int pxa_set_target(struct cpufreq_policy *policy, unsigned int idx)
{
	struct cpufreq_frequency_table *pxa_freqs_table;
	const struct pxa_freqs *pxa_freq_settings;
	struct pxa_cpufreq_data *data = cpufreq_get_driver_data();
	unsigned int new_freq_cpu;
	int ret = 0;

	find_freq_tables(&pxa_freqs_table, &pxa_freq_settings);

	new_freq_cpu = pxa_freq_settings[idx].khz;

	if (freq_debug)
		pr_debug("Changing CPU frequency from %d Mhz to %d Mhz\n",
			 policy->cur / 1000,  new_freq_cpu / 1000);

	if (vcc_core && new_freq_cpu > policy->cur) {
		ret = pxa_cpufreq_change_voltage(&pxa_freq_settings[idx]);
		if (ret)
			return ret;
	}

	clk_set_rate(data->clk_core, new_freq_cpu * 1000);

	if (vcc_core && new_freq_cpu < policy->cur)
		ret = pxa_cpufreq_change_voltage(&pxa_freq_settings[idx]);

	return 0;
}

static int pxa_cpufreq_init(struct cpufreq_policy *policy)
{
	int i;
	unsigned int freq;
	struct cpufreq_frequency_table *pxa255_freq_table;
	const struct pxa_freqs *pxa255_freqs;

	if (cpu_is_pxa27x())
		pxa27x_guess_max_freq();

	pxa_cpufreq_init_voltages();

	policy->cpuinfo.transition_latency = 1000;

	pxa255_max_mhz = pxa255_maxfreq ? pxa255_maxfreq
				       : PXA255_STOCK_MAXFREQ_MHZ;
	if (pxa255_max_mhz > PXA255_STOCK_MAXFREQ_MHZ)
		pr_warn("pxa255 overclock enabled: ceiling %u MHz is above the rated %u MHz -- the SDRAM and the static memory bus are overclocked with the core\n",
			pxa255_max_mhz, PXA255_STOCK_MAXFREQ_MHZ);

	for (i = 0; i < NUM_PXA25x_RUN_FREQS; i++) {
		if (pxa255_run_freqs[i].khz / 1000 > pxa255_max_mhz)
			break;
		pxa255_run_freq_table[i].frequency = pxa255_run_freqs[i].khz;
		pxa255_run_freq_table[i].driver_data = i;
	}
	pxa255_run_freq_table[i].frequency = CPUFREQ_TABLE_END;

	for (i = 0; i < NUM_PXA25x_TURBO_FREQS; i++) {
		if (pxa255_turbo_freqs[i].khz / 1000 > pxa255_max_mhz)
			break;
		pxa255_turbo_freq_table[i].frequency =
			pxa255_turbo_freqs[i].khz;
		pxa255_turbo_freq_table[i].driver_data = i;
	}
	pxa255_turbo_freq_table[i].frequency = CPUFREQ_TABLE_END;

	pxa255_turbo_table = !!pxa255_turbo_table;

	for (i = 0; i < NUM_PXA27x_FREQS; i++) {
		freq = pxa27x_freqs[i].khz;
		if (freq > pxa27x_maxfreq)
			break;
		pxa27x_freq_table[i].frequency = freq;
		pxa27x_freq_table[i].driver_data = i;
	}
	pxa27x_freq_table[i].driver_data = i;
	pxa27x_freq_table[i].frequency = CPUFREQ_TABLE_END;

	if (cpu_is_pxa25x()) {
		find_freq_tables(&pxa255_freq_table, &pxa255_freqs);
		pr_info("using %s frequency table\n",
			pxa255_turbo_table ? "turbo" : "run");

		policy->freq_table = pxa255_freq_table;
	}
	else if (cpu_is_pxa27x()) {
		policy->freq_table = pxa27x_freq_table;
	}

	pr_info("frequency change support initialized\n");

	return 0;
}

static struct cpufreq_driver pxa_cpufreq_driver = {
	.flags	= CPUFREQ_NEED_INITIAL_FREQ_CHECK,
	.verify	= cpufreq_generic_frequency_table_verify,
	.target_index = pxa_set_target,
	.init	= pxa_cpufreq_init,
	.get	= pxa_cpufreq_get,
	.name	= "PXA2xx",
	.driver_data = &pxa_cpufreq_data,
};

static int __init pxa_cpu_init(void)
{
	int ret = -ENODEV;

	pxa_cpufreq_data.clk_core = clk_get_sys(NULL, "core");
	if (IS_ERR(pxa_cpufreq_data.clk_core))
		return PTR_ERR(pxa_cpufreq_data.clk_core);

	if (cpu_is_pxa25x() || cpu_is_pxa27x())
		ret = cpufreq_register_driver(&pxa_cpufreq_driver);
	return ret;
}

static void __exit pxa_cpu_exit(void)
{
	cpufreq_unregister_driver(&pxa_cpufreq_driver);
}

MODULE_AUTHOR("Intrinsyc Software Inc.");
MODULE_DESCRIPTION("CPU frequency changing driver for the PXA architecture");
MODULE_LICENSE("GPL");
module_init(pxa_cpu_init);
module_exit(pxa_cpu_exit);
