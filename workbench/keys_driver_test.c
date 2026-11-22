/*
 * The MT6358 phone's keys (drivers/board/mtk_keys.c), run as a host program against a model
 * of the keypad's register and of the PMIC behind its wrapper (workbench/pwrap_model.h): the volume keys
 * from the keypad, the power key from the PMIC's status, a bounce, and a PMIC that does not answer.
 * What the driver posts is caught where bsd/dev/hakios_input.c would queue it.
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

/* what the driver posts, in order */
static pthread_mutex_t posted_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { uint32_t type, button; } posted[64];
static unsigned posts;
void
hakios_touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y)
{
	(void)x;
	(void)y;
	pthread_mutex_lock(&posted_lock);
	if (posts < 64) {
		posted[posts].type = type;
		posted[posts].button = finger;
	}
	posts++;
	pthread_mutex_unlock(&posted_lock);
}

static unsigned
posts_now(void)
{
	pthread_mutex_lock(&posted_lock);
	unsigned n = posts;
	pthread_mutex_unlock(&posted_lock);
	return n;
}

static pthread_mutex_t pwrap_lock = PTHREAD_MUTEX_INITIALIZER;
void mtk_pwrap_lock(void) { pthread_mutex_lock(&pwrap_lock); }
void mtk_pwrap_unlock(void) { pthread_mutex_unlock(&pwrap_lock); }
bool mtk_mali_ready(void) { return false; }

vm_offset_t
ml_io_map(vm_offset_t phys, vm_size_t size)
{
	(void)size;
	return phys;
}

#define printf to_log
#undef __printflike             /* (the driver's own format attributes name printf: kept the C library's) */
#define __printflike(a, b) __attribute__((format(__printf__, a, b)))
#include "hakios_board.c"
#include "mtk_keys.c"
#undef printf
void hakios_keep(const char *format, ...) { (void)format; }
void hakios_host_keep_add(const char *format, ...) { (void)format; }

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define WAIT_FOR(cond, ms) do { for (int waited = 0; !(cond) && waited < (ms); waited += 5) usleep(5000); } while (0)

#define KEY_UP_BIT      (1u << 0)       /* KP_MEM1: volume up, clear while down */
#define KEY_DOWN_BIT    (1u << 1)
#define PWRKEY_UP       (1u << 1)       /* TOPSTATUS: set while the power key is up */

static void
keypad(uint32_t mem1)
{
	pthread_mutex_lock(&pw.lock);
	pw.keypad_mem1 = mem1;
	pthread_mutex_unlock(&pw.lock);
}

static void
start(const char *args)
{
	board = "phone-a";
	boot_args = args;
	pw.pwrap = hakios_board()->blocks.pwrap;
	pw.keypad = hakios_board()->blocks.keypad;
	pw.reg[MT_TOPSTATUS / 2] = PWRKEY_UP;
	mtk_keys_init();
}

static void
volume_keys(void)
{
	start("buttons=2");
	WAIT_FOR(logged("looked at every 20 ms"), 2000);
	CHECK(logged("PMIC status reachable"), "the wrapper answers: %s", log_text);
	keypad(0xffff & ~KEY_UP_BIT);
	EVENTUALLY(posts_now() == 1, 2000, "volume up down: %u posts", posts_now());
	CHECK(posted[0].type == HAKIOS_BUTTON_DOWN && posted[0].button == HAKIOS_BUTTON_VOLUME_UP, "as a button down of volume up");
	keypad(0xffff);
	EVENTUALLY(posts_now() == 2, 2000, "and up again: %u", posts_now());
	CHECK(posted[1].type == HAKIOS_BUTTON_UP && posted[1].button == HAKIOS_BUTTON_VOLUME_UP, "as a button up");
	keypad(0xffff & ~KEY_DOWN_BIT);
	EVENTUALLY(posts_now() == 3, 2000, "volume down: %u", posts_now());
	CHECK(posted[2].type == HAKIOS_BUTTON_DOWN && posted[2].button == HAKIOS_BUTTON_VOLUME_DOWN, "as volume down");
	keypad(0xffff);
	EVENTUALLY(posts_now() == 4, 2000, "released: %u", posts_now());
}

static void
power_key(void)
{
	start("buttons=2");
	pmic_set(MT_TOPSTATUS, 0);              /* PWRKEY_DEB clear: down */
	EVENTUALLY(posts_now() == 1, 2000, "the power key: %u posts", posts_now());
	CHECK(posted[0].type == HAKIOS_BUTTON_DOWN && posted[0].button == HAKIOS_BUTTON_LOCK, "as the lock button");
	pmic_set(MT_TOPSTATUS, PWRKEY_UP);
	EVENTUALLY(posts_now() == 2, 2000, "and up: %u", posts_now());
	CHECK(pw.elsewhere == 0, "nothing read or written where nothing is: %u", pw.elsewhere);
}

/* a key down for one look only (under the 20 ms of a look) is a bounce: nothing is posted */
static void
bounce(void)
{
	start("buttons=2");
	WAIT_FOR(logged("looked at every 20 ms"), 2000);
	usleep(30 * 1000);
	keypad(0xffff & ~KEY_UP_BIT);
	usleep(5 * 1000);
	keypad(0xffff);
	usleep(200 * 1000);
	CHECK(posts_now() == 0, "a press of 5 ms: nothing (%u posts)", posts_now());
}

static void
pmic_silent(void)
{
	pw.stuck = true;
	start("buttons=2");
	EVENTUALLY(logged("PMIC status NOT REACHABLE"), 3000, "said: %s", log_text);
	keypad(0xffff & ~KEY_DOWN_BIT);
	EVENTUALLY(posts_now() >= 1, 5000, "the keypad's keys work without the PMIC: %u posts", posts_now());
	CHECK(posted[0].button == HAKIOS_BUTTON_VOLUME_DOWN, "volume down, and no power key made up from no answer");
}

static void
off_by_default(void)
{
	start("");
	usleep(200 * 1000);
	CHECK(pw.reads == 0 && !logged("mtkkeys"), "no buttons=2: nothing read");
}

/* the clocks' tables are the MT6768's: on the made-up chip of phone-a (mt0001) neither is touched, and it is said */
static void
clocks_of_another_chip(void)
{
	start("buttons=2 mtk_cpufreq=3 mali_mhz=950");
	WAIT_FOR(logged("looked at every 20 ms"), 2000);
	EVENTUALLY(logged("mali_mhz= is the MT6768's"), 2000, "said: %s", log_text);
	CHECK(logged("mtk_cpufreq= is the MT6768's"), "the processors' clock refused too: %s", log_text);
	CHECK(pw.elsewhere == 0, "nothing read or written where nothing is: %u", pw.elsewhere);
}

static void
board_without_keys(void)
{
	board = NULL;
	boot_args = "buttons=2";
	mtk_keys_init();
	usleep(200 * 1000);
	CHECK(pw.reads == 0 && logged("nothing is touched"), "a board the kernel does not know: %s", log_text);
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "volume_keys", volume_keys }, { "power_key", power_key }, { "bounce", bounce },
		{ "pmic_silent", pmic_silent }, { "off_by_default", off_by_default }, { "board_without_keys", board_without_keys },
		{ "clocks_of_another_chip", clocks_of_another_chip },
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
