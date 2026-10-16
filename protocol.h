#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
bool lc_command_allowed(const uint8_t* cmd, size_t len);
bool lc_patch_parse(const uint8_t* response, size_t len, uint8_t out[6]);
bool lc_uid_parse(const uint8_t* response, size_t len, uint8_t out[8]);
bool lc_uid_is_ti(const uint8_t uid[8]);
bool lc_family_allowed(const uint8_t uid[8], const uint8_t* patch, size_t len);
bool lc_read_command(size_t index, uint8_t out[4]);
bool lc_read_parse(const uint8_t* response, size_t len, size_t blocks, uint8_t* out);
bool lc_hex(const uint8_t* data, size_t len, char* out, size_t capacity);
/* DERIVED sensor SERIAL (11 chars + NUL), the deterministic 5-bit-packed
 * encoding of the wire-order UID documented in glucose-research/SERIAL-IN-FRAM.md
 * and pinned in four upstream implementations. This is DERIVED from the UID, NOT
 * read from FRAM. The leading character is the patch-info FAMILY NIBBLE
 * (patch[2] >> 4) — the LibreTools patchInfo[0] table is NOT used and would
 * mis-prefix EU2 0xC5 sensors as '0MH...' instead of '3MH...'. Requires this
 * scan's uid and patch (both 8/6 bytes) and capacity >= 12; fails closed with the
 * caller's buffer untouched so a partial or fabricated serial is never shown. */
bool lc_serial_from_uid(const uint8_t uid[8], const uint8_t patch[6], char* out, size_t capacity);
bool lc_timestamp_valid(uint32_t timestamp);
