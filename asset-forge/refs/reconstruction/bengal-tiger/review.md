# Bengal tiger candidate

Saved draft: `bengal_tiger_anatomy_pilot-0001`. This is a pilot candidate, not
user approval or runtime completion.

Source and modifications are in `attribution.json`. The walking master has a
short broad muzzle, rounded ears, substantial shoulders and forepaws, striped
flanks, pale underside and ringed tail. These features were checked against
[Animal Diversity Web's physical description](https://animaldiversity.org/accounts/Panthera_tigris/).
That account's historical taxonomy and broad species size range are not used as
Bengal-specific measurement evidence. The source does not identify a subspecies.

The first voxel pass enlarged individual whiskers into a grey moustache and
left detached cells. `refine_tiger_master.py` removes 32 inspected whisker mesh
components, preserving teeth, eyes, claws, ears and tail. The refined study has
210,538 occupied voxels, 26,916 surface voxels, one connected component and
12.5 mm pitch. Six unlit projections and the first oblique render were visually
inspected; the muzzle is cleaner and stripe boundaries remain legible.

Remaining review: measure shoulder and body landmarks against Bengal-specific
evidence; inspect the expanded opposite-side obliques and actual Forge viewport;
compare physical scale with the wolf. Bounding dimensions are 2.675 x 0.5125 x
1.3875 m, including the raised head and walking pose. These are not shoulder
height or a biological nose-to-tail measurement.

Reproduction from the cached licensed source:

1. Blender `prepare_creature_master.py`: source UID in attribution, output
   `bengal-tiger-master/master.glb`, length argument `2.7`.
2. Python `refine_tiger_master.py`.
3. Python `voxelize_reference_mesh.py` on `clean-master.glb`, output
   `bengal-tiger-refined`, `--long-axis 0 --length-m 2.658254861831665`.
4. Blender `inspect_mesh_blender.py` on the study's `colored-voxels.glb`.
5. Python `install_creature_pilot.py` with that study, species identifier,
   `quadruped` and this attribution file. Installer refuses existing entries.

Installer checks passed: connected geometry, surface alignment, VXA occupancy
and pitch roundtrip, and byte-exact RGB viewport payload. Full RGB is a Forge
sidecar; engine VXA appearance and animation remain separate.
