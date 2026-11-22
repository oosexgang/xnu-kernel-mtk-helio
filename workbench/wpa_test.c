/*
 * The kernel's WPA2 arithmetic and handshake (drivers/board/hakios_wpa.c) on the host: SHA-1, HMAC-SHA1, the PMK, the
 * standard's PRF and the AES key unwrap against the standards' own examples (FIPS 180, RFC 2202, IEEE 802.11-2020
 * annex J, RFC 3394; the values checked with Python's hashlib and hmac as well), then a four-way handshake and a group
 * key handshake with an access point of this file's, and the messages the station must refuse.
 *
 * The access point wraps keys with AES-128 encryption, which the kernel does not have: forward_cipher() below, the
 * cipher as FIPS 197 writes it, is shown right by wrapping RFC 3394's example key into its example output.
 *
 *   wpa_test               the scenarios, one a line
 *   wpa_test SCENARIO      one scenario a process
 */
#include <stdio.h>
#include <stdlib.h>

#include "fakekernel.h"
#include "hakios_wpa.c"

static int failures;
#define CHECK(condition, ...) do { if (!(condition)) { failures++; fprintf(stderr, "  FAILED: " __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static void
from_hex(const char *hex, uint8_t *out, unsigned length)
{
	for (unsigned i = 0; i < length; i++) {
		unsigned byte;

		sscanf(hex + 2 * i, "%2x", &byte);
		out[i] = (uint8_t)byte;
	}
}

static int
same_as(const uint8_t *bytes, const char *hex, unsigned length)
{
	uint8_t want[128];

	from_hex(hex, want, length);
	return memcmp(bytes, want, length) == 0;
}

/* ---- the standards' examples -------------------------------------------------------------------- */

static void
sha1_of(const char *text, uint8_t digest[20])
{
	struct sha1 s;

	sha1_begin(&s);
	sha1_more(&s, (const uint8_t *)text, (unsigned)strlen(text));
	sha1_end(&s, digest);
}

static void
hmac_of(const uint8_t *key, unsigned key_length, const char *text, uint8_t digest[20])
{
	const uint8_t *pieces[1] = { (const uint8_t *)text };
	const unsigned lengths[1] = { (unsigned)strlen(text) };

	hmac_sha1(key, key_length, pieces, lengths, 1, digest);
}

static void
sha1_and_hmac(void)
{
	uint8_t digest[20], key[80];

	sha1_of("abc", digest);
	CHECK(same_as(digest, "a9993e364706816aba3e25717850c26c9cd0d89d", 20), "SHA-1 of \"abc\" (FIPS 180)");
	sha1_of("", digest);
	CHECK(same_as(digest, "da39a3ee5e6b4b0d3255bfef95601890afd80709", 20), "SHA-1 of nothing");
	sha1_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", digest);
	CHECK(same_as(digest, "84983e441c3bd26ebaae4aa1f95129e5e54670f1", 20), "SHA-1 of 448 bits: two blocks");

	memset(key, 0x0b, 20);
	hmac_of(key, 20, "Hi There", digest);
	CHECK(same_as(digest, "b617318655057264e28bc0b6fb378c8ef146be00", 20), "HMAC-SHA1, RFC 2202 case 1");
	hmac_of((const uint8_t *)"Jefe", 4, "what do ya want for nothing?", digest);
	CHECK(same_as(digest, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79", 20), "HMAC-SHA1, RFC 2202 case 2");
	memset(key, 0xaa, 80);
	hmac_of(key, 80, "Test Using Larger Than Block-Size Key - Hash Key First", digest);
	CHECK(same_as(digest, "aa4ae5e15272d00e95705637ce8a3b55ed402112", 20), "HMAC-SHA1, RFC 2202 case 6: a key longer than a block");
}

static void
pmk_of_name_and_passphrase(void)
{
	static const struct {
		const char *passphrase, *name, *pmk;
	} examples[] = {
		/* IEEE 802.11-2020, J.4.2 */
		{ "password", "IEEE", "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e" },
		{ "ThisIsAPassword", "ThisIsASSID", "0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af" },
		{ "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ",
		  "becb93866bb8c3832cb777c2f559807c8c59afcb6eae734885001300a981cc62" },
	};

	for (unsigned i = 0; i < sizeof(examples) / sizeof(examples[0]); i++) {
		uint8_t pmk[32];

		lx_wpa_pmk((const unsigned char *)examples[i].passphrase, (unsigned)strlen(examples[i].passphrase),
		    (const unsigned char *)examples[i].name, (unsigned)strlen(examples[i].name), pmk);
		CHECK(same_as(pmk, examples[i].pmk, 32), "the PMK of \"%s\" on \"%s\" (802.11 J.4.2)", examples[i].passphrase,
		    examples[i].name);
	}
}

static void
the_standards_prf(void)
{
	uint8_t key[20], out[64];

	/* IEEE 802.11-2020, J.3.2: PRF-512 */
	memset(key, 0x0b, 20);
	prf(key, 20, "prefix", (const uint8_t *)"Hi There", 8, out, 64);
	CHECK(same_as(out, "bcd4c650b30b9684951829e0d75f9d54b862175ed9f00606e17d8da35402ffee75df78c3d31e0f889f012120c0862beb"
	    "67753e7439ae242edb8373698356cf5a", 64), "PRF-512, J.3.2 test case 1");
	prf((const uint8_t *)"Jefe", 4, "prefix-2", (const uint8_t *)"what do ya want for nothing?", 28, out, 64);
	CHECK(same_as(out, "47c4908e30c947521ad20be9053450ecbea23d3aa604b77326d8b3825ff7475c06f51fb9c5313d1e9f90d897d134b7"
	    "2e090fc23150bc8414382043418678e700", 64), "PRF-512, J.3.2 test case 2");
	/* a length that is not a whole number of blocks: the first 48 bytes are the 64's */
	uint8_t part[48];
	prf((const uint8_t *)"Jefe", 4, "prefix-2", (const uint8_t *)"what do ya want for nothing?", 28, part, 48);
	CHECK(memcmp(part, out, 48) == 0, "PRF-384 is the start of PRF-512");
}

/* ---- AES: the kernel's unwrap, and this file's wrap for the access point ------------------------------ */

static uint8_t
xtime(uint8_t a)
{
	return (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1b : 0));
}

/* FIPS 197, 5.1: the cipher, with the kernel's S-box and key schedule (both shown right by the unwrap examples) */
static void
forward_cipher(const uint8_t round_keys[176], uint8_t s[16])
{
	for (unsigned i = 0; i < 16; i++) {
		s[i] ^= round_keys[i];
	}
	for (unsigned round = 1; round <= 10; round++) {
		uint8_t t[16];

		for (unsigned c = 0; c < 4; c++) {          /* SubBytes and ShiftRows: row r taken from column c + r */
			for (unsigned r = 0; r < 4; r++) {
				t[4 * c + r] = sbox[s[4 * ((c + r) % 4) + r]];
			}
		}
		if (round != 10) {                          /* MixColumns */
			for (unsigned c = 0; c < 4; c++) {
				uint8_t *col = t + 4 * c, a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];

				col[0] = (uint8_t)(xtime(a0) ^ xtime(a1) ^ a1 ^ a2 ^ a3);
				col[1] = (uint8_t)(a0 ^ xtime(a1) ^ xtime(a2) ^ a2 ^ a3);
				col[2] = (uint8_t)(a0 ^ a1 ^ xtime(a2) ^ xtime(a3) ^ a3);
				col[3] = (uint8_t)(xtime(a0) ^ a0 ^ a1 ^ a2 ^ xtime(a3));
			}
		}
		for (unsigned i = 0; i < 16; i++) {
			s[i] = t[i] ^ round_keys[16 * round + i];
		}
	}
}

/* RFC 3394, 2.2.1 */
static void
aes_wrap(const uint8_t kek[16], const uint8_t *plain, unsigned length, uint8_t *wrapped)
{
	unsigned n = length / 8;
	uint8_t round_keys[176], a[8], block[16];

	aes_tables();
	aes_keys(kek, round_keys);
	memset(a, 0xa6, 8);
	memcpy(wrapped + 8, plain, length);
	for (unsigned j = 0; j <= 5; j++) {
		for (unsigned i = 1; i <= n; i++) {
			uint64_t t = (uint64_t)n * j + i;

			memcpy(block, a, 8);
			memcpy(block + 8, wrapped + 8 * i, 8);
			forward_cipher(round_keys, block);
			memcpy(a, block, 8);
			for (unsigned k = 0; k < 8; k++) {
				a[7 - k] ^= (uint8_t)(t >> (8 * k));
			}
			memcpy(wrapped + 8 * i, block + 8, 8);
		}
	}
	memcpy(wrapped, a, 8);
}

static void
key_unwrap(void)
{
	uint8_t kek[16], wrapped[24], plain[16], again[24];

	/* RFC 3394, 4.1: 128 bits of key data with a 128-bit KEK */
	from_hex("000102030405060708090a0b0c0d0e0f", kek, 16);
	from_hex("1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5", wrapped, 24);
	aes_tables();
	CHECK(aes_unwrap(kek, wrapped, 24, plain) == 0, "RFC 3394 4.1 unwraps");
	CHECK(same_as(plain, "00112233445566778899aabbccddeeff", 16), "RFC 3394 4.1: the key data");
	aes_wrap(kek, plain, 16, again);
	CHECK(memcmp(again, wrapped, 24) == 0, "this file's wrap gives RFC 3394 4.1's output (its AES is right)");

	/* whatever is changed, the check value does not come out */
	for (unsigned i = 0; i < 24; i++) {
		uint8_t bent[24];

		memcpy(bent, wrapped, 24);
		bent[i] ^= 0x01;
		CHECK(aes_unwrap(kek, bent, 24, plain) != 0, "a wrapped key with byte %u changed is refused", i);
	}
	CHECK(aes_unwrap(kek, wrapped, 20, plain) != 0, "a length not a multiple of 8 is refused");
	CHECK(aes_unwrap(kek, wrapped, 16, plain) != 0, "fewer than 24 bytes is refused");

	/* longer key data, the way a message 3 carries it: wrapped here, unwrapped by the kernel */
	uint8_t data[48], wrapped_data[56], back[48];
	for (unsigned i = 0; i < sizeof(data); i++) {
		data[i] = (uint8_t)(i * 37 + 11);
	}
	aes_wrap(kek, data, 48, wrapped_data);
	CHECK(aes_unwrap(kek, wrapped_data, 56, back) == 0 && memcmp(back, data, 48) == 0, "48 bytes there and back");
}

/* ---- an access point ------------------------------------------------------------------------------ */

static const uint8_t station[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
static const uint8_t access_point[6] = { 0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee };
/* the station's RSN element, as its association request carries it: CCMP, CCMP, PSK */
static const uint8_t rsn[22] = { 0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01,
				 0x00, 0x00, 0x0f, 0xac, 0x02, 0x00, 0x00 };

struct ap {
	uint8_t pmk[32], anonce[32], snonce[32];
	uint8_t kck[16], kek[16], tk[16], gtk[16];
	uint64_t replay;
};

static void
ap_derive(struct ap *ap)
{
	/* 802.11-2020 12.7.1.3: min and max of the addresses, then of the nonces (this file's order, not the kernel's) */
	uint8_t data[76], keys[48];
	const uint8_t *a1 = access_point, *a2 = station, *n1 = ap->anonce, *n2 = ap->snonce;

	if (memcmp(station, access_point, 6) < 0) {
		a1 = station, a2 = access_point;
	}
	if (memcmp(ap->snonce, ap->anonce, 32) < 0) {
		n1 = ap->snonce, n2 = ap->anonce;
	}
	memcpy(data, a1, 6);
	memcpy(data + 6, a2, 6);
	memcpy(data + 12, n1, 32);
	memcpy(data + 44, n2, 32);
	prf(ap->pmk, 32, "Pairwise key expansion", data, 76, keys, 48);
	memcpy(ap->kck, keys, 16);
	memcpy(ap->kek, keys + 16, 16);
	memcpy(ap->tk, keys + 32, 16);
}

static void
put16(uint8_t *at, unsigned value)
{
	at[0] = (uint8_t)(value >> 8);
	at[1] = (uint8_t)value;
}

static void
sign(const uint8_t kck[16], uint8_t *message, unsigned length)
{
	uint8_t digest[20];
	const uint8_t *pieces[1] = { message };
	const unsigned lengths[1] = { length };

	memset(message + AT_MIC, 0, 16);
	hmac_sha1(kck, 16, pieces, lengths, 1, digest);
	memcpy(message + AT_MIC, digest, 16);
}

/* a key message from the access point: its info bits, the next counter, a nonce, key data; its length */
static unsigned
ap_message(struct ap *ap, uint8_t *message, unsigned info, const uint8_t *nonce, const uint8_t *data, unsigned data_length)
{
	unsigned length = AT_DATA + data_length;

	memset(message, 0, length);
	message[0] = 2;
	message[1] = 3;
	put16(message + 2, length - 4);
	message[AT_KIND] = 2;
	put16(message + AT_INFO, info);
	put16(message + 7, 16);
	ap->replay++;
	for (unsigned k = 0; k < 8; k++) {
		message[AT_REPLAY + 7 - k] = (uint8_t)(ap->replay >> (8 * k));
	}
	if (nonce != NULL) {
		memcpy(message + AT_NONCE, nonce, 32);
	}
	message[AT_RSC] = 0x05;                         /* the group's packet number: 5 */
	put16(message + AT_DATA_LENGTH, data_length);
	memcpy(message + AT_DATA, data, data_length);
	if (info & INFO_MIC) {
		sign(ap->kck, message, length);
	}
	return length;
}

/* message 3's key data, wrapped: the access point's RSN element, the GTK's KDE with `index`, padding */
static unsigned
ap_key_data(const struct ap *ap, unsigned index, uint8_t *wrapped)
{
	uint8_t plain[48];
	unsigned n = 0;

	memcpy(plain, rsn, sizeof(rsn));
	n = sizeof(rsn);
	const uint8_t kde[8] = { 0xdd, 6 + 16, 0x00, 0x0f, 0xac, 0x01, (uint8_t)index, 0 };
	memcpy(plain + n, kde, 8);
	memcpy(plain + n + 8, ap->gtk, 16);
	n += 24;
	plain[n++] = 0xdd;                              /* padding to a multiple of 8: 0xdd then zeros */
	while (n % 8 != 0) {
		plain[n++] = 0;
	}
	aes_wrap(ap->kek, plain, n, wrapped);
	return n + 8;
}

static int
mic_right(const uint8_t kck[16], const uint8_t *message, unsigned length)
{
	uint8_t copy[MESSAGE_MOST];

	memcpy(copy, message, length);
	sign(kck, copy, length);
	return memcmp(copy + AT_MIC, message + AT_MIC, 16) == 0;
}

static unsigned
info_of(const uint8_t *message)
{
	return (unsigned)message[AT_INFO] << 8 | message[AT_INFO + 1];
}

/* a station and an access point that know the same passphrase (or not), and message 1 answered */
static void
begin(struct ap *ap, struct lx_wpa *wpa, const char *station_passphrase, uint8_t *reply, unsigned *reply_length)
{
	uint8_t message[MESSAGE_MOST];
	unsigned length;
	const char *why;

	memset(ap, 0, sizeof(*ap));
	memset(wpa, 0, sizeof(*wpa));
	lx_wpa_pmk((const unsigned char *)"correct horse", 13, (const unsigned char *)"hakios-test", 11, ap->pmk);
	lx_wpa_pmk((const unsigned char *)station_passphrase, (unsigned)strlen(station_passphrase),
	    (const unsigned char *)"hakios-test", 11, wpa->pmk);
	for (unsigned i = 0; i < 32; i++) {
		ap->anonce[i] = (uint8_t)(0x40 + i);
		wpa->own_nonce[i] = (uint8_t)(0x90 - i);
	}
	for (unsigned i = 0; i < 16; i++) {
		ap->gtk[i] = (uint8_t)(0xc0 | i);
	}
	memcpy(wpa->own, station, 6);
	memcpy(wpa->peer, access_point, 6);
	wpa->rsn = rsn;
	wpa->rsn_length = sizeof(rsn);

	length = ap_message(ap, message, 2 | INFO_PAIRWISE | INFO_ACK, ap->anonce, NULL, 0);
	CHECK(lx_wpa_message(wpa, message, length, reply, reply_length, &why) == LX_WPA_ANSWER, "message 1 is answered (%s)",
	    why);
	memcpy(ap->snonce, reply + AT_NONCE, 32);
	ap_derive(ap);
}

static unsigned
message_3(struct ap *ap, uint8_t *message)
{
	uint8_t wrapped[64];
	unsigned wrapped_length = ap_key_data(ap, 1, wrapped);

	return ap_message(ap, message, 2 | INFO_PAIRWISE | INFO_INSTALL | INFO_ACK | INFO_MIC | INFO_SECURE | INFO_ENCRYPTED,
	    ap->anonce, wrapped, wrapped_length);
}

static void
four_way_handshake(void)
{
	struct ap ap;
	struct lx_wpa wpa;
	uint8_t reply[LX_WPA_REPLY_MOST], message[MESSAGE_MOST];
	unsigned reply_length, length;
	const char *why;

	begin(&ap, &wpa, "correct horse", reply, &reply_length);
	/* message 2: our nonce, our RSN element, the counter of message 1, signed with the KCK both derive */
	CHECK(reply_length == AT_DATA + sizeof(rsn), "message 2 is %u bytes", reply_length);
	CHECK(info_of(reply) == (2 | INFO_PAIRWISE | INFO_MIC), "message 2's bits: %#x", info_of(reply));
	CHECK(memcmp(reply + AT_NONCE, wpa.own_nonce, 32) == 0, "message 2 carries the station's nonce");
	CHECK(memcmp(reply + AT_DATA, rsn, sizeof(rsn)) == 0, "message 2 carries the station's RSN element");
	CHECK(reply[AT_REPLAY + 7] == 1, "message 2 has message 1's counter");
	CHECK(mic_right(ap.kck, reply, reply_length), "message 2's MIC is right with the access point's KCK");

	length = message_3(&ap, message);
	CHECK(lx_wpa_message(&wpa, message, length, reply, &reply_length, &why) == LX_WPA_KEYS, "message 3 gives the keys (%s)",
	    why);
	CHECK(memcmp(wpa.pairwise, ap.tk, 16) == 0, "the pairwise key is the access point's");
	CHECK(wpa.group_length == 16 && memcmp(wpa.group, ap.gtk, 16) == 0, "the group key is the access point's");
	CHECK(wpa.group_index == 1, "the group key's number: %u", wpa.group_index);
	CHECK(wpa.group_counter[0] == 5, "the group's packet number comes from the RSC");
	/* message 4: no nonce, no data, secure, signed */
	CHECK(reply_length == AT_DATA, "message 4 has no key data (%u bytes)", reply_length);
	CHECK(info_of(reply) == (2 | INFO_PAIRWISE | INFO_MIC | INFO_SECURE), "message 4's bits: %#x", info_of(reply));
	CHECK(mic_right(ap.kck, reply, reply_length), "message 4's MIC is right");
	CHECK(reply[AT_REPLAY + 7] == 2, "message 4 has message 3's counter");

	/* later, a new group key */
	uint8_t wrapped[64];
	memset(ap.gtk, 0x5a, 16);
	unsigned wrapped_length = ap_key_data(&ap, 2, wrapped);
	length = ap_message(&ap, message, 2 | INFO_ACK | INFO_MIC | INFO_SECURE | INFO_ENCRYPTED, NULL, wrapped, wrapped_length);
	CHECK(lx_wpa_message(&wpa, message, length, reply, &reply_length, &why) == LX_WPA_GROUP, "a group message (%s)", why);
	CHECK(memcmp(wpa.group, ap.gtk, 16) == 0 && wpa.group_index == 2, "the new group key, number 2");
	CHECK(info_of(reply) == (2 | INFO_MIC | INFO_SECURE) && mic_right(ap.kck, reply, reply_length), "its answer");
}

static void
what_is_refused(void)
{
	struct ap ap;
	struct lx_wpa wpa;
	uint8_t reply[LX_WPA_REPLY_MOST], message[MESSAGE_MOST];
	unsigned reply_length, length;
	const char *why;

	/* a wrong passphrase: message 3's signature does not come out, and nothing is learnt */
	begin(&ap, &wpa, "wrong horse", reply, &reply_length);
	CHECK(!mic_right(ap.kck, reply, reply_length), "with a wrong passphrase message 2's MIC is not the access point's");
	length = message_3(&ap, message);
	CHECK(lx_wpa_message(&wpa, message, length, reply, &reply_length, &why) == LX_WPA_BAD && strstr(why, "signature"),
	    "message 3 under another passphrase is refused (%s)", why);
	CHECK(wpa.group_length == 0 && reply_length == 0, "and nothing is kept or sent");

	/* message 3 played again */
	begin(&ap, &wpa, "correct horse", reply, &reply_length);
	length = message_3(&ap, message);
	CHECK(lx_wpa_message(&wpa, message, length, reply, &reply_length, &why) == LX_WPA_KEYS, "message 3");
	CHECK(lx_wpa_message(&wpa, message, length, reply, &reply_length, &why) == LX_WPA_BAD && strstr(why, "counter"),
	    "the same message 3 again is refused (%s)", why);

	/* message 3 with another nonce than message 1's */
	begin(&ap, &wpa, "correct horse", reply, &reply_length);
	ap.anonce[0] ^= 1;
	length = message_3(&ap, message);
	CHECK(lx_wpa_message(&wpa, message, length, reply, &reply_length, &why) == LX_WPA_BAD && strstr(why, "nonce"),
	    "message 3 with a nonce not message 1's (%s)", why);

	/* key data not marked encrypted */
	begin(&ap, &wpa, "correct horse", reply, &reply_length);
	uint8_t wrapped[64];
	unsigned wrapped_length = ap_key_data(&ap, 1, wrapped);
	length = ap_message(&ap, message, 2 | INFO_PAIRWISE | INFO_INSTALL | INFO_ACK | INFO_MIC | INFO_SECURE, ap.anonce, wrapped,
	    wrapped_length);
	CHECK(lx_wpa_message(&wpa, message, length, reply, &reply_length, &why) == LX_WPA_BAD && strstr(why, "unwrap"),
	    "key data not marked encrypted (%s)", why);

	/* a signed message before any message 1 */
	struct lx_wpa fresh;
	memset(&fresh, 0, sizeof(fresh));
	length = message_3(&ap, message);
	CHECK(lx_wpa_message(&fresh, message, length, reply, &reply_length, &why) == LX_WPA_BAD && strstr(why, "no message 1"),
	    "a signed message with no message 1 before it (%s)", why);

	/* what is not a key message at all, or not one of this kind */
	uint8_t start[4] = { 2, 1, 0, 0 };              /* EAPOL-Start */
	CHECK(lx_wpa_message(&wpa, start, 4, reply, &reply_length, &why) == LX_WPA_NOTHING, "an EAPOL frame of another type");
	length = ap_message(&ap, message, 2 | INFO_PAIRWISE | INFO_ACK, ap.anonce, NULL, 0);
	CHECK(lx_wpa_message(&wpa, message, AT_DATA - 1, reply, &reply_length, &why) == LX_WPA_BAD, "a key message cut short");
	message[AT_KIND] = 254;
	CHECK(lx_wpa_message(&wpa, message, length, reply, &reply_length, &why) == LX_WPA_BAD && strstr(why, "RSN"),
	    "a key descriptor of WPA's first kind (%s)", why);
	message[AT_KIND] = 2;
	put16(message + AT_INFO, 1 | INFO_PAIRWISE | INFO_ACK);
	CHECK(lx_wpa_message(&wpa, message, length, reply, &reply_length, &why) == LX_WPA_BAD && strstr(why, "version 2"),
	    "descriptor version 1 (%s)", why);
	put16(message + AT_INFO, 2 | INFO_PAIRWISE | INFO_ACK);
	put16(message + AT_DATA_LENGTH, 400);
	CHECK(lx_wpa_message(&wpa, message, length, reply, &reply_length, &why) == LX_WPA_BAD && strstr(why, "longer"),
	    "key data said to be longer than the message (%s)", why);
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "sha1_and_hmac", sha1_and_hmac }, { "pmk_of_name_and_passphrase", pmk_of_name_and_passphrase },
		{ "the_standards_prf", the_standards_prf }, { "key_unwrap", key_unwrap },
		{ "four_way_handshake", four_way_handshake }, { "what_is_refused", what_is_refused },
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
