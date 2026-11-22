/*
 * The MT6358 battery driver (kernel/overlay/bsd/dev/mtk_battery.c), run as a host program against a
 * model of the PMIC wrapper and the PMIC (workbench/pwrap_model.h): the percentage Android left and the
 * gauge's counter, the voltage alone, the two too far apart, the charger, and a PMIC that does not
 * answer. What the driver hands on is caught where bsd/dev/hakios_battery.c would take it.
 */
#include <stdarg.h>
#include <unistd.h>

#include "fakekernel.h"
#define HAKIOS_MMIO_TEST 1
#include "pwrap_model.h"

extern char log_text[];
extern bool logged(const char *text);
void loader_log_putc(char c);

static void
to_log(const char *format, ...)
{
	char line[512];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
}

#define KERNEL 1
#include <sys/hakios_input.h>

/* what the driver reports, as hakios_battery.c would be given it */
static pthread_mutex_t reported_lock = PTHREAD_MUTEX_INITIALIZER;
static struct hakios_battery_state reported;
static unsigned reports;
void
hakios_battery_report(const struct hakios_battery_state *state)
{
	pthread_mutex_lock(&reported_lock);
	reported = *state;
	reports++;
	pthread_mutex_unlock(&reported_lock);
}

static pthread_mutex_t pwrap_lock = PTHREAD_MUTEX_INITIALIZER;
void mtk_pwrap_lock(void) { pthread_mutex_lock(&pwrap_lock); }
void mtk_pwrap_unlock(void) { pthread_mutex_unlock(&pwrap_lock); }

vm_offset_t
ml_io_map(vm_offset_t phys, vm_size_t size)
{
	(void)size;
	return phys;            /* the block's own address: the model knows it by that */
}

#define printf to_log
#undef __printflike             /* (the driver's own format attributes name printf: kept the C library's) */
#define __printflike(a, b) __attribute__((format(__printf__, a, b)))
#include "hakios_board.c"
#include "mtk_battery.c"
#undef printf

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define WAIT_FOR(cond, ms) do { for (int waited = 0; !(cond) && waited < (ms); waited += 5) usleep(5000); } while (0)

/* the counts BATADC gives for `mv` (the driver's COUNT_NV: 164795 nV a count) */
static uint16_t
counts_of(int mv)
{
	return (uint16_t)((int64_t)mv * 1000000 / COUNT_NV + 1);
}

static struct hakios_battery_state
last(unsigned *count)
{
	struct hakios_battery_state s;

	pthread_mutex_lock(&reported_lock);
	s = reported;
	*count = reports;
	pthread_mutex_unlock(&reported_lock);
	return s;
}

/* the phone of workbench/devices/phone-a, its PMIC wrapper where its SoC file says; the driver started */
static void
start(const char *args)
{
	board = "phone-a";
	boot_args = args;
	pw.pwrap = hakios_board()->blocks.pwrap;
	fake_delay_divisor = 1000;      /* the first look after 20 ms, then every 2 or 10 */
	mtk_battery_init();
}

/* a reading after the first few, so that the smoothing has settled */
static struct hakios_battery_state
settled(void)
{
	unsigned count = 0;

	EVENTUALLY(last(&count).size != 0 && count >= 8, 5000, "eight reports: %u", count);
	return last(&count);
}

static void
android_percentage_then_counted(void)
{
	pw.batadc = counts_of(3900);
	pw.reg[MT_RTC_AL_MTH / 2] = 57 << 8;
	pw.reg[MT_FGADC_CAR_CON0 / 2] = 0x0000;
	pw.reg[MT_FGADC_CAR_CON1 / 2] = 0x0100;
	start("mtkbatt=2");
	struct hakios_battery_state s = settled();
	CHECK(s.size == sizeof(s) && s.present == 1, "a report of the right size");
	CHECK(s.percent_hundredths == 5700, "Android's 57%%, the counter unchanged: %d", s.percent_hundredths);
	CHECK(s.voltage_uv / 1000 >= 3899 && s.voltage_uv / 1000 <= 3901, "3.9 V: %d uV", s.voltage_uv);
	CHECK(logged("Android left 57%"), "said where the percentage comes from: %s", log_text);
	/* 1% of 4000 mAh = 40 mAh = 372353 counts of 107425 pAh: the counter falls by that */
	uint32_t charge = 0x01000000u - 372353u;
	pmic_set(MT_FGADC_CAR_CON0, (uint16_t)charge);
	pmic_set(MT_FGADC_CAR_CON1, (uint16_t)(charge >> 16));
	unsigned count = 0;
	EVENTUALLY(last(&count).percent_hundredths == 5600, 3000, "one percent less by the counter: %d", last(&count).percent_hundredths);
	CHECK(pw.latches > 0 && pw.elsewhere == 0, "the gauge latched, nothing written where nothing is: %u", pw.elsewhere);
}

static void
voltage_alone(void)
{
	pw.batadc = counts_of(3900);    /* no percentage from Android: 3885..3985 mV is 50..60% */
	start("mtkbatt=2");
	struct hakios_battery_state s = settled();
	CHECK(s.percent_hundredths >= 5140 && s.percent_hundredths <= 5160, "about 51.5%% by the voltage: %d", s.percent_hundredths);
	CHECK(logged("by the voltage alone"), "said: %s", log_text);
	CHECK(s.external == 0 && s.status == 2, "no charger: discharging (%d, %d)", s.external, s.status);
}

static void
too_far_apart(void)
{
	pw.batadc = counts_of(3900);
	pw.reg[MT_RTC_AL_MTH / 2] = 95 << 8;    /* Android said 95%, the voltage says some 51% */
	start("mtkbatt=2");
	struct hakios_battery_state s = settled();
	CHECK(s.percent_hundredths >= 5140 && s.percent_hundredths <= 5160, "the voltage believed: %d", s.percent_hundredths);
	CHECK(logged("the voltage is believed from here"), "said: %s", log_text);
}

static void
charging(void)
{
	pw.batadc = counts_of(4000);
	pw.reg[MT_CHR_CON0 / 2] = 1u << 5;                      /* CHRDET */
	pw.reg[MT_FGADC_CUR_CON0 / 2] = 1000;                   /* 1000 counts: 762 mA in */
	start("mtkbatt=2");
	struct hakios_battery_state s = settled();
	CHECK(s.external == 1 && s.status == 1, "a charger and current going in: charging (%d, %d)", s.external, s.status);
	CHECK(s.current_ua >= 762000 && s.current_ua <= 763000, "762 mA: %d uA", s.current_ua);
	/* the charger is there and nothing goes in: not charging */
	pmic_set(MT_FGADC_CUR_CON0, (uint16_t)-200);
	unsigned count = 0;
	EVENTUALLY(last(&count).status == 3, 3000, "not charging: %d", last(&count).status);
}

static void
no_conversion(void)
{
	pw.adc_never_ready = true;
	start("mtkbatt=2");
	EVENTUALLY(logged("no conversion of BATADC"), 3000, "said: %s", log_text);
	usleep(100 * 1000);
	unsigned count = 0;
	(void)last(&count);
	CHECK(count == 0, "nothing reported on looks that read nothing: %u", count);
}

static void
wrapper_stuck(void)
{
	pw.stuck = true;
	pw.batadc = counts_of(3900);
	start("mtkbatt=2");
	usleep(300 * 1000);
	unsigned count = 0;
	(void)last(&count);
	CHECK(count == 0, "a wrapper that never comes back: nothing reported, and no hang: %u", count);
	CHECK(pw.writes == 0, "nothing written: %u", pw.writes);
}

static void
off_by_default(void)
{
	pw.batadc = counts_of(3900);
	start("");
	usleep(200 * 1000);
	CHECK(pw.reads == 0 && pw.writes == 0, "no mtkbatt=: the PMIC is not touched (%u, %u)", pw.reads, pw.writes);
}

static void
board_unknown(void)
{
	board = NULL;
	boot_args = "mtkbatt=1";
	mtk_battery_init();
	usleep(200 * 1000);
	CHECK(pw.reads == 0 && pw.writes == 0, "a board the kernel does not know: nothing touched");
	CHECK(logged("nothing is touched"), "said: %s", log_text);
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "android_percentage_then_counted", android_percentage_then_counted }, { "voltage_alone", voltage_alone },
		{ "too_far_apart", too_far_apart }, { "charging", charging }, { "no_conversion", no_conversion },
		{ "wrapper_stuck", wrapper_stuck }, { "off_by_default", off_by_default }, { "board_unknown", board_unknown },
	};

	if (argc < 2) {
		for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
			printf("%s\n", scenarios[i].name);
		}
		return 0;
	}
	for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
		if (strcmp(argv[1], scenarios[i].name) == 0) {
			scenarios[i].run();
			printf("%-32s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures != 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
