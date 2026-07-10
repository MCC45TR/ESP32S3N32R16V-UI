#!/usr/bin/env python3
"""Behavior tests for the first-party secret scanner."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from scan_secrets import scan_file


class SecretScanTests(unittest.TestCase):
    def rules(self, name: str, text: str) -> set[str]:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
            return {str(issue["rule"]) for issue in scan_file(path, root)}

    def test_hardcoded_password_is_rejected(self) -> None:
        self.assertIn("literal_secret_assignment", self.rules("config.cpp", 'password = "realistic-long-value";'))

    def test_variable_password_assignment_is_allowed(self) -> None:
        self.assertEqual(self.rules("handler.cpp", "password = request.value();"), set())

    def test_placeholder_define_is_allowed(self) -> None:
        self.assertEqual(self.rules("example.h", '#define WIFI_PASSWORD "<SET_WIFI_PASSWORD>"'), set())

    def test_hardcoded_secret_define_is_rejected(self) -> None:
        self.assertIn("literal_secret_define", self.rules("config.h", '#define API_TOKEN "realistic-long-token"'))

    def test_private_key_block_is_rejected(self) -> None:
        self.assertIn("private_key_block", self.rules("material.txt", "-----BEGIN PRIVATE KEY-----"))

    def test_github_token_is_rejected(self) -> None:
        token = "ghp_" + "A" * 36
        self.assertIn("github_token", self.rules("config.txt", token))


if __name__ == "__main__":
    unittest.main()
