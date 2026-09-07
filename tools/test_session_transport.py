import json
from pathlib import Path
import runpy
import tempfile
import unittest

create = runpy.run_path(str(Path(__file__).with_name("create-session-transport.py")))["create"]


class TransportInviteTests(unittest.TestCase):
    def test_unique_invites_match_server_without_shared_keys(self):
        with tempfile.TemporaryDirectory() as parent:
            output = Path(parent) / "private"
            create(output, 16)
            server = json.loads((output / "server.json").read_text())
            self.assertEqual(len(server["keys"]), 16)
            self.assertEqual(len(set(server["keys"].values())), 16)
            for number in range(1, 17):
                invite = json.loads((output / f"invite-{number}.json").read_text())
                self.assertTrue(server["keys"][invite["id"]] == invite["key"])
                self.assertEqual(len(bytes.fromhex(invite["key"])), 32)
                self.assertEqual(set(invite), {"version", "id", "key"})

    def test_existing_credentials_are_never_overwritten(self):
        with tempfile.TemporaryDirectory() as parent:
            output = Path(parent) / "private"
            create(output, 2)
            before = (output / "server.json").read_bytes()
            with self.assertRaises(FileExistsError):
                create(output, 2)
            self.assertTrue((output / "server.json").read_bytes() == before)

    def test_invalid_count_creates_nothing(self):
        with tempfile.TemporaryDirectory() as parent:
            output = Path(parent) / "private"
            for count in (0, -1, 17):
                with self.assertRaises(ValueError):
                    create(output, count)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
