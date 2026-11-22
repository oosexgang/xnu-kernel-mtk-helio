/*
 * The GPU's platform on a MediaTek chip (drivers/board/mtk_mfg.h) against a model of the registers it writes: topckgen,
 * the SPM with its power domains, infracfg's bus protection, mfgcfg. The MT6768's table must do exactly what the
 * driver did before the chip became a table: original_mt6768() below is that code, as it was, and both run against
 * a fresh model with every write recorded.
 *
 *   mfg_power_test               the scenarios, one a line
 *   mfg_power_test SCENARIO      one scenario a process
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uintptr_t vm_offset_t;

/* ---- the model ---------------------------------------------------------------------------------- */

enum { TOPCKGEN = 1, SPM, INFRACFG, MFGCFG, BLOCKS };
#define BASE(block)     ((vm_offset_t)(block) << 16)
#define BLOCK(base)     ((unsigned)((base) >> 16))

static uint32_t regs[BLOCKS][0x1000 / 4];
static struct write {
	unsigned block;
	uint32_t offset, value;
} writes[256];
static unsigned write_count;

/* the MT6768's domains as the model knows them: PWR_CON, its status bit, whether it has SRAM, whether it powers up */
static struct {
	uint32_t con, status;
	bool sram, dead;
} model_domains[] = {
	{ 0x334, 1u << 14, false, false },
	{ 0x338, 1u << 11, true, false },
	{ 0x34c, 1u << 12, true, false },
	{ 0x310, 1u << 13, true, false },
};

static void
model_reset(void)
{
	memset(regs, 0, sizeof(regs));
	write_count = 0;
	for (unsigned i = 0; i < sizeof(model_domains) / sizeof(model_domains[0]); i++) {
		/* off, as the preloader leaves them: isolated, clock off, SRAM down and saying so */
		regs[SPM][model_domains[i].con / 4] = (1u << 1) | (1u << 4) | (model_domains[i].sram ? (1u << 8) | (1u << 12) : 0);
		model_domains[i].dead = false;
	}
}

static uint32_t
rd(vm_offset_t base, uint32_t at)
{
	return regs[BLOCK(base)][at / 4];
}

static void
wr(vm_offset_t base, uint32_t at, uint32_t value)
{
	unsigned block = BLOCK(base);

	if (write_count < sizeof(writes) / sizeof(writes[0])) {
		writes[write_count++] = (struct write){ block, at, value };
	}
	regs[block][at / 4] = value;
	if (block != SPM) {
		return;
	}
	for (unsigned i = 0; i < sizeof(model_domains) / sizeof(model_domains[0]); i++) {
		if (model_domains[i].con != at || model_domains[i].dead) {
			continue;
		}
		/* both switches on: the domain says so in PWR_STATUS and PWR_STATUS_2ND; its SRAM up: the ack goes */
		if ((value & (1u << 2)) && (value & (1u << 3))) {
			regs[SPM][0x180 / 4] |= model_domains[i].status;
			regs[SPM][0x184 / 4] |= model_domains[i].status;
		}
		if (model_domains[i].sram && !(value & (1u << 8))) {
			regs[SPM][at / 4] &= ~(1u << 12);
		}
	}
}

static bool
wait_for(vm_offset_t base, uint32_t at, uint32_t mask, uint32_t want, uint32_t ms)
{
	(void)ms;
	return (rd(base, at) & mask) == want;
}

#include "mtk_mfg.h"

/* ---- the driver's code before the chip was a table (drivers/board/mtk_mali.c, mali_up and domain_on) ---------- */

static const struct {
	const char *name;
	uint32_t con, status;
	bool sram;
} original_domains[4] = {
	{ "MFG_ASYNC", 0x334, 1u << 14, false },
	{ "MFG", 0x338, 1u << 11, true },
	{ "MFG_CORE0", 0x34c, 1u << 12, true },
	{ "MFG_CORE1", 0x310, 1u << 13, true },
};

static bool
original_domain_on(unsigned i, vm_offset_t spm)
{
	uint32_t con = original_domains[i].con, bit = original_domains[i].status;

	wr(spm, con, rd(spm, con) | (1u << 2));
	wr(spm, con, rd(spm, con) | (1u << 3));
	if (!wait_for(spm, 0x180, bit, bit, 20) || !wait_for(spm, 0x184, bit, bit, 20)) {
		return false;
	}
	wr(spm, con, rd(spm, con) & ~(1u << 4));
	wr(spm, con, rd(spm, con) & ~(1u << 1));
	wr(spm, con, rd(spm, con) | (1u << 0));
	if (original_domains[i].sram) {
		wr(spm, con, rd(spm, con) & ~(1u << 8));
		return wait_for(spm, con, 1u << 12, 0, 20);
	}
	return true;
}

/* how far it came: 1 the clock, 2 the domains, 3 the gate */
static int
original_mt6768(void)
{
	wr(BASE(TOPCKGEN), 0x58, 3u | (1u << 7));
	wr(BASE(TOPCKGEN), 0x54, 2u);
	for (unsigned i = 0; i < 4; i++) {
		if (!original_domain_on(i, BASE(SPM))) {
			return 1;
		}
		if (i == 1) {
			wr(BASE(INFRACFG), 0x2a4, (1u << 21) | (1u << 22));
			wr(BASE(INFRACFG), 0x2a4, 1u << 25);
		}
	}
	wr(BASE(MFGCFG), 0x008, 1u);
	return 3;
}

static int
table(const struct mtk_mfg *chip)
{
	mtk_mfg_clock(chip, BASE(TOPCKGEN));
	if (mtk_mfg_domains_on(chip, BASE(SPM), BASE(INFRACFG)) != NULL) {
		return 1;
	}
	mtk_mfg_gate(chip, BASE(MFGCFG));
	return 3;
}

/* ---- scenarios ---------------------------------------------------------------------------------- */

static int failures;
#define CHECK(condition, ...) do { if (!(condition)) { failures++; fprintf(stderr, "  FAILED: " __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static struct write before[256];
static unsigned before_count;
static uint32_t before_spm[0x1000 / 4];

static void
compare(int original_reached, int table_reached, const char *what)
{
	CHECK(original_reached == table_reached, "%s: the table came to step %d, the driver before it to %d", what,
	    table_reached, original_reached);
	CHECK(write_count == before_count, "%s: %u writes, %u before", what, write_count, before_count);
	for (unsigned i = 0; i < write_count && i < before_count; i++) {
		CHECK(writes[i].block == before[i].block && writes[i].offset == before[i].offset && writes[i].value == before[i].value,
		    "%s: write %u is block %u +%#x = %#x, before it was block %u +%#x = %#x", what, i, writes[i].block,
		    writes[i].offset, writes[i].value, before[i].block, before[i].offset, before[i].value);
	}
	CHECK(memcmp(regs[SPM], before_spm, sizeof(before_spm)) == 0, "%s: the SPM is left otherwise than before", what);
}

static void
run_both(unsigned dead, const char *what)
{
	int reached;

	model_reset();
	if (dead < 4) {
		model_domains[dead].dead = true;
	}
	reached = original_mt6768();
	memcpy(before, writes, sizeof(writes));
	before_count = write_count;
	memcpy(before_spm, regs[SPM], sizeof(before_spm));

	model_reset();
	if (dead < 4) {
		model_domains[dead].dead = true;
	}
	compare(reached, table(mtk_mfg_find("mt6768")), what);
}

static void
mt6768_as_before(void)
{
	run_both(4, "every domain up");
	CHECK(before_count == 28, "the MT6768's bring-up is 28 writes, not %u", before_count);
	CHECK((regs[SPM][0x180 / 4] & 0x7800) == 0x7800, "the four domains say they are on (PWR_STATUS %#x)", regs[SPM][0x180 / 4]);
	CHECK(regs[MFGCFG][0x008 / 4] == 1, "BG3D runs");
}

static void
a_domain_that_stays_off(void)
{
	/* each domain in turn never comes up: the table stops where the driver stopped, with the same writes */
	for (unsigned dead = 0; dead < 4; dead++) {
		char what[32];

		snprintf(what, sizeof(what), "domain %u off", dead);
		run_both(dead, what);
		model_reset();
		model_domains[dead].dead = true;
		const struct mtk_mfg *chip = mtk_mfg_find("mt6768");
		mtk_mfg_clock(chip, BASE(TOPCKGEN));
		const struct mtk_mfg_domain *failed = mtk_mfg_domains_on(chip, BASE(SPM), BASE(INFRACFG));
		CHECK(failed == &chip->domains[dead], "%s: the domain said to have failed is %s", what, failed ? failed->name : "none");
	}
}

static void
a_chip_without_a_table(void)
{
	CHECK(mtk_mfg_find("mt6768") != NULL, "the MT6768 has a table");
	CHECK(mtk_mfg_find("mt6789") == NULL, "a chip nobody wrote has none");
	CHECK(mtk_mfg_find("mt676") == NULL && mtk_mfg_find("mt67680") == NULL, "a name is found whole, not by its start");
	CHECK(mtk_mfg_find("") == NULL && mtk_mfg_find(NULL) == NULL, "no name, no table");
}

static void
every_table_is_whole(void)
{
	for (unsigned i = 0; i < sizeof(mtk_mfg_chips) / sizeof(mtk_mfg_chips[0]); i++) {
		const struct mtk_mfg *chip = &mtk_mfg_chips[i];
		uint32_t seen = 0;

		CHECK(chip->soc != NULL && chip->gpu != NULL, "chip %u has a name and a GPU", i);
		if (chip->power_on != NULL) {
			continue;
		}
		CHECK(chip->domain_count > 0 && chip->domains != NULL, "%s: domains", chip->soc);
		CHECK(chip->mux_clr < 0x1000 && chip->mux_set < 0x1000 && chip->cg_clr < 0x1000 && chip->pwr_status < 0x1000 &&
		    chip->pwr_status_2nd < 0x1000, "%s: every register inside its block's page", chip->soc);
		CHECK(chip->mux_clr_bits != 0 && chip->cg_clr_bits != 0, "%s: the mux and the gate have bits to write", chip->soc);
		for (unsigned k = 0; k < chip->domain_count; k++) {
			const struct mtk_mfg_domain *domain = &chip->domains[k];

			CHECK(domain->con < 0x1000 && domain->con % 4 == 0, "%s %s: PWR_CON inside the SPM's page", chip->soc, domain->name);
			CHECK(domain->status != 0 && (domain->status & (domain->status - 1)) == 0 && !(seen & domain->status),
			    "%s %s: one status bit of its own", chip->soc, domain->name);
			seen |= domain->status;
			CHECK((domain->sram_pdn == 0) == (domain->sram_ack == 0), "%s %s: SRAM bits and their acknowledgement together",
			    chip->soc, domain->name);
			for (unsigned r = 0; r < 2; r++) {
				CHECK(domain->release[r].offset < 0x1000 && (domain->release[r].offset == 0) == (domain->release[r].bits == 0),
				    "%s %s: a bus protection release is a register and its bits", chip->soc, domain->name);
			}
		}
	}
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "mt6768_as_before", mt6768_as_before }, { "a_domain_that_stays_off", a_domain_that_stays_off },
		{ "a_chip_without_a_table", a_chip_without_a_table }, { "every_table_is_whole", every_table_is_whole },
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
			fprintf(stderr, "%-30s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
