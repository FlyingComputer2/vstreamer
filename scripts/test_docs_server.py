#!/usr/bin/env python3
"""Flask test client checks for docs_server PUT token."""

from __future__ import annotations

import tempfile
import unittest
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from docs_server import create_app  # noqa: E402


class DocsServerTokenTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.docs_dir = Path(self.tmp.name)
        (self.docs_dir / "readme.md").write_text("# Readme\n", encoding="utf-8")
        self.token = "test-token-abc"
        self.app = create_app(self.docs_dir, self.token)
        self.client = self.app.test_client()

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_get_without_token(self) -> None:
        resp = self.client.get("/readme.md")
        self.assertEqual(resp.status_code, 200)

    def test_put_without_token_forbidden(self) -> None:
        resp = self.client.put("/api/doc/new.md", data="# New\n", content_type="text/markdown")
        self.assertEqual(resp.status_code, 403)

    def test_put_wrong_token_forbidden(self) -> None:
        resp = self.client.put(
            "/api/doc/new.md",
            data="# New\n",
            content_type="text/markdown",
            headers={"X-Docs-Token": "wrong"},
        )
        self.assertEqual(resp.status_code, 403)

    def test_put_with_token_ok(self) -> None:
        resp = self.client.put(
            "/api/doc/new.md",
            data="# New\n",
            content_type="text/markdown",
            headers={"X-Docs-Token": self.token},
        )
        self.assertEqual(resp.status_code, 200)
        self.assertTrue((self.docs_dir / "new.md").is_file())


if __name__ == "__main__":
    unittest.main()
