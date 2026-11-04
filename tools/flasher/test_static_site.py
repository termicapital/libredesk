from pathlib import Path
import unittest


HTML = Path(__file__).with_name("index.html")


class FlasherStaticSiteTests(unittest.TestCase):
    def test_installer_describes_the_private_live_data_build(self) -> None:
        page = HTML.read_text(encoding="utf-8")
        self.assertIn("Libre Desk — Live Data MVP Installer", page)
        self.assertIn("choose the 2.4 GHz Wi-Fi during installation", page)
        self.assertIn("contains a revocable device token and Wi-Fi password", page)
        self.assertIn("authenticated HTTPS bridge", page)
        self.assertIn("two successive real readings", page)
        self.assertIn("zero-padded 24-hour HH:MM clock", page)
        self.assertIn("centered glucose number", page)
        self.assertIn("larger clock", page)
        self.assertIn("without a visible unit label", page)
        self.assertIn("0.5.4-webserial-wifi", page)
        self.assertIn("Install and add Wi-Fi", page)
        self.assertIn("Network Name", page)
        self.assertIn("Password", page)
        self.assertIn("sent directly over the USB cable", page)
        self.assertIn("BOOT setup remains available as a fallback", page)
        self.assertIn('manifest="manifest.json"', page)
        self.assertNotIn("0.3.0-clock-ui", page)
        self.assertNotIn("0.2.0-runtime-foundation", page)
        self.assertNotIn("reading with unit", page)
        self.assertNotIn("temporary HTTPS bridge", page)
        self.assertNotIn("contains no Wi-Fi credentials", page)
        self.assertNotIn("Display Spike", page)


if __name__ == "__main__":
    unittest.main()
