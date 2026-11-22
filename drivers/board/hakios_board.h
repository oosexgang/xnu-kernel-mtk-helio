/*
 * The board this kernel runs on, as data.
 *
 * One kernel serves every board. Which one it is, the loader says (/chosen/darwin-board); what that
 * board has and where, the repository's arch/arm64/boot/dts/NAME/board.toml and arch/arm64/boot/dts/mediatek/SOC.toml say, and
 * tools/boards.py turns them into the table of hakios_board_table.h. A driver asks here instead of
 * carrying its own addresses:
 *
 *	if (!hakios_board_wants(HAKIOS_HAS_GPU_MALI_G52, "mali=1")) {
 *		return;                 // this board has no such thing (or is not known at all)
 *	}
 *	mali = ml_io_map(hakios_board()->blocks.mali, 0x4000);
 *
 * A board that is not in the table (a new port's first boots, a loader that names none) gets NULL and
 * no feature: nothing of a phone is touched on a machine that was not described. Adding a device is a
 * directory under arch/arm64/boot/dts/ (Documentation/porting.md), not a change to the drivers.
 */
#ifndef _HAKIOS_BOARD_H
#define _HAKIOS_BOARD_H

#include <stdbool.h>
#include <stdint.h>

/*
 * What a board can have a driver started for. The names in board.toml's `features` are these in
 * lower case with dashes ("gpu-mali-g52"); tools/boards.py reads this list, so a new one is added
 * here and nowhere else.
 */
enum {
	HAKIOS_HAS_EMMC_MSDC        = 1u << 0,  /* the root on an eMMC behind MSDC0, as LK left the card (emmc_root=1) */
	HAKIOS_HAS_KEYS_MT6358      = 1u << 1,  /* power and volume keys: the keypad block and the MT6358's key detector through the
	                                         * PMIC wrapper; the cores' and the GPU's clocks beside them (buttons=2) */
	HAKIOS_HAS_GPU_MALI_G52     = 1u << 2,  /* a Mali-G52 in MFG's power domains (mali=1) */
	HAKIOS_HAS_SCP_SENSORS      = 1u << 3,  /* the sensor hub (SCP) and its accelerometer (scp_step=N) */
	HAKIOS_HAS_BATTERY_MT6358   = 1u << 4,  /* the MT6358's fuel gauge and BATADC; the cell is the board's [battery] (mtkbatt=1) */
	HAKIOS_HAS_DISPLAY_OVL_FLIP = 1u << 5,  /* frames shown by pointing OVL0's layer 0 at a second picture inside LK's
	                                         * framebuffer (MiniIOMobileFramebuffer.cpp, fb_flip=1) */
	/*
	 * Drivers of one part, wired the way one phone has it (the pins, the bus, the chip): a board lists
	 * one only if it has that part wired that way. Asked for on any other board, the driver says so and
	 * does nothing, rather than write to another phone's pins.
	 */
	HAKIOS_HAS_TOUCH_TD4160     = 1u << 8,  /* an OmniVision TD4160 (TouchComm) on SPI2, its pins bit-banged (touchscreen=2) */
	HAKIOS_HAS_CONNSYS_MT6631   = 1u << 9,  /* CONNSYS with an MT6631: Wi-Fi and Bluetooth (connsys_step=N) */
	HAKIOS_HAS_CAMERA_S5KJN1    = 1u << 10, /* a Samsung S5K JN1 on CSI through SENINF and CAMSV (mtkcam=1) */
};

struct hakios_board {
	const char *name;                       /* /chosen/darwin-board */
	const char *description;
	const char *soc;
	uint32_t features;                      /* HAKIOS_HAS_* */
	const char *system_partition;           /* the GPT partition with the system image (board: [loader]); NULL: none */

	/* the machine (soc: [platform]) */
	struct {
		uint64_t gic_dist, gic_redist;
		uint64_t uart0;
	} platform;

	/* where the chip's blocks are (soc: [blocks]); 0: the SoC file does not say */
	struct {
		uint64_t topckgen, infracfg_ao, gpio, spm, toprgu, apmixed, pwrap, keypad;
		uint64_t scp_sram, scp_cfg, scp_clkctrl;
		uint64_t ap_dma_btif_tx, i2c2, btif, i2c4, spi2, msdc0, msdc1, mipi_rx;
		uint64_t mfgcfg, mali;
		uint64_t mmsys, disp_mutex0, smi_common, disp_ovl0;
		uint64_t conn, conn_mcu, conn_hif_on, conn_top_misc_off, conn_wbg, conn_misc_on;
		uint64_t camsys, smi_larb3, seninf, camsv0, camsv1;
	} blocks;

	/* the panel and the framebuffer LK leaves on it (board: [display]) */
	struct {
		uint64_t fb_base, fb_size;
		uint32_t fb_pitch;              /* bytes a line */
		uint32_t width, height;         /* the panel's pixels */
	} display;

	/* the cell (board: [battery]) */
	struct {
		uint32_t design_uah;
	} battery;

	/* CONNSYS's memory outside the kernel's (board: [connsys]): what Android's kernel reserved for it */
	struct {
		uint64_t emi;                   /* consys-reserve-memory: the firmware's own */
		uint32_t emi_size;
		uint64_t wifi_dma;              /* wifi-reserve-memory: the Wi-Fi's DMA and the BTIF's transmit FIFO */
		uint32_t wifi_dma_size;
	} connsys;
};

/* The row for the board the loader named, or NULL. */
const struct hakios_board *hakios_board(void);
/* Has this board the feature (HAKIOS_HAS_*)? False on a board that is not known. */
bool hakios_board_has(uint32_t feature);
/* hakios_board_has(), and when it has not, a line that names the feature the board file would have to
 * list: for a driver whose boot argument asked for it. */
bool hakios_board_wants(uint32_t feature, const char *driver);
void hakios_board_init(void);

#endif /* _HAKIOS_BOARD_H */
