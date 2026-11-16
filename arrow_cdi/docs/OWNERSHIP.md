# Memory ownership and lifetime model

## 1. Payload and views

All batches and views point at one reference-counted **payload**:

* a library-created payload owns deep-copied buffers (and bit-packs the
  one-byte-per-row validity the caller supplied);
* an imported payload owns **source holders** that keep the moved peer
  `ArrowSchema`/`ArrowArray` structs alive and remember their original
  release callbacks.

A view (`slice`, `project`, the root batch) holds:

* one schema-side reference and one array-side reference to the payload,
* a row offset/length,
* a column-index permutation.

Slicing and projection allocate only this small view bookkeeping. They
never copy value, offset or validity bytes, so re-slicing a nonzero-offset
view just composes offsets.

## 2. Two independent refcounts

Each payload carries `schema_refs` and `array_refs`, independently:

* the array side owns the data buffers (and the array-side source holders);
* the schema side owns type/name metadata (and the schema-side holders).

Releasing a schema can therefore never tear down an array that is still
alive, and vice versa. The payload memory is freed only when both sides
have reached zero.

## 3. Import is a move

`ac_batch_import` runs in two phases:

1. **Read-only validation.** The caller-owned source structs are never
   mutated, so any rejection leaves ownership entirely with the caller.
2. **Commit.** The library allocates the payload and one holder per moved
   struct, swaps each struct's `private_data` to its holder, then sets the
   struct's `release` to NULL to complete the move.

If any allocation fails during commit, every holder taken so far is rolled
back: the original `private_data` is restored and no peer callback is
invoked, so the source returns to the caller in its original state.

When an imported payload is finally destroyed, each holder restores the
peer's original `release`/`private_data` pair and invokes the peer callback
exactly once.

## 4. Export produces independent shells

`ac_batch_export` allocates a brand-new struct tree (root, children,
dictionary structs). It never returns the address of an internal struct as
a permanent handle. The shell buffers point at the shared payload bytes
(zero copy); each side takes its own payload reference.

The root release callback cascades through the whole tree, matching how
consumers such as PyArrow release an imported pair by calling only the root
release. Every struct's release is also individually callable and idempotent.

## 5. Close ordering and GC

* The original batch, any number of slices/projections, and exported shells
  may coexist and be closed in any order.
* The underlying bytes stay valid until the last user releases; after that,
  every real allocation is freed exactly once (asserted by the counters and
  AddressSanitizer).
* `close()` is idempotent at the object level and safe on the Python
  garbage-collection path. The Python wrapper does not require users to
  destroy views in a particular order; dropping the last reference runs the
  same C release as an explicit `close()`.
* In C, a freed pointer cannot be re-entered (standard language rule); the
  idempotency contract is that repeated release of an *allocated* handle is
  a no-op and that peer callbacks are never invoked twice. NULL handles are
  always safe.

## 6. Allocation accounting and failpoints

Every heap allocation goes through the counted wrappers in `ac_alloc.c`.

* `ac_total_allocations()` / `ac_total_frees()` count real malloc/free
  calls; `ac_live_allocations()` / `ac_live_bytes()` report what is still
  held.
* `ac_fail_arm(n)` makes the n-th allocation fail; the failpoint is one-shot
  (it disarms on firing) so failure-rollback allocations are not themselves
  forced to fail, modelling a transient OOM.
* `ac_fail_disarm()` switches the failpoint off without resetting the
  cumulative counters; `ac_fail_reset()` disarms and zeroes the counters.
* On any allocation failure the library recycles the shell it was building
  and leaves already-published objects usable. The test suite walks every
  allocation point and checks both source ownership (for imports) and the
  survival of existing views.
