/*
 * The first phone's eMMC as a block device: the "xnusys" partition, where the system image lives, read
 * when the kernel asks for it rather than held in memory. Boot-arg emmc_root=1 makes it the root
 * (iokit/bsddev/IOKitBSDInit.cpp asks mtk_emmc_root for it); /dev/emmc0 and /dev/remmc0 are its
 * nodes once devfs is up.
 *
 * Until now the loader read the whole image -- 2.4 GB of it -- into DRAM as the RAM disk md0,
 * in the 4 GB the kernel has: 2.5 GB wired before the first program ran, the rest too little for
 * The system with its wallpaper (run 320: 10-26 MB free, the system "unhealthy", jetsam at work), and
 * three and a half minutes of every boot spent reading it (11 MB/s).
 *
 * Nothing is set up here: LK has powered the card, set its bus up and left it in the transfer
 * state, and mtkloader has read the GPT through it the same way (mtkloader/msdc.h) -- MSDC0 in PIO
 * mode, plain block reads (CMD17, or CMD18 then CMD12). Registers as Linux's
 * drivers/mmc/host/mtk-sd.c has them.
 *
 * Read only, by construction: the strategy routine refuses writes and opening for writing is
 * refused. One request at a time, under a mutex, in pieces of EMMC_MAX_BLOCKS.
 *
 * DMA (boot-arg emmc_dma=0: PIO only). In PIO every 32-bit word of the card's data is a register read of the
 * controller's FIFO -- about 11 MB/s, and the processor busy for all of it: an application opening read 15 to 40 MB of
 * the system (page faults of ~28 KB), and the window server's own faults waited behind them (run 599: 6 to 25 MB in
 * 3 s while the frames came at 2 to 4 a second, the processors and the GPU mostly idle). The controller's descriptor
 * DMA, as Linux's drivers/mmc/host/mtk-sd.c drives it: one GPD (a second, empty, as the end of the chain) pointing at
 * a chain of BDs, each a piece of a bounce buffer below 4 GB, descriptors checksummed (MSDC_DMA_CFG DECSEN); the
 * command sent, DMA started at its response, the transfer's end waited for, DMA stopped. The bounce buffer is cleaned
 * and invalidated around it and copied out. At the mount the first 128 KB of the partition are read both ways and
 * compared: a difference, or three failed DMA reads later, and the driver says so and stays with PIO.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/mtk_emmc.h>
#include "hakios_board.h"
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <mach/vm_param.h>
#include <vm/vm_kern.h>
#include <vm/vm_kern_xnu.h>

extern uint64_t kvtophys(vm_offset_t va);
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);
extern void hakios_host_keep_add(const char *format, ...) __printflike(1, 2);

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define MSDC0_PHYS              ((vm_offset_t)hakios_board()->blocks.msdc0)
#define EMMC_PART        (hakios_board()->system_partition)      /* the board file's [loader] system_partition */
#define EMMC_MAX_BLOCKS  256                /* 128 KB a command */

#define MSDC_CFG         0x00
#define MSDC_CFG_PIO     (1u << 3)
#define MSDC_INT         0x0c
#define MSDC_FIFOCS      0x14
#define MSDC_FIFOCS_CLR  (1u << 31)
#define MSDC_RXDATA      0x1c
#define SDC_CMD          0x34
#define SDC_ARG          0x38
#define SDC_STS          0x3c
#define SDC_STS_BUSY     3u                 /* SDCBUSY, CMDBUSY */
#define SDC_BLK_NUM      0x50

#define INT_CMDRDY       (1u << 8)
#define INT_CMDTMO       (1u << 9)
#define INT_RSPCRCERR    (1u << 10)
#define INT_XFER_COMPL   (1u << 12)
#define INT_DATTMO       (1u << 14)
#define INT_DATCRCERR    (1u << 15)
#define INT_BDCSERR      (1u << 17)
#define INT_GPDCSERR     (1u << 18)
#define INT_DMAPROTECT   (1u << 19)

#define DMA_SA_H4BIT     0x8c
#define MSDC_DMA_SA      0x90
#define MSDC_DMA_CTRL    0x98
#define DMA_CTRL_START   (1u << 0)
#define DMA_CTRL_STOP    (1u << 1)
#define DMA_CTRL_MODE    (1u << 8)          /* descriptor DMA */
#define DMA_CTRL_BURST   (7u << 12)
#define DMA_BURST_64B    (6u << 12)
#define MSDC_DMA_CFG     0x9c
#define DMA_CFG_STS      (1u << 0)          /* running */
#define DMA_CFG_DECSEN   (1u << 1)          /* descriptors checksummed */

#define GPD_HWO          (1u << 0)
#define GPD_BDP          (1u << 1)
#define BD_EOL           (1u << 0)
#define BD_PIECE         (32u * 1024)       /* a BD's length is 16 bits (the 24-bit form only on some chips) */
#define BOUNCE_SIZE      (EMMC_MAX_BLOCKS * 512u)

#define CMD_RSP_R1       (1u << 7)
#define CMD_RSP_R1B      (6u << 7)
#define CMD_DTYPE_SINGLE (1u << 11)
#define CMD_DTYPE_MULTI  (2u << 11)
#define CMD_STOP         (1u << 14)
#define CMD_BLKLEN(n)    (((n) & 0xfffu) << 16)

static vm_offset_t msdc;
static uint64_t emmc_lba, emmc_blocks;      /* the partition: its first block, and the image's length in blocks */
static int emmc_bmajor = -1, emmc_cmajor = -1;
static uint32_t emmc_int_seen;              /* MSDC_INT bits read (reading clears them) but not yet used */
static const char *emmc_failed;
static uint32_t emmc_failed_status;
static uint64_t emmc_reads, emmc_read_blocks, emmc_errors;
static bool emmc_dma;                       /* the DMA path checked at the mount and in use */
static unsigned emmc_dma_failures;
static uint64_t emmc_dma_reads;
static LCK_GRP_DECLARE(emmc_lck_grp, "hakios-emmc");
static LCK_MTX_DECLARE(emmc_lock, &emmc_lck_grp);

static inline uint32_t
rd(uint32_t reg)
{
	return *(volatile uint32_t *)(msdc + reg);
}

static inline void
wr(uint32_t reg, uint32_t value)
{
	*(volatile uint32_t *)(msdc + reg) = value;
}

static bool
fail(const char *what, uint32_t status)
{
	emmc_failed = what;
	emmc_failed_status = status;
	return false;
}

/* (spins are counted in register reads, as the loader does: about a microsecond each, give or take) */
static bool
wait_idle(void)
{
	for (uint32_t i = 0; i < 2000000; i++) {
		if (!(rd(SDC_STS) & SDC_STS_BUSY)) {
			return true;
		}
	}
	return fail("controller busy", rd(SDC_STS));
}

static bool
wait_int(uint32_t want, uint32_t bad, const char *what)
{
	for (uint32_t i = 0; i < 20000000; i++) {
		uint32_t status = rd(MSDC_INT);

		if (status) {
			wr(MSDC_INT, status);
			emmc_int_seen |= status;
		}
		if (emmc_int_seen & bad) {
			status = emmc_int_seen;
			emmc_int_seen = 0;
			return fail(what, status);
		}
		if (emmc_int_seen & want) {
			emmc_int_seen &= ~want;
			return true;
		}
	}
	return fail(what, emmc_int_seen);
}

static void
pio_mode(void)
{
	wr(MSDC_CFG, rd(MSDC_CFG) | MSDC_CFG_PIO);
	wr(MSDC_INT, rd(MSDC_INT));
	emmc_int_seen = 0;
	wr(MSDC_FIFOCS, rd(MSDC_FIFOCS) | MSDC_FIFOCS_CLR);
	for (uint32_t i = 0; i < 100000 && (rd(MSDC_FIFOCS) & MSDC_FIFOCS_CLR); i++) {
	}
}

static bool
stop(void)
{
	wr(SDC_ARG, 0);
	wr(SDC_CMD, 12 | CMD_RSP_R1B | CMD_STOP);
	return wait_int(INT_CMDRDY, INT_CMDTMO | INT_RSPCRCERR, "stop") && wait_idle();
}

/* blocks of 512 bytes from the card's block lba into dst (4-aligned) */
static bool
read_blocks(uint64_t lba, uint32_t blocks, uint32_t *dst)
{
	if (!wait_idle()) {
		return false;
	}
	pio_mode();
	wr(SDC_BLK_NUM, blocks);
	wr(SDC_ARG, (uint32_t)lba);                 /* high capacity: the unit is a block */
	wr(SDC_CMD, (blocks == 1 ? 17 : 18) | CMD_RSP_R1 | (blocks == 1 ? CMD_DTYPE_SINGLE : CMD_DTYPE_MULTI) | CMD_BLKLEN(512));
	if (!wait_int(INT_CMDRDY, INT_CMDTMO | INT_RSPCRCERR, "command")) {
		return false;
	}
	for (uint32_t left = blocks * 512 / 4; left;) {
		uint32_t fifo = rd(MSDC_FIFOCS) & 0xff, spin = 0, words;

		while (fifo < 4) {
			uint32_t status = rd(MSDC_INT);

			if (status & (INT_DATTMO | INT_DATCRCERR)) {
				wr(MSDC_INT, status);
				if (blocks > 1) {
					(void)stop();
				}
				return fail("data", status);
			}
			if (++spin > 20000000) {
				return fail("data stalled", rd(MSDC_FIFOCS));
			}
			fifo = rd(MSDC_FIFOCS) & 0xff;
		}
		words = fifo / 4 < left ? fifo / 4 : left;
		for (uint32_t i = 0; i < words; i++) {
			*dst++ = rd(MSDC_RXDATA);
		}
		left -= words;
	}
	if (!wait_int(INT_XFER_COMPL, INT_DATTMO | INT_DATCRCERR, "transfer end")) {
		return false;
	}
	return blocks == 1 || stop();
}

/* ---- DMA ---- */

struct gpd {
	uint32_t info, next, ptr, len, arg, blknum, cmd;
};

struct bd {
	uint32_t info, next, ptr, len;
};

static vm_offset_t dma_desc, dma_bounce;    /* a page of descriptors (two GPDs, then the BDs); the bounce buffer */
static uint64_t dma_desc_phys, dma_bounce_phys;

static uint32_t
desc_checksum(const void *desc)
{
	const uint8_t *b = desc;
	uint32_t sum = 0;

	for (unsigned i = 0; i < 16; i++) {
		sum += b[i];
	}
	return (0xffu - (sum & 0xffu)) & 0xffu;
}

/* below 4 GB (the descriptors' addresses are 32 bits; DMA_SA_H4BIT and the descriptors' high nibbles left 0) */
static bool
dma_alloc(void)
{
	const ppnum_t below_4g = (ppnum_t)(atop(0x100000000ull) - 1);

	if (kmem_alloc_contig(kernel_map, &dma_desc, PAGE_SIZE, PAGE_MASK, below_4g, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS ||
	    kmem_alloc_contig(kernel_map, &dma_bounce, BOUNCE_SIZE, PAGE_MASK, below_4g, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return false;
	}
	dma_desc_phys = kvtophys(dma_desc);
	dma_bounce_phys = kvtophys(dma_bounce);
	return dma_desc_phys != 0 && dma_bounce_phys != 0 && dma_desc_phys + PAGE_SIZE <= 0x100000000ull &&
	       dma_bounce_phys + BOUNCE_SIZE <= 0x100000000ull;
}

/* the chain for `bytes` into the bounce buffer: GPD 0 (HWO, BD pointer) -> its BDs, GPD 1 the empty end */
static void
dma_chain(uint32_t bytes)
{
	struct gpd *gpd = (struct gpd *)dma_desc;
	struct bd *bd = (struct bd *)(dma_desc + 128);   /* (the GPDs take 56 bytes; the BDs on 16 bytes after them) */
	const uint64_t bd_phys = dma_desc_phys + 128;
	unsigned n = (bytes + BD_PIECE - 1) / BD_PIECE;

	bzero((void *)dma_desc, PAGE_SIZE);
	for (unsigned i = 0; i < n; i++) {
		uint32_t len = bytes - i * BD_PIECE < BD_PIECE ? bytes - i * BD_PIECE : BD_PIECE;

		bd[i].ptr = (uint32_t)(dma_bounce_phys + (uint64_t)i * BD_PIECE);
		bd[i].len = len;
		bd[i].next = i + 1 < n ? (uint32_t)(bd_phys + (i + 1) * sizeof(struct bd)) : 0;
		bd[i].info = i + 1 == n ? BD_EOL : 0;
		bd[i].info |= desc_checksum(&bd[i]) << 8;
	}
	gpd[0].next = (uint32_t)(dma_desc_phys + sizeof(struct gpd));
	gpd[0].ptr = (uint32_t)bd_phys;
	gpd[0].info = GPD_HWO | GPD_BDP;
	gpd[0].info |= desc_checksum(&gpd[0]) << 8;
	FlushPoC_DcacheRegion(dma_desc, PAGE_SIZE);
	FlushPoC_DcacheRegion(dma_bounce, BOUNCE_SIZE);
}

static bool
dma_stop(void)
{
	wr(MSDC_DMA_CTRL, rd(MSDC_DMA_CTRL) | DMA_CTRL_STOP);
	for (uint32_t i = 0; i < 2000000; i++) {
		if (!(rd(MSDC_DMA_CFG) & DMA_CFG_STS)) {
			return true;
		}
	}
	return fail("DMA would not stop", rd(MSDC_DMA_CFG));
}

/* blocks of 512 bytes from the card's block lba into dst, by DMA through the bounce buffer (blocks <= EMMC_MAX_BLOCKS) */
static bool
read_blocks_dma(uint64_t lba, uint32_t blocks, void *dst)
{
	const uint32_t errors = INT_DATTMO | INT_DATCRCERR | INT_BDCSERR | INT_GPDCSERR | INT_DMAPROTECT;

	if (!wait_idle()) {
		return false;
	}
	/* the controller out of PIO, its interrupts and FIFO clear */
	wr(MSDC_CFG, rd(MSDC_CFG) & ~MSDC_CFG_PIO);
	wr(MSDC_INT, rd(MSDC_INT));
	emmc_int_seen = 0;
	wr(MSDC_FIFOCS, rd(MSDC_FIFOCS) | MSDC_FIFOCS_CLR);
	for (uint32_t i = 0; i < 100000 && (rd(MSDC_FIFOCS) & MSDC_FIFOCS_CLR); i++) {
	}
	dma_chain(blocks * 512);
	wr(MSDC_DMA_CFG, rd(MSDC_DMA_CFG) | DMA_CFG_DECSEN);
	wr(MSDC_DMA_CTRL, (rd(MSDC_DMA_CTRL) & ~(DMA_CTRL_BURST | DMA_CTRL_MODE)) | DMA_BURST_64B | DMA_CTRL_MODE);
	wr(DMA_SA_H4BIT, 0);
	wr(MSDC_DMA_SA, (uint32_t)dma_desc_phys);
	__builtin_arm_dsb(0xf);

	wr(SDC_BLK_NUM, blocks);
	wr(SDC_ARG, (uint32_t)lba);
	wr(SDC_CMD, (blocks == 1 ? 17 : 18) | CMD_RSP_R1 | (blocks == 1 ? CMD_DTYPE_SINGLE : CMD_DTYPE_MULTI) | CMD_BLKLEN(512));
	if (!wait_int(INT_CMDRDY, INT_CMDTMO | INT_RSPCRCERR, "command")) {
		return false;
	}
	wr(MSDC_DMA_CTRL, rd(MSDC_DMA_CTRL) | DMA_CTRL_START);
	if (!wait_int(INT_XFER_COMPL, errors, "DMA transfer")) {
		(void)dma_stop();
		if (blocks > 1) {
			(void)stop();
		}
		return false;
	}
	if (!dma_stop()) {
		return false;
	}
	if (blocks > 1 && !stop()) {
		return false;
	}
	FlushPoC_DcacheRegion(dma_bounce, blocks * 512);
	memcpy(dst, (const void *)dma_bounce, blocks * 512);
	emmc_dma_reads++;
	return true;
}

/* the same, tried again once after a failure (the controller cleared and the card told to stop) */
static bool
read_retrying(uint64_t lba, uint32_t blocks, uint32_t *dst)
{
	if (emmc_dma && blocks <= EMMC_MAX_BLOCKS) {
		if (read_blocks_dma(lba, blocks, dst)) {
			return true;
		}
		emmc_errors++;
		printf("emmc: DMA of %u blocks at %llu: %s (%#x); by PIO\n", blocks, lba, emmc_failed, emmc_failed_status);
		hakios_host_keep_add("emmc: DMA of %u blocks at %llu: %s (%#x); by PIO", blocks, lba, emmc_failed,
		    emmc_failed_status);
		if (++emmc_dma_failures >= 3) {
			emmc_dma = false;
			hakios_host_keep_add("emmc: DMA failed 3 times: PIO from now on");
		}
		pio_mode();
		(void)stop();
	}
	if (read_blocks(lba, blocks, dst)) {
		return true;
	}
	emmc_errors++;
	printf("emmc: reading %u blocks at %llu: %s (%#x); trying again\n", blocks, lba, emmc_failed, emmc_failed_status);
	pio_mode();
	(void)stop();
	return read_blocks(lba, blocks, dst);
}

/* ---- the partition ---- */

static uint32_t sector[128] __attribute__((aligned(16)));
static uint32_t entries[128 * 128 / 4] __attribute__((aligned(16)));

static bool
find_partition(void)
{
	const uint8_t *s = (const uint8_t *)sector;
	uint64_t entries_lba, at;
	uint32_t count, size;

	if (!read_retrying(1, 1, sector) || memcmp(s, "EFI PART", 8) != 0) {
		printf("emmc: no GPT (%s)\n", emmc_failed ? emmc_failed : "no signature");
		return false;
	}
	memcpy(&entries_lba, s + 72, 8);
	memcpy(&count, s + 80, 4);
	memcpy(&size, s + 84, 4);
	if (count > 128 || size != 128 || !read_retrying(entries_lba, count * size / 512, entries)) {
		printf("emmc: the GPT's entries: %u of %u bytes\n", count, size);
		return false;
	}
	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *e = (const uint8_t *)entries + i * size;
		size_t k;
		uint64_t first, last;

		for (k = 0; EMMC_PART[k] && e[56 + 2 * k] == (uint8_t)EMMC_PART[k] && e[57 + 2 * k] == 0; k++) {
		}
		if (EMMC_PART[k] != 0 || e[56 + 2 * k] != 0 || e[57 + 2 * k] != 0) {
			continue;
		}
		memcpy(&first, e + 32, 8);
		memcpy(&last, e + 40, 8);
		if (first == 0 || last < first) {
			return false;
		}
		emmc_lba = first;
		emmc_blocks = last - first + 1;
		break;
	}
	if (emmc_lba == 0) {
		printf("emmc: no %s partition\n", EMMC_PART);
		return false;
	}
	/* as long as its APFS containers, back to back, as the loader measured it for md0 */
	for (at = 0; at < emmc_blocks * 512;) {
		uint32_t bs;
		uint64_t n;

		if (!read_retrying(emmc_lba + at / 512, 1, sector) || sector[8] != 0x4253584eu) {   /* "NXSB" */
			break;
		}
		bs = sector[9];
		memcpy(&n, s + 40, 8);
		if (bs < 512 || bs > 65536 || n == 0 || n * bs > emmc_blocks * 512 - at) {
			break;
		}
		at += n * bs;
	}
	if (at == 0) {
		printf("emmc: no APFS container in %s (tools/system-flash.sh writes it)\n", EMMC_PART);
		return false;
	}
	printf("emmc: %s at block %llu, %llu MB of %llu MB in its APFS containers\n", EMMC_PART, emmc_lba, at >> 20,
	    (emmc_blocks * 512) >> 20);
	emmc_blocks = at / 512;
	return true;
}

/* ---- the devices ---- */

static int
emmc_open(__unused dev_t dev, int flags, __unused int devtype, __unused proc_t p)
{
	return (flags & FWRITE) ? EROFS : 0;
}

static int
emmc_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	return 0;
}

static void
emmc_strategy(buf_t bp)
{
	uint64_t block = (uint64_t)buf_blkno(bp);
	uint32_t count = buf_count(bp), done = 0;
	uint8_t *data = (uint8_t *)buf_dataptr(bp);
	int error = 0;

	if (!(buf_flags(bp) & B_READ)) {
		error = EROFS;                      /* this device never writes */
		goto out;
	}
	if (block >= emmc_blocks) {
		buf_setresid(bp, count);
		error = block == emmc_blocks ? 0 : EINVAL;
		goto out;
	}
	if (count > (emmc_blocks - block) * 512) {
		count = (uint32_t)((emmc_blocks - block) * 512);
	}
	lck_mtx_lock(&emmc_lock);
	while (done < count && error == 0) {
		uint32_t blocks = (count - done) / 512;

		if (blocks == 0 || ((uintptr_t)(data + done) & 3)) {
			/* a part block, or a buffer not 4-aligned: one block at a time through the sector buffer */
			uint32_t n = count - done < 512 ? count - done : 512;

			if (!read_retrying(emmc_lba + block + done / 512, 1, sector)) {
				error = EIO;
				break;
			}
			memcpy(data + done, sector, n);
			done += n;
			continue;
		}
		if (blocks > EMMC_MAX_BLOCKS) {
			blocks = EMMC_MAX_BLOCKS;
		}
		if (!read_retrying(emmc_lba + block + done / 512, blocks, (uint32_t *)(void *)(data + done))) {
			error = EIO;
			break;
		}
		emmc_reads++;
		emmc_read_blocks += blocks;
		done += blocks * 512;
	}
	lck_mtx_unlock(&emmc_lock);
	buf_setresid(bp, buf_count(bp) - done);
	if (error) {
		printf("emmc: block %llu, %u bytes: %s (%#x)\n", block, count, emmc_failed, emmc_failed_status);
	}
out:
	if (error) {
		buf_seterror(bp, error);
	}
	buf_biodone(bp);
}

static int
emmc_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int fflag, __unused proc_t p)
{
	switch (cmd) {
	case DKIOCGETBLOCKSIZE:
		*(uint32_t *)data = 512;
		return 0;
	case DKIOCSETBLOCKSIZE:
		/* (spec_open sets 512 and back again, and takes a refusal for a broken device) */
		return *(uint32_t *)data == 512 ? 0 : EINVAL;
	case DKIOCGETBLOCKCOUNT:
		*(uint64_t *)data = emmc_blocks;
		return 0;
	case DKIOCISWRITABLE:
		*(uint32_t *)data = 0;
		return 0;
	case DKIOCGETMAXBLOCKCOUNTREAD:
		*(uint64_t *)data = EMMC_MAX_BLOCKS;
		return 0;
	default:
		return ENOTTY;
	}
}

static int
emmc_size(__unused dev_t dev)
{
	return 512;
}

static int
emmc_read(dev_t dev, struct uio *uio, __unused int ioflag)
{
	return physio(emmc_strategy, NULL, dev, B_READ, (u_int (*)(buf_t))emmc_size, uio, 512);
}

static const struct bdevsw emmc_bdevsw = {
	.d_open     = emmc_open,
	.d_close    = emmc_close,
	.d_strategy = emmc_strategy,
	.d_ioctl    = emmc_ioctl,
	.d_dump     = eno_dump,
	.d_psize    = emmc_size,
	.d_type     = D_DISK,
};

static const struct cdevsw emmc_cdevsw = {
	.d_open       = emmc_open,
	.d_close      = emmc_close,
	.d_read       = emmc_read,
	.d_write      = eno_rdwrt,
	.d_ioctl      = emmc_ioctl,
	.d_stop       = eno_stop,
	.d_reset      = eno_reset,
	.d_ttys       = NULL,
	.d_select     = eno_select,
	.d_mmap       = eno_mmap,
	.d_strategy   = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type       = D_DISK,
};

int
mtk_emmc_root(dev_t *root)
{
	uint64_t t0 = mach_absolute_time(), ns;
	uint32_t *probe;

	if (emmc_bmajor >= 0) {
		*root = makedev(emmc_bmajor, 0);
		return 0;
	}
	if (!hakios_board_wants(HAKIOS_HAS_EMMC_MSDC, "emmc_root=1")) {
		return ENXIO;
	}
	msdc = ml_io_map(MSDC0_PHYS, 0x1000);
	if (!find_partition()) {
		return ENXIO;
	}
	/* how fast: the first 4 MB, which the root's mount reads anyway */
	probe = kalloc_data(EMMC_MAX_BLOCKS * 512, Z_WAITOK);
	if (probe != NULL) {
		uint32_t pio_ms, dma_on = 1;

		for (uint32_t at = 0; at < 4 * 2048 && at + EMMC_MAX_BLOCKS <= emmc_blocks; at += EMMC_MAX_BLOCKS) {
			if (!read_retrying(emmc_lba + at, EMMC_MAX_BLOCKS, probe)) {
				break;
			}
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
		pio_ms = (uint32_t)(ns / 1000000);
		printf("emmc: 4 MB in %u ms by PIO\n", pio_ms);
		/* DMA: the same 128 KB both ways, compared, then 4 MB timed */
		PE_parse_boot_argn("emmc_dma", &dma_on, sizeof(dma_on));
		uint8_t *by_dma = dma_on ? kalloc_data(EMMC_MAX_BLOCKS * 512, Z_WAITOK) : NULL;
		if (by_dma != NULL && dma_alloc() && read_blocks(emmc_lba, EMMC_MAX_BLOCKS, probe) &&
		    read_blocks_dma(emmc_lba, EMMC_MAX_BLOCKS, by_dma)) {
			if (memcmp(probe, by_dma, EMMC_MAX_BLOCKS * 512) == 0) {
				uint64_t t1 = mach_absolute_time();

				emmc_dma = true;
				for (uint32_t at = 0; at < 4 * 2048 && at + EMMC_MAX_BLOCKS <= emmc_blocks; at += EMMC_MAX_BLOCKS) {
					if (!read_retrying(emmc_lba + at, EMMC_MAX_BLOCKS, probe)) {
						break;
					}
				}
				absolutetime_to_nanoseconds(mach_absolute_time() - t1, &ns);
				hakios_host_keep_add("emmc: DMA on: 128 KB the same as by PIO; 4 MB in %llu ms (PIO %u ms); "
				    "descriptors at %#llx, bounce at %#llx", ns / 1000000, pio_ms, dma_desc_phys, dma_bounce_phys);
			} else {
				hakios_host_keep_add("emmc: DMA read 128 KB other than PIO did: PIO only");
			}
		} else if (dma_on) {
			pio_mode();
			(void)stop();
			hakios_host_keep_add("emmc: no DMA (%s, %#x): PIO, 4 MB in %u ms", emmc_failed ? emmc_failed : "no memory",
			    emmc_failed_status, pio_ms);
		}
		if (!emmc_dma) {
			pio_mode();
		}
		if (by_dma != NULL) {
			kfree_data(by_dma, EMMC_MAX_BLOCKS * 512);
		}
		kfree_data(probe, EMMC_MAX_BLOCKS * 512);
	}
	emmc_bmajor = bdevsw_add(-1, &emmc_bdevsw);
	if (emmc_bmajor < 0 || (emmc_cmajor = cdevsw_add_with_bdev(-1, &emmc_cdevsw, emmc_bmajor)) < 0) {
		printf("emmc: no room among the devices (%d, %d)\n", emmc_bmajor, emmc_cmajor);
		return ENXIO;
	}
	*root = makedev(emmc_bmajor, 0);
	return 0;
}

void
mtk_emmc_devfs(void)
{
	if (emmc_bmajor < 0 || emmc_cmajor < 0) {
		return;
	}
	if (devfs_make_node(makedev(emmc_bmajor, 0), DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0640, "emmc0") == NULL ||
	    devfs_make_node(makedev(emmc_cmajor, 0), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0640, "remmc0") == NULL) {
		printf("emmc: no /dev nodes\n");
	}
}

/* sysctl kern.hakios.emmc: how much it has read, and its errors */
static int
emmc_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char line[160];

	snprintf(line, sizeof(line), "emmc: %llu reads, %llu MB, %llu errors, %llu by DMA%s%s", emmc_reads,
	    emmc_read_blocks >> 11, emmc_errors, emmc_dma_reads, emmc_failed ? "; last: " : "", emmc_failed ? emmc_failed : "");
	return SYSCTL_OUT(req, line, strlen(line) + 1);
}

SYSCTL_DECL(_kern_hakios);
SYSCTL_PROC(_kern_hakios, OID_AUTO, emmc, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, emmc_sysctl, "A",
    "the eMMC's reads so far");
