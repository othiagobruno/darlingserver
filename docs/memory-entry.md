# Named memory entries

How `mach_make_memory_entry` / `mach_vm_map` work in darlingserver's duct-tape
layer (`duct-tape/src/memory.c`).

## Design

A named entry is a kobject port (`IKOT_NAMED_ENTRY`) whose kobject is a
`struct vm_named_entry`. Darling adds one field, `dtape_descriptor`, to that
struct (`duct-tape/xnu/osfmk/vm/vm_map.h`). When set, the entry is backed by a
Linux memfd, wrapped in the same refcounted `dtape_map_shared_descriptor_t` that
`mach_vm_remap_external_shared` uses. `offset`/`size` describe the window of the
memfd the entry exposes and `protection` is the maximum protection.

Lifetime:

- The port is created with one send right and a no-more-senders request.
  When the last send right goes away, the generic kobject path calls
  `mach_destroy_memory_entry`, which drops the entry's reference
  on the descriptor.
- Every entry and every mapping made from it holds its own descriptor
  reference, so the memfd stays open until the last of them is gone.
  Destroying the port does not tear down existing mappings.

## `_mach_make_memory_entry`

| Case | Status |
|---|---|
| `MAP_MEM_NAMED_CREATE` | **Real.** `memfd_create` + `ftruncate` to the size rounded up to the host page size; `*size` is updated to the rounded size. `offset` is ignored. Cache mode, `MAP_MEM_PURGABLE*`, and `MAP_MEM_GRAB_SECLUDED` are accepted and ignored. |
| `parent_entry` set (sub-entry) | **Real.** Window into the parent's memfd. Start is truncated and end rounded to pages. Requested protection must be a subset of the parent's (`KERN_INVALID_RIGHT` otherwise); the range must lie inside the parent (`KERN_INVALID_ARGUMENT`). |
| Wrapping an existing range of `target_map` | **Stub** (`KERN_NOT_SUPPORTED`). |
| `MAP_MEM_ONLY`, `NAMED_REUSE`, `VM_COPY`, `VM_SHARE`, `USE_DATA_ADDR`, `4K_DATA_ADDR`, `LEDGER_TAGGED` | **Stub** (`KERN_NOT_SUPPORTED`). |

Open question: a zero-sized `NAMED_CREATE` currently fails with
`KERN_INVALID_ARGUMENT`. Whether the original returns success with a null
handle is not established and needs black-box confirmation.

## `mach_vm_map_external`

Mapping a named entry is **real**: the memfd is mapped `MAP_SHARED` into the
target process through the existing `task_map_file` hook (the server-to-client
mmap path, `Process::mapFile` -> `Thread::mapFile` -> `_mmap`), at
`entry offset + offset`.

- `VM_FLAGS_ANYWHERE` passes no hint; otherwise the mapping is fixed
  (`MAP_FIXED_NOREPLACE`, or `MAP_FIXED` with `VM_FLAGS_OVERWRITE`).
- Sizes are rounded up to pages. `offset` and fixed addresses must be
  page-aligned. The range must lie within the entry.
- `cur_protection` must be a subset of `max_protection`, and `max_protection`
  a subset of the entry's protection (`KERN_INVALID_RIGHT`).
- The mapping is registered as a shared entry of the target map, so
  `vm_remap` of the whole region reuses the same memfd (the page offset of the
  entry is now honored in that path too).

Not implemented / limitations:

- A null memory-object port maps zero-filled anonymous memory (**real**),
  allocated through the same remote mmap hook as `mach_vm_allocate`
  (`task_allocate_pages`) with `cur_protection`. Sizes, alignment, flags and
  protection checks match the named-entry case (`max_protection` is not
  checked against an entry, there being none); `offset` is ignored.
- **Stub:** `copy == TRUE` returns `KERN_NOT_SUPPORTED`; the remote mmap hook
  only does `MAP_SHARED`. Needs a `MAP_PRIVATE` option on the hook.
- **Stub:** alignment masks larger than a page with `VM_FLAGS_ANYWHERE`
  return `KERN_NOT_SUPPORTED`; the remote mmap cannot request alignment.
- `inheritance` is not applied (shared mappings are inherited shared on fork).
- Non-null ports that are not memfd-backed named entries return
  `KERN_INVALID_ARGUMENT`.
- `task_map_file` returns 0 on any failure and does not carry `errno`, so
  failures are reported as `KERN_NO_SPACE` (fixed) or `KERN_RESOURCE_SHORTAGE`.
- A fixed `VM_FLAGS_OVERWRITE` mapping over existing registered shared
  regions does not unregister them (same as the existing remap code).
- `vm_remap` of a sub-range of a named-entry mapping hits the existing
  "complexly remap existing shared regions" stub.

## Provenance

Based on the public Mach VM API contract (`mach_make_memory_entry_64`,
`mach_vm_map`, `<mach/memory_object_types.h>`, `<mach/vm_prot.h>`), the
already-vendored XNU headers/open-source VM code in this tree, and Linux
`memfd_create(2)` / `mmap(2)`. No disassembly, backtraces, or restricted
sources were used. Nothing was built or run; this change is untested.
