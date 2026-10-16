"""Pinned firmware u8g2 width calculations only, not a physical screen test."""
from pathlib import Path
import json
import os
import re
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class Layout(unittest.TestCase):
    def test_draw_literals_and_full_numeric_range_fit_128_pixels(self):
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as tmp:
            exe=Path(tmp)/'width'
            subprocess.run(['gcc','-w','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-I'+str(ROOT/'tests/font-reference'),*[str(ROOT/'tests/font-reference'/n) for n in ['width.c','fonts.c','u8g2_font.c','u8x8_8x8.c']],'-o',str(exe)],check=True)
            draw=(ROOT/'app.c').read_text().split('static void draw(',1)[1].split('\n}\n',1)[0]
            texts=[json.loads('"'+s+'"') for s in re.findall(r'"([^"\n]*)"',draw) if '%' not in s]
            texts+=['Age65535/65535m Q1FF/3','CRC OK; unknown(255)','Life exceeded; quality']
            # Freshness + remaining-life + rescan strings (sensor clock only).
            texts+=['Fresh 0m=now','Fresh 5m=old','Fresh unknown',
                    'Left 12d 6h','Left 7h 12m','Left 45d 12h','Left 10h 28m',
                    'Left 12d 6h; record clear','Left 12d 6h; quality',
                    'Life exceeded','Life unknown','Rescan the sensor','No usable value']
            # The combined estimate row (freshness; remaining life) must fit 128.
            for combined in ['Fresh 0m=now; Left 12d 6h',
                             'Fresh 0m=now; Life exceeded',
                             'Fresh unknown; Life unknown']:
                texts.append(combined)
            # Result header row: the acquisition clock alone, right-aligned.
            def width(font,text):
                return int(subprocess.check_output([str(exe),str(font),text],text=True).split(' ',1)[0])
            # Every hex glyph in each of 16 positions, including widest repeated glyph.
            uid_widths=[width(0,c*16) for c in '0123456789ABCDEF']
            self.assertLessEqual(max(uid_widths),128)
            print('Full 16-hex wire UID maximum pinned-font width:',max(uid_widths),'px')
            self.assertLessEqual(width(0,'EU2 sensor reader'),128)
            self.assertLessEqual(width(0,'Read-only; SD logs the scan'),128)
            self.assertLessEqual(98+width(0,'23:59'),128)
            self.assertLessEqual(98+width(0,'--:--'),128)
            # The DERIVED sensor serial (11 chars) has its own result row; the
            # "Serial " prefix plus the widest possible serial must fit 128 px,
            # and the safety rows it shares a page with must still fit.
            worst='Serial '+('0'+'W'*10)   # digit + 10 widest glyphs (7 px each)
            self.assertLessEqual(width(0,worst),128,worst)
            for row in ['Serial 3M00009DHCR','Serial 3DTAM8DT224','Serial unavailable',
                        'Serial -----------',worst,
                        '<:info >:scan','Full 16-hex wire UID','Serial derived from UID',
                        'Serial unavailable  <:UID >:scan','Freshness unknown']:
                self.assertLessEqual(width(0,row),128,row)
            print('Derived serial row worst-case pinned-font width:',width(0,worst),'px')
            widths={}
            for font,items in [(0,texts),(1,[f'{value//10}.{value%10} mmol/L' for value in range(5556)])]:
                for offset in range(0,len(items),300):
                    run=subprocess.run([str(exe),str(font),*items[offset:offset+300]],capture_output=True,text=True,check=True)
                    for line in run.stdout.splitlines():
                        width,text=line.split(' ',1);self.assertLessEqual(int(width),128,text)
                        widths[text]=int(width)
            print('Pinned-font width:',len(widths),'strings; numeric 0.0..555.5 mmol/L, includes 100+; max',max(widths.values()),'px')
if __name__=='__main__':unittest.main()