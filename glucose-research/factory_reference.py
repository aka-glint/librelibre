"""Offline RESEARCH reference for DiaBLE's older temperature-based formula.

NOT Abbott's current complete algorithm. NEVER imported by the Flipper app.
No crude raw/10 fallback: absent calibration is an error, not glucose.
MIT adaptation, Copyright (c) 2026 Guido Soranzio. See ../offline/reference/LICENSE.md.
Pinned source and exact table SHA bound below; see GLUCOSE-FEASIBILITY.md.
"""
import hashlib
import math
import re
from pathlib import Path


def candidate(record, coefficients):
    c = coefficients
    if not isinstance(record, dict) or not isinstance(c, dict):
        raise ValueError('record and calibration must be objects')
    bounds = {'i1': (0, 7), 'i2': (1, 1023), 'i3': (-255, 255),
              'i4': (0, 16383), 'i5': (0, 16380), 'i6': (1, 16380)}
    for key, (low, high) in bounds.items():
        if type(c.get(key)) is not int or not low <= c[key] <= high:
            raise ValueError('invalid factory coefficient: ' + key)
    bounds = {'raw_adc': (1, 16383), 'raw_temperature': (1, 16380),
              'temperature_adjustment': (-2044, 2044), 'sensor_minute': (0, 65535)}
    for key, (low, high) in bounds.items():
        if type(record.get(key)) is not int or not low <= record[key] <= high:
            raise ValueError('invalid raw record field: ' + key)
    if record.get('has_error') is not False or type(record.get('quality_bits')) is not int or record['quality_bits'] != 0 or type(record.get('quality_flags')) is not int or record['quality_flags'] != 0:
        raise ValueError('error or unknown quality record; no calibration fallback')
    if c['i4'] <= c['i3'] or record['temperature_adjustment'] + c['i6'] <= 0:
        raise ValueError('invalid calibration denominator')
    source = (Path(__file__).resolve().parent / 'reference/diable-Glucose.swift').read_bytes()
    if hashlib.sha256(source).hexdigest() != 'ed44d885bf2af099c1d1fc3cc6baa60d51e1b1c4dd9d9d4e1ea584bf00387e94':
        raise ValueError('pinned formula/table source mismatch')
    text = source.decode('utf-8')
    tables = {name: [float(v) for v in re.search(r'let ' + name + r' = \[(.*?)\]', text, re.S)[1].split(',') if v.strip()]
              for name in ('t1', 't2')}
    c = coefficients
    resistance = record['raw_temperature'] * 72500.0 / (record['temperature_adjustment'] + c['i6']) - 1000.0
    log_r = math.log(resistance)
    temperature = 1.0 / (log_r**3 * 0.00000005283566 + log_r**2 * 0.0000007061775 + log_r * 0.0001964561 + 0.0009180023) - 273.15
    g1 = 65.0 * (record['raw_adc'] - c['i3']) / (c['i4'] - c['i3'])
    g2 = 1.045 ** (32.5 - temperature)
    value = (g1 * g2 - tables['t1'][c['i2']-1]) / tables['t2'][c['i2']-1]
    if not math.isfinite(temperature) or not math.isfinite(value) or value <= 0:
        raise ValueError('nonfinite or nonpositive research candidate')
    rounded = math.floor(value + 0.5)
    return dict(temperature_c=temperature, unrounded_candidate_mg_dl=value, candidate_mg_dl=rounded,
                glucose_available=False, clinical_validity_claimed=False,
                algorithm='DiaBLE older LibreLink-2.3-derived factory formula; research only')
