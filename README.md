# HTram

HTram is a message-aggregation library for Charm++. Application code hands it
one small item at a time; it batches items per destination, ships a single
message per batch, and calls back on the receiving side with the whole batch.

The SMP library is `htram_group`. A non-SMP variant (`tramNonSmp`) used to live
here and was retired in September 2026: it had diverged from the SMP path,
two of its dependents no longer compiled, and nothing in the evaluation used
it. Recover it from history if it is ever needed again.

## Item type

The item type is fixed at library build time rather than templated, so the
library can ship as a static archive. Pick the flavour with a `-D` and, for
`GRAPH`, point the build at the header that defines the payload:

| Flavour | Item type | Defined in |
|---|---|---|
| `-DHISTO` | `int` | `types.h` |
| `-DIG` | `std::pair<int,int>` | `types.h` |
| `-DUNIONFIND` | `std::pair<int,int>` | `types.h` |
| `-DGRAPH` | `Update` | `HTRAM_GRAPH_TYPES_HEADER` |

Step 8 of the SC27 plan replaces this with a byte-oriented core plus a header-
only typed facade, at which point one archive serves every payload.

## Building

    make libhtram_group_graph.a GRAPH_INCLUDE=/path/to/charm_graph_code

`charmc`'s location is machine-specific. It is taken from, in increasing
precedence: the `CHARMC_SMP ?=` default in `Makefile.common`, an untracked
`config.mk` next to it, and a variable on make's command line.

    echo 'CHARMC_SMP = /u/rao1/charm_reconverse/bin/charmc' > config.mk

## Using it

1.  In your `.ci` file:

        extern module htram_group;
        readonly CProxy_HTram tram_proxy;

2.  In your C++ file, `#include "htram_group.h"`.

3.  Create the two node groups and the group itself in `Main`:

        CProxy_HTramRecv     recv = CProxy_HTramRecv::ckNew();
        CProxy_HTramNodeGrp  src  = CProxy_HTramNodeGrp::ckNew();
        tram_proxy = CProxy_HTram::ckNew(recv.ckGetGroupID(), src.ckGetGroupID(),
                                         buffer_size, enable_timed_flushing,
                                         flush_timer, ret_item, request, start_cb);

    `buffer_size` is in items and must be in `1..BUFSIZE`; it is a live knob,
    not a hint. Messages are varsize, so a smaller buffer really does put fewer
    bytes on the wire.

4.  Get the local branch with `tram_proxy.ckLocalBranch()` and register the
    receive callbacks. The batch form is what the graph code uses:

        tram->set_func_ptr_retarr(deliver_batch,      // (void*, datatype*, int)
                                  dest_pe_of_item,    // (void*, datatype) -> int
                                  batch_done,         // (void*)
                                  this);

5.  Send with `insertValue(item, dest_pe)`, or, when the application has a
    priority bucket for the item, `sendItemPrioDeferredDest(item, bucket)` --
    which lets the library hold low-priority items back until
    `changeThreshold()` admits their bucket. Call `setHistoBucketCount()` once
    to declare how many buckets you use.

6.  `tflush()` sends every partially filled buffer; `flush_everything()` also
    drains the per-destination hold.

## Aggregation modes

`agg` selects where batching happens: `WPs` (worker to per-source node buffer),
`WsP`, `PP`, and `WW` (worker to worker). `WPs` is the default for the graph
build. `-DBUCKETS_BY_DEST` additionally keys the priority hold by destination.
