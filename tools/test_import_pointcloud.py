import hashlib
import json
import struct
import tempfile
import unittest
from pathlib import Path
import importlib.util


MODULE_PATH = Path(__file__).with_name("import_pointcloud.py")
SPEC = importlib.util.spec_from_file_location("import_pointcloud", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


class ImportPointCloudTest(unittest.TestCase):
    def test_golden_bytes_and_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "root"
            root.mkdir()
            source = Path(directory) / "points.txt"
            source.write_text("1 2 3 label\n4.5 5.5 6.5 ignored\n\n", encoding="utf-8")
            manifest = Path(directory) / "output.json"
            result = MODULE.import_pointcloud(source, root, "pc-1", "workpiece", manifest)
            expected = struct.pack("<fff", 1.0, 2.0, 3.0) + struct.pack(
                "<fff", 4.5, 5.5, 6.5)
            path = root / "inputs" / "pc-1" / "pointcloud.xyzf32le"
            self.assertEqual(path.read_bytes(), expected)
            self.assertEqual(result["size_bytes"], 24)
            self.assertEqual(result["point_count"], 2)
            self.assertEqual(result["sha256"], hashlib.sha256(expected).hexdigest())
            self.assertEqual(json.loads(manifest.read_text(encoding="utf-8")), result)

    def test_rejects_malformed_nonfinite_empty_and_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "root"
            root.mkdir()
            for name, text in (("bad", "1 2\n"), ("nan", "NaN 1 2\n"),
                               ("empty", "\n")):
                source = Path(directory) / f"{name}.txt"
                source.write_text(text, encoding="utf-8")
                with self.assertRaises(ValueError):
                    MODULE.import_pointcloud(source, root, name, "frame",
                                             Path(directory) / f"{name}.json")
            source = Path(directory) / "ok.txt"
            source.write_text("0 0 0\n", encoding="utf-8")
            MODULE.import_pointcloud(source, root, "same", "frame",
                                     Path(directory) / "same.json")
            with self.assertRaises(ValueError):
                MODULE.import_pointcloud(source, root, "same", "frame",
                                         Path(directory) / "same2.json")


if __name__ == "__main__":
    unittest.main()
