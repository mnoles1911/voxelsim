"""Generate/check the GPU coverage body from the editable material authority."""
import argparse
import hashlib
from pathlib import Path
import runpy

def generated():
    root = Path(__file__).resolve().parents[1]
    body = runpy.run_path(str(root / "Tools/tree_foliage_mask.py"))["SHAPED_MASK_BODY"]
    derivative = "float footprint=max(length(ddx(p)),length(ddy(p)));"
    if body.count(derivative) != 1:
        raise ValueError("authority derivative binding changed; review explicit footprint mapping")
    mask = body.replace(derivative, "float footprint=max(0.0,PatternFootprint);")
    mask = mask.replace("float t=v.y/.62;", "float t=v.y/.62; // lint-shader-ub: allow SIGNED_DIVISION - floating point division by positive nonzero scalar, not signed integer division")
    mask = mask.replace("d=min(d,max(abs(t),abs(v.x)/(width*max(.06,1-t*t))));", "d=min(d,max(abs(t),abs(v.x)/(width*max(.06,1-t*t)))); // lint-shader-ub: allow SIGNED_DIVISION - floating point numerator and strictly positive float denominator")
    text = "// Generated from Tools/tree_foliage_mask.py SHAPED_MASK_BODY.\n// Body SHA256 " + hashlib.sha256(body.encode()).hexdigest() + "\n// Only derivative footprint replaced by explicit pattern-space footprint.\nfloat VoxelApprovedFoliageCoverage(float2 UV,float Needle,float PatternFootprint)\n{\nfloat Opening=.45;\n" + mask + "\nreturn FoliageCoverage;\n}\n"
    return root / "Shaders/VoxelApprovedFoliageMask.ush", text

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    path, text = generated()
    if args.check:
        if not path.exists() or path.read_text() != text:
            raise SystemExit("STALE: regenerate approved foliage include and review source change")
        print("approved foliage include matches material authority exactly")
    else:
        path.write_text(text)
