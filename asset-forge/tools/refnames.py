"""Hand-supplied scientific names, and an INDEPENDENT check that each one is
the animal the spec is named after.

WHY THIS FILE EXISTS. `tools/reffit.py resolve_latin` asks GBIF to turn a common
name into a scientific one, and constrained to Mammalia it is right most of the
time. Most is not enough when the output selects which photographs a species is
fitted against. From the run of 2026-08-15, verbatim from
`refs/species-latin.json` before correction:

    lion              -> Macaca silenus      (a lion-tailed macaque)
    nile-crocodile    -> Tragelaphus gratus  (an antelope)
    nile-monitor      -> Mungos mungo        (a banded mongoose)
    spectacled-caiman -> Cebus imitator      (a capuchin monkey)
    fisher            -> Peratherium cuvieri (an extinct marsupial)

Every one of those came back as a single confident result with a clean binomial,
and `nile-monitor` went on to produce a "fit-quality" reference with two
agreeing silhouettes -- of a mongoose. Nothing downstream could have caught it:
the silhouettes are real, the measurement is valid, the agreement is genuine.
The only thing wrong is which animal it is.

The three reptile failures share one cause and it is not GBIF's fault: the
Mammalia constraint that fixes "brown bear -> Protea speciosa" also guarantees a
wrong answer for every crocodile and lizard in the library. A constraint that
cannot be right for part of the input has to be told about that part.

HOW A NAME GETS `checked_by_hand`. Not by my say-so. Each binomial below is put
back to GBIF's `species/match`, which must agree that the name is ACCEPTED, at
SPECIES rank, in the expected class -- and GBIF's own vernacular list for that
key must contain a name that overlaps the spec's name. `lion -> Panthera leo`
passes because GBIF lists "Lion" against *Panthera leo*; `lion -> Macaca
silenus` would fail because its vernaculars are macaque names. That is a real
check with a real failure mode, not a rubber stamp.

    python tools/refnames.py --check      # verify, write nothing
    python tools/refnames.py --write      # verify and update the latin map
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LATIN = ROOT / "refs" / "species-latin.json"

# Corrections and additions, hand-entered from the spec name and checked below.
# A name here OVERRIDES whatever GBIF's common-name search produced.
#
# The `class` column is what makes the reptiles work at all: the Mammalia
# constraint used by the automatic pass cannot return a crocodile.
HAND: dict[str, tuple[str, str]] = {
    # --- outright wrong before, all five verified in the docstring above
    "lion": ("Panthera leo", "Mammalia"),
    "nile-crocodile": ("Crocodylus niloticus", "Reptilia"),
    "nile-monitor": ("Varanus niloticus", "Reptilia"),
    "spectacled-caiman": ("Caiman crocodilus", "Reptilia"),
    "fisher": ("Pekania pennanti", "Mammalia"),
    # --- 2026-08-16: four names that were never hand-entered, so they carried
    # whatever the bulk resolver produced, and three of them were wrong. Found
    # by tools/densityref.py, which could not join them to PanTHERIA and said so
    # rather than silently dropping the species. `rock-hyrax` is the one that
    # mattered: on file as the YELLOW-SPOTTED hyrax, it was contributing the
    # single largest density in the library (5,752/km2 against Procavia's
    # 2,734) and moving every population total with it.
    "rock-hyrax": ("Procavia capensis", "Mammalia"),
    "capybara": ("Hydrochoerus hydrochaeris", "Mammalia"),   # was Hydrochoeris
    "european-hedgehog": ("Erinaceus europaeus", "Mammalia"),  # was Erimaceus
    "chamois": ("Rupicapra rupicapra", "Mammalia"),  # carpatica is not a species
    # --- 2026-08-16: fourteen names that the bulk resolver got RIGHT but that
    # nobody had ever confirmed, so every density taken under them carried a
    # warning. Each was put through the same check as the rest -- ACCEPTED, at
    # SPECIES rank, in Mammalia, with GBIF's own vernaculars overlapping the
    # spec name -- and all fourteen passed. Listed so the warning is EARNED
    # rather than blanket: an unverified name should mean "nobody looked", and
    # after this it does.
    "alpine-marmot": ("Marmota marmota", "Mammalia"),
    "black-tailed-jackrabbit": ("Lepus californicus", "Mammalia"),
    "black-tailed-prairie-dog": ("Cynomys ludovicianus", "Mammalia"),
    "bongo": ("Tragelaphus eurycerus", "Mammalia"),
    "brown-throated-sloth": ("Bradypus variegatus", "Mammalia"),
    "cape-hare": ("Lepus capensis", "Mammalia"),
    "chacma-baboon": ("Papio ursinus", "Mammalia"),
    "dorcas-gazelle": ("Gazella dorcas", "Mammalia"),
    "eastern-grey-squirrel": ("Sciurus carolinensis", "Mammalia"),
    "european-otter": ("Lutra lutra", "Mammalia"),
    "red-kangaroo": ("Macropus rufus", "Mammalia"),
    "saiga-antelope": ("Saiga tatarica", "Mammalia"),
    "small-indian-mongoose": ("Herpestes javanicus", "Mammalia"),
    "vicuna": ("Vicugna vicugna", "Mammalia"),
    # --- resolved to the wrong species of the right genus
    "hippopotamus": ("Hippopotamus amphibius", "Mammalia"),
    "moose": ("Alces alces", "Mammalia"),
    "warthog": ("Phacochoerus africanus", "Mammalia"),
    "guanaco": ("Lama guanicoe", "Mammalia"),
    "raccoon": ("Procyon lotor", "Mammalia"),
    "red-squirrel": ("Sciurus vulgaris", "Mammalia"),
    "pine-marten": ("Martes martes", "Mammalia"),
    "wild-yak": ("Bos mutus", "Mammalia"),
    "white-rhinoceros": ("Ceratotherium simum", "Mammalia"),
    "klipspringer": ("Oreotragus oreotragus", "Mammalia"),
    "forest-buffalo": ("Syncerus caffer", "Mammalia"),
    "north-american-porcupine": ("Erethizon dorsatum", "Mammalia"),
    "pika": ("Ochotona princeps", "Mammalia"),
    "european-souslik": ("Spermophilus citellus", "Mammalia"),
    # --- names the automatic pass could not resolve at all
    "brown-bear": ("Ursus arctos", "Mammalia"),
    "red-deer-stag": ("Cervus elaphus", "Mammalia"),
    "red-deer-hind": ("Cervus elaphus", "Mammalia"),
    "przewalskis-horse": ("Equus ferus", "Mammalia"),
    "wood-bison": ("Bison bison", "Mammalia"),
    "bengal-tiger": ("Panthera tigris", "Mammalia"),
    "sand-lizard": ("Lacerta agilis", "Reptilia"),
    "ocellated-lizard": ("Timon lepidus", "Reptilia"),
    "viviparous-lizard": ("Zootoca vivipara", "Reptilia"),
    "spiny-tailed-lizard": ("Uromastyx aegyptia", "Reptilia"),
    "water-monitor": ("Varanus salvator", "Reptilia"),
    "desert-monitor": ("Varanus griseus", "Reptilia"),
    "marine-iguana": ("Amblyrhynchus cristatus", "Reptilia"),
    "fire-salamander": ("Salamandra salamandra", "Amphibia"),
    "poison-dart-frog": ("Dendrobates tinctorius", "Amphibia"),
    # --- correct already; listed so the check covers what will be fitted
    "plains-zebra": ("Equus quagga", "Mammalia"),
    "american-bison": ("Bison bison", "Mammalia"),
    "american-black-bear": ("Ursus americanus", "Mammalia"),
    "cape-buffalo": ("Syncerus caffer", "Mammalia"),
    "greater-kudu": ("Tragelaphus strepsiceros", "Mammalia"),
    "gemsbok": ("Oryx gazella", "Mammalia"),
    "okapi": ("Okapia johnstoni", "Mammalia"),
    "roe-deer": ("Capreolus capreolus", "Mammalia"),
    "fallow-deer": ("Dama dama", "Mammalia"),
    "white-tailed-deer": ("Odocoileus virginianus", "Mammalia"),
    "reindeer": ("Rangifer tarandus", "Mammalia"),
    "sika-deer": ("Cervus nippon", "Mammalia"),
    "wild-boar": ("Sus scrofa", "Mammalia"),
    "coyote": ("Canis latrans", "Mammalia"),
    "grey-wolf": ("Canis lupus", "Mammalia"),
    "red-fox": ("Vulpes vulpes", "Mammalia"),
    "cheetah": ("Acinonyx jubatus", "Mammalia"),
    "eurasian-lynx": ("Lynx lynx", "Mammalia"),
    "bobcat": ("Lynx rufus", "Mammalia"),
    "caracal": ("Caracal caracal", "Mammalia"),
    "spotted-hyena": ("Crocuta crocuta", "Mammalia"),
    "striped-hyena": ("Hyaena hyaena", "Mammalia"),
    "african-bush-elephant": ("Loxodonta africana", "Mammalia"),
    "muskox": ("Ovibos moschatus", "Mammalia"),
    "lowland-tapir": ("Tapirus terrestris", "Mammalia"),
    "wolverine": ("Gulo gulo", "Mammalia"),
    "european-badger": ("Meles meles", "Mammalia"),
    "blue-wildebeest": ("Connochaetes taurinus", "Mammalia"),
    "addax": ("Addax nasomaculatus", "Mammalia"),
    "african-wild-dog": ("Lycaon pictus", "Mammalia"),
    "dromedary-camel": ("Camelus dromedarius", "Mammalia"),
    "maned-wolf": ("Chrysocyon brachyurus", "Mammalia"),
    # --- ADDED 2026-08-15, second pass. The name gate is what stops a species
    # being fitted, not the reference, so every species in `refs/` that reaches
    # fit quality on EITHER limb thickness or stance is entered here and put
    # through the same GBIF check as the rest. One of them was already wrong in
    # the automatic map and is exactly the failure this file was written for:
    # `common-frog` had resolved to *Mustela lutreola*, a EUROPEAN MINK, because
    # the Mammalia constraint that fixes the shrubs cannot return an amphibian.
    "common-frog": ("Rana temporaria", "Amphibia"),
    "alpaca": ("Vicugna pacos", "Mammalia"),
    "arctic-fox": ("Vulpes lagopus", "Mammalia"),
    "bighorn-sheep": ("Ovis canadensis", "Mammalia"),
    "corsac-fox": ("Vulpes corsac", "Mammalia"),
    "elk-wapiti": ("Cervus canadensis", "Mammalia"),
    "european-polecat": ("Mustela putorius", "Mammalia"),
    "european-wildcat": ("Felis silvestris", "Mammalia"),
    "fennec-fox": ("Vulpes zerda", "Mammalia"),
    "golden-jackal": ("Canis aureus", "Mammalia"),
    "nine-banded-armadillo": ("Dasypus novemcinctus", "Mammalia"),
    "pronghorn": ("Antilocapra americana", "Mammalia"),
    "striped-skunk": ("Mephitis mephitis", "Mammalia"),
    "western-lowland-gorilla": ("Gorilla gorilla", "Mammalia"),
    "african-forest-elephant": ("Loxodonta cyclotis", "Mammalia"),
    "alpine-ibex": ("Capra ibex", "Mammalia"),
    "chimpanzee": ("Pan troglodytes", "Mammalia"),
    "coati": ("Nasua narica", "Mammalia"),
    "eastern-chipmunk": ("Tamias striatus", "Mammalia"),
    "eastern-grey-kangaroo": ("Macropus giganteus", "Mammalia"),
    "european-bison": ("Bison bonasus", "Mammalia"),
    "european-mole": ("Talpa europaea", "Mammalia"),
    "european-rabbit": ("Oryctolagus cuniculus", "Mammalia"),
    "giant-anteater": ("Myrmecophaga tridactyla", "Mammalia"),
    "impala": ("Aepyceros melampus", "Mammalia"),
    "mandrill": ("Mandrillus sphinx", "Mammalia"),
    "meerkat": ("Suricata suricatta", "Mammalia"),
    "mountain-goat": ("Oreamnos americanus", "Mammalia"),
    "red-river-hog": ("Potamochoerus porcus", "Mammalia"),
    "sand-cat": ("Felis margarita", "Mammalia"),
    "siberian-flying-squirrel": ("Pteromys volans", "Mammalia"),
    "steppe-polecat": ("Mustela eversmanii", "Mammalia"),
    "stoat": ("Mustela erminea", "Mammalia"),
    "virginia-opossum": ("Didelphis virginiana", "Mammalia"),
    "dingo": ("Canis dingo", "Mammalia"),
}

# Species whose SPEC NAME cannot overlap its own vernacular list, with the
# reason. Each of these is a real taxonomic fact, not a waiver of the check.
KNOWN_NAME_GAPS = {
    "elk-wapiti": "GBIF lumps wapiti into Cervus elaphus; the library keeps them apart",
    "red-deer-stag": "sex-and-age spec name; GBIF has no vernacular for 'stag'",
    "red-deer-hind": "sex-and-age spec name; GBIF has no vernacular for 'hind'",
    "wood-bison": "subspecies B. b. athabascae; GBIF accepts only Bison bison",
    "forest-buffalo": "subspecies S. c. nanus; GBIF accepts only Syncerus caffer",
    "przewalskis-horse": "GBIF accepts Equus ferus; the vernacular is 'wild horse'",
    "bengal-tiger": "subspecies P. t. tigris; GBIF accepts only Panthera tigris",
    "poison-dart-frog": "a common name for a whole family; one species stands in",
}


def _match(name: str) -> dict:
    url = ("https://api.gbif.org/v1/species/match?strict=true&name="
           + name.replace(" ", "%20"))
    r = subprocess.run(["curl", "-sSL", "-m", "40", url], capture_output=True)
    return json.loads(r.stdout or b"{}")


def _vernaculars(key: int) -> list[str]:
    url = f"https://api.gbif.org/v1/species/{key}/vernacularNames?limit=100"
    r = subprocess.run(["curl", "-sSL", "-m", "40", url], capture_output=True)
    try:
        j = json.loads(r.stdout or b"{}")
    except ValueError:
        return []
    return [v.get("vernacularName", "") for v in j.get("results", [])]


# GBIF DOES NOT USE "Reptilia" AS A CLASS. It returns Squamata for lizards and
# monitors and Crocodylia for crocodiles and caimans, so an expected class of
# "Reptilia" rejected every reptile in the library on the first run -- eight
# species refused for being correctly identified. The table below is what GBIF
# actually answers, not what a textbook would.
CLASS_OK = {
    "Mammalia": {"Mammalia"},
    "Reptilia": {"Squamata", "Crocodylia", "Testudines", "Reptilia"},
    "Amphibia": {"Amphibia"},
}


def _words(s: str) -> set[str]:
    """THREE LETTERS, NOT FOUR. At >3 this rejected `red-fox` -> *Vulpes
    vulpes*: "red" and "fox" are both three letters, so the spec name
    contributed no words at all and the overlap test compared an empty set.
    The check reported a correct mapping as REJECTED, which is the same class of
    silent-wrong-answer this file exists to catch -- in the checker itself."""
    return {w for w in re.split(r"[^a-z]+", s.lower()) if len(w) >= 3}


def check(write: bool) -> int:
    latin = json.loads(LATIN.read_text(encoding="utf-8")) if LATIN.exists() else {}
    print(f"{'spec name':<26} {'scientific name':<28} {'class':<10} {'rank/status':<18} name")
    ok = bad = 0
    for spec_name, (binomial, klass) in sorted(HAND.items()):
        m = _match(binomial)
        got_class = m.get("class", "")
        rank, status = m.get("rank", "?"), m.get("status", "?")
        key = m.get("usageKey")
        # SYNONYM IS ACCEPTED AND RECORDED, NOT REFUSED. GBIF treats
        # *Lama guanicoe* as a synonym of *Lama glama* and *Bos mutus* as a
        # synonym of *Bos grunniens* -- i.e. it does not separate the wild
        # animal from its domesticated form. The library does separate them, and
        # for a SHAPE reference the wild binomial is the one that fetches the
        # right silhouettes. Refusing these would have dropped the guanaco and
        # the wild yak on a naming convention rather than on an error.
        good = (rank == "SPECIES" and status in ("ACCEPTED", "SYNONYM")
                and got_class in CLASS_OK.get(klass, {klass}))
        vern = _vernaculars(key) if key else []
        overlap = bool(_words(spec_name) & {w for v in vern for w in _words(v)})
        note = ""
        if not overlap:
            if spec_name in KNOWN_NAME_GAPS:
                note = f"  name gap OK: {KNOWN_NAME_GAPS[spec_name]}"
                overlap = True
            else:
                note = f"  NO VERNACULAR OVERLAP (GBIF says: {vern[:4]})"
        verdict = "ok" if (good and overlap) else "REJECTED"
        if good and overlap:
            ok += 1
            latin[spec_name] = {
                "latin": m.get("canonicalName", binomial),
                "gbif_key": key,
                "gbif_class": got_class,
                "gbif_vernacular": vern[:5],
                "resolved_from": "hand-entered, tools/refnames.py",
                "checked_by_hand": True,
            }
        else:
            bad += 1
            if spec_name in latin:
                latin[spec_name]["checked_by_hand"] = False
        print(f"{spec_name:<26} {binomial:<28} {got_class:<10} "
              f"{rank + '/' + status:<18} {verdict}{note}")
    print(f"\n  {ok} verified, {bad} rejected")
    if write:
        LATIN.write_text(json.dumps(latin, indent=2, sort_keys=True) + "\n",
                         encoding="utf-8")
        print(f"  wrote {LATIN}")
    else:
        print("  (--check only; nothing written)")
    return 1 if bad else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    if not (a.write or a.check):
        ap.error("pick --check or --write")
    return check(a.write)


if __name__ == "__main__":
    raise SystemExit(main())
