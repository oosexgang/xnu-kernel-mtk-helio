/*
 * The test of Documentation/examples/example_pwrkey.c, as Documentation/writing-a-driver.md describes it: the driver's
 * own .c included, the kernel faked (workbench/fakekernel), the hardware a model (workbench/pwrap_model.h).
 * One scenario a process: workbench/run.sh lists them by running this with no argument.
 */
#include <stdarg.h>
#include <unistd.h>

#include "fakekernel.h"
#define HAKIOS_MMIO_TEST 1
#include "pwrap_model.h"

static pthread_mutex_t pwrap_lock = PTHREAD_MUTEX_INITIALIZER;
void mtk_pwrap_lock(void) { pthread_mutex_lock(&pwrap_lock); }
void mtk_pwrap_unlock(void) { pthread_mutex_unlock(&pwrap_lock); }
vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size) { (void)size; return phys; }

void loader_log_putc(char c);

/* what the driver prints goes to the log the fake kernel keeps, where logged() looks */
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
#define printf to_log
#include "hakios_board.c"
#include "example_pwrkey.c"
#undef printf

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static void
start(const char *args)
{
	board = "phone-a";
	boot_args = args;
	pw.pwrap = hakios_board()->blocks.pwrap;
	pw.reg[MT_TOPSTATUS / 2] = PWRKEY_DEB;  /* up */
	example_pwrkey_init();
}

static void
reads_the_key(void)
{
	start("example_pwrkey=1");
	CHECK((int32_t)get("example_pwrkey") == 0, "up: %d", (int32_t)get("example_pwrkey"));
	pmic_set(MT_TOPSTATUS, 0);
	CHECK((int32_t)get("example_pwrkey") == 1, "down: %d", (int32_t)get("example_pwrkey"));
	CHECK(pw.reads == 2 && pw.writes == 0 && pw.elsewhere == 0, "two reads, nothing written (%u, %u, %u)",
	    pw.reads, pw.writes, pw.elsewhere);
}

static void
pmic_silent(void)
{
	pw.stuck = true;
	start("example_pwrkey=1");
	CHECK((int32_t)get("example_pwrkey") == -1, "no answer: -1, and no hang");
}

static void
off_by_default(void)
{
	start("");
	CHECK((int32_t)get("example_pwrkey") == -1 && pw.reads == 0, "not asked for: -1, the PMIC untouched");
}

static void
board_unknown(void)
{
	board = NULL;
	boot_args = "example_pwrkey=1";
	example_pwrkey_init();
	CHECK((int32_t)get("example_pwrkey") == -1 && pw.reads == 0 && logged("nothing is touched"), "an unknown board: nothing");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "reads_the_key", reads_the_key }, { "pmic_silent", pmic_silent },
		{ "off_by_default", off_by_default }, { "board_unknown", board_unknown },
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
