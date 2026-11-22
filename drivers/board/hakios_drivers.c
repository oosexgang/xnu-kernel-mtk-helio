/*
 * Where the kernel's drivers are started: one list, in the order they start.
 *
 * bsd_init() calls hakios_drivers_init() once, when /dev is there. Every driver's start function is
 * called from here and from nowhere else, whatever the board: a driver decides for itself whether it
 * has anything to do -- its boot argument asks for it, the board lists its feature
 * (hakios_board_has()), the SoC file says where its blocks are -- and returns at once if not. So the
 * list has no conditions but one: which touch controller's driver feeds /dev/hakios-touch, since a
 * board has one.
 *
 * A new driver is one more line here, after what it needs: the board first, then the processor, then
 * what a person touches and sees, then what talks to the other processors.
 */
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <sys/hakios_apfs_check.h>
#include <sys/hakios_audio.h>
#include <sys/hakios_input.h>
#include <sys/mtk_emmc.h>
#include <sys/hakios_sparse.h>
#include <sys/hakios_virtio_blk.h>

#include "hakios_board.h"

extern void hakios_prof_start(void);            /* osfmk/arm64/hakios_prof.c */

/*
 * Every driver, started once, in this order (the MediaTek phones: the order the 16.5 kernel's
 * hakios_input_init() kept). Each does nothing unless its boot argument asks and its board lists it.
 */
void
hakios_drivers_init(void)
{
	/* what everything else reads */
	hakios_input_init();           /* /dev/hakios-touch: the queue every source of touches and keys feeds */
	hakios_board_init();            /* which board this is and what it has, in the log (hakios_board.c) */

	/* what a person touches and sees */
	hakios_virtio_input_init();    /* boot-arg virtio_input=1: QEMU's pointer and keyboard */
	mtk_tcm_init();                 /* boot-arg touchscreen=2: the TD4160 touch screen (feature touch-td4160) */
	mtk_keys_init();                /* boot-arg buttons=2: the power and volume keys (keys-mt6358) */
	hakios_prof_start();            /* boot-arg hakios_prof=NAME: a sampling profiler */
	hakios_net_init();             /* en0, if something carries its frames */
	mtk_mali_init();                /* boot-arg mali=1: the Mali-G52 brought up */

	/* what talks to the other processors */
	mtk_connsys_init();             /* boot-arg connsys_step=N: CONNSYS, Wi-Fi and Bluetooth */
	mtk_scp_init();                 /* boot-arg scp_step=N: the sensor hub */
	mtk_camera_init();              /* boot-arg mtkcam=1: the main camera */
	mtk_battery_init();             /* boot-arg mtkbatt=1: the battery from the PMIC */

	/* sound */
	hakios_audio_init();           /* /dev/hakios-audio: what the system plays, to a sink */
	hakios_virtio_sound_init();    /* boot-arg virtio_sound=1: QEMU's sound device */

	/* storage */
	mtk_emmc_devfs();               /* /dev/emmc0: the system image on the eMMC (emmc_root=1) */
	hakios_virtio_blk_init();      /* /dev/rvblk0: a disk image of the host (QEMU) */
	hakios_sparse_init();          /* /dev/rsp0: part of an image too big to hold */
#if APFS
	hakios_apfs_check_init();      /* mounts and walks /dev/rvblk0 once userland has /private/var up */
#endif
}
