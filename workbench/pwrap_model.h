/*
 * A model of the PMIC wrapper's second channel (WACS2) and of the MT6358 behind it, as
 * bsd/dev/mtk_keys.c and bsd/dev/mtk_battery.c drive them, for the tests of those drivers.
 *
 *   PWRAP + 0xc20  WACS2_CMD     bit 31 write; bits 30:16 the register's address / 2; bits 15:0 what to write
 *   PWRAP + 0xc24  WACS2_RDATA   bits 18:16 the state (0 idle, 6 a read waits to be taken); bits 15:0 what was read
 *   PWRAP + 0xc28  WACS2_VLDCLR  1: the read was taken, back to idle
 *
 * The PMIC's registers are 16 bits wide, at even addresses. A test sets what they hold and, for the
 * registers that answer a write (a conversion asked for, the gauge latched), what the model does.
 * The driver's accesses come here through its HAKIOS_MMIO_TEST seam: hakios_test_rd/wr below,
 * called with the address ml_io_map() gave it, which is the block's own (the driver tests).
 * The keypad's register is kept here too, since mtk_keys.c reads it through the same seam.
 */
#ifndef PWRAP_MODEL_H
#define PWRAP_MODEL_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#define PMIC_REGS               0x2000

static struct pwrap_model {
	uint64_t pwrap, keypad;         /* where the blocks are: the board's addresses */
	uint16_t reg[PMIC_REGS / 2];
	uint32_t keypad_mem1;           /* KP_MEM1: a bit clear while that key is down */
	uint32_t state;                 /* the WACS2 state machine: 0 or 6 */
	uint16_t rdata;
	bool stuck;                     /* the wrapper never comes back to idle */
	bool adc_never_ready;           /* a conversion is asked for and never ends */
	uint16_t batadc;                /* the counts a BATADC conversion gives */
	unsigned reads, writes, elsewhere, conversions, latches;
	pthread_mutex_t lock;
} pw = { .lock = PTHREAD_MUTEX_INITIALIZER, .keypad_mem1 = 0xffff };

#define MT_RTC_AL_MTH           0x05aa
#define MT_CHR_CON0             0x0a88
#define MT_FGADC_CON1           0x0d0a
#define MT_FGADC_CAR_CON0       0x0d14
#define MT_FGADC_CAR_CON1       0x0d16
#define MT_FGADC_CUR_CON0       0x0d8a
#define MT_AUXADC_ADC0          0x1088
#define MT_AUXADC_RQST0         0x1108
#define MT_TOPSTATUS            0x0028

static inline void
pmic_set(uint32_t reg, uint16_t value)
{
	pthread_mutex_lock(&pw.lock);
	pw.reg[(reg % PMIC_REGS) / 2] = value;
	pthread_mutex_unlock(&pw.lock);
}

/* a PMIC register written through the wrapper: what the real one does with the few the drivers write */
static inline void
pmic_written(uint32_t reg, uint16_t value)
{
	pw.writes++;
	if (reg == MT_AUXADC_RQST0 && (value & 1)) {
		pw.conversions++;
		pw.reg[MT_AUXADC_ADC0 / 2] = pw.adc_never_ready ? 0 : (uint16_t)(0x8000 | (pw.batadc & 0x7fff));
		return;
	}
	if (reg == MT_FGADC_CON1) {
		uint16_t kept = pw.reg[reg / 2] & 0x8000;

		if (value & (1u << 0)) {                /* READ: the current and the counter latched */
			pw.latches++;
			kept = 0x8000;
		}
		if (value & (1u << 3)) {                /* CLEAR: let go */
			kept = 0;
		}
		pw.reg[reg / 2] = (uint16_t)((value & 0x7ff6) | kept);
		return;
	}
	pw.reg[(reg % PMIC_REGS) / 2] = value;
}

uint32_t
hakios_test_rd(vm_offset_t address)
{
	uint32_t value = 0;

	pthread_mutex_lock(&pw.lock);
	if (address == pw.keypad + 0x4) {
		value = pw.keypad_mem1;
	} else if (address == pw.pwrap + 0xc24) {
		value = (pw.stuck ? 1u : pw.state) << 16 | pw.rdata;
	} else {
		pw.elsewhere++;
	}
	pthread_mutex_unlock(&pw.lock);
	return value;
}

void
hakios_test_wr(vm_offset_t address, uint32_t value)
{
	pthread_mutex_lock(&pw.lock);
	if (address == pw.pwrap + 0xc20 && !pw.stuck) {
		uint32_t reg = ((value >> 16) & 0x7fff) << 1;

		if (pw.state != 0 || reg >= PMIC_REGS) {
			pw.elsewhere++;         /* a command while one waits, or past the PMIC */
		} else if (value & (1u << 31)) {
			pmic_written(reg, (uint16_t)value);
		} else {
			pw.reads++;
			pw.rdata = pw.reg[reg / 2];
			pw.state = 6;
		}
	} else if (address == pw.pwrap + 0xc28 && value == 1) {
		pw.state = 0;
	} else if (!pw.stuck) {
		pw.elsewhere++;
	}
	pthread_mutex_unlock(&pw.lock);
}

#endif
