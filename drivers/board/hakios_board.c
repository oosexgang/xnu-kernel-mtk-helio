/*
 * Which board this is (hakios_board.h): the row of the generated table whose name the loader put in
 * /chosen/darwin-board. Also as sysctls, for user space and for a person at a shell:
 *
 *   hw.board.name          the package's name; "unknown" when the loader named none or the table has not got it
 *   hw.board.soc           "mt6768"
 *   hw.board.description   "Maker Model"
 *   hw.board.features      "emmc-msdc keys-mt6358 ..."
 *
 * The table is made when the kernel is built (tools/boards.py gen, by kernel/build.sh), with a row for
 * every device package that was there: a kernel knows the boards it was built with.
 */
#include <libkern/libkern.h>
#include <machine/machine_routines.h>
#include <pexpert/device_tree.h>
#include <pexpert/pexpert.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

#include "hakios_board.h"
#include "hakios_board_table.h"

extern void loader_log_putc(char c);

static const struct hakios_board *found;
static bool looked;

/*
 * Is this the board the loader named (/chosen/darwin-board, a string with its end)? The name is the
 * loader's word and nothing else is: a machine whose loader named none is no board at all -- which
 * is not permission to touch anything of a phone.
 */
static bool
loader_named(const char *name)
{
	DTEntry chosen;
	const void *value;
	unsigned int size;
	size_t length = strlen(name) + 1;

	return SecureDTLookupEntry(NULL, "/chosen", &chosen) == kSuccess &&
	       SecureDTGetProperty(chosen, "darwin-board", &value, &size) == kSuccess &&
	       size == length && memcmp(value, name, length) == 0;
}

const struct hakios_board *
hakios_board(void)
{
	if (!looked) {
		/* (twice at once finds the same row twice: nothing to guard) */
		for (unsigned i = 0; i < hakios_board_count; i++) {
			if (loader_named(hakios_boards[i].name)) {
				found = &hakios_boards[i];
				break;
			}
		}
		looked = true;
	}
	return found;
}

bool
hakios_board_has(uint32_t feature)
{
	const struct hakios_board *board = hakios_board();

	return board != NULL && (board->features & feature) != 0;
}

static const char *
feature_name(uint32_t feature)
{
	for (unsigned bit = 0; bit < hakios_feature_count; bit++) {
		if (feature == (1u << bit)) {
			return hakios_feature_names[bit];
		}
	}
	return "?";
}

bool
hakios_board_wants(uint32_t feature, const char *driver)
{
	const struct hakios_board *board = hakios_board();

	if (hakios_board_has(feature)) {
		return true;
	}
	printf("%s: asked for, but %s%s does not list \"%s\" in its board file's features: nothing is touched\n", driver,
	    board != NULL ? "the board " : "this board is not one the kernel knows, and so", board != NULL ? board->name : "",
	    feature_name(feature));
	return false;
}

/* "emmc-msdc keys-mt6358", or "" */
static void
features_text(const struct hakios_board *board, char *text, size_t size)
{
	size_t at = 0;

	text[0] = 0;
	for (unsigned bit = 0; board != NULL && bit < hakios_feature_count; bit++) {
		if ((board->features & (1u << bit)) && hakios_feature_names[bit][0] != 0) {
			at += (size_t)snprintf(text + at, size - at, "%s%s", at ? " " : "", hakios_feature_names[bit]);
			if (at >= size) {
				break;
			}
		}
	}
}

void
hakios_board_init(void)
{
	const struct hakios_board *board = hakios_board();
	char line[320], features[224];

	features_text(board, features, sizeof(features));
	if (board != NULL) {
		snprintf(line, sizeof(line), "board: %s (%s, %s): %s\n", board->name, board->description, board->soc,
		    features[0] ? features : "no features listed");
	} else {
		snprintf(line, sizeof(line), "board: not one of the %u this kernel knows (/chosen/darwin-board): no device "
		    "of a phone is touched\n", hakios_board_count);
	}
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	if (ml_get_interrupts_enabled()) {
		printf("%s", line);
	}
}

SYSCTL_NODE(_hw, OID_AUTO, board, CTLFLAG_RD | CTLFLAG_LOCKED, 0, "the board this kernel runs on");

static int
sysctl_board SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1)
	const struct hakios_board *board = hakios_board();
	char features[224];
	const char *text;

	switch (arg2) {
	case 0: text = board ? board->name : "unknown"; break;
	case 1: text = board ? board->soc : "unknown"; break;
	case 2: text = board ? board->description : "unknown"; break;
	default:
		features_text(board, features, sizeof(features));
		text = features;
		break;
	}
	return SYSCTL_OUT(req, text, strlen(text) + 1);
}

SYSCTL_PROC(_hw_board, OID_AUTO, name, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, sysctl_board, "A", "");
SYSCTL_PROC(_hw_board, OID_AUTO, soc, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 1, sysctl_board, "A", "");
SYSCTL_PROC(_hw_board, OID_AUTO, description, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 2, sysctl_board, "A", "");
SYSCTL_PROC(_hw_board, OID_AUTO, features, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 3, sysctl_board, "A", "");
