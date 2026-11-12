# Vulkan synchronization diagnostics and GS barrier experiment

The GS compatibility split remains enabled on Adreno. This experiment keeps its
effects draw, pure raster draw, pass boundaries, attachment operations and command
order. It does not relax any resource dependency or device workaround.

`DM_VK_GS_BARRIER_COALESCE=1` permits one narrow transformation: omit the full
memory barrier at `begin_pass()` if the immediately preceding recorded command
was the GS wrapper's identical full memory barrier, in the same command buffer.
The first barrier remains in its original position. The default is zero.
The switch is sampled at present boundaries; restart with zero for rollback.

Every declared Vulkan recording command is wrapped by
`gfx_vk_sync_commands.h`. A command, timestamp, transfer, rendering command,
command-buffer lifetime operation or upload invalidates the adjacency gate.
Submission entry and return from the synthetic raster draw invalidate it as
well, including skipped/early-return draws. Original rasterizer-discard draws
never create a candidate. An unknown/intervening command retains the barrier.
`tests/test_vk_sync.py` audits wrapper coverage and drives the production macros
against a recording stub. `vk_sync_gate.h` is the production eligibility seam.

## Diagnostic controls

- `DM_VK_SYNC_STATS=0` (default): no diagnostic file output, pass hashing or extra
  GPU queries. Recording wrappers retain a disabled fast-path check.
- `DM_VK_SYNC_STATS=1`: fixed per-frame counters and source-category counts.
  `vk-sync-frames.tsv` is written by a reporter thread under `DM_ROOT`.
- `DM_VK_SYNC_STATS=2`: also emit bounded source/stage/access aggregates to
  `vk-sync-categories.tsv` and canonical pass-group metadata to
  `vk-sync-groups.tsv`. No individual draw/barrier/pass instance is logged.
- `DM_VK_PASS_TIMES=1`: existing GPU queries additionally carry an immutable
  synchronization group ID through the submission completion queue. Group GPU
  costs do not depend on the pass ordinal. Keep queries off for clean A/B timing.

Diagnostic levels are startup controls. Coalescing may be changed through the
existing `DM_CTL` environment command while the scene is stationary.

The frame publisher runs in the serialized graphics replay/present path and
uses a fixed queue. It never waits for the file writer. Overflow/query exhaustion
and queue drops are explicit; discard affected measurements. Pass-group entries
are never recycled during a process, so delayed GPU results cannot attach to a
new pass with a reused ID. Metadata publication uses release/acquire semantics;
GPU accumulators are atomic. Formatting and I/O occur only on the reporter.

## Data interpretation and bounds

Frame counters refer to the game's recording thread and distinguish its main,
upload and auxiliary command buffers. Presentation/overlay work on the separate
present thread is outside these frame totals. Do not present game-main barrier
counts as a census of all work submitted by the application.

`L` category records contain frame, source file, line, kind (0 full barrier helper,
1 actual committed-pass end), count. No-open-pass end attempts and unused prepared
passes are separate frame counters. Source line numbers are build-local labels,
not cross-capture pass identities. `B` records aggregate actual Vulkan stage and
access flags, dependency flags and barrier element type (0 memory, 1 buffer,
2 image). Barrier commands and barrier elements are different counters. Actual
image-layout changes are counted separately from memory-only barriers.

`G` records identify canonical pass groups and cumulative pass/query counts and
GPU nanoseconds. `S` gives a shader triple, actual specialized GS key, split/
discard/depth/dual-source flags, topology and draw count. `A` gives all observed
color/depth attachments, guest storage pool/offset/range, format, extent, samples,
aspect, view level/layer/count and load/store. `R` gives conservative directly
bound buffer ranges and observed sampled-image storage ranges, with NVN stage
bits (VS=1, FS=2, GS=4, compute=32), access bits (read=1, possible write=2), and
binding kind (100 denotes a sampled-image storage observation).

Group identity excludes transient resource addresses and the ending source
line; compare complete shader and attachment descriptions as well. The immutable
resource records are representative observations; incomplete flag bit 4 marks
groups where resource/attachment storage varies. Multiple pass instances may
share a group. Neither raw
group IDs, memory addresses nor legacy pass ordinals are stable across captures.
Canonical metadata is checked on hash equality before two groups are combined.

There are 32 shader triples and 48 resource ranges per pass and 2048 groups per
process. Group incomplete flags mark truncated shader (bit 1) or resource (bit 2)
metadata. Global overflow is separate. These are diagnostic observations, not a
complete alias/dependency analysis: indirect handles, storage-image access,
shadow buffers, sparse mapping generations and physical backing aliases can
require further tracking. Resource access is conservative and cannot authorize
narrowing or removing a barrier. Only the exact command-adjacency gate changes
recording in this experiment.

Group GPU aggregates arrive asynchronously and are cumulative. Use interval
differences of matching group metadata, state query coverage, and exclude
loading. They do not isolate the cost of a shader or barrier. Some boundary work
lies outside the existing pass timestamps. A reduction in counter values is not
evidence of a performance improvement; compare whole-frame GPU and frame
distributions, visual correctness and stability under matched settings.
