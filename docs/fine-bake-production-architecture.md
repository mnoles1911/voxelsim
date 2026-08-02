# SUPERSEDED — see `world-generation-architecture.md`

This document framed the production question as **"server bakes or client
bakes"** and answered "server, because a client cannot compute a correct tile
even in principle."

**That framing was wrong, and so was the strength of the claim.**

Two things overturned it, both from the owner on 2026-08-02:

1. **The world is infinite, generated and served on demand.** The original doc
   left "is the world finite?" open and leaned on assumptions that only hold
   for a finite world.

2. **The game must also support offline single-player.** With no server, the
   client *must* be able to bake. So "server or client" was never the question
   — the client has to be capable either way, and the server exists to
   accelerate and arbitrate.

The correctness argument in the original §1 was also overstated. It is true
that a tile baked against an **incomplete superblock** is permanently wrong,
and that remains the central constraint. But that is an argument for making
superblock completeness a **publish gate**, not an argument that clients can
never bake: the bake is integer-only and deterministic, so any machine with a
complete superblock produces identical bytes.

The bandwidth arithmetic also cuts the other way from what was implied — a
coarse tile is 1.5 MB against a fine tile's 190 MB, **127×** — so shipping
coarse and baking locally moves far less data.

Everything still useful here — the superblock argument, the apron and
hydrology measurements, the zstd gap — has been carried into
`world-generation-architecture.md` and corrected. Read that instead.
