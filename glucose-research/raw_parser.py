"""Research-only decrypted EU2 FRAM metadata. Never a glucose UI.

DiaBLE pinned Libre.swift:116-145,179-194; Sensor.swift:77-98.
Copyright (c) 2026 Guido Soranzio; MIT (../offline/reference/LICENSE.md).
No sensor I/O. Unknown freshness always blocks a current-reading claim.
"""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'offline'))
from libre2_offline import section_crcs


def parse(data, patch):
    if len(data) != 344 or len(patch) != 6:
        raise ValueError('requires decrypted FRAM344 and patch6')
    if patch[0] not in (0xC5, 0x9D) or patch[2] != 0x30 or patch[3] != 1:
        raise ValueError('unsupported historical standard EU2 profile')
    if not all(c['passed'] for c in section_crcs(data)):
        raise ValueError('all three FRAM CRCs must pass')
    if data[322:324] != patch[2:4]:
        raise ValueError('footer patch profile mismatch')
    if data[26] >= 16 or data[27] >= 32:
        raise ValueError('invalid ring index')
    statuses = {0: 'unknown', 1: 'not_activated', 2: 'warming_up', 3: 'active',
                4: 'expired', 5: 'shutdown', 6: 'failure'}
    age = int.from_bytes(data[316:318], 'little')
    max_life = int.from_bytes(data[326:328], 'little')
    trend = []
    for i in range(16):
        slot = (data[26] - 1 - i) % 16
        word = int.from_bytes(data[28 + slot * 6:34 + slot * 6], 'little')
        trend.append(dict(slot=slot, sensor_minute=age-i if age >= i else None,
                          within_sensor_age=age >= i, raw_adc=word & 0x3FFF,
                          quality_bits=(word >> 14) & 0x1FF, quality_flags=(word >> 23) & 3,
                          has_error=bool((word >> 25) & 1),
                          raw_temperature=((word >> 26) & 0xFFF) << 2,
                          temperature_adjustment=(((word >> 38) & 0x1FF) << 2) * (-1 if word >> 47 else 1)))
    footer = int.from_bytes(data[336:344], 'little')
    coefficients = dict(i1=data[2] & 7, i2=(int.from_bytes(data[2:4], 'little') >> 3) & 0x3FF,
                        i3=(footer & 255) * (-1 if (footer >> 33) & 1 else 1),
                        i4=(footer >> 8) & 0x3FFF, i5=((footer >> 40) & 0xFFF) << 2,
                        i6=((footer >> 52) & 0xFFF) << 2)
    blockers = []
    if data[4] != 3:
        blockers.append('sensor_not_active')
    if max_life == 0 or age > max_life:
        blockers.append('sensor_life_exceeded' if max_life else 'sensor_life_unknown')
    if trend[0]['has_error']:
        blockers.append('newest_record_error')
    if trend[0]['quality_bits'] or trend[0]['quality_flags']:
        blockers.append('newest_record_quality')
    blockers.extend(['freshness_unestablished', 'calibrated_algorithm_unvalidated'])
    return dict(status_code=data[4], status=statuses.get(data[4], 'unknown'), age_minutes=age,
                max_life_minutes=max_life, trend_index=data[26], history_index=data[27], trend=trend,
                glucose_available=False, freshness='unestablished', sample_age_seconds=None,
                factory_coefficients=coefficients, blockers=blockers)
