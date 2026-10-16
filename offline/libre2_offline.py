#!/usr/bin/env python3
"""Experimental offline byte decoder. No glucose reading; not for treatment.

Adapted from gui-dos/DiaBLE (MIT), pinned in provenance.json.
Copyright (c) 2026 Guido Soranzio. See reference/LICENSE.md.
"""


def crc16_libre(data):
    """Libre FRAM CRC, not ISO15693 link CRC; final 16-bit reversal."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x8408 if crc & 1 else 0)
    return int(f'{crc:016b}'[::-1], 2)


def prepare_variables(uid, x, y):
    if len(uid) != 8 or not (0 <= x <= 0xFFFF and 0 <= y <= 0xFFFF):
        raise ValueError('invalid crypto input dimensions')
    return [(int.from_bytes(uid[4:6], 'little') + x + y) & 0xFFFF,
            int.from_bytes(uid[2:4], 'little'),
            (int.from_bytes(uid[0:2], 'little') + 2 * x) & 0xFFFF,
            0x241A ^ 0x14C6]


def process_crypto(words):
    if len(words) != 4 or any(type(w) is not int or not 0 <= w <= 0xFFFF for w in words):
        raise ValueError('crypto requires four UInt16 words')
    def op(value):
        return (value >> 2) ^ (0x6860 if value & 1 else 0) ^ (0xA0C5 if value & 2 else 0)
    r0 = op(words[0]) ^ words[3]
    r1 = op(r0) ^ words[2]
    r2 = op(r1) ^ words[1]
    r3 = op(r2) ^ words[0]
    r4 = op(r3)
    r5 = op(r4 ^ r0)
    r6 = op(r5 ^ r1)
    r7 = op(r6 ^ r2)
    return [r3 ^ r7, r2 ^ r6, r1 ^ r5, r0 ^ r4]


def decrypt_fram(uid, patch, raw):
    """Reference XOR transform, EU2 path only; caller must gate family."""
    if len(uid) != 8 or len(patch) != 6 or len(raw) != 344:
        raise ValueError('requires UID8, patch6, FRAM344')
    y = int.from_bytes(patch[4:6], 'little') ^ 0x44
    result = bytearray()
    for block in range(43):
        words = process_crypto(prepare_variables(uid, block, y))
        key = b''.join(word.to_bytes(2, 'little') for word in words)
        result.extend(a ^ b for a, b in zip(raw[block*8:block*8+8], key))
    return bytes(result)


def hex_bytes(value, length=None):
    import re
    if not isinstance(value, str) or not re.fullmatch(r'(?:[0-9a-fA-F]{2})*', value):
        raise ValueError('malformed hexadecimal field')
    data = bytes.fromhex(value)
    if length is not None and len(data) != length:
        raise ValueError('incorrect hexadecimal field length')
    return data


def validate_capture(d):
    """Strict validation of the current conservative capture/transcript profile."""
    if not isinstance(d, dict):
        raise ValueError('capture must be an object')
    expected = {'schema': 'libre-nfc-research-capture', 'schema_version': 2,
                'capture_kind': 'conservative_raw_fram',
                'uid_order': 'ISO15693 inventory wire LSB-first',
                'patch_info_length': 6, 'fram_length': 344, 'fram_expected_length': 344,
                'complete': True, 'transport_error': 0, 'glucose_available': False}
    for key, value in expected.items():
        if type(d.get(key)) is not type(value) or d.get(key) != value:
            raise ValueError('invalid capture field: ' + key)
    uid = hex_bytes(d.get('uid_wire_hex'), 8)
    patch = hex_bytes(d.get('patch_info_hex'), 6)
    raw = hex_bytes(d.get('fram_hex'), 344)
    ts = d.get('transcript')
    if not isinstance(ts, list) or len(ts) != 17:
        raise ValueError('expected inventory, patch, and 15 ordered read transcript entries')
    assembled = bytearray()
    for index, item in enumerate(ts):
        if not isinstance(item, dict) or type(item.get('error')) is not int or item['error'] != 0:
            raise ValueError('transcript transport error')
        command = hex_bytes(item.get('command_hex'))
        response = hex_bytes(item.get('response_hex'))
        if type(item.get('response_length')) is not int or item['response_length'] != len(response):
            raise ValueError('transcript response length mismatch')
        if not response or response[0] != 0:
            raise ValueError('transcript ISO15693 status not success')
        if index == 0:
            if command != bytes.fromhex('260100') or len(response) != 10 or response[2:] != uid:
                raise ValueError('inventory transcript mismatch')
        elif index == 1:
            if command != bytes.fromhex('02A107') or response != b'\x00' + patch:
                raise ValueError('patch transcript mismatch')
        else:
            block = (index-2)*3
            count = min(3, 43-block)
            if command != bytes([2, 0x23, block, count-1]) or len(response) != 1+count*8:
                raise ValueError('read transcript command or length mismatch')
            assembled.extend(response[1:])
    if assembled != raw:
        raise ValueError('FRAM differs from read transcript')
    return uid, patch, raw


def family_gate(uid, patch):
    """Historical EU/gen1 candidate gate, not verified product identification."""
    if len(uid) != 8 or uid[6:] != b'\x07\xe0' or len(patch) != 6:
        raise ValueError('unsupported UID manufacturer/order or patch dimensions')
    high, low = patch[2] >> 4, patch[2] & 15
    if patch[0] not in (0xC5, 0x9D) or patch[3] != 1 or not (
            (high == 3 and low < 9) or (high == 7 and low < 4)):
        raise ValueError('unsupported family: only historical EU2/gen1 candidate gate accepted')


def section_crcs(data):
    if len(data) != 344:
        raise ValueError('integrity requires exactly 344 bytes')
    result = []
    for name, start, end in [('header', 0, 24), ('body', 24, 320), ('footer', 320, 344)]:
        stored = int.from_bytes(data[start:start+2], 'little')
        computed = crc16_libre(data[start+2:end])
        result.append({'name': name, 'stored_range': [start, start+2],
                       'data_range': [start+2, end], 'stored_hex': f'{stored:04x}',
                       'computed_hex': f'{computed:04x}', 'passed': stored == computed})
    return result


def load_capture(path):
    import json
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError('duplicate JSON key')
            result[key] = value
        return result
    with open(path, encoding='utf-8') as handle:
        return json.load(handle, object_pairs_hook=pairs,
                         parse_constant=lambda _: (_ for _ in ()).throw(ValueError('nonfinite JSON number')))


def main(argv=None):
    import argparse, hashlib, json, os, pathlib, sys
    parser = argparse.ArgumentParser(description='Experimental offline EU Libre2 FRAM integrity only. No glucose or treatment use.')
    parser.add_argument('capture', type=pathlib.Path)
    parser.add_argument('--output-dir', required=True, type=pathlib.Path, help='new private directory; never overwrite')
    args = parser.parse_args(argv)
    try:
        uid, patch, raw = validate_capture(load_capture(args.capture))
        family_gate(uid, patch)
        decrypted = decrypt_fram(uid, patch, raw)
        raw_crc, decrypted_crc = section_crcs(raw), section_crcs(decrypted)
        passed = all(c['passed'] for c in decrypted_crc)
        report = {'schema': 'experimental-libre2-offline-integrity', 'schema_version': 1,
                  'experimental': True, 'glucose_available': False, 'clinical_validity_claimed': False,
                  'not_for_treatment': True, 'uid_disclosed': False,
                  'family': 'historical standard EU2/gen1 candidate', 'family_verified': False,
                  'family_gate_passed': True, 'capture_validation': 'pass',
                  'transcript_validation': 'pass; SDK bytes only, not physical link CRC or scan freshness',
                  'fram_length': 344, 'reference_revision': 'e6a909c88faeada49f461d30834174cd95db4042',
                  'transform': 'Libre2.decryptFRAM .libre2; wire UID unchanged; 43 blocks',
                  'raw_sha256': hashlib.sha256(raw).hexdigest(),
                  'decrypted_sha256': hashlib.sha256(decrypted).hexdigest(),
                  'raw_crc_sections': raw_crc, 'decrypted_crc_sections': decrypted_crc,
                  'integrity_status': 'pass' if passed else 'fail',
                  'snapshot_stability': 'not established; sequential reads',
                  'warning': 'Byte integrity only; no physiology, glucose, clinical validity, or independent scan claim.'}
        old_umask = os.umask(0o077)
        try:
            args.output_dir.mkdir(mode=0o700)
            for name, content in [('raw.bin', raw), ('decrypted.bin', decrypted),
                                  ('report.json', (json.dumps(report, indent=2)+'\n').encode())]:
                fd = os.open(args.output_dir / name, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
                with os.fdopen(fd, 'wb') as handle:
                    handle.write(content)
        finally:
            os.umask(old_umask)
        print(json.dumps({'experimental': True, 'glucose_available': False,
                          'integrity_status': report['integrity_status'],
                          'crc_sections_passed': [c['passed'] for c in decrypted_crc]}))
        return 0 if passed else 3
    except (ValueError, OSError, UnicodeError):
        print(json.dumps({'experimental': True, 'glucose_available': False,
                          'error': 'invalid capture, unsupported family, or unavailable new output directory'}), file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
