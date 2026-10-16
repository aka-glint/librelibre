/* RED-first tests for the derived sensor SERIAL (SERIAL-IN-FRAM.md spec).
 *
 * The serial is a deterministic 5-bit-packed encoding of the UID that the
 * Flipper already reads via the inventory command. No new NFC command, no FRAM
 * field, no cloud. See glucose-research/SERIAL-IN-FRAM.md.
 *
 * Two independent assertions are required before any implementation:
 *   1. The public LibreTools worked example reproduces exactly.
 *   2. The LEADING character comes from the patch-info FAMILY NIBBLE
 *      (patch[2] >> 4), NOT from the LibreTools patchInfo[0] table. For the
 *      real EU2 sensor that table would wrongly emit 0M0... where the correct
 *      output is 3M0...
 */
#include "protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Public worked example from the LibreTools source comment
 * (glucose-research/evidence/mismatch-source-LibreTools-SensorSerialNumber.swift).
 * Wire order (ISO15693 inventory LSB-first): 5E 90 25 00 00 A0 07 E0.
 * Reversed, last six bytes = A0 00 00 25 90 5E -> serial "0M00009DHCR". The
 * patch family nibble is 0 for this public example (leading '0'). */
static const uint8_t EXAMPLE_UID[8] = {0x5e, 0x90, 0x25, 0x00, 0x00, 0xa0, 0x07, 0xe0};
static const uint8_t EXAMPLE_PATCH[6] = {0xc5, 0x09, 0x00, 0x01, 0x00, 0x00};

/* The real EU2 sensor profile used by the project's captures: patch[2] = 0x30
 * -> family nibble 3 -> leading '3'. The LibreTools patchInfo[0] table lacks
 * the 0xC5 entry and falls through to '0', which is the documented trap. */
static const uint8_t EU2_UID[8] = {0x5e, 0x90, 0x25, 0x00, 0x00, 0xa0, 0x07, 0xe0};
static const uint8_t EU2_PATCH[6] = {0xc5, 0x09, 0x30, 0x01, 0x61, 0x12};

int main(void) {
    char serial[16];

    /* 1. Public worked example must reproduce byte-for-byte. */
    assert(lc_serial_from_uid(EXAMPLE_UID, EXAMPLE_PATCH, serial, sizeof serial));
    assert(strlen(serial) == 11);
    if(strcmp(serial, "0M00009DHCR") != 0) {
        fprintf(stderr, "FAIL worked example: got '%s', want '0M00009DHCR'\n", serial);
        return 1;
    }
    puts("PASS public LibreTools worked example reproduces exactly (0M00009DHCR)");

    /* 2. Leading character is the patch-info FAMILY NIBBLE, not patchInfo[0].
     * Real EU2 UID + EU2 patch: nibble 3 -> '3' prefix. The LibreTools table
     * would emit '0' here (0xC5 is absent from its {0x70,0x9D,0xDF,0xE5} map). */
    assert(lc_serial_from_uid(EU2_UID, EU2_PATCH, serial, sizeof serial));
    assert(serial[0] == '3');
    assert(strncmp(serial, "3M0", 3) == 0);
    assert(serial[0] != '0');
    puts("PASS leading char from family nibble patch[2]>>4 (3M0..., not 0M0...)");

    /* 2b. Same tail, different family nibble -> only the leading char changes. */
    uint8_t patch0[6];
    memcpy(patch0, EU2_PATCH, 6);
    patch0[2] = 0x00; /* family nibble 0 */
    char s0[16];
    assert(lc_serial_from_uid(EU2_UID, patch0, s0, sizeof s0));
    assert(s0[0] == '0' && strcmp(s0 + 1, serial + 1) == 0);
    patch0[2] = 0x70; /* family nibble 7 */
    assert(lc_serial_from_uid(EU2_UID, patch0, s0, sizeof s0));
    assert(s0[0] == '7' && strcmp(s0 + 1, serial + 1) == 0);
    puts("PASS family nibble selects only the leading character");

    /* 3. The alphabet omits B, I, O and S; no serial char may be one of them. */
    for(size_t i = 1; i < 11; i++) {
        assert(serial[i] != 'B' && serial[i] != 'I' && serial[i] != 'O' && serial[i] != 'S');
    }

    /* 4. Deterministic: the same UID always yields the same serial. */
    char again[16];
    assert(lc_serial_from_uid(EU2_UID, EU2_PATCH, again, sizeof again));
    assert(strcmp(serial, again) == 0);

    /* 5. Byte-reversal is required: a wire-order (unreversed) read would differ.
     * Compared against a UID whose ends are distinct. */
    const uint8_t a[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x07, 0xe0};
    const uint8_t b[8] = {0xe0, 0x07, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
    char sa[16], sb[16];
    assert(lc_serial_from_uid(a, EU2_PATCH, sa, sizeof sa));
    assert(lc_serial_from_uid(b, EU2_PATCH, sb, sizeof sb));
    assert(strcmp(sa, sb) != 0);

    /* 6. Two synthetic sensors produce two different serials (discrimination). */
    const uint8_t c[8] = {0xde, 0xad, 0xbe, 0xef, 0x00, 0x00, 0x77, 0x88};
    char sc[16];
    assert(lc_serial_from_uid(c, EU2_PATCH, sc, sizeof sc));
    assert(strcmp(sc, sa) != 0 && strcmp(sc, sb) != 0);

    /* 7. Fail closed on missing inputs / short buffer; never a partial serial. */
    serial[0] = 'X';
    assert(!lc_serial_from_uid(NULL, EU2_PATCH, serial, sizeof serial) && serial[0] == 'X');
    assert(!lc_serial_from_uid(EU2_UID, NULL, serial, sizeof serial) && serial[0] == 'X');
    assert(!lc_serial_from_uid(EU2_UID, EU2_PATCH, NULL, sizeof serial));
    assert(!lc_serial_from_uid(EU2_UID, EU2_PATCH, serial, 11) && serial[0] == 'X');
    assert(lc_serial_from_uid(EU2_UID, EU2_PATCH, serial, 12));

    puts("PASS serial derivation: deterministic, reversed, 32-symbol alphabet, fail-closed");
    return 0;
}
