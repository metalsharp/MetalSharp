"""Tests for tools/sparkle/sparkle_release.py's naming, release selection and appcast."""

import sys
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import sparkle_release as sr  # noqa: E402

NS = {"sparkle": sr.SPARKLE_NS}


def release(tag, *names, draft=False, prerelease=False):
    return {"tag_name": tag, "draft": draft, "prerelease": prerelease, "assets": [{"name": n} for n in names]}


class Names(unittest.TestCase):
    def test_names_follow_the_dmgs(self):
        self.assertEqual(sr.archive_name("1.3.0", "regular"), "MetalSharp-1.3.0-arm64.zip")
        self.assertEqual(sr.archive_name("1.3.0", "fex"), "MetalSharp-1.3.0-arm64-FEX.zip")
        self.assertEqual(sr.delta_name("1.3.1", "fex", "1.3.0"), "MetalSharp-1.3.1-arm64-FEX-from1.3.0.delta")
        self.assertEqual(sr.appcast_name("regular"), "appcast.xml")
        self.assertEqual(sr.appcast_name("fex"), "appcast-fex.xml")
        self.assertEqual(
            sr.download_url("1.3.1", "MetalSharp-1.3.1-arm64.zip"),
            "https://github.com/metalsharp/MetalSharp/releases/download/v1.3.1/MetalSharp-1.3.1-arm64.zip",
        )
        self.assertEqual(sr.download_url("1.3.1", "x.zip", base_url="http://127.0.0.1:8586/"),
                         "http://127.0.0.1:8586/x.zip")


class PreviousVersions(unittest.TestCase):
    def test_only_earlier_published_releases_with_this_builds_archive(self):
        releases = [
            release("v1.4.0", "MetalSharp-1.4.0-arm64.zip"),  # this release or newer
            release("v1.3.2", "MetalSharp-1.3.2-arm64-FEX.zip"),  # the other build only
            release("v1.3.1", "MetalSharp-1.3.1-arm64.zip", "MetalSharp-1.3.1-arm64-FEX.zip"),
            release("v1.3.0", "MetalSharp-1.3.0-arm64.zip"),
            release("v1.2.9", "MetalSharp-1.2.9-arm64.zip", draft=True),
            release("v1.2.8", "MetalSharp-1.2.8-arm64.zip", prerelease=True),
            release("v1.2.0", "MetalSharp-1.2.0-arm64.dmg"),  # before Sparkle
            release("v1.2.10", "MetalSharp-1.2.10-arm64.zip"),
        ]
        self.assertEqual(sr.previous_versions(releases, "1.4.0", "regular", 2), ["1.3.1", "1.3.0"])
        self.assertEqual(sr.previous_versions(releases, "1.4.0", "regular", 5), ["1.3.1", "1.3.0", "1.2.10"])
        self.assertEqual(sr.previous_versions(releases, "1.4.0", "fex", 2), ["1.3.2", "1.3.1"])
        self.assertEqual(sr.previous_versions(releases, "1.3.0", "regular", 2), ["1.2.10"])

    def test_versions_compare_numerically(self):
        self.assertLess(sr.version_key("1.2.9"), sr.version_key("1.2.10"))


class SignUpdate(unittest.TestCase):
    def test_parses_sign_update_output(self):
        self.assertEqual(
            sr.parse_signature('sparkle:edSignature="abc+/=" length="1234"\n'), ("abc+/=", 1234)
        )
        with self.assertRaises(ValueError):
            sr.parse_signature("ERROR")


class Appcast(unittest.TestCase):
    def item(self, deltas=True):
        archive = sr.Signed("MetalSharp-1.3.1-arm64-FEX.zip",
                            sr.download_url("1.3.1", "MetalSharp-1.3.1-arm64-FEX.zip"), 1500, "SIG&<>")
        return sr.Item(
            version="1.3.1",
            variant="fex",
            archive=archive,
            deltas=[
                sr.Delta("a.delta", "https://example.com/a.delta", 10, "D1", from_version="1.2.10"),
                sr.Delta("b.delta", "https://example.com/b.delta", 20, "D2", from_version="1.3.0"),
            ] if deltas else [],
            notes_url="https://github.com/metalsharp/MetalSharp/releases/tag/v1.3.1",
        )

    def test_one_item_with_its_archive_and_deltas(self):
        root = ET.fromstring(sr.appcast_xml(self.item()))
        items = root.findall("./channel/item")
        self.assertEqual(len(items), 1)
        item = items[0]
        self.assertEqual(item.findtext("sparkle:version", namespaces=NS), "1.3.1")
        self.assertEqual(item.findtext("sparkle:shortVersionString", namespaces=NS), "1.3.1")
        self.assertIn("Arm64 with FEX", item.findtext("title"))
        enclosure = item.find("enclosure")
        self.assertEqual(enclosure.get("length"), "1500")
        self.assertEqual(enclosure.get(f"{{{sr.SPARKLE_NS}}}edSignature"), "SIG&<>")
        deltas = item.findall("sparkle:deltas/enclosure", NS)
        self.assertEqual([d.get(f"{{{sr.SPARKLE_NS}}}deltaFrom") for d in deltas], ["1.3.0", "1.2.10"])
        self.assertEqual(deltas[0].get(f"{{{sr.SPARKLE_NS}}}edSignature"), "D2")

    def test_no_deltas_element_without_deltas(self):
        root = ET.fromstring(sr.appcast_xml(self.item(deltas=False)))
        self.assertIsNone(root.find("./channel/item/sparkle:deltas", NS))


if __name__ == "__main__":
    unittest.main()
