/*
 * The battery, as the kernel keeps it for everybody else: the node kern.hakios of the sysctls, the
 * battery's values under it, and the way from a board's gauge driver to the power source the system reads.
 *
 * A gauge driver (bsd/dev/mtk_battery.c: the MT6358's fuel gauge) calls hakios_battery_report()
 * with every reading. The values are kept here for the sysctls kern.hakios.battery_* and passed on
 * to iokit/Kernel/HakiosBattery.cpp, the IOPMPowerSource powerd finds. Nothing here touches hardware.
 */
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <sys/hakios_input.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

static int battery_soc_percent = -1, battery_voltage_mv = -1, battery_current_ma;
static int battery_temperature_c = -273, battery_present, battery_charger_present;
static int battery_charging, battery_soc_estimated, battery_charger_action = -1;
static uint64_t battery_updates;

SYSCTL_NODE(_kern, OID_AUTO, hakios, CTLFLAG_RW | CTLFLAG_LOCKED, 0, "this port's own values");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_soc, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_soc_percent, 0, "battery state of charge, 0..100 percent, or -1 if unknown");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_voltage_mv, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_voltage_mv, 0, "battery voltage in millivolts, or -1 if unknown");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_current_ma, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_current_ma, 0, "battery current in milliamps, more than 0 while it takes charge");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_temperature_c, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_temperature_c, 0, "battery temperature in Celsius, -273 while it is not known");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_present, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_present, 0, "1 if the gauge reports a battery");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_charger_present, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_charger_present, 0, "1 if a charger or a computer is on the USB port");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_charging, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_charging, 0, "1 while an attached charger is charging a non-full battery");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_soc_estimated, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_soc_estimated, 0, "1 only when SOC had to be estimated from voltage");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_charger_action, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_charger_action, 0, "the gauge driver's last status (Linux's POWER_SUPPLY_STATUS_*), for diagnostics");

static int battery_full_uah, battery_design_uah, battery_cycles, battery_reported;

SYSCTL_INT(_kern_hakios, OID_AUTO, battery_full_uah, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_full_uah, 0, "the battery's full charge as the fuel gauge has learnt it, in uAh (0 if unknown)");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_design_uah, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_design_uah, 0, "the battery's charge as designed, in uAh (0 if unknown)");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_cycles, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_cycles, 0, "the battery's charge cycles (0 if unknown)");
/* (the name is from the first phone, whose gauge sat behind its audio DSP; user space reads it under this name) */
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_from_dsp, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_reported, 0, "1 once a gauge driver has reported the battery");

extern void HakiosBatteryReport(const void *state, unsigned size);     /* iokit/Kernel/HakiosBattery.cpp */

void
hakios_battery_report(const struct hakios_battery_state *b)
{
	int percent = (b->percent_hundredths + 50) / 100;

	battery_soc_percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
	battery_voltage_mv = b->voltage_uv / 1000;
	battery_current_ma = b->current_ua / 1000;
	battery_temperature_c = (b->temperature_hundredths + 50) / 100;
	battery_present = b->present != 0;
	battery_charger_present = b->external != 0;
	battery_charging = b->status == 1;      /* POWER_SUPPLY_STATUS_CHARGING */
	battery_soc_estimated = 0;
	battery_charger_action = b->status;
	if (b->full_uah > 0) {
		battery_full_uah = b->full_uah;
	}
	if (b->design_uah > 0) {
		battery_design_uah = b->design_uah;
	}
	if (b->cycles > 0) {
		battery_cycles = b->cycles;
	}
	battery_reported = 1;
	battery_updates++;
	HakiosBatteryReport(b, sizeof(*b));
}
