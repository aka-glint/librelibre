"""Regression guard for actual app inventory completion policy, not RF simulation."""
from pathlib import Path
import unittest

class PersistentScan(unittest.TestCase):
    def test_inventory_has_no_attempt_or_elapsed_completion(self):
        source = (Path(__file__).resolve().parents[1] / 'app.c').read_text()
        self.assertNotIn('app->attempts >=', source, 'Inventory must wait until UID or user cancellation')
        self.assertNotIn('elapsed >= furi_ms_to_ticks', source, 'No automatic wall-clock scan completion')
        self.assertIn('Back: cancel', source)
        self.assertIn('char* json = malloc(LC_JSON_CAPACITY);', source, 'Raw transcript buffers must not overflow 4KiB app stack')
        self.assertIn('free(verified);', source)
        self.assertIn('free(json);', source)
        self.assertIn('EU2 sensor reader', source)
        self.assertIn('app->finished = true; /* Never retry/escalate a custom command. */', source)

if __name__ == '__main__':
    unittest.main()
