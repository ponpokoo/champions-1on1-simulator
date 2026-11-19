#!/usr/bin/env python3
"""Regression tests for P0-1: preventing accidental deletion/overwrite of search runs."""
import http.client
import json
import os
import sys
import tempfile
import threading
import unittest
from http.server import ThreadingHTTPServer
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import server  # noqa: E402


class _DummyProc:
    def __init__(self, returncode=0):
        self.stdout = iter(["[mock] done\n"])
        self._rc = returncode

    def wait(self):
        return self._rc

    def terminate(self):
        pass

    def kill(self):
        pass


class TestRunProtection(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp_runs = Path(self._tmp.name) / "runs"
        self.tmp_runs.mkdir(parents=True, exist_ok=True)
        self._orig_runs_dir = server.RUNS_DIR
        self._orig_jobs = dict(server._JOBS)
        server.RUNS_DIR = self.tmp_runs
        server._JOBS.clear()

    def tearDown(self):
        server.RUNS_DIR = self._orig_runs_dir
        server._JOBS.clear()
        server._JOBS.update(self._orig_jobs)
        self._tmp.cleanup()

    def _make_complete_run(self, name):
        d = self.tmp_runs / name
        d.mkdir(parents=True, exist_ok=True)
        cfg = d / "run_config.json"
        res = d / "finals_results.json"
        done = d / "DONE"
        cfg.write_text(json.dumps({"species": "gyarados", "tier": "light"}), encoding="utf-8")
        res.write_text(json.dumps({"species": "gyarados", "finalists": []}), encoding="utf-8")
        done.write_text("done\n", encoding="utf-8")
        return d, res

    def _make_incomplete_run(self, name):
        d = self.tmp_runs / name
        d.mkdir(parents=True, exist_ok=True)
        cfg = d / "run_config.json"
        marker = d / "stageA1.done"
        cfg.write_text(json.dumps({"species": "gyarados", "tier": "light"}), encoding="utf-8")
        marker.write_text("ok\n", encoding="utf-8")
        return d, marker

    def test_fresh_start_rejects_existing_complete_run_without_touching_files(self):
        _, res = self._make_complete_run("gyarados-light")
        before_bytes = res.read_bytes()
        before_mtime_ns = res.stat().st_mtime_ns

        with self.assertRaises(server.RunConflictError) as ctx:
            server.start_search_job({
                "species": "gyarados",
                "tier": "light",
                "runName": "gyarados-light",
                "shards": 2,
            })

        exc = ctx.exception
        self.assertEqual(exc.code, "RUN_EXISTS")
        self.assertEqual(exc.suggested_name, "gyarados-light-2")
        self.assertTrue(exc.is_complete)
        self.assertFalse(exc.can_resume)
        self.assertEqual(res.read_bytes(), before_bytes)
        self.assertEqual(res.stat().st_mtime_ns, before_mtime_ns)

    def test_fresh_start_rejects_existing_incomplete_run_and_offers_resume(self):
        _, marker = self._make_incomplete_run("gyarados-light")
        before_bytes = marker.read_bytes()
        before_mtime_ns = marker.stat().st_mtime_ns

        with self.assertRaises(server.RunConflictError) as ctx:
            server.start_search_job({
                "species": "gyarados",
                "tier": "light",
                "runName": "gyarados-light",
                "shards": 2,
            })

        exc = ctx.exception
        self.assertEqual(exc.code, "RUN_EXISTS")
        self.assertEqual(exc.suggested_name, "gyarados-light-2")
        self.assertFalse(exc.is_complete)
        self.assertTrue(exc.can_resume)
        self.assertEqual(marker.read_bytes(), before_bytes)
        self.assertEqual(marker.stat().st_mtime_ns, before_mtime_ns)

    def test_sequential_name_suggestion_skips_taken_names(self):
        self._make_complete_run("gyarados-light")
        self._make_complete_run("gyarados-light-2")
        self.assertEqual(server._suggest_unique_run_name("gyarados-light"), "gyarados-light-3")
        self.assertEqual(server._suggest_unique_run_name("gyarados-light-2"), "gyarados-light-3")

    def test_fresh_start_with_distinct_name_and_valid_resume_succeed(self):
        self._make_complete_run("gyarados-light")
        inc_dir, marker = self._make_incomplete_run("gyarados-wip")

        with mock.patch.object(server, "ensure_binaries"), \
             mock.patch.object(server.population3, "load_domain", return_value={}), \
             mock.patch.object(server.subprocess, "Popen", return_value=_DummyProc(0)):
            r_new = server.start_search_job({
                "species": "gyarados",
                "tier": "light",
                "runName": "gyarados-light-2",
                "shards": 2,
            })
            self.assertEqual(r_new["run"], "gyarados-light-2")
            self.assertTrue((self.tmp_runs / "gyarados-light-2").is_dir())

            r_res = server.start_search_job({
                "runName": "gyarados-wip",
                "resume": True,
                "shards": 2,
            })
            self.assertEqual(r_res["run"], "gyarados-wip")
            self.assertEqual(r_res["status"], "running")
            self.assertTrue(marker.exists())
            self.assertEqual(inc_dir, self.tmp_runs / "gyarados-wip")

    def test_resume_rejects_already_complete_run(self):
        self._make_complete_run("gyarados-done")
        with self.assertRaises(server.RunConflictError) as ctx:
            server.start_search_job({"runName": "gyarados-done", "resume": True})
        self.assertEqual(ctx.exception.code, "RUN_COMPLETE")
        self.assertTrue(ctx.exception.is_complete)

    def test_running_job_conflict_rejected(self):
        server._JOBS["gyarados-active"] = {"status": "running"}
        with self.assertRaises(server.RunConflictError) as ctx:
            server.start_search_job({
                "species": "gyarados",
                "tier": "light",
                "runName": "gyarados-active",
            })
        self.assertEqual(ctx.exception.code, "RUN_RUNNING")

    def test_path_validation_rejects_traversal_and_special_chars(self):
        for bad in ("", "   ", "../outside", "a/../b", "C:evil", "bad\x00name", "."):
            with self.subTest(bad=bad):
                with self.assertRaises(ValueError):
                    server._safe_run_dir(bad)

    def test_symlink_run_dir_rejected(self):
        real_dir = self.tmp_runs / "real-run"
        real_dir.mkdir()
        link_dir = self.tmp_runs / "link-run"
        try:
            os.symlink(real_dir, link_dir, target_is_directory=True)
        except OSError as exc:
            self.skipTest(f"symlink creation not permitted in this environment: {exc}")
        with self.assertRaises(ValueError):
            server._safe_run_dir("link-run")

    def test_symlink_guard_rejects_mocked_symlink(self):
        orig_is_symlink = Path.is_symlink

        def fake_is_symlink(p):
            if p.name == "sym-run":
                return True
            return orig_is_symlink(p)

        with mock.patch.object(Path, "is_symlink", fake_is_symlink):
            with self.assertRaises(ValueError):
                server._safe_run_dir("sym-run")

    def test_http_endpoint_returns_409_and_preserves_existing_results(self):
        _, res = self._make_complete_run("gyarados-light")
        before_bytes = res.read_bytes()
        before_mtime_ns = res.stat().st_mtime_ns

        httpd = ThreadingHTTPServer(("127.0.0.1", 0), server.WorkbenchHandler)
        port = httpd.server_address[1]
        th = threading.Thread(target=httpd.serve_forever, daemon=True)
        th.start()
        try:
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
            body = json.dumps({
                "species": "gyarados",
                "tier": "light",
                "runName": "gyarados-light",
                "shards": 2,
            })
            conn.request("POST", "/api/search/start", body=body, headers={"Content-Type": "application/json"})
            resp = conn.getresponse()
            raw = resp.read().decode("utf-8")
            data = json.loads(raw)
            conn.close()
        finally:
            httpd.shutdown()
            httpd.server_close()
            th.join(timeout=2)

        self.assertEqual(resp.status, 409)
        self.assertEqual(data["code"], "RUN_EXISTS")
        self.assertEqual(data["suggestedRunName"], "gyarados-light-2")
        self.assertTrue(data["isComplete"])
        self.assertFalse(data["canResume"])
        self.assertEqual(res.read_bytes(), before_bytes)
        self.assertEqual(res.stat().st_mtime_ns, before_mtime_ns)

    def test_bundled_sample_files_and_js_wrapper_match(self):
        ex_dir = HERE / "examples" / "gyarados-light"
        finals = json.loads((ex_dir / "finals_results.json").read_text(encoding="utf-8"))
        beginner = json.loads((ex_dir / "beginner_scores.json").read_text(encoding="utf-8"))
        meta = json.loads((ex_dir / "sample_meta.json").read_text(encoding="utf-8"))
        js_text = (HERE / "examples" / "sample_data.js").read_text(encoding="utf-8")
        prefix = "window.CHAMPIONS_SAMPLES = "
        self.assertIn(prefix, js_text)
        payload = json.loads(js_text.split(prefix, 1)[1].rsplit(";", 1)[0])
        self.assertGreaterEqual(len(payload), 1)
        sample = payload[0]
        self.assertEqual(sample["meta"], meta)
        self.assertEqual(sample["data"]["finalists"], finals["finalists"])
        self.assertEqual(sample["data"]["matrix"], finals["matrix"])
        self.assertEqual(sample["data"]["beginner_scores"], beginner)

    def test_http_serves_bundled_sample_js(self):
        httpd = ThreadingHTTPServer(("127.0.0.1", 0), server.WorkbenchHandler)
        port = httpd.server_address[1]
        th = threading.Thread(target=httpd.serve_forever, daemon=True)
        th.start()
        try:
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
            conn.request("GET", "/examples/sample_data.js")
            resp = conn.getresponse()
            body = resp.read().decode("utf-8")
            ctype = resp.getheader("Content-Type", "")
            conn.close()
        finally:
            httpd.shutdown()
            httpd.server_close()
            th.join(timeout=2)

        self.assertEqual(resp.status, 200)
        self.assertIn("javascript", ctype)
        self.assertIn("window.CHAMPIONS_SAMPLES", body)


if __name__ == "__main__":
    unittest.main()
