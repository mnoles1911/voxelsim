# Directional tree fall and impact destruction

## Intended behavior

A cut tree commits to a direction, leans slowly, then accelerates under gravity. It retains a temporary wood hinge at the stump until that support fails or the falling trunk meets the ground. An impact does not automatically explode the tree: small branches can snap, contact patches can crush, and a sound trunk can remain whole.

## Current prototype change

`AVoxelFallingTimber` now starts at rest with a 1.5° lean toward the final successful axe strike's horizontal direction. A Chaos constraint pins the base and permits rotation only around the horizontal axis perpendicular to that direction. There is no initial angular kick or timeline controlling the fall. Gravity and the body's inertia determine acceleration. The explicit hinge replaces the conflicting stump-box contact for these bodies.

A ground contact more than one metre from the pivot releases the hinge. The body then continues with its current physical velocities. This is a first support-release rule; retained wood strength should ultimately determine when the hinge tears.

While the hinge exists, the unstable body is kept awake so generic low-speed sleep thresholds do not freeze the initial lean. Normal sleeping resumes after release. The lean is applied before creating the simulated body.

Ground-contact instrumentation estimates normal closing speed at the contact point, including angular velocity, and a box-based effective mass. Units are converted from Unreal centimetres to SI. Estimated available normal impact energy is `0.5 * effective_mass * closing_speed²`. The current provisional fracture threshold is 6,000 J per square metre of trunk cross section. This is a gameplay tuning parameter, **not a measured wood property**. The velocity sample is from the preceding actor tick; substep-accurate pre-solve contacts will improve that estimate.

The existing prepared midpoint break remains the only geometric fracture location in this prototype. Actual localized crushing, branching fracture, and repeated fragmentation are not implemented by this change.

### Verified pilot run

The editor target builds successfully. The completed in-game capture passed `tools/verify-tree-felling.py`: five partial cuts preserved support, the sixth released the upper tree, and its angular speed increased from 1.9°/s at one second to 6.3, 17.3 and 47.8°/s at two, three and four seconds. The centre of mass stayed in the chosen vertical fall plane before contact. Ground impact occurred approximately 4.4 seconds after release and measured an estimated 34.8 kJ with this proxy mass model. The hinge released, the prepared split produced two fragments, and both settled and remained stationary. Normal cuts took 1.658–3.031 ms of CPU mesh work; detachment took 12.5 ms. These are CPU operation timings, not total frame times.

The test caught an important sleep issue: waking once per render tick was insufficient because the body could sleep again between physics substeps. The hinged body's physical material now disables low-speed sleep, and a normal sleeping material is restored on hinge release. The automated validator checks early acceleration as well as final rest so an almost-frozen tree cannot pass merely by eventually falling.

## Recommended production structure

### 1. Tree structure independent of render LOD

Export a structural graph alongside each tree's voxel asset. Nodes represent trunk segments, major branches, and branch junctions. Each stores its occupied wood cells, bounds, mass, centre of mass, inertia, grain direction, and render-section membership. Connections store the actual supporting wood cross section, grain alignment, current damage, and stiffness/strength parameters.

Leaves contribute light distributed mass and broad drag, but do not act as structural wood connections. Small twigs can remain grouped for performance. Build this graph from generator branch data where available, using voxel connectivity to verify it. Do not infer mechanical strength from render LOD or treat every voxel as a rigid body.

### 2. Fall direction and stump support

When the cut becomes unstable, choose the heading from the notch opening, remaining hinge geometry, and actual centre-of-mass lean. Optional wind and a small seeded bias resolve an otherwise balanced case. Keep the heading stable during the tipping phase; do not change it with later camera movement or randomize it each frame.

Use a temporary constrained body with a strength-limited stump connection. Gravity produces torque about the hinge; the mass distribution determines acceleration. A nearly upright tree starts slowly because the gravitational lever arm is small. Release the connection when its bending/tension demand exceeds its remaining capacity. Preserve linear and angular momentum at release.

### 3. Turn contacts into local loads

Map each terrain, rock, or tree contact to the struck structural node and nearby voxel patch. Record the normal and tangential relative velocity, contact position, effective mass, impulse, and contact area estimate. Group solver callbacks into contact episodes so more physics substeps do not create more damage from the same impact.

Use an energy budget and distribute it between elastic motion, damping, local crushing, and connection failure. Propagate bending moments from the contact toward supported joints. Separate compression, bending/tension, and shear thresholds. Material properties should account for species, moisture/rot, grain direction, diameter, and existing cuts. Begin with explicitly tunable game parameters and calibrate against repeatable falls; avoid presenting proxy calculations as a full engineering simulation.

### 4. Crushing preserves cubic cells

Track compression damage in the contacted patch. Small damage changes surface appearance and sound before altering occupancy. When a cell or connection fails, remove or convert that material into a bounded amount of chips/debris and rebuild only the affected mesh sections.

Do not scale cubes into flattened rectangles to portray crushed wood. Keep the authored 50/25 mm environment lattice. Sub-voxel dents can remain damage metadata; visible splinters can use approved finer environment detail where useful. Leaves and fine twigs should mostly collapse or shed through inexpensive visual effects, not hundreds of physics actors.

### 5. Break only failed connections

After damage, run connectivity over the affected part of the structural graph. Each newly disconnected substantial component becomes a rigid body with recomputed mass and inertia. Transfer its meshes, generate exposed wood surfaces locally, and inherit velocity at its new centre of mass (`v + angular_velocity × offset`). Cap the number of new bodies per frame. Small disconnected bits use pooled effects; they must not multiply into an unbounded chain of actors.

A trunk striking broad soft ground may remain intact. A branch trapped under the falling crown should break near its loaded junction. A pre-notched trunk landing on a rock should bend and fail near that weakened region. These outcomes must follow contact and support, not a fixed timer or mandatory midpoint split.

### 6. Terrain, persistence, and performance

Replace the test's fixed 32 m collision patch with cached terrain collision chunks around active bodies. Update affected chunks after digging, and include rocks and sufficiently large branches. Keep a reliable collision fallback while asynchronous updates finish.

Prepare structural graphs, common fracture boundaries, and mesh ownership off the game thread. Apply local geometry updates and new bodies with explicit frame budgets. Sleeping fallen wood should become a persistent harvestable object with lightweight collision and distance LOD. Save its surviving graph, damage, transform and material content; rendering detail must not alter gameplay state.

## Accepted detached-entity cleanup policy (2026-09-06)

The user approved the proposed lifecycle with **10 seconds for particle effects
and small cosmetic chips**. The runtime lifecycle now implements these categories;
see [integration and persistence limits](debris-cleanup.md). Current substantial
timber is exempt from expiry. Standalone detached-object save/load now includes
timber meshes, motion and the edited prototype stump; production streaming and
multiplayer persistence remain separate work.

- Particle effects and non-harvestable small chips expire 10 seconds after spawn.
- Ordinary abandoned harvestable fragments use a 15-minute inactivity timeout.
  Player interaction resets inactivity; nearby players or an object being viewed
  protect harvestable fragments from removal. Exact protection distances remain
  implementation tuning parameters.
- Large fallen trunks and substantial rocks stop active simulation when settled
  and remain lightweight harvestable objects. They are not cosmetic debris and
  must not disappear under the 10-second rule.
- Player-built, placed or explicitly retained objects persist and are exempt
  from automatic debris cleanup.

Apply this lifecycle to detached wood and rock alike, including floating bodies.
Use regional active-body/debris budgets, favoring sleep or lightweight storage
for valuable objects. Preserve remaining harvestable inactivity time across
save/load and streaming so reloads neither erase resources nor restart timers.
Sleeping, unloading and despawning are separate state transitions; a physics
sleep event alone must not delete an object.

## Acceptance tests

- Small notch: standing tree; no disconnected crown.
- Completed cut: repeatable heading, near-zero starting angular velocity, increasing speed before first ground contact, no sideways wobble from an unconstrained base.
- High and low cuts: different inertia/fall timing, conserved material volume and inherited fragment momentum.
- Gentle landing: settling with little damage and no mandatory split.
- Hard point contact: local crushing or a nearby weak connection fails; distant healthy branches remain attached.
- Rock under a branch: junction failure can precede trunk failure.
- Repeated contacts: damage is stable across frame rates/substep counts and does not duplicate impact energy.
- Sleeping fragments: remain on terrain, support harvesting and player collision, survive save/load, and never trigger a whole-tree mesh rebuild per chip.

The immediate next implementation step is to export a small trunk/branch graph for the pilot oak and replace its single prepared split with a few real junctions. Then add contact-local damage before expanding to all environment assets.
