#!/usr/bin/env python3
"""Local Python HTTP server for the Stage 8 Pokemon 1on1 HTML Workbench.

Uses only the Python 3 standard library. Connects viewer/index.html to:
  - engine/engine.c (battle, trace, screenx, matrix verification)
  - search/population3.py, search_tiers.py, search_controller.py (narrowed search)
  - runs/*/finals_results.json (saved search results)

Usage:
  python3 viewer/server.py [--host 127.0.0.1] [--port 8765] [--gcc gcc] [--no-browser]
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import webbrowser
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
RUNS_DIR = ROOT / "runs"
sys.path.insert(0, str(ROOT / "search"))
sys.path.insert(0, str(HERE))

import build_metadata
import population3
import search_tiers

MATRIX_MD5 = (ROOT / "engine" / "fixtures" / "expected_matrix_md5.txt").read_text(encoding="utf-8").strip()
EXE_SUFFIX = ".exe" if os.name == "nt" else ""
ENGINE_BIN = ROOT / "engine" / f"engine{EXE_SUFFIX}"
CATALOG_BIN = ROOT / "search" / f"population3_catalog{EXE_SUFFIX}"

_BUILD_LOCK = threading.Lock()
_JOBS_LOCK = threading.Lock()
_JOBS = {}
_MATRIX_VERIFIED = None
_GCC_PATH = "gcc"


def _safe_run_dir(name):
    if not name or not isinstance(name, str):
        raise ValueError("run name is required")
    clean = name.strip().replace("\\", "/").strip("/")
    if not clean or clean.startswith("/") or ".." in clean.split("/"):
        raise ValueError(f"invalid run path: {name}")
    target = (RUNS_DIR / clean).resolve()
    runs_root = RUNS_DIR.resolve()
    try:
        rel = target.relative_to(runs_root)
    except ValueError:
        raise ValueError(f"run path escapes runs directory: {name}") from None
    if str(rel) in ("", "."):
        raise ValueError("run name cannot be empty")
    return target, rel.as_posix()


def _log(msg):
    """Print an unbuffered status line to the console."""
    print(f"[viewer] {msg}", flush=True)


def ensure_binaries(gcc=None, verbose=False):
    """Build engine/catalog binaries and metadata if missing or stale."""
    cc = gcc or _GCC_PATH
    with _BUILD_LOCK:
        if verbose:
            _log("Checking engine binaries and metadata...")
        eng_src = ROOT / "engine" / "engine.c"
        if not ENGINE_BIN.exists() or eng_src.stat().st_mtime > ENGINE_BIN.stat().st_mtime:
            _log(f"Building {ENGINE_BIN.relative_to(ROOT)}...")
            p = subprocess.run(
                [cc, "-O3", "-march=native", "-o", str(ENGINE_BIN), str(eng_src), "-lm"],
                cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace",
            )
            if p.returncode != 0:
                raise RuntimeError(f"engine build failed: {p.stderr[-800:]}")
        cat_src = ROOT / "search" / "population3_catalog.c"
        if not CATALOG_BIN.exists() or cat_src.stat().st_mtime > CATALOG_BIN.stat().st_mtime:
            _log(f"Building {CATALOG_BIN.relative_to(ROOT)}...")
            p = subprocess.run(
                [cc, "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror",
                 str(cat_src), "-lm", "-o", str(CATALOG_BIN)],
                cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace",
            )
            if p.returncode != 0:
                raise RuntimeError(f"catalog build failed: {p.stderr[-800:]}")
        meta_path = HERE / "metadata.js"
        hdr_src = ROOT / "engine" / "stage8" / "species_data.h"
        if not meta_path.exists() or hdr_src.stat().st_mtime > meta_path.stat().st_mtime:
            _log(f"Generating {meta_path.relative_to(ROOT)}...")
            build_metadata.build_metadata(cc=cc, catalog_exe=CATALOG_BIN)


def _clean_env():
    return {
        k: v for k, v in os.environ.items()
        if k not in ("ENGINE_SEQ", "ENGINE_OBSERVER", "ENGINE_OBS_COMPAT")
    }


def verify_matrix(force=False, verbose=False):
    """Verify plain and compat matrix MD5 digests against the expected fixture."""
    global _MATRIX_VERIFIED
    if _MATRIX_VERIFIED is not None and not force:
        return _MATRIX_VERIFIED
    ensure_binaries()
    base_env = _clean_env()
    digests = {}
    modes = (("plain", {}), ("compat", {"ENGINE_OBSERVER": "1", "ENGINE_OBS_COMPAT": "1"}))
    for idx, (mode, extra) in enumerate(modes, start=1):
        if verbose:
            _log(f"Verifying engine matrix ({idx}/{len(modes)}: {mode}, 3000 games)...")
        started = time.monotonic()
        p = subprocess.run(
            [str(ENGINE_BIN), "matrix", "3000", "12345"],
            cwd=ROOT, env=dict(base_env, **extra), capture_output=True,
        )
        if p.returncode != 0:
            raise RuntimeError(f"matrix failed ({mode}): {p.stderr.decode('utf-8', 'replace')[-400:]}")
        digests[mode] = hashlib.md5(p.stdout.replace(b"\r\n", b"\n")).hexdigest()
        if verbose:
            _log(f"Verified {mode} matrix in {time.monotonic() - started:.1f}s ({digests[mode][:8]})")
    ok = digests["plain"] == MATRIX_MD5 and digests["compat"] == MATRIX_MD5
    _MATRIX_VERIFIED = {"ok": ok, "expected": MATRIX_MD5, **digests}
    return _MATRIX_VERIFIED


def _replace_policy(spec, policy):
    parts = spec.strip().split("|")
    if len(parts) != 8 or ":" not in parts[0]:
        raise ValueError("expected v3 spec with 8 fields")
    parts[2] = policy
    return "|".join(parts)


def validate_specs(specs, allowed_policies=("obsh4",)):
    ensure_binaries()
    check_lines = []
    for raw in specs:
        parts = raw.strip().split("|")
        if len(parts) != 8 or ":" not in parts[0]:
            raise ValueError("expected v3 spec with 8 fields")
        if parts[2] not in allowed_policies:
            raise ValueError(f"方針 '{parts[2]}' はこの操作では使用できません（許可: {', '.join(allowed_policies)}）")
        parts[2] = "obsh4"
        check_lines.append("|".join(parts))
    payload = "\n".join(check_lines) + "\n"
    p = subprocess.run(
        [str(CATALOG_BIN), "check"],
        input=payload, cwd=ROOT, env=_clean_env(),
        capture_output=True, text=True, encoding="utf-8", errors="replace",
    )
    if p.returncode != 0:
        raise ValueError((p.stderr or p.stdout or "invalid spec").strip())


def run_battle(spec_a, spec_b, games, seed):
    validate_specs([spec_a, spec_b], allowed_policies=("obsh4", "generic"))
    base_a = spec_a.split(":", 1)[0]
    base_b = spec_b.split(":", 1)[0]
    if base_a != base_b:
        raise ValueError(f"同種ミラー対戦のみ対応しています ({base_a} vs {base_b})")
    p = subprocess.run(
        [str(ENGINE_BIN), "battle", spec_a, spec_b, str(games), str(seed)],
        cwd=ROOT, env=_clean_env(),
        capture_output=True, text=True, encoding="utf-8", errors="replace",
    )
    if p.returncode != 0:
        raise ValueError((p.stderr or p.stdout or "battle failed").strip())
    out = p.stdout.strip()
    m = re.search(r"A\s+(\d+)\s*-\s*B\s+(\d+)\s*\(引分\s+(\d+)\)\s*平均([0-9.]+)ターン", out)
    if not m:
        raise RuntimeError(f"unexpected battle output: {out}")
    wa, wb, dr, avg_t = int(m.group(1)), int(m.group(2)), int(m.group(3)), float(m.group(4))
    total = wa + wb + dr
    return {
        "wA": wa,
        "wB": wb,
        "draws": dr,
        "games": total,
        "scoreA": (wa + 0.5 * dr) / total if total else 0.0,
        "scoreB": (wb + 0.5 * dr) / total if total else 0.0,
        "avgTurns": avg_t,
        "raw": out,
    }


def run_trace(spec_a, spec_b, seed):
    validate_specs([spec_a, spec_b], allowed_policies=("obsh4", "generic"))
    ga = _replace_policy(spec_a, "generic")
    gb = _replace_policy(spec_b, "generic")
    if ga.split(":", 1)[0] != gb.split(":", 1)[0]:
        raise ValueError("同種ミラー対戦のみ対応しています")
    p = subprocess.run(
        [str(ENGINE_BIN), "trace", ga, gb, str(seed)],
        cwd=ROOT, env=_clean_env(),
        capture_output=True, text=True, encoding="utf-8", errors="replace",
    )
    if p.returncode != 0:
        raise ValueError((p.stderr or p.stdout or "trace failed").strip())
    return {
        "seed": seed,
        "policyUsed": "generic (全情報ベースライン比較)",
        "log": p.stdout,
    }


def _parse_screenx(stdout_text):
    rows = []
    for line in stdout_text.splitlines():
        line = line.strip()
        if not line:
            continue
        nums = list(map(int, line.split()))
        rows.append((nums[0], nums[1:]))
    rows.sort(key=lambda x: x[0])
    return [r[1] for r in rows]


def run_screen_pool(specs, run_name, games, seed):
    if not specs or len(specs) > 32:
        raise ValueError("比較対象の型数は 1〜32 件で指定してください")
    run_dir, rel = _safe_run_dir(run_name)
    res_path = run_dir / "finals_results.json"
    fin_path = run_dir / "finalists.txt"
    if res_path.exists():
        fdata = json.loads(res_path.read_text(encoding="utf-8"))
        finalists = fdata["finalists"]
        ref_ranking = fdata.get("ranking_both_seats") or fdata.get("ranking") or []
    elif fin_path.exists():
        finalists = [ln for ln in fin_path.read_text(encoding="utf-8").splitlines() if ln.strip()]
        ref_ranking = []
    else:
        raise ValueError(f"決勝データが見つかりません: {rel}")
    if not finalists:
        raise ValueError("finalists is empty")
    if len(finalists) > 512:
        raise ValueError("engine screenx pool limit is 512 builds")

    validate_specs(specs)
    pool_species = finalists[0].split(":", 1)[0]
    for s in specs:
        if s.split(":", 1)[0] != pool_species:
            raise ValueError(f"種族が一致しません ({s.split(':', 1)[0]} != {pool_species})")

    with tempfile.TemporaryDirectory(prefix="viewer_screen_") as tmp:
        tmp_dir = Path(tmp)
        cand_f = tmp_dir / "user_cands.txt"
        pool_f = tmp_dir / "finalists.txt"
        cand_f.write_text("\n".join(specs) + "\n", encoding="utf-8")
        pool_f.write_text("\n".join(finalists) + "\n", encoding="utf-8")

        p_a = subprocess.run(
            [str(ENGINE_BIN), "screenx", str(cand_f), str(pool_f), str(games), str(seed)],
            cwd=ROOT, env=_clean_env(), capture_output=True, text=True, encoding="utf-8", errors="replace",
        )
        if p_a.returncode != 0:
            raise RuntimeError(f"screenx (seat A) failed: {p_a.stderr[-400:]}")
        p_b = subprocess.run(
            [str(ENGINE_BIN), "screenx", str(pool_f), str(cand_f), str(games), str(seed + 1)],
            cwd=ROOT, env=_clean_env(), capture_output=True, text=True, encoding="utf-8", errors="replace",
        )
        if p_b.returncode != 0:
            raise RuntimeError(f"screenx (seat B) failed: {p_b.stderr[-400:]}")

    rows_a = _parse_screenx(p_a.stdout)
    rows_b = _parse_screenx(p_b.stdout)
    ref_means = sorted((r["mean"] for r in ref_ranking), reverse=True)

    results = []
    for ci, spec in enumerate(specs):
        flat_a = rows_a[ci]
        per_opp = []
        scores_both = []
        scores_a = []
        for fi, fspec in enumerate(finalists):
            wa, la, da = flat_a[3 * fi:3 * fi + 3]
            sa = (wa + 0.5 * da) / (wa + la + da)
            wb, lb, db = rows_b[fi][3 * ci:3 * ci + 3]
            sb = (lb + 0.5 * db) / (wb + lb + db)
            both = 0.5 * (sa + sb)
            scores_a.append(sa)
            scores_both.append(both)
            per_opp.append({
                "finalistIdx": fi,
                "spec": fspec,
                "scoreBoth": round(both, 4),
                "scoreA": round(sa, 4),
                "scoreB": round(sb, 4),
            })
        mean_both = sum(scores_both) / len(scores_both)
        worst_both = min(scores_both)
        mean_a = sum(scores_a) / len(scores_a)
        worst_a = min(scores_a)
        est_rank = 1 + sum(1 for m in ref_means if m > mean_both) if ref_means else None
        per_opp_sorted = sorted(per_opp, key=lambda x: x["scoreBoth"])
        results.append({
            "spec": spec,
            "meanBoth": round(mean_both, 4),
            "worstBoth": round(worst_both, 4),
            "meanA": round(mean_a, 4),
            "worstA": round(worst_a, 4),
            "estimatedRank": est_rank,
            "poolSize": len(finalists),
            "worst5": per_opp_sorted[:5],
            "best5": list(reversed(per_opp_sorted[-5:])),
        })
    return {
        "run": rel,
        "species": pool_species,
        "gamesPerSide": games,
        "seed": seed,
        "policy": "obsh4",
        "results": results,
    }


def _require_nonempty_list(f, key, label):
    if key not in f or f[key] is None:
        return None
    val = f[key]
    if not isinstance(val, list):
        raise ValueError(f"{label} はリストで指定してください")
    if len(val) == 0:
        raise ValueError(f"{label} を1つ以上選択してください")
    return list(val)


def _parse_filters(payload):
    f = payload.get("filters")
    if f is None:
        return None
    if not isinstance(f, dict):
        raise ValueError("filters はオブジェクトで指定してください")

    allocs = None
    if f.get("allocations"):
        if not isinstance(f["allocations"], list):
            raise ValueError("allocations はリストで指定してください")
        allocs = []
        for idx, row in enumerate(f["allocations"], 1):
            if not isinstance(row, (list, tuple)) or len(row) != 6:
                raise ValueError(f"固定配分 {idx} 行目は 6 個の整数が必要です")
            pts = tuple(int(x) for x in row)
            if any(not 0 <= v <= 32 for v in pts) or sum(pts) != 66:
                raise ValueError(f"固定配分 {idx} 行目は各 0〜32・合計 66 にしてください")
            allocs.append(pts)

    nat_raw = _require_nonempty_list(f, "natures", "性格")
    natures = [tuple(map(int, n)) for n in nat_raw] if nat_raw is not None else None

    stat_min = tuple(map(int, f["stat_min"])) if f.get("stat_min") is not None else None
    stat_max = tuple(map(int, f["stat_max"])) if f.get("stat_max") is not None else None
    if stat_min is not None and (len(stat_min) != 6 or any(not 0 <= x <= 32 for x in stat_min)):
        raise ValueError("stat_min は 0〜32 の整数 6 個が必要です")
    if stat_max is not None and (len(stat_max) != 6 or any(not 0 <= x <= 32 for x in stat_max)):
        raise ValueError("stat_max は 0〜32 の整数 6 個が必要です")
    if stat_min is not None and stat_max is not None and any(lo > hi for lo, hi in zip(stat_min, stat_max)):
        raise ValueError("ポイント配分の最小値が最大値を超えています")
    if stat_min == (0, 0, 0, 0, 0, 0):
        stat_min = None
    if stat_max == (32, 32, 32, 32, 32, 32):
        stat_max = None

    mega_raw = _require_nonempty_list(f, "megas", "メガ条件")
    megas = [int(x) for x in mega_raw] if mega_raw is not None else None
    mcnt_raw = _require_nonempty_list(f, "move_counts", "技数")
    move_counts = [int(x) for x in mcnt_raw] if mcnt_raw is not None else None
    req_moves = list(f["required_moves"]) if f.get("required_moves") else None

    out = {
        "allocations": allocs,
        "stat_min": stat_min,
        "stat_max": stat_max,
        "natures": natures,
        "move_pool": _require_nonempty_list(f, "move_pool", "採用候補技"),
        "required_moves": req_moves,
        "move_counts": move_counts,
        "items": _require_nonempty_list(f, "items", "持ち物"),
        "megas": megas,
        "abilities": _require_nonempty_list(f, "abilities", "特性"),
        "sexes": _require_nonempty_list(f, "sexes", "性別"),
    }
    return out if any(v is not None for v in out.values()) else None


def estimate_search(payload):
    ensure_binaries()
    species = payload.get("species", "").strip()
    tier = payload.get("tier", "light")
    if tier not in search_tiers.TIERS:
        raise ValueError(f"unknown tier: {tier}")
    filters = _parse_filters(payload)
    domain = population3.load_domain(CATALOG_BIN, species)
    target = int(payload["candidates"]) if payload.get("candidates") is not None else search_tiers.TIERS[tier]["candidates"]
    if target < 1:
        raise ValueError("候補上限数は 1 以上を指定してください")
    if not filters:
        return {
            "species": species,
            "tier": tier,
            "filtered": False,
            "enumerationMode": "sampled",
            "targetCandidates": target,
            "spaceSize": None,
            "exact": False,
        }
    kw = dict(
        allocations=filters["allocations"],
        stat_min=filters["stat_min"],
        stat_max=filters["stat_max"],
        natures=population3.NATURES if filters["natures"] is None else filters["natures"],
        move_pool=filters["move_pool"],
        required_moves=filters["required_moves"],
        items=filters["items"],
        megas=filters["megas"],
        abilities=filters["abilities"],
        sexes=filters["sexes"],
        move_counts=(4,) if filters["move_counts"] is None else filters["move_counts"],
    )
    space_size, exact = population3.estimate_filtered_count(domain, **kw)
    mode = "exhaustive" if (exact and space_size <= target) else "sampled"
    return {
        "species": species,
        "tier": tier,
        "filtered": True,
        "enumerationMode": mode,
        "targetCandidates": min(space_size, target) if mode == "exhaustive" else target,
        "spaceSize": space_size,
        "exact": exact,
    }


def _filters_to_cli(filters):
    args = []
    if not filters:
        return args
    for a in filters.get("allocations") or ():
        args.extend(["--allocation", ",".join(map(str, a))])
    if filters.get("stat_min"):
        args.extend(["--stat-min", ",".join(map(str, filters["stat_min"]))])
    if filters.get("stat_max"):
        args.extend(["--stat-max", ",".join(map(str, filters["stat_max"]))])
    for n in filters.get("natures") or ():
        args.extend(["--nature", ",".join(map(str, n))])
    for m in filters.get("move_pool") or ():
        args.extend(["--move", m])
    for m in filters.get("required_moves") or ():
        args.extend(["--require-move", m])
    for mc in filters.get("move_counts") or ():
        args.extend(["--move-count", str(mc)])
    for it in filters.get("items") or ():
        args.extend(["--item", it])
    if filters.get("megas") is not None:
        for mg in filters["megas"]:
            args.extend(["--mega", str(mg)])
    for ab in filters.get("abilities") or ():
        args.extend(["--ability", ab])
    for sx in filters.get("sexes") or ():
        args.extend(["--sex", sx])
    return args


def _job_stages(run_dir, tier_name):
    t = search_tiers.TIERS.get(tier_name, {})
    names = ["build", "gate"]
    for i in range(1, len(t.get("a_rounds", [])) + 1):
        names.append(f"stageA{i}")
    names.extend(["stageB", "finals"])
    return [{"name": n, "done": (run_dir / f"{n}.done").exists()} for n in names]


def start_search_job(payload):
    ensure_binaries()
    run_name = payload.get("runName", "").strip()
    run_dir, rel = _safe_run_dir(run_name)
    resume = bool(payload.get("resume"))
    shards = max(1, min(64, int(payload.get("shards") or 4)))

    with _JOBS_LOCK:
        existing = _JOBS.get(rel)
        if existing and existing["status"] in ("generating", "running"):
            raise ValueError(f"ジョブは既に実行中です: {rel}")

    if resume:
        cfg_path = run_dir / "run_config.json"
        if not cfg_path.exists():
            raise ValueError(f"再開対象の run_config.json がありません: {rel}")
        cfg = json.loads(cfg_path.read_text(encoding="utf-8"))
        species = cfg["species"]
        tier = cfg["tier"]
        filters = None
        cand_override = None
    else:
        species = payload.get("species", "").strip()
        tier = payload.get("tier", "light")
        if tier not in search_tiers.TIERS:
            raise ValueError(f"unknown tier: {tier}")
        filters = _parse_filters(payload)
        cand_override = int(payload["candidates"]) if payload.get("candidates") is not None else None
        if cand_override is not None and cand_override < 1:
            raise ValueError("候補上限数は 1 以上を指定してください")
        # Validate filters upfront before launching background thread.
        domain = population3.load_domain(CATALOG_BIN, species)
        if filters:
            sz, _ = population3.estimate_filtered_count(
                domain,
                allocations=filters["allocations"],
                stat_min=filters["stat_min"],
                stat_max=filters["stat_max"],
                natures=population3.NATURES if filters["natures"] is None else filters["natures"],
                move_pool=filters["move_pool"],
                required_moves=filters["required_moves"],
                items=filters["items"],
                megas=filters["megas"],
                abilities=filters["abilities"],
                sexes=filters["sexes"],
                move_counts=(4,) if filters["move_counts"] is None else filters["move_counts"],
            )
            if sz == 0:
                raise ValueError("指定された絞り込み条件に一致する合法な型が 0 件です")

    job = {
        "run": rel,
        "dir": str(run_dir),
        "species": species,
        "tier": tier,
        "shards": shards,
        "status": "running" if resume else "generating",
        "startedAt": time.time(),
        "finishedAt": None,
        "logs": [],
        "proc": None,
        "stopRequested": False,
        "error": None,
    }
    with _JOBS_LOCK:
        _JOBS[rel] = job

    def append_log(line):
        with _JOBS_LOCK:
            job["logs"].append(line.rstrip("\r\n"))
            if len(job["logs"]) > 200:
                job["logs"] = job["logs"][-200:]

    def worker():
        child_env = dict(_clean_env(), PYTHONUNBUFFERED="1")
        try:
            if not resume:
                cmd_gen = [
                    sys.executable, "-u", str(ROOT / "search" / "search_tiers.py"),
                    "gen", species, tier, str(run_dir), "--gcc", _GCC_PATH,
                ]
                if cand_override:
                    cmd_gen.extend(["--candidates", str(cand_override)])
                cmd_gen.extend(_filters_to_cli(filters))
                append_log("[gen] " + " ".join(cmd_gen))
                proc_g = subprocess.Popen(
                    cmd_gen, cwd=ROOT, env=child_env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    bufsize=1, text=True, encoding="utf-8", errors="replace",
                )
                with _JOBS_LOCK:
                    job["proc"] = proc_g
                for line in proc_g.stdout:
                    append_log(line)
                rc = proc_g.wait()
                if job["stopRequested"]:
                    with _JOBS_LOCK:
                        job["status"] = "stopped"
                        job["finishedAt"] = time.time()
                        job["proc"] = None
                    return
                if rc != 0:
                    raise RuntimeError(f"候補生成に失敗しました (rc={rc})")

            with _JOBS_LOCK:
                job["status"] = "running"
            cmd_ctl = [
                sys.executable, "-u", str(ROOT / "search" / "search_controller.py"),
                str(run_dir), "--gcc", _GCC_PATH, "--shards", str(shards),
            ]
            append_log("[run] " + " ".join(cmd_ctl))
            proc_c = subprocess.Popen(
                cmd_ctl, cwd=ROOT, env=child_env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                bufsize=1, text=True, encoding="utf-8", errors="replace",
            )
            with _JOBS_LOCK:
                job["proc"] = proc_c
            for line in proc_c.stdout:
                append_log(line)
            rc = proc_c.wait()
            with _JOBS_LOCK:
                job["proc"] = None
                job["finishedAt"] = time.time()
                if job["stopRequested"]:
                    job["status"] = "stopped"
                elif rc == 0 and (run_dir / "DONE").exists():
                    job["status"] = "complete"
                else:
                    job["status"] = "error"
                    job["error"] = f"search_controller exited with code {rc}"
        except Exception as exc:
            with _JOBS_LOCK:
                job["proc"] = None
                job["finishedAt"] = time.time()
                job["status"] = "stopped" if job["stopRequested"] else "error"
                job["error"] = str(exc)
            append_log(f"[error] {exc}")

    threading.Thread(target=worker, daemon=True).start()
    return {"run": rel, "status": job["status"]}


def stop_search_job(run_name):
    _, rel = _safe_run_dir(run_name)
    with _JOBS_LOCK:
        job = _JOBS.get(rel)
        if not job or not job["proc"]:
            raise ValueError(f"実行中のジョブが見つかりません: {rel}")
        job["stopRequested"] = True
        proc = job["proc"]
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
    return {"run": rel, "status": "stopping"}


def list_jobs():
    with _JOBS_LOCK:
        items = [{
            "run": j["run"],
            "dir": j["dir"],
            "species": j["species"],
            "tier": j["tier"],
            "shards": j["shards"],
            "status": j["status"],
            "startedAt": j["startedAt"],
            "finishedAt": j["finishedAt"],
            "error": j["error"],
            "logs": list(j["logs"][-60:]),
        } for j in _JOBS.values()]
    out = []
    for j in items:
        run_dir = Path(j.pop("dir"))
        j["stages"] = _job_stages(run_dir, j["tier"])
        out.append(j)
    return sorted(out, key=lambda x: -x["startedAt"])


def list_runs():
    if not RUNS_DIR.exists():
        return []
    out = []
    dirs = sorted({p.parent for p in RUNS_DIR.rglob("run_config.json")} |
                  {p.parent for p in RUNS_DIR.rglob("finals_results.json")})
    for d in dirs:
        cfg_path = d / "run_config.json"
        rel = d.resolve().relative_to(RUNS_DIR.resolve()).as_posix()
        cfg = json.loads(cfg_path.read_text(encoding="utf-8")) if cfg_path.exists() else {}
        sum_path = d / "summary.json"
        meta_path = d / "candidates_meta.json"
        res_path = d / "finals_results.json"
        summary = json.loads(sum_path.read_text(encoding="utf-8")) if sum_path.exists() else {}
        cmeta = json.loads(meta_path.read_text(encoding="utf-8")) if meta_path.exists() else {}
        with _JOBS_LOCK:
            job = _JOBS.get(rel)
            job_status = job["status"] if job else None
        out.append({
            "run": rel,
            "species": cfg.get("species", summary.get("species", "")),
            "tier": cfg.get("tier", summary.get("tier", "")),
            "tierLabel": summary.get("tier_label") or search_tiers.TIERS.get(cfg.get("tier", ""), {}).get("label", ""),
            "complete": res_path.exists() and (d / "DONE").exists(),
            "hasResults": res_path.exists(),
            "hasBeginner": (d / "beginner_scores.json").exists(),
            "candidates": cmeta.get("total") or summary.get("candidates"),
            "enumerationMode": cmeta.get("enumeration_mode"),
            "filters": cfg.get("filters"),
            "summary": summary if summary else None,
            "stages": _job_stages(d, cfg.get("tier", "")),
            "jobStatus": job_status,
            "mtime": res_path.stat().st_mtime if res_path.exists() else cfg_path.stat().st_mtime,
        })
    out.sort(key=lambda x: -x["mtime"])
    return out


def load_run_detail(run_name):
    run_dir, rel = _safe_run_dir(run_name)
    res_path = run_dir / "finals_results.json"
    if not res_path.exists():
        raise ValueError(f"finals_results.json が見つかりません: {rel}")
    data = json.loads(res_path.read_text(encoding="utf-8"))
    sum_path = run_dir / "summary.json"
    meta_path = run_dir / "candidates_meta.json"
    beg_path = run_dir / "beginner_scores.json"
    if sum_path.exists():
        data["summary"] = json.loads(sum_path.read_text(encoding="utf-8"))
    if meta_path.exists():
        data["candidates_meta"] = json.loads(meta_path.read_text(encoding="utf-8"))
    if beg_path.exists():
        data["beginner_scores"] = json.loads(beg_path.read_text(encoding="utf-8"))
    data["run"] = rel
    return data


def run_beginner_eval(run_name, games=50, seed=20264001):
    """Optional evaluation of finalists against 24 simple attack-only normal builds."""
    ensure_binaries()
    run_dir, rel = _safe_run_dir(run_name)
    res_path = run_dir / "finals_results.json"
    if not res_path.exists():
        raise ValueError(f"決勝結果がありません: {rel}")
    fdata = json.loads(res_path.read_text(encoding="utf-8"))
    finalists = fdata["finalists"]
    species = fdata.get("species") or finalists[0].split(":", 1)[0]

    # Load move power info from metadata.js to pick top damaging moves deterministically.
    meta_text = (HERE / "metadata.js").read_text(encoding="utf-8")
    meta_json = meta_text.split("window.CHAMPIONS_META = ", 1)[1].rsplit(";", 1)[0]
    meta = json.loads(meta_json)
    moves_meta = meta["moves"]
    domain = population3.load_domain(CATALOG_BIN, species)
    norm_profs = [p for p in domain.profiles if p.mega == 0 and p.item == "none"]
    if not norm_profs:
        norm_profs = [p for p in domain.profiles if p.mega == 0] or list(domain.profiles)

    spreads = [
        (32, 32, 2, 0, 0, 0),
        (32, 0, 2, 32, 0, 0),
        (2, 32, 0, 0, 0, 32),
        (2, 0, 0, 32, 0, 32),
        (32, 16, 9, 0, 9, 0),
        (22, 22, 0, 0, 0, 22),
    ]
    natures = [(1, 3), (3, 1), (5, 3), (5, 1), (0, 0), (2, 3)]
    rng = population3.Stream(seed)
    pool24 = []
    seen = set()
    i = 0
    while len(pool24) < 24 and i < 500:
        prof = norm_profs[i % len(norm_profs)]
        alloc = spreads[i % len(spreads)]
        up, down = natures[i % len(natures)]
        i += 1
        dmg_moves = [
            m for m in domain.moves_by_sex[prof.sex]
            if moves_meta.get(m, {}).get("p", 0) > 0
        ]
        dmg_moves.sort(key=lambda m: (-moves_meta[m]["p"], m))
        top_pool = dmg_moves[:12] if len(dmg_moves) >= 4 else list(domain.moves_by_sex[prof.sex])
        if not top_pool:
            continue
        chosen = []
        avail = list(top_pool)
        for _ in range(min(4, len(avail))):
            chosen.append(avail.pop(rng.pick(len(avail))))
        spec = (
            f"{species}:{','.join(map(str, alloc))}|{','.join(sorted(chosen))}"
            f"|obsh4|{prof.item}|{prof.mega}|{prof.ability}|{up},{down}|{prof.sex}"
        )
        ident = population3.identity(spec)
        if ident in seen:
            continue
        seen.add(ident)
        pool24.append(spec)

    if not pool24:
        raise ValueError(f"{species}: 簡易プールを生成できませんでした")

    with tempfile.TemporaryDirectory(prefix="viewer_beg_") as tmp:
        tmp_dir = Path(tmp)
        fin_f = tmp_dir / "finalists.txt"
        beg_f = tmp_dir / "beginner24.txt"
        fin_f.write_text("\n".join(finalists) + "\n", encoding="utf-8")
        beg_f.write_text("\n".join(pool24) + "\n", encoding="utf-8")

        p_a = subprocess.run(
            [str(ENGINE_BIN), "screenx", str(fin_f), str(beg_f), str(games), str(seed)],
            cwd=ROOT, env=_clean_env(), capture_output=True, text=True, encoding="utf-8", errors="replace",
        )
        if p_a.returncode != 0:
            raise RuntimeError(p_a.stderr[-400:])
        p_b = subprocess.run(
            [str(ENGINE_BIN), "screenx", str(beg_f), str(fin_f), str(games), str(seed + 1)],
            cwd=ROOT, env=_clean_env(), capture_output=True, text=True, encoding="utf-8", errors="replace",
        )
        if p_b.returncode != 0:
            raise RuntimeError(p_b.stderr[-400:])

    rows_a = _parse_screenx(p_a.stdout)
    rows_b = _parse_screenx(p_b.stdout)
    per_finalist = []
    for fi in range(len(finalists)):
        scores = []
        for bi in range(len(pool24)):
            wa, la, da = rows_a[fi][3 * bi:3 * bi + 3]
            sa = (wa + 0.5 * da) / (wa + la + da)
            wb, lb, db = rows_b[bi][3 * fi:3 * fi + 3]
            sb = (lb + 0.5 * db) / (wb + lb + db)
            scores.append(0.5 * (sa + sb))
        per_finalist.append(round(sum(scores) / len(scores), 4))

    beg_data = {
        "species": species,
        "pool_size": len(pool24),
        "games_per_pair_per_seat": games,
        "seed": seed,
        "policy": "obsh4",
        "pool": pool24,
        "per_finalist": per_finalist,
    }
    (run_dir / "beginner_scores.json").write_text(
        json.dumps(beg_data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    return beg_data


class WorkbenchHandler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        sys.stderr.write("[viewer] " + (fmt % args) + "\n")

    def _send_bytes(self, status, body, content_type):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _send_json(self, status, obj):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self._send_bytes(status, body, "application/json; charset=utf-8")

    def _read_json(self):
        length = int(self.headers.get("Content-Length") or "0")
        raw = self.rfile.read(length) if length > 0 else b"{}"
        return json.loads(raw.decode("utf-8"))

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        try:
            if path in ("/", "/index.html"):
                self._send_bytes(200, (HERE / "index.html").read_bytes(), "text/html; charset=utf-8")
            elif path == "/metadata.js":
                ensure_binaries()
                self._send_bytes(200, (HERE / "metadata.js").read_bytes(), "application/javascript; charset=utf-8")
            elif path == "/api/health":
                ensure_binaries()
                mat = verify_matrix()
                self._send_json(200, {
                    "ok": True,
                    "matrix": mat,
                    "cpuCount": os.cpu_count() or 1,
                    "tiers": search_tiers.TIERS,
                    "tierGames": {k: search_tiers.tier_games(k) for k in search_tiers.TIERS},
                })
            elif path == "/api/runs":
                self._send_json(200, {"runs": list_runs()})
            elif path.startswith("/api/runs/"):
                run_name = urllib.parse.unquote(path[len("/api/runs/"):])
                self._send_json(200, load_run_detail(run_name))
            elif path == "/api/search/jobs":
                self._send_json(200, {"jobs": list_jobs()})
            else:
                self._send_json(404, {"error": f"not found: {path}"})
        except (ValueError, KeyError) as exc:
            self._send_json(400, {"error": str(exc)})
        except Exception as exc:
            self._send_json(500, {"error": str(exc)})

    def do_POST(self):
        path = urllib.parse.urlparse(self.path).path
        try:
            payload = self._read_json()
            if path == "/api/battle":
                games = int(payload.get("games") or 1000)
                if not 1 <= games <= 100000:
                    raise ValueError("games must be in 1..100000")
                seed = int(payload.get("seed") if payload.get("seed") is not None else 12345)
                self._send_json(200, run_battle(payload["specA"], payload["specB"], games, seed))
            elif path == "/api/trace":
                seed = int(payload.get("seed") if payload.get("seed") is not None else 12345)
                self._send_json(200, run_trace(payload["specA"], payload["specB"], seed))
            elif path == "/api/screen_pool":
                games = int(payload.get("games") or 60)
                if not 1 <= games <= 2000:
                    raise ValueError("games must be in 1..2000")
                seed = int(payload.get("seed") if payload.get("seed") is not None else 20263370)
                self._send_json(200, run_screen_pool(payload["specs"], payload["run"], games, seed))
            elif path == "/api/search/estimate":
                self._send_json(200, estimate_search(payload))
            elif path == "/api/search/start":
                self._send_json(200, start_search_job(payload))
            elif path == "/api/search/stop":
                self._send_json(200, stop_search_job(payload["run"]))
            elif path == "/api/search/beginner_eval":
                games = int(payload.get("games") or 50)
                if not 1 <= games <= 500:
                    raise ValueError("games must be in 1..500")
                self._send_json(200, run_beginner_eval(payload["run"], games=games))
            else:
                self._send_json(404, {"error": f"not found: {path}"})
        except (ValueError, KeyError) as exc:
            self._send_json(400, {"error": str(exc)})
        except Exception as exc:
            self._send_json(500, {"error": str(exc)})


def main():
    global _GCC_PATH
    ap = argparse.ArgumentParser(description="Run local Pokemon 1on1 Stage 8 HTML workbench server.")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--gcc", default="gcc")
    ap.add_argument("--no-browser", action="store_true")
    args = ap.parse_args()
    _GCC_PATH = args.gcc
    _log("Starting Pokemon 1on1 Workbench...")
    ensure_binaries(_GCC_PATH, verbose=True)
    mat = verify_matrix(verbose=True)
    if not mat["ok"]:
        raise SystemExit(f"matrix MD5 verification failed: {mat}")
    server = ThreadingHTTPServer((args.host, args.port), WorkbenchHandler)
    url = f"http://{args.host}:{args.port}/"
    print(f"Pokemon 1on1 Workbench running at {url} (Ctrl+C to stop)", flush=True)
    if not args.no_browser:
        threading.Timer(0.4, lambda: webbrowser.open(url)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
