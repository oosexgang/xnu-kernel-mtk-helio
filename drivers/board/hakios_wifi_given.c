/*
 * Two things every radio's driver shares with the station's side (bsd/dev/hakios_sta.c,
 * bsd/dev/mtk_wifi.c): a line for whoever runs the phone, and the network the kernel was
 * given to join by itself -- the file fw/network of the small image, the name on its first line and
 * the passphrase on its second.
 */
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <stdarg.h>
#include <sys/hakios_input.h>
#include <sys/systm.h>

extern void loader_log_putc(char c);
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);     /* hakios_image.c */

/* A line for whoever runs the phone: into the loader's log and the kernel's. */
int
hakios_wifi_tell(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	printf("%s", line);
	return 0;
}

bool
hakios_wifi_network(uint8_t *name, unsigned *name_length, uint8_t *passphrase, unsigned *passphrase_length)
{
	uint64_t size = 0;
	const uint8_t *file = hakios_image_file("fw", "network", &size, NULL), *end = file + size, *line = file;
	unsigned lengths[2] = { 0, 0 }, sizes[2] = { *name_length, *passphrase_length };
	uint8_t *where[2] = { name, passphrase };

	if (file == NULL) {
		printf("wifi: which network to join: none was given (fw/network of the small image)\n");
		return false;
	}
	for (unsigned n = 0; n < 2 && line < end; n++) {
		const uint8_t *stop = line;

		while (stop < end && *stop != '\n') {
			stop++;
		}
		lengths[n] = (unsigned)(stop - line) - (stop > line && stop[-1] == '\r' ? 1 : 0);
		if (lengths[n] > sizes[n]) {
			lengths[n] = 0;
		} else {
			memcpy(where[n], line, lengths[n]);
		}
		line = stop + 1;
	}
	printf("wifi: which network to join: a name of %u bytes and a passphrase of %u bytes were given%s\n",
	    lengths[0], lengths[1], lengths[0] == 0 || lengths[1] < 8 || lengths[1] > 63 ? ": NOT A NAME OF 1 TO 32 BYTES AND A "
	    "PASSPHRASE OF 8 TO 63" : "");
	if (lengths[0] == 0 || lengths[1] < 8 || lengths[1] > 63) {
		return false;
	}
	*name_length = lengths[0];
	*passphrase_length = lengths[1];
	return true;
}
