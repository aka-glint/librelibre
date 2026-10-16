# LibreLibre integrity and raw-status attribution

`integrity.c` adapts historical transform, CRC and raw sensor metadata from
[gui-dos/DiaBLE](https://github.com/gui-dos/DiaBLE), revision
`e6a909c88faeada49f461d30834174cd95db4042`.

Pinned sources retained in `offline/reference/`: `Libre2.swift` (EU2 transform),
`Extensions.swift` (CRC support), `Libre.swift:116-145,179-181,247-261`
(status/age/rings/newest record/max-life/CRC). Exact status enum is in
`glucose-research/reference/diable-Sensor.swift:77-98`.
`glucose-research/raw_parser.py` supplies the conservative application profile,
footer and index gates and the same raw-field extraction for differential tests.

Only raw diagnostics are integrated. No glucose conversion, calibration,
activation, sensor writes or life modification is included. Status and stored
age/life do not establish freshness, sensor authenticity or clinical validity.

## Upstream license (verbatim)

MIT License

Copyright (c) 2026 Guido Soranzio

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
