/*
 * The GPU's platform on a MediaTek chip: what has to happen before the Mali's own registers answer, which is the
 * chip's and not the GPU's. On every Helio with a Mali it is the same three steps, with the chip's registers:
 *
 *   1  the GPU's clock: the "mfg_sel" multiplexer in topckgen pointed at a source that runs
 *   2  MFG's power domains in the SPM, one after the other, each switched on as MediaTek's scpsys does it (Linux's
 *      drivers/soc/mediatek/mtk-scpsys.c, scpsys_power_on), and the bus protection between the GPU and the rest of
 *      the chip released once the domain behind it has power
 *   3  the block's own clock gate (BG3D in mfgcfg)
 *
 * So a chip is a table, struct mtk_mfg, and mtk_mfg_chips[] below has one for each chip whose numbers are known; a
 * chip whose GPU comes up some other way (a GPU with its own microcontroller, a PMIC rail to raise first) gives a
 * function instead (mtk_mfg.power_on). drivers/board/mtk_mali.c finds the table by hw.board.soc and does nothing on a
 * chip that has none, saying so: there are no defaults for what only the chip's documents can tell. The numbers come
 * from the chip's Android kernel (its scpsys or spm driver's MFG domains, its clock driver's mfg_sel and BG3D, its
 * infracfg bus protection masks) and are checked on the phone with the loader's GPU probe (tools/run/gpu-probe.sh)
 * before the kernel is asked: Documentation/metal-porting.md.
 *
 * This header is the whole of it, for mtk_mali.c and for its host test (workbench/mfg_power_test.c), which includes
 * it with a model of the registers behind rd(), wr() and wait_for(): whoever includes it defines those three, and
 * vm_offset_t, first.
 */
#ifndef MTK_MFG_H
#define MTK_MFG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* a power domain's control register in the SPM (scpsys' PWR_CON) */
#define MFG_PWR_RST_B           (1u << 0)
#define MFG_PWR_ISO             (1u << 1)
#define MFG_PWR_ON              (1u << 2)
#define MFG_PWR_ON_2ND          (1u << 3)
#define MFG_PWR_CLK_DIS         (1u << 4)

/* a write that releases part of the bus protection (infracfg's ..._PROTECTEN_CLR registers) */
struct mtk_mfg_release {
	uint32_t offset, bits;
};

struct mtk_mfg_domain {
	const char *name;
	uint32_t con;                           /* its PWR_CON in the SPM */
	uint32_t status;                        /* its bit in PWR_STATUS and PWR_STATUS_2ND */
	uint32_t sram_pdn, sram_ack;            /* its SRAM's power-down bits in PWR_CON, and their acknowledgement; 0: none */
	struct mtk_mfg_release release[2];      /* written once it is on, in order; offset 0: nothing */
};

struct mtk_mfg {
	const char *soc;                        /* hw.board.soc, the SoC file's name: "mt6768" */
	const char *gpu;                        /* the GPU the chip's vendor names, for the log */
	/* 1: topckgen's mux: these bits cleared through mux_clr, then mux_set_bits written to mux_set */
	uint32_t mux_clr, mux_clr_bits, mux_set, mux_set_bits;
	/* 2: the SPM's status registers, and the domains in the order they are switched on */
	uint32_t pwr_status, pwr_status_2nd;
	const struct mtk_mfg_domain *domains;
	unsigned domain_count;
	/* 3: mfgcfg's clock gate: cg_clr_bits written to cg_clr */
	uint32_t cg_clr, cg_clr_bits;
	/* a chip whose GPU does not come up by the three steps: the whole of it here instead, true when the Mali's
	 * registers answer (the tables above are then not used) */
	bool (*power_on)(vm_offset_t topckgen, vm_offset_t spm, vm_offset_t infracfg, vm_offset_t mfgcfg);
	/* the Mali's job interrupt, the GIC's number of it (an SPI's plus 32; 0: none known, the jobs are polled), and
	 * MediaTek's polarity inverter in front of the GIC (mediatek,mt6577-sysirq: a bit an SPI, set for one that is
	 * active low, as the Mali's are; 0: none) */
	uint32_t job_irq;
	uint64_t intpol;
};

/*
 * The MT6768 (Helio G85; the G80, G70 and P65 are the same chip), as MediaTek's kernel for it brings its Mali-G52 MC2
 * up and the loader's GPU probe proved on the first phone (arch/arm64/boot/mtkmali.h has the same steps with their
 * sources): mfg_sel to syspll_d3 (364 MHz), MFG_ASYNC, MFG, MFG_CORE0 and MFG_CORE1 with the bus protection
 * released after MFG, BG3D. The PMIC's rails VGPU and VSRAM_GPU are left on by the preloader.
 */
static const struct mtk_mfg_domain mt6768_mfg_domains[] = {
	{ "MFG_ASYNC", 0x334, 1u << 14, 0, 0, { { 0, 0 } } },
	{ "MFG", 0x338, 1u << 11, 1u << 8, 1u << 12, { { 0x2a4, (1u << 21) | (1u << 22) }, { 0x2a4, 1u << 25 } } },
	{ "MFG_CORE0", 0x34c, 1u << 12, 1u << 8, 1u << 12, { { 0, 0 } } },
	{ "MFG_CORE1", 0x310, 1u << 13, 1u << 8, 1u << 12, { { 0, 0 } } },
};

static const struct mtk_mfg mtk_mfg_chips[] = {
	{
		.soc = "mt6768", .gpu = "Mali-G52 MC2",
		.mux_clr = 0x58, .mux_clr_bits = 3u | (1u << 7),        /* CLK_CFG_1_CLR: mfg_sel's source and its gate */
		.mux_set = 0x54, .mux_set_bits = 2u,                    /* CLK_CFG_1_SET: syspll_d3 */
		.pwr_status = 0x180, .pwr_status_2nd = 0x184,
		.domains = mt6768_mfg_domains,
		.domain_count = sizeof(mt6768_mfg_domains) / sizeof(mt6768_mfg_domains[0]),
		.cg_clr = 0x008, .cg_clr_bits = 1u,                     /* MFG_CG_CLR: BG3D runs */
		/* the Android tree's mali node: interrupts GPU, MMU, JOB = SPI 274, 275, 276, level low; its
		 * intpol-controller at 0x0c53a650 */
		.job_irq = 32 + 276, .intpol = 0x0c53a650,
	},
};

/* the chip's table, or NULL: a chip whose GPU platform nobody has written */
static inline const struct mtk_mfg *
mtk_mfg_find(const char *soc)
{
	for (unsigned i = 0; soc != NULL && i < sizeof(mtk_mfg_chips) / sizeof(mtk_mfg_chips[0]); i++) {
		const char *a = mtk_mfg_chips[i].soc, *b = soc;

		while (*a != 0 && *a == *b) {
			a++, b++;
		}
		if (*a == 0 && *b == 0) {
			return &mtk_mfg_chips[i];
		}
	}
	return NULL;
}

/* step 1 */
static inline void
mtk_mfg_clock(const struct mtk_mfg *chip, vm_offset_t topckgen)
{
	wr(topckgen, chip->mux_clr, chip->mux_clr_bits);
	wr(topckgen, chip->mux_set, chip->mux_set_bits);
}

/* one domain of step 2, as scpsys switches one on */
static inline bool
mtk_mfg_domain_on(const struct mtk_mfg *chip, const struct mtk_mfg_domain *domain, vm_offset_t spm)
{
	uint32_t con = domain->con, bit = domain->status;

	wr(spm, con, rd(spm, con) | MFG_PWR_ON);
	wr(spm, con, rd(spm, con) | MFG_PWR_ON_2ND);
	if (!wait_for(spm, chip->pwr_status, bit, bit, 20) || !wait_for(spm, chip->pwr_status_2nd, bit, bit, 20)) {
		return false;
	}
	wr(spm, con, rd(spm, con) & ~MFG_PWR_CLK_DIS);
	wr(spm, con, rd(spm, con) & ~MFG_PWR_ISO);
	wr(spm, con, rd(spm, con) | MFG_PWR_RST_B);
	if (domain->sram_pdn != 0) {
		wr(spm, con, rd(spm, con) & ~domain->sram_pdn);
		return wait_for(spm, con, domain->sram_ack, 0, 20);
	}
	return true;
}

/* step 2: the domains in order, each one's bus protection released after it; the one that did not come up, or NULL */
static inline const struct mtk_mfg_domain *
mtk_mfg_domains_on(const struct mtk_mfg *chip, vm_offset_t spm, vm_offset_t infracfg)
{
	for (unsigned i = 0; i < chip->domain_count; i++) {
		const struct mtk_mfg_domain *domain = &chip->domains[i];

		if (!mtk_mfg_domain_on(chip, domain, spm)) {
			return domain;
		}
		for (unsigned k = 0; k < 2 && domain->release[k].offset != 0; k++) {
			wr(infracfg, domain->release[k].offset, domain->release[k].bits);
		}
	}
	return NULL;
}

/* step 3 */
static inline void
mtk_mfg_gate(const struct mtk_mfg *chip, vm_offset_t mfgcfg)
{
	wr(mfgcfg, chip->cg_clr, chip->cg_clr_bits);
}

#endif /* MTK_MFG_H */
