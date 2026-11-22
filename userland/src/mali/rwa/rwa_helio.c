/*
 * The MediaTek Helio chips and their GPUs, for the native Metal library: which chip a board has (the kernel's
 * hw.board.soc, from arch/arm64/boot/dts/mediatek/NAME.toml), the GPU in it, and what this library can do with that GPU.
 *
 * The library writes Bifrost v7 job descriptors (genxml v7, Panfrost's PAN_ARCH 7): a Mali-G52 or G76 is served.
 * The rest of the Helio line is here so that a port to another phone starts from a sentence, not from a fault:
 *
 *   Bifrost v6 (G71, G72)     planned: genxml v6 is v7's predecessor and Panfrost builds both from one source with
 *                             PAN_ARCH; this library would be built a second time with PAN_ARCH=6, its few v7-only
 *                             fields (the frame shader's pre-frame modes, the tiler's hierarchy) checked against v6
 *   Valhall v9 (G57)          planned: a backend of its own -- the renderer state and the attribute tables become
 *                             shader program and resource descriptors (genxml v9, Panfrost's PAN_ARCH 9)
 *   Midgard (T860, T880)      not planned: another shader compiler (midgard) and the v4/v5 descriptors
 *   PowerVR (GE8320, GM9446)  no Mali: no native path at all (the Gallium library may still draw in software)
 *
 * The GPU product ids are Panfrost's (pan_props.c's MODEL table: GPU_ID >> 16). The chip to GPU pairs are the
 * vendor's published ones; only mt6768 has been run on a phone (the Infinix X6826B, a Helio G85: product 0x7402).
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>

#include "rwa_priv.h"

static const struct rwa_helio helio[] = {
	/* Bifrost v7: served */
	{ "mt6768", "Helio P65, G70, G80, G85, G88 (MT6768, MT6769)", "Mali-G52 MC2", { 0x7402, 0x7212 }, true },
	{ "mt6785", "Helio G90, G90T, G95 (MT6785)", "Mali-G76 MC4", { 0x7211 }, false },
	/* Bifrost v6 */
	{ "mt6771", "Helio P60, P70 (MT6771)", "Mali-G72 MP3", { 0x6221 }, false },
	{ "mt6763", "Helio P23, P30 (MT6763)", "Mali-G71 MP2", { 0x6000 }, false },
	/* Valhall v9 */
	{ "mt6781", "Helio G96 (MT6781)", "Mali-G57 MC2", { 0x9091, 0x9093 }, false },
	{ "mt6789", "Helio G99 (MT6789)", "Mali-G57 MC2", { 0x9091, 0x9093 }, false },
	/* Midgard */
	{ "mt6797", "Helio X20, X25, X27 (MT6797)", "Mali-T880 MP4", { 0x880 }, false },
	{ "mt6757", "Helio P20, P25 (MT6757)", "Mali-T880 MP2", { 0x880 }, false },
	{ "mt6755", "Helio P10 (MT6755)", "Mali-T860 MP2", { 0x860 }, false },
	/* PowerVR: no Mali */
	{ "mt6761", "Helio A22 (MT6761)", "PowerVR GE8320", { 0 }, false },
	{ "mt6762", "Helio P22, G25 (MT6762)", "PowerVR GE8320", { 0 }, false },
	{ "mt6765", "Helio P35, G35, G37 (MT6765)", "PowerVR GE8320", { 0 }, false },
	{ "mt6779", "Helio P90, P95 (MT6779)", "PowerVR GM9446", { 0 }, false },
};

const struct rwa_helio *
rwa_helio_find(const char *soc, unsigned gpu_id)
{
	for (unsigned i = 0; soc && i < sizeof(helio) / sizeof(helio[0]); i++)
		if (strcmp(helio[i].soc, soc) == 0)
			return &helio[i];
	for (unsigned i = 0; gpu_id && i < sizeof(helio) / sizeof(helio[0]); i++)
		for (unsigned k = 0; k < 2; k++)
			if (helio[i].gpu_id[k] == gpu_id)
				return &helio[i];
	return NULL;
}

/* the kernel's hw.board.soc ("mt6768"), or "" on a board the kernel was not told of (and on the Mac) */
const char *
rwa_board_soc(void)
{
	static char soc[32];
	static bool read;

	if (!read) {
		read = true;
#ifndef RWA_HOST_FAKE
		size_t n = sizeof(soc) - 1;
		if (sysctlbyname("hw.board.soc", soc, &n, NULL, 0) != 0)
			soc[0] = 0;
#endif
	}
	return soc;
}

/* what this library does with a GPU of architecture ARCH (pan_arch of its product id) */
const char *
rwa_arch_status(unsigned arch, unsigned gpu_id)
{
	if (gpu_id == 0)
		return "no Mali: no native path (PowerVR)";
	switch (arch) {
	case 7:
		return "Bifrost v7: served";
	case 6:
		return "Bifrost v6: planned (this library built again with PAN_ARCH=6), not built yet";
	case 9:
		return "Valhall v9: planned (a backend of its own: shader program and resource descriptors), not written yet";
	case 4:
	case 5:
		return "Midgard: not planned (another compiler and the v4/v5 descriptors)";
	default:
		return "an architecture this library has no plan for";
	}
}
