/*
 * The example driver of docs/writing-a-driver.md: the power key's state, read from the MT6358 PMIC
 * through the PMIC wrapper's second channel each time it is asked for, as the sysctl
 * kern.hakios.example_pwrkey (1 while the key is down, 0 while it is up, -1 when the PMIC does not
 * answer or the driver was not asked for). It is not built into the kernel; workbench/run.sh runs it against workbench/pwrap_model.h.
 *
 * Nothing is done unless boot-arg example_pwrkey=1 asks for it, and only on a board whose file lists
 * "keys-mt6358": the wrapper's address is that board's (hakios_board()->blocks.pwrap). The register is
 * the one bsd/dev/mtk_keys.c reads on the first phone: TOPSTATUS (0x28), bit 1 clear while the key is
 * down.
 */
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include "hakios_board.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void mtk_pwrap_lock(void);       /* one transaction at a time on the wrapper: every driver of it takes this */
extern void mtk_pwrap_unlock(void);

#define PWRAP_WACS2_CMD         0xc20
#define PWRAP_WACS2_RDATA       0xc24
#define PWRAP_WACS2_VLDCLR      0xc28
#define PWRAP_FSM(r)            (((r) >> 16) & 7u)
#define PWRAP_FSM_IDLE          0
#define PWRAP_FSM_WFVLDCLR      6
#define MT6358_TOPSTATUS        0x28
#define PWRKEY_DEB              (1u << 1)

static vm_offset_t pwrap;

#ifdef HAKIOS_MMIO_TEST
extern uint32_t hakios_test_rd(vm_offset_t address);
extern void hakios_test_wr(vm_offset_t address, uint32_t value);
#define rd(at)                  hakios_test_rd(pwrap + (at))
#define wr(at, value)           hakios_test_wr(pwrap + (at), (value))
#else
#define rd(at)                  (*(volatile uint32_t *)(pwrap + (at)))
#define wr(at, value)           (*(volatile uint32_t *)(pwrap + (at)) = (value))
#endif

/* the wrapper in `state`, looked at a bounded number of times: a wrapper that never answers is an answer too */
static bool
wait_for(uint32_t state)
{
	for (uint32_t spin = 0; spin < 200000; spin++) {
		if (PWRAP_FSM(rd(PWRAP_WACS2_RDATA)) == state) {
			return true;
		}
	}
	return false;
}

static bool
pmic_read(uint32_t reg, uint16_t *value)
{
	bool ok = false;

	mtk_pwrap_lock();
	if (wait_for(PWRAP_FSM_IDLE)) {
		wr(PWRAP_WACS2_CMD, (reg >> 1) << 16);
		if (wait_for(PWRAP_FSM_WFVLDCLR)) {
			*value = (uint16_t)rd(PWRAP_WACS2_RDATA);
			wr(PWRAP_WACS2_VLDCLR, 1);
			ok = true;
		}
	}
	mtk_pwrap_unlock();
	return ok;
}

static int
example_pwrkey_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	uint16_t top = 0;
	int down = -1;

	if (pwrap != 0 && pmic_read(MT6358_TOPSTATUS, &top)) {
		down = (top & PWRKEY_DEB) ? 0 : 1;
	}
	return SYSCTL_OUT(req, &down, sizeof(down));
}

SYSCTL_DECL(_kern_hakios);
SYSCTL_PROC(_kern_hakios, OID_AUTO, example_pwrkey, CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0,
    example_pwrkey_sysctl, "I", "the power key: 1 down, 0 up, -1 no answer (docs/examples/example_pwrkey.c)");

void
example_pwrkey_init(void)
{
	uint32_t on = 0;

	if (!PE_parse_boot_argn("example_pwrkey", &on, sizeof(on)) || on != 1) {
		return;
	}
	if (!hakios_board_wants(HAKIOS_HAS_KEYS_MT6358, "example_pwrkey=1")) {
		return;
	}
	pwrap = ml_io_map((vm_offset_t)hakios_board()->blocks.pwrap, 0x1000);
	printf("example_pwrkey: kern.hakios.example_pwrkey reads the power key\n");
}
