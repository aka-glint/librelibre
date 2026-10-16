"""Real host C helpers; synthetic buffers only, never sensor I/O."""
from pathlib import Path
import ctypes as C
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class RawProtocol(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='libre-raw-', dir=os.environ.get('TMPDIR'))
        lib = Path(cls.tmp.name) / 'protocol.so'
        subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic', '-shared', '-fPIC', str(ROOT/'protocol.c'), '-o', str(lib)], check=True)
        cls.lib = C.CDLL(str(lib))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_candidate_gate_matches_captured_patch_not_all_ti(self):
        self.assertTrue(hasattr(self.lib, 'lc_family_allowed'), 'Missing conservative EU2/gen1 family gate')
        gate = self.lib.lc_family_allowed
        gate.argtypes = [C.c_void_p, C.c_void_p, C.c_size_t]
        gate.restype = C.c_bool
        uid = (C.c_uint8*8)(0,0,0,0,0,0,7,0xe0)  # synthetic UID
        def check(p, size=6):
            return gate(uid, (C.c_uint8*6)(*p), size)
        self.assertTrue(check(bytes.fromhex('C50930010000')))
        for first in range(256):
            for generation in range(256):
                expected = first in (0x9d,0xc5) and ((generation>>4==3 and generation&15<9) or (generation>>4==7 and generation&15<4))
                self.assertEqual(check([first,0,generation,0,0,0]), expected)
        for size in range(10):
            if size != 6: self.assertFalse(check([0xc5,0,0x30,0,0,0], size))
        uid[6] = 2
        self.assertFalse(check([0xc5,0,0x30,0,0,0]))
        self.assertFalse(gate(None, None, 6))

    def test_exact_fifteen_read_frames_and_response_lengths(self):
        self.assertTrue(hasattr(self.lib, 'lc_read_command'), 'Missing bounded 43-block planner')
        plan = self.lib.lc_read_command
        plan.argtypes = [C.c_size_t, C.c_void_p]
        plan.restype = C.c_bool
        allowed = self.lib.lc_command_allowed
        allowed.argtypes = [C.c_void_p, C.c_size_t]
        allowed.restype = C.c_bool
        parse = self.lib.lc_read_parse
        parse.argtypes = [C.c_void_p, C.c_size_t, C.c_size_t, C.c_void_p]
        parse.restype = C.c_bool
        cmd = (C.c_uint8*4)()
        covered = 0
        for index in range(15):
            self.assertTrue(plan(index, cmd))
            self.assertEqual(bytes(cmd), bytes([2,0x23,index*3,2 if index<14 else 0]))
            self.assertTrue(allowed(cmd,4))
            blocks = cmd[3]+1
            covered += blocks
            out = (C.c_uint8*24)(*[99]*24)
            for length in range(28):
                response = (C.c_uint8*28)(0,*range(1,28))
                ok = parse(response, length, blocks, out)
                self.assertEqual(ok, length == 1+blocks*8)
                if ok: self.assertEqual(bytes(out)[:blocks*8], bytes(response)[1:1+blocks*8])
            for flags in range(1,256):
                response = (C.c_uint8*25)(flags)
                self.assertFalse(parse(response,1+blocks*8,blocks,out))
        self.assertEqual(covered,43)
        self.assertFalse(plan(15,cmd)); self.assertFalse(plan(0,None))
        for start in range(256):
            for count in range(256):
                frame = (C.c_uint8*4)(2,0x23,start,count)
                expected = (start<42 and start%3==0 and count==2) or (start==42 and count==0)
                self.assertEqual(allowed(frame,4),expected)
        self.assertFalse(parse(None,25,3,cmd)); self.assertFalse(parse(cmd,4,0,cmd))

if __name__ == '__main__': unittest.main()
