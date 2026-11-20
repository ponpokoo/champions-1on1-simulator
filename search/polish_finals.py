#!/usr/bin/env python3
"""Post-process finals_results.json according to the agreed specification:
1. Measure actual per-move usage rates across the original finals round-robin via `engine screenu`.
2. Diagnose all finalists (unused moves <1%, wasted A/C SP based on active moves, Scarf -Spe, nature mismatch).
3. Keep all 4 moves intact; generate local-fix variants (reallocate wasted A/C SP, rationalize nature,
   and for Scarf -Spe also allow S-stat-preserving S-point reallocation) for every finalist with stat/nature issues.
4. Re-evaluate variants vs the original 64-finalist pool (both seats) and apply the 3-condition safety gate:
   - delta_mean >= -0.5pt (vs original 63 opponents)
   - delta_worst >= -2.0pt (vs original 63 opponents)
   - vs_original >= 45.0%
5. Replace gated-in finalists to form the polished 64-build finals pool, run a full `screenu` round-robin
   on the polished pool (new matrix, rankings, worst matchups, and updated move usage), group into archetypes,
   and save everything to `finals_polished.json` without touching `finals_results.json`.

Usage:
  python search/polish_finals.py <run_dir> [--games 80] [--polish-games 80] [--shards 10] [--seed 20265000]
"""
import argparse
import hashlib
import json
import math
import subprocess
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

NATURE_NAMES_JA = {
    "0,0": "無補正",
    "1,2": "さみしがり(A↑B↓)",
    "1,3": "いじっぱり(A↑C↓)",
    "1,4": "やんちゃ(A↑D↓)",
    "1,5": "ゆうかん(A↑S↓)",
    "2,1": "ずぶとい(B↑A↓)",
    "2,3": "わんぱく(B↑C↓)",
    "2,4": "のうてんき(B↑D↓)",
    "2,5": "のんき(B↑S↓)",
    "3,1": "ひかえめ(C↑A↓)",
    "3,2": "おっとり(C↑B↓)",
    "3,4": "うっかりや(C↑D↓)",
    "3,5": "れいせい(C↑S↓)",
    "4,1": "おだやか(D↑A↓)",
    "4,2": "おとなしい(D↑B↓)",
    "4,3": "しんちょう(D↑C↓)",
    "4,5": "なまいき(D↑S↓)",
    "5,1": "おくびょう(S↑A↓)",
    "5,2": "せっかち(S↑B↓)",
    "5,3": "ようき(S↑C↓)",
    "5,4": "むじゃき(S↑D↓)",
}

CHOICE_ITEMS = {"choicescarf", "choiceband", "choicespecs"}

NON_ATK_PHYSICAL = {
    "foulplay", "bodypress", "counter", "metalburst", "endeavor",
    "superfang", "naturesmadness", "ruination", "seismictoss",
    "nightshade", "bide", "finalgambit", "guillotine", "fissure", "horndrill",
}

NON_SPA_SPECIAL = {
    "sonicboom", "dragonrage", "psywave", "mirrorcoat", "sheercold",
    "nightshade", "finalgambit", "naturesmadness", "ruination",
}

GATE_MIN_DELTA_MEAN = -0.005
GATE_MIN_DELTA_WORST = -0.020
GATE_MIN_VS_ORIGINAL = 0.450


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def find_engine(explicit: str | None = None) -> Path:
    if explicit:
        p = Path(explicit)
        if p.exists():
            return p
        raise FileNotFoundError(f"specified engine binary not found: {explicit}")
    for cand in (
        ROOT / "engine" / "engine.exe",
        ROOT / "engine" / "engine",
        ROOT / "engine_search.exe",
        ROOT / "engine_search",
    ):
        if cand.exists():
            return cand
    raise FileNotFoundError("engine binary not found under engine/ or root")


def load_metadata() -> dict:
    meta_path = ROOT / "viewer" / "metadata.js"
    text = meta_path.read_text(encoding="utf-8")
    raw = text.split("window.CHAMPIONS_META = ", 1)[1].rsplit(";", 1)[0]
    return json.loads(raw)


def get_base_spe(species: str, meta: dict) -> int:
    sp = meta.get("species", {}).get(species)
    if not sp:
        return 80
    if "base" in sp and len(sp["base"]) == 6:
        return int(sp["base"][5])
    if "normal" in sp and isinstance(sp["normal"], dict):
        for v in sp["normal"].values():
            if isinstance(v, dict) and "base" in v:
                return int(v["base"][5])
    if "forms" in sp and isinstance(sp["forms"], list) and sp["forms"]:
        b = sp["forms"][0].get("base")
        if b and len(b) == 6:
            return int(b[5])
    return 80


def calc_lv50_non_hp(base: int, sp: int, mult: float) -> int:
    raw = ((2 * base + 31) // 2) + 5 + sp
    return int(math.floor(raw * mult + 1e-9))


def min_sp_for_stat(base: int, target_stat: int, mult: float) -> int:
    for s in range(33):
        if calc_lv50_non_hp(base, s, mult) >= target_stat:
            return s
    return 32


def parse_spec(spec: str) -> dict:
    parts = spec.strip().split("|")
    sp_stats = parts[0].split(":")
    species = sp_stats[0]
    sp = [int(x) for x in sp_stats[1].split(",")]
    moves = [m for m in parts[1].split(",") if m]
    policy = parts[2]
    item = parts[3]
    mega = int(parts[4])
    ability = parts[5]
    nat_up, nat_down = [int(x) for x in parts[6].split(",")]
    sex = parts[7]
    return {
        "species": species,
        "sp": sp,  # [H, A, B, C, D, S]
        "moves": moves,
        "policy": policy,
        "item": item,
        "mega": mega,
        "ability": ability,
        "nat_up": nat_up,
        "nat_down": nat_down,
        "sex": sex,
    }


def format_spec(d: dict) -> str:
    sp_str = ",".join(str(x) for x in d["sp"])
    mv_str = ",".join(d["moves"])
    return (
        f"{d['species']}:{sp_str}|{mv_str}|{d['policy']}|{d['item']}|"
        f"{d['mega']}|{d['ability']}|{d['nat_up']},{d['nat_down']}|{d['sex']}"
    )


def uses_atk(move_id: str, moves_meta: dict) -> bool:
    info = moves_meta.get(move_id)
    if not info:
        return True
    return info["c"] == 0 and info["p"] > 0 and move_id not in NON_ATK_PHYSICAL


def uses_spa(move_id: str, moves_meta: dict) -> bool:
    info = moves_meta.get(move_id)
    if not info:
        return True
    return info["c"] == 1 and info["p"] > 0 and move_id not in NON_SPA_SPECIAL


def is_status_move(move_id: str, moves_meta: dict) -> bool:
    info = moves_meta.get(move_id)
    return bool(info and info["c"] == 2)


def run_screenu_round_robin(engine: Path, builds: list[str], games: int, seed: int, shards: int) -> dict:
    """Run `engine screenu` over builds x builds in parallel shards.
    Returns both-seat move usage AND full seat-A win-rate matrix.
    """
    n = len(builds)
    shards = max(1, min(shards, n))
    with tempfile.TemporaryDirectory(prefix="polish_u_") as td:
        tdir = Path(td)
        pool_file = tdir / "pool.txt"
        pool_file.write_text("\n".join(builds) + "\n", encoding="utf-8")

        chunks = []
        base = n // shards
        rem = n % shards
        cur = 0
        for s in range(shards):
            sz = base + (1 if s < rem else 0)
            if sz == 0:
                continue
            cfile = tdir / f"cand_{s:02d}.txt"
            cfile.write_text("\n".join(builds[cur : cur + sz]) + "\n", encoding="utf-8")
            chunks.append((s, cur, sz, cfile))
            cur += sz

        def _worker(args):
            s, start_idx, sz, cfile = args
            cmd = [str(engine), "screenu", str(cfile), str(pool_file), str(games), str(seed + s * 1009)]
            res = subprocess.run(cmd, capture_output=True, text=True, check=True)
            return start_idx, res.stdout

        sel = [[0, 0, 0, 0] for _ in range(n)]
        gused = [[0, 0, 0, 0] for _ in range(n)]
        tot_games = [0] * n
        matrix = [[0.5] * n for _ in range(n)]

        with ThreadPoolExecutor(max_workers=len(chunks)) as ex:
            for start_idx, out in ex.map(_worker, chunks):
                for line in out.splitlines():
                    if not line.strip():
                        continue
                    cols = [int(x) for x in line.split()]
                    local_i, p, w, l, d = cols[0], cols[1], cols[2], cols[3], cols[4]
                    gi = start_idx + local_i
                    ng = w + l + d
                    matrix[gi][p] = round((w + 0.5 * d) / ng, 6) if ng else 0.5
                    if gi == p:
                        continue  # exclude self-mirror from move usage stats
                    tot_games[gi] += ng
                    for m in range(4):
                        sel[gi][m] += cols[5 + m * 2]
                        gused[gi][m] += cols[6 + m * 2]
                    tot_games[p] += ng
                    for m in range(4):
                        sel[p][m] += cols[13 + m * 2]
                        gused[p][m] += cols[14 + m * 2]

    usage = []
    for i, spec in enumerate(builds):
        parsed = parse_spec(spec)
        nm = len(parsed["moves"])
        tg = max(1, tot_games[i])
        tot_sel = sum(sel[i][:nm])
        per_move = []
        for m, mv in enumerate(parsed["moves"]):
            per_move.append({
                "move": mv,
                "selections": sel[i][m],
                "games_used": gused[i][m],
                "total_games": tot_games[i],
                "game_usage_rate": round(gused[i][m] / tg, 6),
                "turn_share": round(sel[i][m] / tot_sel, 6) if tot_sel > 0 else 0.0,
            })
        usage.append({
            "finalist": i,
            "total_games": tot_games[i],
            "moves": per_move,
        })
    return {"games_per_pair": games, "seed": seed, "matrix": matrix, "per_finalist": usage}


def compute_both_stats(matrix: list[list[float]]) -> list[dict]:
    n = len(matrix)
    out = []
    for i in range(n):
        row = []
        for j in range(n):
            if i == j:
                continue
            row.append(0.5 * (matrix[i][j] + (1.0 - matrix[j][i])))
        out.append({
            "finalist": i,
            "mean": sum(row) / len(row) if row else 0.5,
            "worst": min(row) if row else 0.5,
        })
    return out


def diagnose_build(parsed: dict, move_usage_entry: dict | None, moves_meta: dict, unused_thresh: float = 0.01) -> dict:
    """Diagnose issues using active moves (usage >= 1%) as the basis for stat/nature checks."""
    moves = parsed["moves"]
    sp = parsed["sp"]
    item = parsed["item"]
    nat_up, nat_down = parsed["nat_up"], parsed["nat_down"]

    unused_moves = []
    active_moves = list(moves)
    core_moves = list(moves)

    if move_usage_entry is not None:
        um = move_usage_entry["moves"]
        unused_moves = [x["move"] for x in um if x["game_usage_rate"] < unused_thresh]
        active_moves = [x["move"] for x in um if x["game_usage_rate"] >= unused_thresh]
        if not active_moves:
            best = max(um, key=lambda x: (x["game_usage_rate"], x["selections"]))
            active_moves = [best["move"]]
        core_moves = [
            x["move"]
            for x in sorted(um, key=lambda x: (-x["game_usage_rate"], -x["selections"]))
            if x["game_usage_rate"] >= 0.15
        ]
        if not core_moves:
            core_moves = [max(um, key=lambda x: (x["game_usage_rate"], x["selections"]))["move"]]

    has_atk_act = any(uses_atk(m, moves_meta) for m in active_moves)
    has_spa_act = any(uses_spa(m, moves_meta) for m in active_moves)

    issues = []
    has_fixable_stat_or_nature = False

    if unused_moves:
        issues.append({
            "code": "unused_move",
            "label": f"未使用技({len(unused_moves)})",
            "detail": ",".join(unused_moves),
        })

    if not has_atk_act and sp[1] > 0:
        has_fixable_stat_or_nature = True
        issues.append({
            "code": "wasted_atk",
            "label": f"A無駄振り({sp[1]})",
            "detail": f"実戦使用技に物理技なしなのにA={sp[1]}",
        })

    if not has_spa_act and sp[3] > 0:
        has_fixable_stat_or_nature = True
        issues.append({
            "code": "wasted_spa",
            "label": f"C無駄振り({sp[3]})",
            "detail": f"実戦使用技に特殊技なしなのにC={sp[3]}",
        })

    if item == "choicescarf" and nat_down == 5:
        has_fixable_stat_or_nature = True
        issues.append({
            "code": "scarf_minus_spe",
            "label": "S↓スカーフ",
            "detail": f"こだわりスカーフなのに性格{NATURE_NAMES_JA.get(f'{nat_up},{nat_down}', f'{nat_up},{nat_down}')}",
        })

    if has_atk_act and not has_spa_act:
        if nat_down == 1 or nat_up == 3:
            has_fixable_stat_or_nature = True
            issues.append({
                "code": "nature_mismatch",
                "label": "性格ミスマッチ(物理軸)",
                "detail": f"物理軸なのに性格{NATURE_NAMES_JA.get(f'{nat_up},{nat_down}', f'{nat_up},{nat_down}')}",
            })
    elif has_spa_act and not has_atk_act:
        if nat_down == 3 or nat_up == 1:
            has_fixable_stat_or_nature = True
            issues.append({
                "code": "nature_mismatch",
                "label": "性格ミスマッチ(特殊軸)",
                "detail": f"特殊軸なのに性格{NATURE_NAMES_JA.get(f'{nat_up},{nat_down}', f'{nat_up},{nat_down}')}",
            })

    status_cnt = sum(1 for m in moves if is_status_move(m, moves_meta))
    if item in CHOICE_ITEMS and status_cnt >= 2:
        issues.append({
            "code": "choice_status",
            "label": f"こだわり変化技{status_cnt}",
            "detail": f"{item}持ちで変化技が{status_cnt}枠",
        })

    return {
        "unused_moves": unused_moves,
        "active_moves": active_moves,
        "core_moves": core_moves,
        "issues": issues,
        "issue_count": len(issues),
        "has_fixable_stat_or_nature": has_fixable_stat_or_nature,
    }


def reallocate_sp(sp: list[int], zero_atk: bool, zero_spa: bool, order: list[int], extra_freed: int = 0) -> list[int]:
    out = list(sp)
    freed = extra_freed
    if zero_atk and out[1] > 0:
        freed += out[1]
        out[1] = 0
    if zero_spa and out[3] > 0:
        freed += out[3]
        out[3] = 0
    for idx in order:
        if freed <= 0:
            break
        if zero_atk and idx == 1:
            continue
        if zero_spa and idx == 3:
            continue
        room = 32 - out[idx]
        if room > 0:
            add = min(room, freed)
            out[idx] += add
            freed -= add
    # Fallback if still leftover (should not happen when sum==66 and >=3 stats available)
    for idx in (0, 2, 4, 5, 1, 3):
        if freed <= 0:
            break
        room = 32 - out[idx]
        if room > 0:
            add = min(room, freed)
            out[idx] += add
            freed -= add
    assert sum(out) == 66 and all(0 <= x <= 32 for x in out), f"bad sp: {out}"
    return out


def generate_local_fix_variants(orig_spec: str, diag: dict, moves_meta: dict, base_spe: int) -> list[dict]:
    """Generate local-fix variants while keeping all 4 moves intact.
    - Reallocate wasted A/C SP (based on active moves).
    - Rationalize mismatched nature (and Scarf -Spe).
    - For Scarf -Spe, also allow S-stat-preserving S-point reallocation when switching away from -Spe.
    """
    if not diag["has_fixable_stat_or_nature"]:
        return []

    base = parse_spec(orig_spec)
    active_moves = diag["active_moves"]
    has_atk = any(uses_atk(m, moves_meta) for m in active_moves)
    has_spa = any(uses_spa(m, moves_meta) for m in active_moves)
    zero_atk = not has_atk
    zero_spa = not has_spa

    variants = []
    seen = set()

    def _add(d: dict, tags: list[str]):
        s = format_spec(d)
        if s == orig_spec or s in seen:
            return
        seen.add(s)
        variants.append({"spec": s, "tags": tags})

    if has_atk and not has_spa:
        orders = [
            ("火力+S優先", [1, 5, 0, 2, 4]),
            ("S+耐久優先", [5, 0, 1, 2, 4]),
            ("耐久優先", [0, 2, 4, 1, 5]),
        ]
    elif has_spa and not has_atk:
        orders = [
            ("火力+S優先", [3, 5, 0, 4, 2]),
            ("S+耐久優先", [5, 0, 3, 2, 4]),
            ("耐久優先", [0, 4, 2, 3, 5]),
        ]
    else:
        orders = [
            ("S+H優先", [5, 0, 1, 3, 2, 4]),
            ("耐久優先", [0, 2, 4, 5, 1, 3]),
        ]

    cur_nat = (base["nat_up"], base["nat_down"])
    is_scarf_minus_spe = base["item"] == "choicescarf" and cur_nat[1] == 5
    cur_nat_bad = (
        is_scarf_minus_spe
        or (has_atk and not has_spa and (cur_nat[1] == 1 or cur_nat[0] == 3))
        or (has_spa and not has_atk and (cur_nat[1] == 3 or cur_nat[0] == 1))
    )

    nat_candidates = []
    if not cur_nat_bad:
        nat_candidates.append(("性格維持", cur_nat))
    else:
        if has_atk and not has_spa:
            if base["item"] == "choicescarf":
                nat_candidates.extend([("いじっぱり化", (1, 3)), ("ようき化", (5, 3))])
            else:
                nat_candidates.extend([("いじっぱり化", (1, 3)), ("ようき化", (5, 3)), ("わんぱく化", (2, 3)), ("しんちょう化", (4, 3))])
        elif has_spa and not has_atk:
            if base["item"] == "choicescarf":
                nat_candidates.extend([("ひかえめ化", (3, 1)), ("おくびょう化", (5, 1))])
            else:
                nat_candidates.extend([("おくびょう化", (5, 1)), ("ひかえめ化", (3, 1)), ("ずぶとい化", (2, 1)), ("おだやか化", (4, 1))])
        else:
            if base["item"] == "choicescarf":
                nat_candidates.extend([("おくびょう化", (5, 1)), ("ようき化", (5, 3)), ("無補正化", (0, 0))])
            else:
                nat_candidates.append(("無補正化", (0, 0)))

    uniq_nats = []
    seen_n = set()
    for nlabel, nat in nat_candidates:
        if nat not in seen_n:
            seen_n.add(nat)
            uniq_nats.append((nlabel, nat))

    has_waste = (zero_atk and base["sp"][1] > 0) or (zero_spa and base["sp"][3] > 0)
    orig_spe_stat = calc_lv50_non_hp(base_spe, base["sp"][5], 0.9) if is_scarf_minus_spe else 0

    for nlabel, (nu, nd) in uniq_nats:
        sp_options = []
        seen_sp = set()

        # (a) Standard waste reallocation (or keep SP if no waste)
        if not has_waste:
            t = tuple(base["sp"])
            seen_sp.add(t)
            sp_options.append(("配分維持", list(base["sp"])))
        else:
            for olabel, order in orders:
                nsp = reallocate_sp(base["sp"], zero_atk, zero_spa, order)
                t = tuple(nsp)
                if t not in seen_sp:
                    seen_sp.add(t)
                    sp_options.append((f"無駄振り再配分({olabel})", nsp))

        # (b) Scarf -Spe special local transform: keep original S stat with fewer S points, shift freed S SP
        if is_scarf_minus_spe and nd != 5:
            new_mult = 1.1 if nu == 5 else 1.0
            min_s = min_sp_for_stat(base_spe, orig_spe_stat, new_mult)
            if min_s < base["sp"][5]:
                freed_s = base["sp"][5] - min_s
                base_trimmed = list(base["sp"])
                base_trimmed[5] = min_s
                for olabel, order in orders:
                    # Prefer putting freed S into non-S stats first
                    order_non_s = [x for x in order if x != 5] + [5]
                    nsp = reallocate_sp(base_trimmed, zero_atk, zero_spa, order_non_s, extra_freed=freed_s)
                    t = tuple(nsp)
                    if t not in seen_sp:
                        seen_sp.add(t)
                        sp_options.append((f"S実数値維持再配分({olabel})", nsp))

        for slabel, nsp in sp_options:
            tags = []
            if slabel != "配分維持":
                tags.append(slabel)
            if (nu, nd) != cur_nat:
                tags.append(nlabel)
            if not tags:
                continue
            d = dict(base, sp=nsp, nat_up=nu, nat_down=nd)  # moves kept intact!
            _add(d, tags)

    return variants


def run_screenx_both_seats(
    engine: Path,
    cands: list[str],
    pool: list[str],
    games: int,
    seed: int,
    shards: int,
) -> list[list[float]]:
    nc = len(cands)
    np_ = len(pool)
    if nc == 0:
        return []
    score_a = [[0.0] * np_ for _ in range(nc)]
    score_b = [[0.0] * np_ for _ in range(nc)]

    with tempfile.TemporaryDirectory(prefix="polish_x_") as td:
        tdir = Path(td)
        pool_file = tdir / "pool.txt"
        pool_file.write_text("\n".join(pool) + "\n", encoding="utf-8")

        s_a = max(1, min(shards, nc))
        chunks_a = []
        base = nc // s_a
        rem = nc % s_a
        cur = 0
        for s in range(s_a):
            sz = base + (1 if s < rem else 0)
            if sz == 0:
                continue
            cf = tdir / f"candA_{s:02d}.txt"
            cf.write_text("\n".join(cands[cur : cur + sz]) + "\n", encoding="utf-8")
            chunks_a.append((s, cur, cf))
            cur += sz

        def _work_a(args):
            s, start_idx, cf = args
            cmd = [str(engine), "screenx", str(cf), str(pool_file), str(games), str(seed + s * 1009)]
            res = subprocess.run(cmd, capture_output=True, text=True, check=True)
            return start_idx, res.stdout

        with ThreadPoolExecutor(max_workers=len(chunks_a)) as ex:
            for start_idx, out in ex.map(_work_a, chunks_a):
                for line in out.splitlines():
                    if not line.strip():
                        continue
                    cols = [int(x) for x in line.split()]
                    ci = start_idx + cols[0]
                    for p in range(np_):
                        w, l, d = cols[1 + 3 * p], cols[2 + 3 * p], cols[3 + 3 * p]
                        tot = w + l + d
                        score_a[ci][p] = (w + 0.5 * d) / tot if tot else 0.5

        cand_chunks = []
        step = 400
        for off in range(0, nc, step):
            sub = cands[off : off + step]
            pf = tdir / f"candPool_{off}.txt"
            pf.write_text("\n".join(sub) + "\n", encoding="utf-8")
            cand_chunks.append((off, len(sub), pf))

        s_b = max(1, min(shards, np_))
        pool_shards = []
        base_p = np_ // s_b
        rem_p = np_ % s_b
        cur = 0
        for s in range(s_b):
            sz = base_p + (1 if s < rem_p else 0)
            if sz == 0:
                continue
            cf = tdir / f"poolA_{s:02d}.txt"
            cf.write_text("\n".join(pool[cur : cur + sz]) + "\n", encoding="utf-8")
            pool_shards.append((s, cur, cf))
            cur += sz

        tasks_b = []
        for off, clen, cpool in cand_chunks:
            for s, pstart, cf in pool_shards:
                tasks_b.append((off, clen, cpool, s, pstart, cf))

        def _work_b(args):
            off, clen, cpool, s, pstart, cf = args
            cmd = [str(engine), "screenx", str(cf), str(cpool), str(games), str(seed + 50000 + off * 17 + s * 1009)]
            res = subprocess.run(cmd, capture_output=True, text=True, check=True)
            return off, clen, pstart, res.stdout

        with ThreadPoolExecutor(max_workers=max(1, min(shards, len(tasks_b)))) as ex:
            for off, clen, pstart, out in ex.map(_work_b, tasks_b):
                for line in out.splitlines():
                    if not line.strip():
                        continue
                    cols = [int(x) for x in line.split()]
                    pi = pstart + cols[0]
                    for k in range(clen):
                        w, l, d = cols[1 + 3 * k], cols[2 + 3 * k], cols[3 + 3 * k]
                        tot = w + l + d
                        score_b[off + k][pi] = (l + 0.5 * d) / tot if tot else 0.5

    return [[0.5 * (score_a[ci][p] + score_b[ci][p]) for p in range(np_)] for ci in range(nc)]


def build_archetypes(builds: list[str], usage_list: list[dict], both_stats: list[dict], rank_pos: dict, moves_meta: dict, items_ja: dict) -> list[dict]:
    order = sorted(both_stats, key=lambda x: (-x["mean"], -x["worst"], x["finalist"]))
    arch_map: dict[tuple, list[int]] = {}
    for r in order:
        fi = r["finalist"]
        parsed = parse_spec(builds[fi])
        um = usage_list[fi]["moves"]
        top_core = [
            x["move"]
            for x in sorted(um, key=lambda x: (-x["game_usage_rate"], -x["selections"]))
            if x["game_usage_rate"] >= 0.15
        ][:2]
        if not top_core:
            top_core = [max(um, key=lambda x: (x["game_usage_rate"], x["selections"]))["move"]]
        key = (parsed["item"], tuple(sorted(top_core)))
        arch_map.setdefault(key, []).append(fi)

    archetypes = []
    for (item, core_tuple), members in arch_map.items():
        rep = members[0]
        item_label = items_ja.get(item, {}).get("ja", item) if isinstance(items_ja.get(item), dict) else items_ja.get(item, item)
        core_ja = [moves_meta.get(m, {}).get("ja", m) for m in core_tuple]
        archetypes.append({
            "id": f"{item}:" + "+".join(core_tuple),
            "label": f"{item_label} / {' + '.join(core_ja)}",
            "item": item,
            "core_moves": list(core_tuple),
            "representative": rep,
            "rep_rank_both": rank_pos[rep],
            "rep_mean_both": round(both_stats[rep]["mean"], 6),
            "rep_worst_both": round(both_stats[rep]["worst"], 6),
            "member_count": len(members),
            "members": members,
        })
    archetypes.sort(key=lambda a: (-a["rep_mean_both"], -a["rep_worst_both"]))
    return archetypes


def main():
    ap = argparse.ArgumentParser(description="Local-fix & re-evaluate all finalists with 3-condition safety gate.")
    ap.add_argument("run_dir", help="Path to run directory containing finals_results.json")
    ap.add_argument("--games", type=int, default=80, help="Games/pair for screenu round-robin (default: 80)")
    ap.add_argument("--polish-games", type=int, default=60, help="Games/pair/seat for gate screening (default: 60)")
    ap.add_argument("--shards", type=int, default=10, help="Parallel worker count (default: 10)")
    ap.add_argument("--seed", type=int, default=20265000, help="Base RNG seed")
    ap.add_argument("--engine", default=None, help="Optional explicit path to engine binary")
    args = ap.parse_args()

    t0 = time.time()
    run_dir = Path(args.run_dir).resolve()
    res_path = run_dir / "finals_results.json"
    if not res_path.exists():
        raise FileNotFoundError(f"finals_results.json not found in {run_dir}")

    engine = find_engine(args.engine)
    engine_sha = sha256_file(engine)
    meta = load_metadata()
    moves_meta = meta["moves"]
    items_ja = meta.get("items", {})

    raw = json.loads(res_path.read_text(encoding="utf-8"))
    finalists = raw["finalists"]
    orig_matrix = raw["matrix"]
    species = raw.get("species") or finalists[0].split(":", 1)[0]
    base_spe = get_base_spe(species, meta)
    n = len(finalists)

    orig_both = compute_both_stats(orig_matrix)
    orig_order = sorted(orig_both, key=lambda x: (-x["mean"], -x["worst"], x["finalist"]))
    orig_rank_pos = {r["finalist"]: idx + 1 for idx, r in enumerate(orig_order)}

    print(f"[1/4] Measuring original move usage via screenu ({n}x{n} x {args.games} games, {args.shards} shards)...", flush=True)
    t1 = time.time()
    orig_usage = run_screenu_round_robin(engine, finalists, args.games, args.seed, args.shards)
    print(f"      done in {time.time() - t1:.1f}s", flush=True)

    # Diagnose all 64 original finalists
    orig_diagnostics = []
    for i, spec in enumerate(finalists):
        parsed = parse_spec(spec)
        diag = diagnose_build(parsed, orig_usage["per_finalist"][i], moves_meta)
        diag["finalist"] = i
        diag["rank_both"] = orig_rank_pos[i]
        diag["orig_mean_both"] = round(orig_both[i]["mean"], 6)
        diag["orig_worst_both"] = round(orig_both[i]["worst"], 6)
        orig_diagnostics.append(diag)

    # Generate local-fix variants for ALL finalists with fixable stat/nature issues
    all_specs = []
    spec_to_ci = {}
    target_variants = {}
    target_indices = [i for i in range(n) if orig_diagnostics[i]["has_fixable_stat_or_nature"]]
    for fi in target_indices:
        vs = generate_local_fix_variants(finalists[fi], orig_diagnostics[fi], moves_meta, base_spe)
        target_variants[fi] = vs
        for v in vs:
            s = v["spec"]
            if s not in spec_to_ci:
                spec_to_ci[s] = len(all_specs)
                all_specs.append(s)

    print(
        f"[2/4] Gate-testing {len(all_specs)} local-fix variants for {len(target_indices)}/{n} finalists "
        f"vs original {n}-pool ({args.polish_games} games/pair/seat, {args.shards} shards)...",
        flush=True,
    )
    t2 = time.time()
    eval_matrix = run_screenx_both_seats(engine, all_specs, finalists, args.polish_games, args.seed + 100000, args.shards)
    print(f"      done in {time.time() - t2:.1f}s", flush=True)

    # Apply 3-condition safety gate and build polished 64-finalist pool
    polished_finalists = list(finalists)
    polished_entries = []
    adopted_count = 0
    rejected_count = 0

    for fi in range(n):
        orig_spec = finalists[fi]
        o_mean = orig_both[fi]["mean"]
        o_worst = orig_both[fi]["worst"]
        diag = orig_diagnostics[fi]

        if fi not in target_variants or not target_variants[fi]:
            polished_entries.append({
                "finalist": fi,
                "orig_rank_both": orig_rank_pos[fi],
                "status": "clean_original",
                "adopted": False,
                "orig_spec": orig_spec,
                "final_spec": orig_spec,
                "orig_mean_both": round(o_mean, 6),
                "orig_worst_both": round(o_worst, 6),
                "orig_issues": diag["issues"],
                "unused_moves": diag["unused_moves"],
                "active_moves": diag["active_moves"],
                "best_candidate": None,
                "variants_tested": 0,
            })
            continue

        evaluated = []
        for v in target_variants[fi]:
            ci = spec_to_ci[v["spec"]]
            row = eval_matrix[ci]
            row_excl = [row[j] for j in range(n) if j != fi]
            m63 = sum(row_excl) / len(row_excl)
            w63 = min(row_excl)
            vs_orig = row[fi]
            dm = m63 - o_mean
            dw = w63 - o_worst
            passed = (
                dm >= GATE_MIN_DELTA_MEAN
                and dw >= GATE_MIN_DELTA_WORST
                and vs_orig >= GATE_MIN_VS_ORIGINAL
            )
            evaluated.append({
                "spec": v["spec"],
                "tags": v["tags"],
                "mean_vs_63": round(m63, 6),
                "worst_vs_63": round(w63, 6),
                "vs_original": round(vs_orig, 6),
                "delta_mean": round(dm, 6),
                "delta_worst": round(dw, 6),
                "gate_passed": passed,
            })

        passing = [e for e in evaluated if e["gate_passed"]]
        if passing:
            best = max(passing, key=lambda e: (e["mean_vs_63"], e["worst_vs_63"]))
            polished_finalists[fi] = best["spec"]
            adopted_count += 1
            status = "optimized"
            adopted = True
        else:
            best = max(evaluated, key=lambda e: (e["mean_vs_63"], e["worst_vs_63"])) if evaluated else None
            rejected_count += 1
            status = "kept_original_gate_rejected"
            adopted = False

        polished_entries.append({
            "finalist": fi,
            "orig_rank_both": orig_rank_pos[fi],
            "status": status,
            "adopted": adopted,
            "orig_spec": orig_spec,
            "final_spec": polished_finalists[fi],
            "orig_mean_both": round(o_mean, 6),
            "orig_worst_both": round(o_worst, 6),
            "orig_issues": diag["issues"],
            "unused_moves": diag["unused_moves"],
            "active_moves": diag["active_moves"],
            "best_candidate": best,
            "variants_tested": len(evaluated),
        })

    print(
        f"[3/4] Running full round-robin on polished {n}-pool "
        f"(adopted={adopted_count}, gate_rejected={rejected_count}, clean={n - adopted_count - rejected_count}; "
        f"{args.games} games/pair, {args.shards} shards)...",
        flush=True,
    )
    t3 = time.time()
    new_rr = run_screenu_round_robin(engine, polished_finalists, args.games, args.seed + 200000, args.shards)
    print(f"      done in {time.time() - t3:.1f}s", flush=True)

    new_both = compute_both_stats(new_rr["matrix"])
    new_order = sorted(new_both, key=lambda x: (-x["mean"], -x["worst"], x["finalist"]))
    new_rank_pos = {r["finalist"]: idx + 1 for idx, r in enumerate(new_order)}

    # Diagnose polished finalists using their NEW move usage in the polished pool
    new_diagnostics = []
    for i, spec in enumerate(polished_finalists):
        parsed = parse_spec(spec)
        diag = diagnose_build(parsed, new_rr["per_finalist"][i], moves_meta)
        diag["finalist"] = i
        diag["rank_both"] = new_rank_pos[i]
        diag["mean_both"] = round(new_both[i]["mean"], 6)
        diag["worst_both"] = round(new_both[i]["worst"], 6)
        new_diagnostics.append(diag)

    # Attach new-pool rank/mean/worst to polished_entries
    for e in polished_entries:
        fi = e["finalist"]
        e["new_rank_both"] = new_rank_pos[fi]
        e["new_mean_both"] = round(new_both[fi]["mean"], 6)
        e["new_worst_both"] = round(new_both[fi]["worst"], 6)

    archetypes = build_archetypes(
        polished_finalists, new_rr["per_finalist"], new_both, new_rank_pos, moves_meta, items_ja
    )

    out_obj = {
        "schema_version": 2,
        "species": species,
        "policy": raw.get("policy", "obsh4"),
        "engine_sha256": engine_sha,
        "usage_games_per_pair": args.games,
        "polish_games_per_pair_per_seat": args.polish_games,
        "seed": args.seed,
        "gate": {
            "min_delta_mean": GATE_MIN_DELTA_MEAN,
            "min_delta_worst": GATE_MIN_DELTA_WORST,
            "min_vs_original": GATE_MIN_VS_ORIGINAL,
        },
        "summary": {
            "total": n,
            "adopted": adopted_count,
            "gate_rejected": rejected_count,
            "clean_original": n - adopted_count - rejected_count,
        },
        # Polished finals pool (replaces original view as main display)
        "polished_finalists": polished_finalists,
        "polished_matrix": new_rr["matrix"],
        "polished_move_usage": new_rr["per_finalist"],
        "polished_diagnostics": new_diagnostics,
        "archetypes": archetypes,
        # Original diagnostics & move usage (for Before view & audit)
        "orig_move_usage": orig_usage["per_finalist"],
        "orig_diagnostics": orig_diagnostics,
        # Per-finalist Before/After ledger
        "polished": sorted(polished_entries, key=lambda e: e["new_rank_both"]),
    }

    out_path = run_dir / "finals_polished.json"
    out_path.write_text(json.dumps(out_obj, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"[4/4] Wrote {out_path} in {time.time() - t0:.1f}s total.", flush=True)

    print("\n=== Top Archetypes (Polished 64-Pool, Both Seats) ===")
    for idx, a in enumerate(archetypes[:10], 1):
        fi = a["representative"]
        pe = next(x for x in polished_entries if x["finalist"] == fi)
        tag = "最適化済み" if pe["adopted"] else ("元型維持" if pe["status"] == "kept_original_gate_rejected" else "元型クリーン")
        print(
            f"{idx:2d}. {a['label']} ({a['member_count']}型) | 代表 #{fi} "
            f"(新{a['rep_rank_both']}位 / 元{pe['orig_rank_both']}位, 平均 {a['rep_mean_both']*100:.1f}%, 最悪 {a['rep_worst_both']*100:.1f}%, {tag})"
        )

    print("\n=== Top 12 in Polished 64-Pool (Both Seats) ===")
    for r in new_order[:12]:
        fi = r["finalist"]
        pe = next(x for x in polished_entries if x["finalist"] == fi)
        tag = "最適化済み" if pe["adopted"] else "元型維持"
        bc = pe["best_candidate"]
        chg = " / ".join(bc["tags"]) if pe["adopted"] and bc else "-"
        print(
            f"新{new_rank_pos[fi]:2d}位 (元{pe['orig_rank_both']:2d}位) #{fi:2d} [{tag}] "
            f"新平均 {r['mean']*100:5.1f}% (最悪 {r['worst']*100:4.1f}%) | {chg}"
        )
        print(f"    {polished_finalists[fi]}")


if __name__ == "__main__":
    main()
