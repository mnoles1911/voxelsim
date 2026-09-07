# Bird pilot completed checkpoint

Both CC0 museum birds are saved at 12.5 mm and user accepted on 2026-09-07. See common-raven/review.md
and golden-eagle/review.md for final limits. The fused perches and damaged foot regions
were removed explicitly. New tools refine_raven_voxels.py and refine_eagle_pilot.py
reconstruct articulated feet with three forward digits and one hallux. Eagle lower
legs are feathered to the toes. These are authored reconstructions, not scan-exact feet.
Expanded oblique review confirms both body hemispheres. No remaining perch fragments
were accepted. Both saved models pass one-component and exact RGB surface checks.

## Historical source experiments (superseded)

# Bird source checkpoint

## Common raven: selected study source

Virtual Museums of Malopolska, specimen MP 040, The Krystyna and Wlodzimierz
Tomek Natural Science Museum in Ciezkowice. Source metadata identifies Corvus
corax and CC0: https://sketchfab.com/3d-models/ec9c0ac738fd4495af334ea2092e8d89
Institution record: https://muzea.malopolska.pl/en/objects-list/2247

This is a perched specimen with an integral branch. Source obliques and the
12.5 mm six-view study show a substantial bill, throat feathering, folded wings
and a long tail. Consulted diagnostic reference:
https://www.allaboutbirds.org/guide/Common_Raven/id
The reference photographs are not redistributed.

`common-raven-museum-study` has 6,956 voxels including the branch. A first
location-and-color perch removal study has 6,348 voxels and five components.
It still visibly contains support fragments and is NOT ready to install.
The retired `refine_raven_master.py` recorded this failed experiment (available in Git history). The historical next step was explicit perch
segmentation and toe review; do not discard fragments indiscriminately or
label the current study anatomically complete. Darken faded specimen coloration
with reference-informed feather, bill and foot regions after geometry review.

## Rejected alternative

Korppi (Corvus corax), UID da4bf1396118436888fd32bd3ff5730e, CC BY 4.0.
The front views looked intact, but expanded source views 4 and 5 expose a large
missing back surface. Both Blender-prepared and directly transformed source
geometry reproduce the defect. It is a source coverage failure, not evidence
of a transform bug. The scan was not installed. This prompted adding opposite
hemisphere cameras to `inspect_mesh_blender.py`.

## Golden eagle: downloaded, not yet segmented

Same museum collection, specimen MP 407, Aquila chrysaetos, CC0:
https://sketchfab.com/3d-models/dd0552b1f93b441cbdd626a5b2abebbf
Institution record: https://muzea.malopolska.pl/en/objects-list/2248
Consulted identification reference: https://www.allaboutbirds.org/guide/Golden_Eagle/id

Source has a branch, pale specimen head and leg feathers and glossy toes.
Need full hemisphere coverage, age/plumage decision, branch removal, toe and
bill checks, physical scale and authored coloring. Prepared source is
`golden-eagle-master/with-perch.glb`; not a completed animal.
