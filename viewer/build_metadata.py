#!/usr/bin/env python3
"""Build viewer/metadata.js from engine/stage8/species_data.h, catalog-all, and items_ja.json."""
import json
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent

TYPES = [
    {"id": "normal", "en": "Normal", "ja": "ノーマル"},
    {"id": "fire", "en": "Fire", "ja": "ほのお"},
    {"id": "water", "en": "Water", "ja": "みず"},
    {"id": "electric", "en": "Electric", "ja": "でんき"},
    {"id": "grass", "en": "Grass", "ja": "くさ"},
    {"id": "ice", "en": "Ice", "ja": "こおり"},
    {"id": "fighting", "en": "Fighting", "ja": "かくとう"},
    {"id": "poison", "en": "Poison", "ja": "どく"},
    {"id": "ground", "en": "Ground", "ja": "じめん"},
    {"id": "flying", "en": "Flying", "ja": "ひこう"},
    {"id": "psychic", "en": "Psychic", "ja": "エスパー"},
    {"id": "bug", "en": "Bug", "ja": "むし"},
    {"id": "rock", "en": "Rock", "ja": "いわ"},
    {"id": "ghost", "en": "Ghost", "ja": "ゴースト"},
    {"id": "dragon", "en": "Dragon", "ja": "ドラゴン"},
    {"id": "dark", "en": "Dark", "ja": "あく"},
    {"id": "steel", "en": "Steel", "ja": "はがね"},
    {"id": "fairy", "en": "Fairy", "ja": "フェアリー"},
]


def _extract_block(header, decl):
    return header.split(decl + " = {\n", 1)[1].split("\n};", 1)[0]


def ensure_catalog_exe(cc="gcc"):
    exe_name = "population3_catalog.exe" if os.name == "nt" else "population3_catalog"
    exe = ROOT / "search" / exe_name
    src = ROOT / "search" / "population3_catalog.c"
    if not exe.exists() or src.stat().st_mtime > exe.stat().st_mtime:
        subprocess.run(
            [cc, "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror", str(src), "-lm", "-o", str(exe)],
            check=True, cwd=ROOT,
        )
    return exe


def build_metadata(cc="gcc", catalog_exe=None):
    header = (ROOT / "engine" / "stage8" / "species_data.h").read_text(encoding="utf-8")
    items_ja = json.loads((HERE / "items_ja.json").read_text(encoding="utf-8"))

    # Parse S8_SPECIES_ABILITIES and S8_MEGA_LINKS
    sp_ab_nums = [int(x) for x in re.findall(r"-?\d+", _extract_block(header, "static const int S8_SPECIES_ABILITIES[]"))]
    mega_links = [
        tuple(map(int, m))
        for m in re.findall(r"\{(\d+),(\d+),(\d+)\}", _extract_block(header, "static const S8MegaLink S8_MEGA_LINKS[]"))
    ]

    # Parse S8_SPECIES
    sp_rows = []
    sp_pat = re.compile(
        r'\{"([^"]+)", "([^"]+)", "([^"]+)", (\d+), (\d+), (\d+), (\d+), '
        r'\{(-?\d+),(-?\d+)\}, \{([^}]+)\}, (\d+), (\d+), (\d+), (\d+), (\d+), (\d+), (\d+), (\d+)\}'
    )
    for line in _extract_block(header, "static const S8Species S8_SPECIES[]").splitlines():
        m = sp_pat.search(line)
        if not m:
            continue
        sp_rows.append({
            "idx": len(sp_rows),
            "id": m.group(1),
            "en": m.group(2),
            "ja": m.group(3),
            "base_id": int(m.group(4)),
            "form_kind": int(m.group(5)),
            "initial": int(m.group(6)),
            "types": [int(m.group(8)), int(m.group(9))],
            "base": [int(x) for x in m.group(10).split(",")],
            "sex_mask": int(m.group(12)),
            "ab_off": int(m.group(13)),
            "ab_cnt": int(m.group(14)),
            "mega_off": int(m.group(17)),
            "mega_cnt": int(m.group(18)),
        })

    # Parse S8_MOVES
    moves = {}
    mv_pat = re.compile(
        r'\{"([^"]+)", "([^"]+)", "([^"]+)", "([^"]+)", "[^"]+", '
        r'(\d+), (\d+), (\d+), (\d+), (-?\d+), (\d+), (\d+),'
    )
    for line in _extract_block(header, "static const S8Move S8_MOVES[]").splitlines():
        m = mv_pat.search(line)
        if not m:
            continue
        moves[m.group(1)] = {
            "code": m.group(2),
            "en": m.group(3),
            "ja": m.group(4),
            "t": int(m.group(5)),
            "c": int(m.group(6)),
            "p": int(m.group(7)),
            "acc": int(m.group(8)),
            "prio": int(m.group(9)),
            "contact": int(m.group(10)),
            "pp": int(m.group(11)),
            "exec": False,
        }

    # Parse S8_ABILITIES
    ab_list = []
    abilities = {}
    ab_pat = re.compile(r'\{"([^"]+)", "([^"]+)", "([^"]+)", (\d+)\}')
    for line in _extract_block(header, "static const S8Ability S8_ABILITIES[]").splitlines():
        m = ab_pat.search(line)
        if not m:
            continue
        ab_list.append(m.group(1))
        abilities[m.group(1)] = {"en": m.group(2), "ja": m.group(3)}

    # Parse S8_ITEMS
    item_list = []
    items = {"none": {"en": "None", "ja": items_ja.get("none", "なし"), "stone": 0, "legal": True}}
    it_pat = re.compile(r'\{"([^"]+)", "([^"]+)", (\d+)\}')
    for line in _extract_block(header, "static const S8Item S8_ITEMS[]").splitlines():
        m = it_pat.search(line)
        if not m:
            continue
        iid = m.group(1)
        item_list.append(iid)
        items[iid] = {
            "en": m.group(2),
            "ja": items_ja.get(iid, m.group(2)),
            "stone": int(m.group(3)),
            "legal": False,
        }

    # Parse S8_TYPE_CHART
    chart_text = _extract_block(header, "static const double S8_TYPE_CHART[18][18]")
    type_chart = [
        [float(x) if "." in x else int(x) for x in row.split(",")]
        for row in re.findall(r"\{([^}]+)\}", chart_text)
    ]

    # Query catalog-all from compiled C engine admissibility rules
    exe = Path(catalog_exe) if catalog_exe else ensure_catalog_exe(cc)
    cat_out = subprocess.check_output([str(exe), "catalog-all"], text=True, encoding="utf-8", cwd=ROOT)
    cat_moves = {}
    cat_profiles = {}
    cur_sp = None
    for line in cat_out.splitlines():
        parts = line.split("\t")
        if parts[0] == "species":
            cur_sp = parts[1]
            cat_moves[cur_sp] = {}
            cat_profiles[cur_sp] = []
        elif parts[0] == "move":
            sex, mv = parts[1], parts[2]
            cat_moves[cur_sp].setdefault(sex, []).append(mv)
            if mv in moves:
                moves[mv]["exec"] = True
        elif parts[0] == "profile":
            sex, item, mega, ab = parts[1], parts[2], int(parts[3]), parts[4]
            cat_profiles[cur_sp].append((sex, item, mega, ab))
            if item in items:
                items[item]["legal"] = True

    sp_by_id = {r["id"]: r for r in sp_rows}
    species = {}
    for r in sp_rows:
        if not (r["initial"] and r["form_kind"] == 0):
            continue
        sid = r["id"]
        profs = cat_profiles.get(sid, [])
        sexes = sorted({p[0] for p in profs}, key=lambda s: ("m", "f", "n").index(s))
        norm_abs = {}
        for sex in sexes:
            abs_for_sex = []
            for p_sex, _, p_mega, p_ab in profs:
                if p_sex == sex and p_mega in (0, 2) and p_ab not in abs_for_sex:
                    abs_for_sex.append(p_ab)
            norm_abs[sex] = abs_for_sex

        # Mega forms (including female meowstic special case)
        mega_map = {}
        sex_sources = [(sex, sp_by_id["meowsticf"] if sid == "meowstic" and sex == "f" else r) for sex in sexes]
        for sex, src in sex_sources:
            for k in range(src["mega_cnt"]):
                _, tgt_idx, stone_idx = mega_links[src["mega_off"] + k]
                tgt = sp_rows[tgt_idx]
                stone_id = item_list[stone_idx]
                mega_ab = ab_list[sp_ab_nums[tgt["ab_off"]]]
                legal_m1 = any(p[0] == sex and p[1] == stone_id and p[2] == 1 for p in profs)
                legal_m2 = any(p[0] == sex and p[1] == stone_id and p[2] == 2 for p in profs)
                if not (legal_m1 or legal_m2):
                    continue
                entry = mega_map.setdefault((tgt["id"], stone_id), {
                    "id": tgt["id"],
                    "en": tgt["en"],
                    "ja": tgt["ja"],
                    "stone": stone_id,
                    "types": tgt["types"],
                    "base": tgt["base"],
                    "ability": mega_ab,
                    "sexes": [],
                    "modes": sorted({m for m, ok in ((1, legal_m1), (2, legal_m2)) if ok}),
                })
                if sex not in entry["sexes"]:
                    entry["sexes"].append(sex)

        sp_obj = {
            "id": sid,
            "en": r["en"],
            "ja": r["ja"],
            "types": r["types"],
            "base": r["base"],
            "sexes": sexes,
            "abilities": norm_abs,
            "moves": {sex: cat_moves[sid].get(sex, []) for sex in sexes},
            "megas": list(mega_map.values()),
        }
        if sid == "meowstic" and "meowsticf" in sp_by_id:
            rf = sp_by_id["meowsticf"]
            sp_obj["femaleNormal"] = {"types": rf["types"], "base": rf["base"]}
        species[sid] = sp_obj

    meta = {
        "types": TYPES,
        "typeChart": type_chart,
        "moves": moves,
        "abilities": abilities,
        "items": items,
        "species": species,
    }
    out_path = HERE / "metadata.js"
    out_path.write_text(
        "// Generated by viewer/build_metadata.py; do not edit.\n"
        "window.CHAMPIONS_META = " + json.dumps(meta, ensure_ascii=False, separators=(",", ":")) + ";\n",
        encoding="utf-8",
    )
    return out_path, meta


if __name__ == "__main__":
    out, meta = build_metadata(sys.argv[1] if len(sys.argv) > 1 else "gcc")
    print(f"wrote {out} ({out.stat().st_size} bytes): {len(meta['species'])} species, {len(meta['moves'])} moves")
