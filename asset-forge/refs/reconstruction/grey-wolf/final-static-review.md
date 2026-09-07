# Finished static wolf — 12.5 mm

Asset Forge library entry: `grey_wolf_anatomy_pilot-0001`.
Rebuild/install with `python tools/finish_wolf_pilot.py` after the coat and
proportion studies. The procedural `grey-wolf` baseline is retained.

The current art direction permits expressive stylization while requiring
believable anatomy. Six orthographic views and oblique inspection show an
elongated muzzle, paired upright ears, deep chest, tucked abdomen, separate
forelegs, bent rear hocks and a descending bushy tail. The feet remain simplified
at this pitch; this is an artist-authored animal, not a measured specimen.
The final pass neutralizes violet inner-ear shadows while retaining the accepted
grey/buff coat, dark dorsal marking and amber eyes.

69,525 occupied cubic voxels; 11,387 surface voxels; one connected component.
Bounding size: 1.675 × 0.3375 × 0.95 m. Pitch: exactly 0.0125 m, verified through
VXA write/read. RGB viewer payload is byte-checked against the authored sidecar.

The library carries `attribution.json` crediting rhcreations under CC BY 4.0,
with the source URL and modification description. `colored-voxels.glb` preserves
the full coat for interchange. `appearance.npz` adds full RGB to the Forge
viewer through an optional RGB1 trailer, leaving material IDs intact.

This completes the static modeling/display deliverable. It is not yet a rigged
game creature. Engine VXA appearance still uses the shared material palette;
the full-color viewer is not evidence of full-color engine support. The library
entry remains a draft so it cannot silently enter the approved runtime bank.
