#!/usr/bin/env python3
"""Pictures and stats for the admin Bosses/Hunters editors, all from the
client's own files: data/admin-art/art.json plus PNGs under data/admin-art/.

  ships    "<Class><H|M|L>_T<n>" -> ships/<that>.png  (Generic/UI/ships/Tiers,
           the hangar's tier art, 512x320 downscaled to 256x160)
  modules  ability class path -> name, English description, icon, cooldown,
           range, tooltip rows (AB_* Default__ walked up its parent classes,
           since a child only stores what it overrides; texts from the en locres)
  weapons  WP_* blueprint name -> name, description, icon, tooltip rows, and
           the DN_Weapons_OTS_DT stat row of the same name
  paints   coating item id -> the colour set's base/custom 1-3/light colours
           (linear -> sRGB hex), for swatches
  vanity   pattern/decal/emblem item id -> picture (data/vanity/VanityIcons.json,
           already decoded into data/market-images)
  feats    Havoc boost feat -> title, description, icon (DN_HavocBoosts_DT rows)
  officers briefing feat (MODULERECYCLER) -> name, description, icon, type,
           tooltip rows (Generic/Officer/Perk/PRK_*_BP, the _Hero ones skipped)
  pawns    "<Class><H|M|L>[_T<n>]" AI pawn -> hull, hull regen, regen delay,
           energy, energy regen, ship mesh (walked up the blueprint parents
           into Generic/Ships, where the components' defaults live)
  ui       tier groups / tiers / manufacturers / classes -> the client's own
           badge, numeral, logo and class-symbol art under ui/, plus each
           picture's 4-colour palette (by pixel count) for borders
  previews decal / shared pattern id -> previews/<id>.png, its own texture,
           where the market icon is only the slot's generic picture
  patterns hull pattern id -> hull line + colour-mask PNGs per material (models/)
  models   (--models) ship mesh glTF + diffuse/colour-mask textures under
           models/, per hull line's base mesh and per hero ship part (category
           20) and emblem (21, figureheads), exported by tools/meshx; models/ is not committed (~100 MB)

Needs the client content extracted (tools/pakx.py) under DN_ART_CONTENT
(default /tmp/dnart/DreadGame/Content): Generic/UI/{ships,abilities,weapons},
Generic/Abilities/**/AB_*, Generic/Weapons/**/WP_*, VanityItems colour sets and
Localization/DreadGame/en. --extract runs pakx for those first.
"""
import glob
import json
import os
import re
import struct
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
import tex2png  # noqa: E402

CONTENT = os.environ.get("DN_ART_CONTENT", "/tmp/dnart/DreadGame/Content")
OUT = os.path.join(REPO, "data", "admin-art")
BPDUMP = os.path.join(REPO, "tools", "bpdump", "bin", "Debug", "net10.0", "bpdump.dll")
PAKS = os.environ.get(
    "DN_PAKS",
    os.path.expanduser("~/.local/share/Steam/steamapps/common/Dreadnought/"
                       "DreadGame/DreadGame/Content/Paks"))
EXTRACT = (r"Generic/UI/(ships/Tiers|abilities|weapons|perks|Havoc)/|Generic/Abilities/.*/AB_[^/]*\.uasset$|"
           r"Generic/Weapons/.*/WP_[^/]*\.uasset$|VanityItems/_Shared/ColorSets/|Localization/DreadGame/en/|"
           r"Generic/Officer/Perk/|DataTables/Progression/Havoc/DN_HavocBoosts_DT|Generic/AI/AIPawns/VH_|"
           r"Generic/Ships/.*/VH_[^/]*Pawn[^/]*_BP\.uasset$|SelectFleet/UI_SelectFleet_Icon|Generic/UI/tiers/|"
           r"ManufacturersScreen/UI_ManufacturersScreen_(Secondary)?[A-Za-z]+Logo|ShipIcons/UI_ShipClass_|"
           r"VanityItems/(_Shared/Decal/|Patterns/)|DevGroup/Meta/Factions/Texture/VAN_DCL_|"
           r"DataTables/UI/DN_QuestMarker|Generic/UI/UI_HUD_ShortCommands_")
MESHX = os.path.join(REPO, "tools", "meshx", "bin", "Debug", "net10.0", "meshx.dll")


def fstring(b, o):
    n = struct.unpack_from("<i", b, o)[0]
    o += 4
    if n == 0:
        return "", o
    if n < 0:
        n = -n
        return b[o:o + 2 * n].decode("utf-16le").rstrip("\0"), o + 2 * n
    return b[o:o + n].decode("latin1").rstrip("\0"), o + n


def parse_locres(path):
    """UE 4.13 legacy .locres (same reader as gen-item-headline-keys.py)."""
    b = open(path, "rb").read()
    out, o = {}, 4
    for _ in range(struct.unpack_from("<I", b, 0)[0]):
        _, o = fstring(b, o)
        keys = struct.unpack_from("<I", b, o)[0]
        o += 4
        for _ in range(keys):
            key, o = fstring(b, o)
            o += 4
            text, o = fstring(b, o)
            out[key] = text
    return out


def game_path(file_key):
    """'DreadGame/Content/Generic/X/AB_Y_BP.uasset' -> class path."""
    rel = file_key.split("/Content/", 1)[1][:-len(".uasset")]
    base = os.path.basename(rel)
    return f"/Game/{rel}.{base}_C"


def dump(subdir, pattern):
    out = subprocess.run(["dotnet", BPDUMP, "--loadouts", os.path.join(CONTENT, subdir), pattern],
                         capture_output=True, text=True, timeout=900).stdout
    rows = {}
    for line in out.splitlines():
        try:
            r = json.loads(line)
        except ValueError:
            continue
        if "error" not in r:
            r["_file"] = os.path.join(os.path.dirname(CONTENT), r["file"].split("/", 1)[1])
            rows[game_path(r["file"])] = r
    return rows


def inherited(rows, path, key):
    seen = set()
    while path in rows and path not in seen:
        seen.add(path)
        v = rows[path].get(key)
        if v not in (None, "", [], {}):
            return v
        path = rows[path].get("super")
    return None


def ui_field(rows, path, field):
    seen = set()
    while path in rows and path not in seen:
        seen.add(path)
        v = (rows[path].get("m_itemUIData") or {}).get(field)
        if v not in (None, "", []):
            return v
        path = rows[path].get("super")
    return None


ICONS = {}
LOWER = {}


def icon(asset_path, sub):
    """'/Game/Generic/UI/abilities/UI_X.UI_X' -> 'icons/<sub>/UI_X.png' (decoded once)."""
    if not asset_path:
        return ""
    pkg = asset_path.split(".")[0]
    name = os.path.basename(pkg)
    if name in ICONS:
        return ICONS[name]
    src = os.path.join(CONTENT, pkg[len("/Game/"):] + ".uasset")
    if not os.path.exists(src):  # some references are lower-cased (ui_ability_purgeram)
        if not LOWER:
            for f in glob.glob(os.path.join(CONTENT, "Generic/UI/**/*.uasset"), recursive=True):
                LOWER[f.lower()] = f
        src = LOWER.get(src.lower(), src)
    rel = f"icons/{sub}/{name}.png"
    ICONS[name] = ""
    try:
        img = tex2png.decode(open(src, "rb").read())
        os.makedirs(os.path.join(OUT, "icons", sub), exist_ok=True)
        if img.width > 128:
            img.thumbnail((128, 128))
        img.save(os.path.join(OUT, rel), optimize=True)
        ICONS[name] = rel
    except Exception as e:  # missing or not a texture
        print(f"  icon {name}: {e}", file=sys.stderr)
    return ICONS[name]


UI_ART = {
    # Select-fleet screen badges: bronze/silver/gold.
    "groups": {g: f"UserInterface/Textures/SelectFleet/UI_SelectFleet_Icon{g.title()}" for g in ("recruit", "veteran", "legendary")},
    "tiers": {str(n): f"Generic/UI/tiers/UI_tier_{n}" for n in range(1, 6)},
    "manufacturers": {m: f"UserInterface/Textures/ManufacturersScreen/UI_ManufacturersScreen_{m}Logo"
                      for m in ("JupiterArms", "AkulaVektor", "Oberon")},
    "wordmarks": {m: f"UserInterface/Textures/ManufacturersScreen/UI_ManufacturersScreen_Secondary{m}Logo"
                  for m in ("JupiterArms", "AkulaVektor", "Oberon")},
    "classes": {c: f"UserInterface/Textures/ShipIcons/UI_ShipClass_{n}" for c, n in (
        ("Scout", "Corvette"), ("Support", "TacticalCruiser"), ("Assault", "Destroyer"),
        ("Sniper", "ArtilleryCruiser"), ("Dreadnought", "Dreadnought"))},
}


def ui():
    """{kind: {key: {img, pal}}}; pal = the picture's 4 quantized colours, most pixels first."""
    from PIL import Image
    out = {}
    os.makedirs(os.path.join(OUT, "ui"), exist_ok=True)
    for kind, items in UI_ART.items():
        out[kind] = {}
        for key, rel in items.items():
            try:
                img = tex2png.decode(open(os.path.join(CONTENT, rel + ".uasset"), "rb").read()).convert("RGBA")
            except Exception as e:
                print(f"  ui {rel}: {e}", file=sys.stderr)
                continue
            img.thumbnail((256, 256))
            name = f"ui/{os.path.basename(rel)}.png"
            img.save(os.path.join(OUT, name), optimize=True)
            px = [q[:3] for q in img.resize((96, 96)).get_flattened_data() if q[3] > 200]
            pal = []
            if px:
                q = Image.new("RGB", (len(px), 1))
                q.putdata(px)
                q = q.quantize(4)
                p = q.getpalette()
                pal = ["#%02x%02x%02x" % tuple(p[i * 3:i * 3 + 3]) for _, i in sorted(q.getcolors(), reverse=True)]
            out[kind][key] = {"img": name, "pal": pal}
    return out


def text(loc, key):
    return loc.get(key, "") if key else ""


def tips(loc, rows, path):
    out = []
    for t in ui_field(rows, path, "m_toolTipValues") or []:
        n, v = text(loc, t.get("m_name")), text(loc, t.get("m_value"))
        if n or v:
            out.append([n, v])
    return out


def ships():
    out = {}
    os.makedirs(os.path.join(OUT, "ships"), exist_ok=True)
    for src in sorted(glob.glob(os.path.join(CONTENT, "Generic/UI/ships/Tiers/UI_*_T[0-9].uasset"))):
        key = os.path.basename(src)[3:-len(".uasset")]
        try:
            img = tex2png.decode(open(src, "rb").read())
        except Exception as e:
            print(f"  ship {key}: {e}", file=sys.stderr)
            continue
        img.thumbnail((256, 160))
        img.save(os.path.join(OUT, "ships", key + ".png"), optimize=True)
        out[key] = f"ships/{key}.png"
    return out


def modules(loc):
    rows = dump("Generic/Abilities", "AB_*.uasset")
    out = {}
    for path, r in rows.items():
        name = inherited(rows, path, "m_abilityName") or ""
        head = text(loc, ui_field(rows, path, "m_headline"))
        out[path] = {
            "name": head or name,
            "desc": text(loc, ui_field(rows, path, "m_description")),
            "icon": icon(ui_field(rows, path, "m_icon"), "abilities"),
            "cooldown": inherited(rows, path, "m_coolDown"),
            "active": inherited(rows, path, "m_activeTime"),
            "range": inherited(rows, path, "m_maxDistance"),
            "tips": tips(loc, rows, path),
        }
    return out


WEAPON_ICON = re.compile(rb"/Game/Generic/UI/weapons/(UI_weapon_\d+)(?:_High)?\b")


def weapons(loc):
    rows = dump("Generic/Weapons", "WP_*.uasset")
    try:
        stats = json.load(open(os.path.join(REPO, "data", "datatables", "DN_Weapons_OTS_DT.json")))["rows"]
    except (OSError, ValueError, KeyError):
        stats = {}
    out = {}
    for path, r in rows.items():
        # The icon is a UI_weapon_<n> import the blueprint (or a parent) names.
        ic, p, seen = "", path, set()
        while p in rows and p not in seen and not ic:
            seen.add(p)
            m = WEAPON_ICON.search(open(rows[p]["_file"], "rb").read())
            if m:
                name = m.group(1).decode()
                ic = icon(f"/Game/Generic/UI/weapons/{name}.{name}", "weapons")
            p = rows[p].get("super")
        bp = os.path.basename(path.split(".")[0])
        out[bp] = {
            "name": text(loc, ui_field(rows, path, "m_headline")) or inherited(rows, path, "m_name") or bp,
            "desc": text(loc, ui_field(rows, path, "m_description")),
            "icon": ic,
            "tips": tips(loc, rows, path),
            "stats": stats.get(bp, {}),
        }
    return out


LINEAR = re.compile(r"^\s+(m_baseColor|m_customColor0[123]|m_lightColor) = \(([-\d.e, ]+)\)")


def srgb(c):
    c = max(0.0, min(1.0, c))
    return round(255 * (12.92 * c if c <= 0.0031308 else 1.055 * c ** (1 / 2.4) - 0.055))


def paints():
    sets = {}
    for src in sorted(glob.glob(os.path.join(CONTENT, "Generic/VanityItems/_Shared/ColorSets/**/*.uasset"),
                                recursive=True)):
        res = subprocess.run(["dotnet", BPDUMP, "--props", src], capture_output=True, text=True, timeout=60)
        cols = {}
        for line in res.stdout.splitlines():
            m = LINEAR.match(line)
            if m:
                r, g, b = (float(x) for x in m.group(2).split(",")[:3])
                cols[m.group(1)] = "#%02x%02x%02x" % (srgb(r), srgb(g), srgb(b))
        rel = src.split("/Content/", 1)[1][:-len(".uasset")]
        sets[f"/Game/{rel}.{os.path.basename(rel)}"] = [cols.get(k, "") for k in (
            "m_baseColor", "m_customColor01", "m_customColor02", "m_customColor03", "m_lightColor")]
    out = {}
    with open(os.path.join(REPO, "data", "vanity", "VanityItems_cooked.jsonl")) as f:
        for line in f:
            v = json.loads(line)
            iid = (v.get("m_itemSystemData") or {}).get("m_itemID", 0)
            cs = v.get("m_colorSet")
            if (iid >> 24) & 0xff == 22 and cs in sets:
                out[str(iid)] = sets[cs]
    return out


def feats(loc):
    src = os.path.join(CONTENT, "Generic/DataTables/Progression/Havoc/DN_HavocBoosts_DT.uasset")
    res = subprocess.run(["dotnet", BPDUMP, "--rows", src], capture_output=True, text=True, timeout=120)
    out = {}
    for line in res.stdout.splitlines():
        r = json.loads(line)
        if "row" not in r:
            continue
        ic = re.search(r"'(/Game/[^']+)'", r.get("m_iconPath") or "")
        out[r.get("m_feats") or r["row"]] = {
            "title": text(loc, r.get("m_title")),
            "desc": text(loc, r.get("m_description")),
            "icon": icon(ic.group(1) if ic else "", "feats"),
        }
    return out


def officers(loc):
    rows = dump("Generic/Officer/Perk", "PRK_*.uasset")
    out = {}
    for path, r in rows.items():
        if "_Hero_" in path or not r.get("m_perkFeatName"):
            continue
        m = re.search(r"/PRK_(COM|ENG|NAV|WPN)_", path)
        out[r["m_perkFeatName"]] = {
            "id": (r.get("m_itemSystemData") or {}).get("m_itemID", 0),
            "name": text(loc, ui_field(rows, path, "m_headline")) or r.get("m_perkName", ""),
            "desc": text(loc, ui_field(rows, path, "m_description")) or r.get("m_description", ""),
            "icon": icon(ui_field(rows, path, "m_icon"), "perks"),
            "type": m.group(1).lower() if m else "",
            "tips": tips(loc, rows, path),
            "bp": os.path.basename(path.split(".")[0]),
        }
    return out


PAWN_STATS = re.compile(r"^\s+(m_maxHealth|m_passiveRegenAmount|m_passiveRegenerationDelay|m_maxEnergy|"
                        r"m_energyRegenerationRate) = ([-\d.e]+)\s+\[FloatProperty\]")
SHIP_MESH = re.compile(r'"export":"ShipMesh".*?"SkeletalMesh":"([^"]+)"')


def pawns():
    rows = dump("Generic/AI/AIPawns", "VH_*Pawn*_BP.uasset")
    rows.update(dump("Generic/Ships", "VH_*Pawn*_BP.uasset"))
    own = {}
    for path, r in rows.items():
        st = {}
        res = subprocess.run(["dotnet", BPDUMP, "--props", r["_file"]], capture_output=True, text=True, timeout=60)
        for line in res.stdout.splitlines():
            m = PAWN_STATS.match(line)
            if m:
                st.setdefault(m.group(1), float(m.group(2)))
        res = subprocess.run(["dotnet", BPDUMP, "--rows", r["_file"]], capture_output=True, text=True, timeout=60)
        m = SHIP_MESH.search(res.stdout)
        if m:
            st["mesh"] = m.group(1)
        own[path] = st
    out = {}
    names = {"m_maxHealth": "hull", "m_passiveRegenAmount": "regen", "m_passiveRegenerationDelay": "regen_delay",
             "m_maxEnergy": "energy", "m_energyRegenerationRate": "energy_regen", "mesh": "mesh"}
    for path in rows:
        m = re.search(r"/VH_(Assault|Dread|Scout|Sniper|Support)([HML])_AI_Pawn(?:_T(\d))?_BP\.", path, re.I)
        if not m or "/Havoc/" in path:
            continue
        key = m.group(1).capitalize() + m.group(2).upper() + (f"_T{m.group(3)}" if m.group(3) else "")
        st, p, seen = {}, path, set()
        while p in rows and p not in seen:  # the child's own value wins
            seen.add(p)
            for k, v in own.get(p, {}).items():
                st.setdefault(names[k], v)
            p = rows[p].get("super")
        if "mesh" not in st:
            # GUESS: VH_AssaultM_Pawn_BP's ShipMesh names no SkeletalMesh (the
            # one hull line without it); its mesh follows the others' naming,
            # VH_<Class><W>_SK beside the player pawn (exports with that name).
            c = "Dread" if m.group(1).lower() == "dread" else m.group(1).capitalize()
            d = {"Dread": "Dreadnought"}.get(c, c)
            w = {"H": "Heavy", "M": "Medium", "L": "Light"}[m.group(2).upper()]
            n = f"VH_{c}{m.group(2).upper()}_SK"
            st["mesh"] = f"/Game/Generic/Ships/{d}/{w}/{n}.{n}"
        out[key] = st
    return out


def models(pawn_stats):
    """Base hull meshes + hero ship parts -> models/ (glb + 512 px textures)."""
    from PIL import Image
    meshes = {}  # object path -> key
    for key, st in pawn_stats.items():
        if st.get("mesh") and "_T" not in key:
            meshes[st["mesh"]] = "base:" + key
    with open(os.path.join(REPO, "data", "vanity", "VanityItems_cooked.jsonl")) as f:
        for line in f:
            v = json.loads(line)
            iid = (v.get("m_itemSystemData") or {}).get("m_itemID", 0)
            if (iid >> 24) & 0xff == 20 and v.get("m_mesh"):
                meshes.setdefault(v["m_mesh"], f"part:{iid}")
            if (iid >> 24) & 0xff == 21 and v.get("m_mesh"):  # emblems are figurehead meshes
                meshes.setdefault(v["m_mesh"], f"emblem:{iid}")
    tmp = "/tmp/dn-meshx"
    subprocess.run(["rm", "-rf", tmp], check=True)
    res = subprocess.run(["dotnet", MESHX, PAKS, tmp], input="\n".join(meshes) + "\n", capture_output=True,
                         text=True, timeout=3600)
    print(res.stdout.strip().splitlines()[-1] if res.stdout.strip() else res.stderr[-400:])
    mdir = os.path.join(OUT, "models")
    os.makedirs(mdir, exist_ok=True)
    out, tex_done = {"base": {}, "parts": {}, "emblems": {}}, {}

    def tex(name, kind):
        if name in tex_done:
            return tex_done[name]
        src = os.path.join(tmp, name + ".png")
        rel = ""
        if os.path.exists(src):
            img = Image.open(src)
            img.thumbnail((512, 512))
            if kind == "d":
                rel = f"models/{name}.jpg"
                img.convert("RGB").save(os.path.join(OUT, rel), quality=82)
            else:
                rel = f"models/{name}.png"
                img.convert("RGB").save(os.path.join(OUT, rel), optimize=True)
        tex_done[name] = rel
        return rel

    for obj, key in meshes.items():
        asset = os.path.basename(obj.split(".")[0])
        glb = os.path.join(tmp, asset + ".glb")
        if not os.path.exists(glb):
            print(f"  model {asset}: not exported", file=sys.stderr)
            continue
        b = open(glb, "rb").read()
        n = struct.unpack_from("<I", b, 12)[0]
        doc = json.loads(b[20:20 + n])
        mats = []
        for mt in doc.get("materials", []):
            d = mc = ""
            try:
                inst = json.load(open(os.path.join(tmp, mt.get("name", "") + ".json")))
                t = inst.get("Textures", {})
                pick = lambda *ks: next((re.sub(r"^Texture2D'|'$", "", t[k]["ObjectName"]) for k in ks if k in t), "")
                d, mc = tex(pick("Albedo", "PM_Diffuse", "Diffuse"), "d"), tex(pick("colorMask", "PM_SpecularMasks"), "mc")
            except (OSError, ValueError, KeyError):
                pass
            mats.append({"d": d, "mc": mc})
        rel = f"models/{asset}.glb"
        with open(os.path.join(OUT, rel), "wb") as f:
            f.write(b)
        kind, k = key.split(":", 1)
        out[{"base": "base", "part": "parts", "emblem": "emblems"}[kind]][k] = {"glb": rel, "mats": mats}
    return out


def vanity_rows():
    with open(os.path.join(REPO, "data", "vanity", "VanityItems_cooked.jsonl")) as f:
        for line in f:
            v = json.loads(line)
            yield (v.get("m_itemSystemData") or {}).get("m_itemID", 0), v


def asset_file(obj):
    return os.path.join(CONTENT, obj.split(".")[0].replace("/Game/", "", 1) + ".uasset")


def previews(icons):
    """Decals and patterns whose market icon is only the slot's generic one (or none):
    the decal's own texture / the shared pattern's detail mask -> previews/<id>.png."""
    from PIL import Image
    os.makedirs(os.path.join(OUT, "previews"), exist_ok=True)
    out = {}
    for iid, v in vanity_rows():
        if (iid >> 24) not in (23, 24) or not icons.get(str(iid), "UI_Vanity_").startswith("UI_Vanity_"):
            continue
        src = v.get("m_texture") or v.get("m_detailColorMask")
        if not src:
            continue
        try:
            img = tex2png.decode(open(asset_file(src), "rb").read())
        except (OSError, SystemExit, Exception) as e:
            print(f"  preview {iid} {src}: {e}", file=sys.stderr)
            continue
        img.thumbnail((192, 192))
        rel = f"previews/{iid}.png"
        img.save(os.path.join(OUT, rel), optimize=True)
        out[str(iid)] = rel
    return out


def patterns():
    """Hull patterns (m_colorMaskTextures): pattern id -> {line, mc: {ID01|ID02|"": png}}.
    Each texture replaces the hull material's colour mask whose name has the same IDnn."""
    from PIL import Image
    os.makedirs(os.path.join(OUT, "models"), exist_ok=True)
    out = {}
    for iid, v in vanity_rows():
        texs = v.get("m_colorMaskTextures") or []
        m = re.search(r"/Patterns/(\w+)/(Heavy|Medium|Light)/", v.get("file", ""))
        if (iid >> 24) != 23 or not texs or not m:
            continue
        mc = {}
        for t in texs:
            name = os.path.basename(t.split(".")[0])
            rel = f"models/{name}.png"
            try:
                if not os.path.exists(os.path.join(OUT, rel)):
                    img = tex2png.decode(open(asset_file(t), "rb").read())
                    img.thumbnail((512, 512))
                    img.convert("RGB").save(os.path.join(OUT, rel), optimize=True)
            except (OSError, SystemExit, Exception) as e:
                print(f"  pattern {iid} {name}: {e}", file=sys.stderr)
                continue
            idm = re.search(r"_(ID0\d)_", name)
            mc[idm.group(1) if idm else ""] = rel
        if mc:
            out[str(iid)] = {"line": m.group(1).replace("Dreadnought", "Dread") + m.group(2)[0], "mc": mc}
    return out


def markers(loc):
    """HUD quest markers (DN_QuestMarker_DT; Onslaught maps use DN_QuestMarker_Onslaught_DT):
    {row: {marker: EYUIQM_*, icon, onslaught_icon, text, style}}. The HUD draws a row's own
    icon for the EYUIQuestMarker value the server sends (MulticastShowHUDMarkerOnActor)."""
    out = {}
    for table, key in (("DN_QuestMarker_DT", "icon"), ("DN_QuestMarker_Onslaught_DT", "onslaught_icon")):
        src = os.path.join(CONTENT, "Generic/DataTables/UI", table + ".uasset")
        try:
            res = subprocess.run(["dotnet", BPDUMP, "--rows", src], capture_output=True, text=True, timeout=300)
            cooked = json.load(open(os.path.join(REPO, "data", "datatables", "UI", table + ".json")))["rows"]
        except (OSError, ValueError, subprocess.SubprocessError) as e:
            print(f"  markers {table}: {e}", file=sys.stderr)
            continue
        for line in res.stdout.splitlines():
            r = json.loads(line)
            if "row" not in r:
                continue
            c = cooked.get(r["row"], {})
            m = out.setdefault(r["row"], {"marker": c.get("m_marker", "").split("::")[-1],
                                         "text": text(loc, r.get("m_markerText")),
                                         "style": c.get("m_questMarkerColorStyle", "").split("::")[-1]})
            m[key] = icon(r.get("m_icon"), "markers")
    return out


def main():
    if "--extract" in sys.argv:
        subprocess.run([sys.executable, os.path.join(REPO, "tools", "pakx.py"), PAKS, EXTRACT,
                        os.path.dirname(os.path.dirname(CONTENT))], check=True, timeout=1800)
    loc = {}
    for p in sorted(glob.glob(os.path.join(CONTENT, "Localization/DreadGame/en/*.locres"))):
        loc.update(parse_locres(p))
    if not loc:
        sys.exit(f"no English .locres under {CONTENT}; run with --extract")
    os.makedirs(OUT, exist_ok=True)
    art = {"ships": ships()}
    print(f"ships {len(art['ships'])}")
    art["modules"] = modules(loc)
    print(f"modules {len(art['modules'])}")
    art["weapons"] = weapons(loc)
    print(f"weapons {len(art['weapons'])}")
    art["paints"] = paints()
    print(f"paints {len(art['paints'])}")
    art["feats"] = feats(loc)
    print(f"feats {len(art['feats'])}")
    art["officers"] = officers(loc)
    print(f"officers {len(art['officers'])}")
    art["ui"] = ui()
    art["markers"] = markers(loc)
    print(f"markers {len(art['markers'])}")
    art["pawns"] = pawns()
    print(f"pawns {len(art['pawns'])}")
    if "--models" in sys.argv:
        art["models"] = models(art["pawns"])
        print(f"models {len(art['models']['base'])} base, {len(art['models']['parts'])} parts")
    else:
        try:
            art["models"] = json.load(open(os.path.join(OUT, "art.json"))).get("models", {})
        except (OSError, ValueError):
            art["models"] = {}
    try:
        art["vanity"] = json.load(open(os.path.join(REPO, "data", "vanity", "VanityIcons.json")))
    except (OSError, ValueError):
        art["vanity"] = {}
    art["previews"] = previews(art["vanity"])
    art["patterns"] = patterns()
    print(f"previews {len(art['previews'])}, hull patterns {len(art['patterns'])}")
    with open(os.path.join(OUT, "art.json"), "w") as f:
        json.dump(art, f, separators=(",", ":"), sort_keys=True)
    print(f"icons {sum(1 for v in ICONS.values() if v)} -> {OUT}")


if __name__ == "__main__":
    main()
