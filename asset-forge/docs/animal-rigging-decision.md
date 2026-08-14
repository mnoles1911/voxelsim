# Animals are rigid-part animated, and ship in one pose

**Decision (owner, 2026-08-14): rigid-part animation. A single pose for all
animals.**

This supersedes the multi-stance plan in `docs/biomes/README.md` §4, which
assumed five authored stances per quadruped, and it changes what birds ship as.

## Why one pose is the right answer

A quadruped's stances — standing, walking, grazing, alert — differ by **joint
angles**, not by shape. The neck goes down to graze, the head comes up alert,
the legs swing. Those are rotations of rigid pieces about joints, which is how
cubic voxel games have always animated: rotate boxes about joint origins and
accept the seams at the joints as part of the look.

And the argument that settles it: **five baked poses cannot produce a walk
cycle.** Walking is continuous, four discrete grids give four frames and a pop
between them. Baking stances is not a way of animating, it is a decision not to.

## What this needs that we do not have

**A baked asset has to say which part each voxel belongs to.** Today `.vxa`
carries material ids and nothing else, so an asset arrives in the engine as one
undifferentiated box of voxels and nothing can rotate a leg.

The good news is that the generators already know. `forge/bird.py:228` defines
`T_BODY, T_NECK, T_HEAD, T_BILL, T_TAIL, T_WING, T_LEG, T_CREST` and draws every
one of them into a scratch tag grid, because the paint pass needs to tell a wing
from a body without a second geometry pass. `fish._fins` returns a `kind` array
beside its mask for the same reason. **The segmentation exists at build time and
is discarded at export.** So this is plumbing, not new geometry:

1. Carry the tag grid out of the generator on the `Asset`, alongside `grid`.
2. Give the format somewhere to put it — either a per-voxel part id parallel to
   the material runs, or a small table of part extents and joint origins beside
   them. The per-voxel form is simpler and run-length compresses about as well
   as the materials do, since parts are contiguous.
3. Publish joint origins. A part id says which voxels move together; it does not
   say what they rotate about. A shoulder, a hip, a neck base and a tail base are
   points the generator knows and the file does not.
4. Engine side: read them, same shape of change as the voxel size in v2.

## The ordering, which matters

**Do not collapse the birds to one pose until parts ship.** `bird.pose` exists
because a folded wing and a spread wing are genuinely different shapes, not one
shape rotated — a folded wing is tucked and overlapping. Under rigid-part
animation a wing becomes a plate that rotates at the shoulder, and one bake is
enough. But until the file can say "these voxels are the wing and they turn
about this point", removing the second pose leaves birds unable to fold at all.

So: parts first, then one pose. The canonical bird pose should be the one whose
geometry a rotation can reach both ways, which is wings out rather than tucked.

## What becomes dead, and what does not

`spec.seed_hash` and `spec.SEED_INVARIANT` were built so that a raven perched
and the same raven flying are one raven (`docs/bird-shape-research.md` §10). With
one pose per animal the pose field stops mattering — but the machinery should
stay. It is the right answer for any future field that describes a presentation
rather than an individual, and it costs nothing when the exclusion set is empty.

`bird.sex` is unaffected and stays outside the exclusion set. Sex is not a
posture: there is no individual that is "the same mallard, but female".

## The seam: a ball at every joint, owned by the parent

**Decision (owner, 2026-08-14): whichever looks more natural.** That is a ball
joint, and it is worth saying why rather than just recording the choice, because
the two obvious alternatives are both worse and one of them is what most voxel
games ship.

Rotating a rigid limb about a joint opens a wedge of empty space on the outside
of the bend. Three ways to deal with it:

* **Leave it.** Minecraft does. It reads as a style rather than a defect at that
  blockiness, and it costs nothing. It is also the thing the owner asked us not
  to do.
* **An overlapping collar** — duplicate the voxels near the joint into both
  parts so there is always material spanning the gap. It works at small angles
  and fails at large ones: the collar is a shape, and a shape only covers the
  wedge from the directions it was built to cover. Push the leg forward far
  enough and the seam opens on the other side.
* **A ball at the joint, belonging to the PARENT part.** A sphere of the
  parent's material centred on the joint, with a radius a little over the
  child's cross-section. This is the one that actually works, and the reason is
  that **a sphere is rotationally symmetric**: it presents the same silhouette
  from every direction, so it covers the wedge at *any* angle rather than at the
  angles someone anticipated. A shoulder stays a shoulder through the whole
  swing. It is also what the real shape is — a shoulder IS a ball — so it reads
  as anatomy rather than as a patch.

So: each joint gets a cap sphere, owned by the parent, radius scaled from the
child limb's own thickness. Cheap, and it is drawn by machinery that already
exists (`grid.ball` / the same ellipsoid stamps the generators use everywhere).

**Two limits worth knowing before this is built.** On a small animal the cap can
eat the limb: a 22 cm squirrel at 2 cm has legs two or three voxels thick, and a
ball big enough to cover their swing is most of the leg. Below about three
voxels of limb thickness the cap should be skipped — at that size the wedge is
one voxel and invisible, so it is buying nothing. And the cap must be drawn as
part of the PARENT, not shared: a voxel that belongs to two parts has no defined
answer when both rotate, which is the overlapping-collar problem wearing a
different hat.
