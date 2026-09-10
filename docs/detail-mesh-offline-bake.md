# Offline detail mesh bake — source implementation, validation pending

The editor-only `VoxelBakeDetailMeshes` commandlet derives persistent static
meshes from the same geometry, appearance and optional LOD builder as runtime.
It does not alter publication, endorsement, placement or the runtime load path.
All packages remain uncooked editor source assets. Platform cooking and runtime
asynchronous cache use are separate unfinished phases.

Run only after building the editor module and outside performance captures:

```powershell
& D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe D:\voxelsim\ue-project\VoxelEarth.uproject -run=VoxelBakeDetailMeshes -Publication=<asset-directory> -ApprovedLibrary=D:\voxelsim\asset-forge\library -RunId=<fresh-safe-name> -VoxelDetailMeshLOD -unattended
```

Authoritative mode checks each published detail row against its exact library
metadata (`endorsed`, `visual_approved`, not an inventory candidate) and VXA
digest. It never searches or endorses candidates. No endorsed understory means
no authoritative detail output; private fixtures cannot fill that gap implicitly.

Private fixture experiments require explicit `-PreviewOnly` instead of
`-ApprovedLibrary`. Those outputs go under `/Game/Voxel/Generated/DetailPreview/`
instead of `/Game/Voxel/Generated/Detail/`, and the manifest records preview-only
status. Both modes refuse an existing run directory. Failed runs may leave
partial packages but no completed manifest. Use a fresh run ID after failure.

Then terminate the process and verify in a fresh editor commandlet process:

`Tools/verify-detail-mesh-bake.ps1 -Manifest <absolute-detail-bake.json>
-Output <fresh-output-directory>` provides a repeatable wrapper. It refuses an
existing UE session, pins the manifest hash, requires a matching positive model
count, and records exit/failure status. Its syntax is checked; an actual completed
bake must still be run through it. It records the manifest schema and verification scope. Schema 2 requires an
explicit attribute-persistence completion marker as well as exit 0 and the full
model count; schema 1 remains explicitly legacy counts-only verification.

```powershell
& D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe D:\voxelsim\ue-project\VoxelEarth.uproject -run=VoxelBakeDetailMeshes -VerifyManifest=<absolute-detail-bake.json> -unattended
```

Schema 2 records source SHA-256 identities, builder-source hashes, exact engine
version, host platform, effective LOD/CPU-access settings, material package hash,
derived content keys, saved package hashes, material object bindings and mesh
facts. Fingerprint contract 1 adds SHA-256 values for every LOD and the aggregate:
positions, full tangent basis/normal, RGBA, all UV channels (including cutout and
wind), primary/depth/reversed/reversed-depth/wireframe indices, section ranges and
material indices/flags, and explicit screen sizes. Descriptive counts remain.

Encoding is explicit little-endian integer/IEEE float and RGBA, with negative zero
canonicalized and nonfinite data refused. This hashes decoded attribute values,
not C++ structure padding, pointer addresses or 16/32-bit index storage choices.
Material packages are pinned separately by base-material and material
instance package hashes; loaded mesh material binding is checked as well.

Persistent bake adapters must enable CPU access before build/save. Fresh-process
verification requires CPU-readable resident LOD buffers, and fails if attributes
cannot safely be read. It must not race streaming or mutation. This retained CPU
copy costs memory; it is a validation constraint, not a recommendation to keep
all shipping mesh CPU copies permanently resident. A separate cooked verification
strategy can release copies after validation; none is claimed here.

Schema 1 manifests/packages remain immutable and can still run explicit legacy
counts/bounds/material verification. They do not prove attribute persistence and
must not qualify as schema 2 runtime cache entries. New bakes require a fresh run
identity; builder hashes and `schema=2;...;cpuAccess=1;fingerprint=1` settings ensure
new derived keys. Do not upgrade old manifests by merely adding a schema number.

Matching fingerprints proves the built render buffers survived save/reload. It
does not prove the original source was meshed correctly, acceptable LOD color or
silhouette error, cutout/wind rendering, cooked loading or player-visible quality.
Those retain their separate source/render tests. Schema 2 commandlet integration
is source prepared and still requires a new complete bake and fresh-process run.

Cooking must explicitly include selected generated packages; target platform,
shader format and cook identity belong in a separate cooked index. Preview paths
must never automatically enter authoritative cooking. Runtime async integration
is separate work and must reject mismatched or legacy cache entries.
