#include "htram_group.h"
#include <algorithm>
#include <thread>
#include <mutex>

// ACIC_COMM_SHARE (step 7.6o): time spent handing buffers to Charm++, per
// thread, so a client can split its own time into work and sends. One rdtsc
// pair per message sent.
#ifdef ACIC_COMM_SHARE
#include <x86intrin.h>
thread_local unsigned long htram_send_tsc = 0;
#define HTRAM_TIMED_SEND(stmt)                   \
  do {                                           \
    const unsigned long htram_t0_ = __rdtsc();   \
    stmt                                         \
    htram_send_tsc += __rdtsc() - htram_t0_;     \
  } while (0);
#else
#define HTRAM_TIMED_SEND(stmt) stmt
#endif

//#define DEBUG 1

CkReductionMsg *msgStatsCollection(int nMsg, CkReductionMsg **rdmsgs) {
  double *msg_stats;
  msg_stats = (double *)rdmsgs[0]->getData();
  for (int i = 1; i < nMsg; i++) {
    CkAssert(rdmsgs[i]->getSize() == STATS_COUNT * sizeof(double));
    if (rdmsgs[i]->getSize() != STATS_COUNT * sizeof(double)) {
      CkPrintf("Error!!! Reduction not correct. Msg size is %d\n",
               rdmsgs[i]->getSize());
      CkAbort("Incorrect Reduction size in MetaBalancer\n");
    }
    double *m = (double *)rdmsgs[i]->getData();
    msg_stats[TOTAL_LATENCY] += m[TOTAL_LATENCY];
    msg_stats[MAX_LATENCY] = max(m[MAX_LATENCY], msg_stats[MAX_LATENCY]);
    msg_stats[MIN_LATENCY] = min(m[MIN_LATENCY], msg_stats[MIN_LATENCY]);
    msg_stats[TOTAL_MSGS] += m[TOTAL_MSGS];
  }
  return CkReductionMsg::buildNew(rdmsgs[0]->getSize(), NULL,
                                  rdmsgs[0]->getReducer(), rdmsgs[0]);
}

/*global*/ CkReduction::reducerType msgStatsCollectionType;
/*initnode*/ void registerMsgStatsCollection(void) {
  msgStatsCollectionType =
      CkReduction::addReducer(msgStatsCollection, true, "msgStatsCollection");
}

void periodic_tflush(void *htram_obj, double time);

HTram::HTram(CkGroupID recv_ngid, CkGroupID src_ngid, int buffer_size,
             bool enable_buffer_flushing, double time_in_ms, bool ret_item,
             bool req, CkCallback start_cb) {
  request = req;
  get_dest_proc = nullptr;
  tram_done = nullptr;
  flush_time = time_in_ms;
  enable_flush = enable_buffer_flushing;
  msg_stats[MIN_LATENCY] = 100.0;
  agg_msg_count = 0;
  flush_msg_count = 0;
  tot_send_count = 0;
  tot_recv_count = 0;
  local_updates = 0;
  num_nodes = CkNumNodes();
  ret_list = !ret_item;
  // The buffer_size argument used to be accepted and then ignored -- bufSize
  // was always BUFSIZE -- so the only way to change the aggregation buffer
  // size was to edit htram_group.h and rebuild both libraries. Messages are
  // varsize now, so this is a live runtime knob.
  CkAssert(buffer_size > 0 && buffer_size <= BUFSIZE);
  bufSize = buffer_size;
  updateReleaseLevel();

#ifdef BUCKETS_BY_DEST
  nodesize = CkNodeSize(0);
  nodeOf = new int[CkNumPes()];
  for (int i = 0; i < CkNumPes(); i++)
    nodeOf[i] = i / nodesize;
  agg = WPs;
  for (int i = 0; i < CkNumNodes(); i++) {
    std::vector<HTramMessage *> vec2;
    fillerOverflowBuffers.push_back(vec2);
    std::vector<int> int_min;
    std::vector<int> int_max;
    fillerOverflowBuffersBucketMin.push_back(int_min);
    fillerOverflowBuffersBucketMax.push_back(int_max);
  }
  histo_bucket_count = 2048;
  // The outer arrays stay at CkNumPes() so that a later reset_stats() can
  // switch to WW -- where the index is a PE -- without reallocating. Only the
  // destinations actually in use get rows. Under WPs a row is per node, so
  // filling CkNumPes() of them allocated CkNodeSize() times too many:
  // histo_bucket_count queues each, on every PE.
  tram_hold = new hold_queue *[CkNumPes()];
  updates_in_tram = new int[CkNumPes()];
  full_sends = new int[CkNumPes()];
  for (int i = 0; i < CkNumPes(); i++) {
    tram_hold[i] = nullptr;
    updates_in_tram[i] = 0;
    full_sends[i] = 0;
  }
  for (int i = 0; i < destCount(); i++)
    tram_hold[i] = new hold_queue[histo_bucket_count];
  hold_words = (histo_bucket_count + 63) / 64;
  hold_bits.assign((size_t)CkNumPes() * hold_words, 0);
#else
  nodesize = 0;
  nodeOf = nullptr;
  agg = WsP;
  tram_hold = nullptr;
#endif

  myPE = CkMyPe();
  num_dest = destCount();
  msgBuffers = new HTramMessage *[CkNumPes()];

  if (thisIndex == 0) {
    if (agg == WPs)
      CkPrintf("Aggregation type: WPs with buffer size %d\n", BUFSIZE);
    else if (agg == WsP)
      CkPrintf("Aggregation type: WsP with buffer size %d\n", BUFSIZE);
    else if (agg == PP)
      CkPrintf("Aggregation type: PP with buffer size %d and local buffer size %d\n",
               BUFSIZE, LOCAL_BUFSIZE);
    else if (agg == WW)
      CkPrintf("Aggregation type: WW with buffer size %d\n", BUFSIZE);
  }

  // Same reasoning: the pointer array is CkNumPes() wide, but a full-capacity
  // HTramMessage is ~48 KB, so only the destinations in use are given one.
  for (int i = 0; i < CkNumPes(); i++)
    msgBuffers[i] = nullptr;
  for (int i = 0; i < destCount(); i++)
    msgBuffers[i] = newHTramMessage(bufSize);
  min_key_.assign(CkNumPes(), LLONG_MAX);

  localBuffers = new std::vector<itemT>[CkNumPes()];

  local_buf = new HTramLocalMessage *[CkNumNodes()];
  local_idx.reset(new int[CkNumNodes()]);
  for (int i = 0; i < CkNumNodes(); i++) {
    local_buf[i] = newHTramLocalMessage(LOCAL_BUFSIZE);
    local_idx[i] = 0;
  }

  nodeGrpProxy = CProxy_HTramRecv(recv_ngid);
  srcNodeGrpProxy = CProxy_HTramNodeGrp(src_ngid);

  srcNodeGrp = (HTramNodeGrp *)srcNodeGrpProxy.ckLocalBranch();
  nodeGrp = (HTramRecv *)nodeGrpProxy.ckLocalBranch();
  // PP's shared buffers start at this bufSize; every PE of the process
  // offers the same value and the first one sets it
  for (int i = 0; i < CkNumNodes(); i++) {
    int unset = 0;
    srcNodeGrp->pp_limit[i].compare_exchange_strong(unset, bufSize);
  }
  int unset = 0;
  srcNodeGrp->pp_size.compare_exchange_strong(unset, bufSize);

  CkGroupID my_gid = ckGetGroupID();
  nodeGrp->setTramProxy(my_gid);

  if (enable_flush)
    periodic_tflush((void *)this, flush_time);
#ifdef IDLE_FLUSH
  CkCallWhenIdle(CkIndex_HTram::idleFlush(), this);
#endif
  contribute(start_cb);
}

void HTram::trim(HTramMessage *m) {
  trimHTramMessage(m);
  bytes_sent += m->usedBytes();
  bytes_alloc += m->payloadOffset() +
                 ALIGN_DEFAULT(sizeof(itemT) * (size_t)m->cap);
  msgs_sent++;
}

// Reduce the byte counters to the caller's callback as
// {msgs_sent, bytes_sent, bytes_alloc, node_msgs, node_msg_bytes,
//  stale_flushes, items absorbed by the holds, items that entered them,
//  idle_flushes}.
// The node-message figures are per node, so only rank 0 contributes them.
void HTram::tramStats(CkCallback cb) {
  unsigned long long absorbed = 0, entered = 0;
#ifdef BUCKETS_BY_DEST
  if (holds)
    for (int d = 0; d < destCount(); d++) {
      absorbed += holds[d].absorbed();
      entered += holds[d].inserted() + holds[d].absorbed();
    }
#endif
  unsigned long long values[9] = {msgs_sent, bytes_sent, bytes_alloc, 0, 0,
                                  stale_flushes, absorbed, entered,
                                  idle_flushes};
  if (CkMyRank() == 0) {
    values[3] = nodeGrp->node_msgs.load(std::memory_order_relaxed);
    values[4] = nodeGrp->node_msg_bytes.load(std::memory_order_relaxed);
  }
  contribute(9 * sizeof(unsigned long long), values,
             CkReduction::sum_ulong_long, cb);
}

void HTram::initPPLimit(int n) {
  CkAssert(agg == PP && n > 0 && n <= bufSize && n % LOCAL_BUFSIZE == 0);
  for (int i = 0; i < CkNumNodes(); i++) {
    // Nothing is in flight yet: the constructor's value is replaced outright
    CkAssert(srcNodeGrp->get_idx[i].load(std::memory_order_relaxed) == 0);
    srcNodeGrp->pp_limit[i].store(n, std::memory_order_relaxed);
  }
  srcNodeGrp->pp_size.store(n, std::memory_order_relaxed);
}

void HTram::setBufferSize(int new_size) {
  CkAssert(new_size > 0 && new_size <= BUFSIZE);
  if (agg == PP && new_size % LOCAL_BUFSIZE)
    CkAbort("htram: PP needs a buffer size that is a multiple of %d, not %d",
            LOCAL_BUFSIZE, new_size);
  if (new_size == bufSize)
    return;
  if (agg == PP) {
    // The shared buffers take the new size as each is next installed (see
    // HTramNodeGrp::pp_limit). Only growth: a copy may have read a limit
    // from before an install, which is safe only if limits never shrink.
    if (new_size < srcNodeGrp->pp_size.load(std::memory_order_relaxed))
      CkAbort("htram: PP buffers can grow but not shrink (%d to %d)",
              srcNodeGrp->pp_size.load(std::memory_order_relaxed), new_size);
    int cur = srcNodeGrp->pp_size.load(std::memory_order_relaxed);
    while (cur < new_size &&
           !srcNodeGrp->pp_size.compare_exchange_weak(cur, new_size))
      ;
  }
#ifdef BUCKETS_BY_DEST
  // Per-destination buffers can change size mid-run, which is what a client
  // steering the size from its own feedback needs. Every fill site sends when
  // next reaches bufSize exactly, so two invariants must hold on return: each
  // buffer can hold bufSize items, and none already holds that many. A buffer
  // that is too small is copied into a larger one; one that is already at or
  // past a smaller size leaves now, as a full send would have.
  if (!holds && (agg == WPs || agg == WW)) {
    bufSize = new_size;
    updateReleaseLevel();
    for (int d = 0; d < destCount(); d++) {
      HTramMessage *m = msgBuffers[d];
      if (m->cap < bufSize) {
        HTramMessage *g = newHTramMessage(bufSize);
        std::copy(m->items(), m->items() + m->next, g->items());
        g->next = m->next;
        delete m;
        msgBuffers[d] = g;
      } else if (m->next >= bufSize) {
        updates_in_tram[d] -= m->next;
        shipBuffer(d, true);
      }
      if (updates_in_tram[d] > release_level)
        insertBucketsByDest(tram_threshold, d);
    }
    return;
  }
#endif
  // Buffers are allocated at exactly the capacity they need now, so raising
  // bufSize past an existing buffer's capacity would overrun it. Reallocate
  // instead of trusting the caller to have drained first; refuse outright if
  // anything is still buffered, since that data would be silently dropped.
  for (int i = 0; i < destCount(); i++) {
    if (!msgBuffers[i])
      continue;
    if (msgBuffers[i]->next != 0)
      CkAbort("htram: setBufferSize called with items still buffered");
    delete msgBuffers[i];
    msgBuffers[i] = newHTramMessage(new_size);
  }
  bufSize = new_size;
  updateReleaseLevel();
}

bool HTram::idleFlush() {
#ifdef IDLE_FLUSH
  tflush(true);
#endif
  return true;
}

void HTram::reset_stats(int btype, int buf_size, int agtype) {
  std::fill_n(msg_stats, STATS_COUNT, 0.0);
  msg_stats[MIN_LATENCY] = 100.0;
  std::fill_n(nodeGrp->msg_stats, STATS_COUNT, 0.0);
  nodeGrp->msg_stats[MIN_LATENCY] = 100.0;
  agg = agtype;
  num_dest = destCount();
  // agg may have just widened from nodes to PEs, so back-fill anything the
  // constructor did not allocate before refreshing the buffers.
  for (int i = 0; i < destCount(); i++) {
#ifdef BUCKETS_BY_DEST
    if (tram_hold && !tram_hold[i])
      tram_hold[i] = new hold_queue[histo_bucket_count];
#endif
    msgBuffers[i] = newHTramMessage(bufSize);
  }
}

void HTram::avgLatency(CkCallback cb) {
  return_cb = cb;
  msg_stats[TOTAL_LATENCY] /= (2 * msg_stats[TOTAL_MSGS]);
  contribute(STATS_COUNT * sizeof(double), msg_stats, msgStatsCollectionType, cb);
}

void HTramRecv::avgLatency(CkCallback cb) {
  return_cb = cb;
  msg_stats[TOTAL_LATENCY] /= (2 * msg_stats[TOTAL_MSGS]);
  contribute(STATS_COUNT * sizeof(double), msg_stats, msgStatsCollectionType, cb);
}

HTram::HTram(CkGroupID cgid, CkCallback ecb) {
  client_gid = cgid;
  endCb = ecb;
  myPE = CkMyPe();
  bufSize = BUFSIZE; // this constructor takes no buffer size
  updateReleaseLevel();
  local_idx.reset(new int[CkNumNodes()]);
  for (int i = 0; i < CkNumNodes(); i++)
    local_idx[i] = 0;
#ifndef NODE_SRC_BUFFER
  msgBuffers = new HTramMessage *[CkNumNodes()];
  for (int i = 0; i < CkNumNodes(); i++)
    msgBuffers[i] = newHTramMessage(bufSize);
#endif
}

void HTram::set_func_ptr(void (*func)(void *, datatype), void *obPtr) {
  cb = func;
  objPtr = obPtr;
}

// 2-arg form: used by UNIONFIND / paratreet builds (no bucket routing).
void HTram::set_func_ptr_retarr(void (*func)(void *, datatype *, int),
                                void *obPtr) {
  cb_retarr = func;
  objPtr = obPtr;
#ifndef BUCKETS_BY_DEST
  if (CkMyRank() == 0)
    nodeGrp->set_func_ptr_retarr(func, obPtr);
#endif
}

// 4-arg form: used by GRAPH builds (adds get_dest_proc and done callbacks).
void HTram::set_func_ptr_retarr(void (*func)(void *, datatype *, int),
                                int  (*func2)(void *, datatype),
                                void (*func3)(void *), void *obPtr) {
  cb_retarr = func;
  get_dest_proc = func2;
  tram_done = func3;
  objPtr = obPtr;
}

HTram::HTram(CkMigrateMessage *msg) {}

// The application tells the library how many priority buckets it uses. This
// used to be shareArrayOfBuckets(), which also took a pointer to the caller's
// own array of per-bucket vectors and then never read it -- the application
// was paying for HISTO_BUCKET_COUNT std::vectors per PE, each with a reserved
// 4096 entries, to hand over a pointer that was dropped on the floor.
//
// The count itself is real, but it can only be accepted while it still agrees
// with what the constructor allocated: tram_hold's rows are sized by it at
// construction, and insertBucketsByDest() walks up to histo_bucket_count. A
// larger count arriving later would have walked straight off the end of every
// row. Since nothing needs to change it, refuse rather than pretend.
void HTram::setHistoBucketCount(int bucket_count) {
  if (bucket_count != histo_bucket_count)
    CkAbort("htram: the application uses %d priority buckets but the library "
            "allocated %d. The per-destination hold is sized at construction, "
            "so this cannot be changed afterwards.",
            bucket_count, histo_bucket_count);
}

#ifdef BUCKETS_BY_DEST
void HTram::changeThreshold(int _directThreshold, int _newtramThreshold,
                            float _selectivity) {
  if ((_newtramThreshold == tram_threshold) &&
      (_directThreshold == direct_threshold) && (_selectivity == selectivity))
    return;
  int num_dest = CkNumNodes();
  if (agg == WW)
    num_dest = CkNumPes();
  // With combining on, a hold's per-bucket count is exact; its lists are not,
  // because decrease-key leaves stale references behind.
  if (holds) {
    if (_newtramThreshold > tram_threshold) {
      for (int k = 0; k < num_dest; k++)
        for (int i = tram_threshold + 1; i <= _newtramThreshold; i++)
          updates_in_tram[k] += holds[k].live(i);
    } else if (tram_threshold > _newtramThreshold) {
      for (int k = 0; k < num_dest; k++)
        for (int i = tram_threshold; i > _newtramThreshold; i--)
          updates_in_tram[k] -= holds[k].live(i);
    }
  } else if (_newtramThreshold != tram_threshold) {
    const int lo = std::min(tram_threshold, _newtramThreshold) + 1;
    const int hi = std::max(tram_threshold, _newtramThreshold);
    const int sign = _newtramThreshold > tram_threshold ? 1 : -1;
    for (int k = 0; k < num_dest; k++)
      for (int i = holdNext(k, lo); i <= hi; i = holdNext(k, i + 1))
        updates_in_tram[k] += sign * (int)tram_hold[k][i].size();
  }
#ifdef DEBUG
  for (int k = 0; k < CkNumNodes(); k++)
    CkPrintf("\nupdates_in_tram[PE-%d] for node#%d = %d", thisIndex, k,
             updates_in_tram[k]);
#endif
  tram_threshold = _newtramThreshold;
  direct_threshold = _directThreshold;
  selectivity = _selectivity;
  updateReleaseLevel();
  for (int dest_node = 0; dest_node < num_dest; dest_node++)
    if (updates_in_tram[dest_node] > selectivity * bufSize) {
      if (holds)
        releaseFull(dest_node);
      else
        insertBucketsByDest(tram_threshold, dest_node);
    }
}
#else
void HTram::changeThreshold(int _directThreshold, int _newtramThreshold,
                            float _selectivity) {
  if (_newtramThreshold > tram_threshold) {
    for (int i = tram_threshold + 1; i <= _newtramThreshold; i++)
      updates_in_tram_count += tram_hold[i].size();
  } else if (tram_threshold > _newtramThreshold) {
    for (int i = tram_threshold; i > _newtramThreshold; i--)
      updates_in_tram_count -= tram_hold[i].size();
  }
#ifdef DEBUG
  CkPrintf("\nupdates_in_tram[PE-%d] = %d", thisIndex, updates_in_tram_count);
#endif
  tram_threshold = _newtramThreshold;
  direct_threshold = _directThreshold;
  selectivity = _selectivity;
  updateReleaseLevel();
  if (updates_in_tram_count > selectivity * bufSize * CkNumNodes())
    insertBuckets(tram_threshold);
}
#endif

#ifdef BUCKETS_BY_DEST
void HTram::sendItemPrioDeferredDest(datatype new_update, int neighbor_bucket) {
  sendItemPrioDeferredDest(new_update, neighbor_bucket,
                           get_dest_proc(objPtr, new_update));
}

void HTram::sendItemPrioDeferredDest(datatype new_update, int neighbor_bucket,
                                     int dest_proc) {
  // nodeOf[] is dest_proc / nodesize, tabulated: this runs once per item, and
  // the division was a measurable share of the whole solve.
  const int dest_node = (agg == WW) ? dest_proc : nodeOf[dest_proc];
  if (dest_node < 0 || dest_node >= num_dest) {
    CkPrintf("\nError");
    CkAbort("err");
  }
  if (holds) {
    // The admitted count follows the item across the threshold: a fold that
    // improves a held item can move it from above the threshold to below.
    CombiningHold::InsertResult r =
        holds[dest_node].insert(&new_update, neighbor_bucket);
    const bool admitted = r.new_bucket <= tram_threshold;
    if (!r.absorbed) {
      if (admitted)
        updates_in_tram[dest_node]++;
    } else if (r.old_bucket != r.new_bucket) {
      const bool was_admitted = r.old_bucket <= tram_threshold;
      if (admitted && !was_admitted)
        updates_in_tram[dest_node]++;
      else if (was_admitted && !admitted)
        updates_in_tram[dest_node]--;
    }
    if (updates_in_tram[dest_node] >= release_level)
      releaseFull(dest_node);
    return;
  }
  if (neighbor_bucket > tram_threshold) {
    holdPush(dest_node, neighbor_bucket, new_update);
  } else {
    updates_in_tram[dest_node]++;
    if (neighbor_bucket > direct_threshold) {
      holdPush(dest_node, neighbor_bucket, new_update);
    } else {
      insertValueWPsTo(new_update, dest_proc, dest_node);
    }
  }
  if (updates_in_tram[dest_node] > release_level)
    insertBucketsByDest(tram_threshold, dest_node);
}
#else
void HTram::sendItemPrioDeferredDest(datatype new_update, int neighbor_bucket) {
  int dest_proc = get_dest_proc(objPtr, new_update);
  int dest_node = dest_proc / CkNodeSize(0);
  if (neighbor_bucket > tram_threshold) {
    tram_hold[neighbor_bucket].push(new_update);
  } else {
    updates_in_tram_count++;
    if (neighbor_bucket > direct_threshold) {
      tram_hold[neighbor_bucket].push(new_update);
    } else
      insertValueWPs(new_update, dest_proc);
  }
  if (updates_in_tram_count > selectivity * bufSize * num_nodes)
    insertBuckets(tram_threshold);
}
#endif

// Buckets are merged in ascending order: bucket i's items go to i / k, which
// is below i and has already given its own items away, so no queue is moved
// twice. An item's admitted status can only change one way. Every old bucket
// at or below the threshold t lands at or below t / k, but old buckets
// t+1 .. k*(t/k+1)-1 land on t / k as well and are admitted from now on.
long long HTram::admittedDrift() const {
  long long drift = 0;
#ifdef BUCKETS_BY_DEST
  if (holds)
    return 0;
  for (int d = 0; d < destCount(); d++) {
    long long want = msgBuffers[d] ? msgBuffers[d]->next : 0;
    if (tram_hold[d])
      for (int i = holdNext(d, 0); i <= tram_threshold; i = holdNext(d, i + 1))
        want += tram_hold[d][i].size();
    drift += std::llabs(want - (long long)updates_in_tram[d]);
  }
#endif
  return drift;
}

/**
 * What this PE is still holding for its destinations, split three ways:
 * `held` is every item waiting in the per-destination hold whatever its
 * bucket, `admitted` is the library's own count of how many of those the
 * threshold has released for sending, and `buffered` is what sits in partly
 * filled buffers with nowhere to go until something flushes them. A client
 * that has stopped making progress needs all three to tell "the threshold is
 * not admitting it" from "it is admitted and nothing has flushed it".
 *
 * Walks every held queue, so it belongs in a stall report and not in a round.
 */
void HTram::pendingItems(long long *held, long long *admitted,
                         long long *buffered) const {
  long long h = 0, a = 0, b = 0;
#ifdef BUCKETS_BY_DEST
  for (int d = 0; d < destCount(); d++) {
    b += msgBuffers[d] ? msgBuffers[d]->next : 0;
    a += updates_in_tram[d];
    if (holds) {
      for (int i = 0; i < histo_bucket_count; i++)
        h += holds[d].live(i);
    } else if (tram_hold[d]) {
      for (int i = holdNext(d, 0); i < histo_bucket_count; i = holdNext(d, i + 1))
        h += tram_hold[d][i].size();
    }
  }
#else
  for (int d = 0; d < CkNumNodes(); d++)
    b += msgBuffers[d] ? msgBuffers[d]->next : 0;
  a = updates_in_tram_count;
  if (tram_hold)
    for (int i = 0; i < histo_bucket_count; i++)
      h += tram_hold[i].size();
#endif
  *held = h;
  *admitted = a;
  *buffered = b;
}

void HTram::coarsenBuckets(int k, bool keep_top) {
  if (k < 2)
    return;
#ifdef BUCKETS_BY_DEST
  if (holds)
    CkAbort("htram: coarsenBuckets is not supported with combining on");
#endif
  const int new_tram = tram_threshold / k;
  const int merged_top = keep_top ? histo_bucket_count - 1 : histo_bucket_count;
  const int admit_below = std::min((new_tram + 1) * k, merged_top);
#ifdef BUCKETS_BY_DEST
  for (int d = 0; d < destCount(); d++) {
    if (!tram_hold[d])
      continue;
    for (int i = holdNext(d, tram_threshold + 1); i < admit_below;
         i = holdNext(d, i + 1))
      updates_in_tram[d] += tram_hold[d][i].size();
    // Ascending, so i / k <= i has already been visited: a bucket is either
    // a source or a destination by the time it is reached, never moved twice.
    for (int i = holdNext(d, 1); i < merged_top; i = holdNext(d, i + 1)) {
      if (i / k == i)
        continue;
      hold_queue &src = tram_hold[d][i];
      while (!src.empty()) {
        holdPush(d, i / k, src.front());
        src.pop();
      }
      holdEmptied(d, i);
    }
  }
#else
  for (int i = tram_threshold + 1; i < admit_below; i++)
    updates_in_tram_count += tram_hold[i].size();
  for (int i = 1; i < merged_top; i++) {
    hold_queue &src = tram_hold[i];
    hold_queue &dst = tram_hold[i / k];
    while (!src.empty()) {
      dst.push(src.front());
      src.pop();
    }
  }
#endif
  tram_threshold = new_tram;
  // A negative direct threshold means nothing goes direct; integer division
  // would round it up to 0.
  if (direct_threshold > 0)
    direct_threshold /= k;
#ifdef BUCKETS_BY_DEST
  for (int d = 0; d < destCount(); d++)
    if (updates_in_tram[d] > selectivity * bufSize)
      insertBucketsByDest(tram_threshold, d);
#else
  if (updates_in_tram_count > selectivity * bufSize * num_nodes)
    insertBuckets(tram_threshold);
#endif
}

#ifdef BUCKETS_BY_DEST
void HTram::insertBucketsByDest(int high, int dest_node) {
  HTramMessage *destMsg = msgBuffers[dest_node];
  for (int i = holdNext(dest_node, 0); i <= high;
       i = holdNext(dest_node, i + 1)) {
    while (!tram_hold[dest_node][i].empty()) {
      datatype item = tram_hold[dest_node][i].front();
      tram_hold[dest_node][i].pop();
      if (tram_hold[dest_node][i].empty())
        holdEmptied(dest_node, i);
      setItem(destMsg->items()[destMsg->next], item, destForItem(item));
      destMsg->next++;
      if (destMsg->next == bufSize) {
        tot_send_count += destMsg->next;
        updates_in_tram[dest_node] -= destMsg->next;
        noteFullSend(dest_node);
        trim(destMsg);
        if (agg == WW)
          HTRAM_TIMED_SEND(thisProxy[dest_node].receiveOnPE(destMsg);)
        else
          HTRAM_TIMED_SEND(nodeGrpProxy[dest_node].receive(destMsg);)
        renewBuffer(dest_node);
        destMsg = msgBuffers[dest_node];
      }
      if (updates_in_tram[dest_node] < release_level)
        break;
    }
  }
  // Optional: the 2-argument registration never sets this, and the GRAPH
  // client no longer needs it either. Calling it unconditionally was a
  // null dereference waiting for the first client that did not supply one.
  if (tram_done)
    tram_done(objPtr);
}
#else
void HTram::insertBuckets(int high) {
  for (int i = 0; i <= high; i++) {
    while (!tram_hold[i].empty()) {
      datatype item = tram_hold[i].front();
      tram_hold[i].pop();
      int dest_proc = get_dest_proc(objPtr, item);
      int dest_node = dest_proc / CkNodeSize(0);
      HTramMessage *destMsg = msgBuffers[dest_node];
      setItem(destMsg->items()[destMsg->next], item, dest_proc);
      destMsg->next++;
      if (destMsg->next == bufSize) {
        tot_send_count += destMsg->next;
        updates_in_tram_count -= destMsg->next;
        trim(destMsg);
        HTRAM_TIMED_SEND(nodeGrpProxy[dest_node].receive(destMsg);)
        renewBuffer(dest_node);
      }
      if (updates_in_tram_count < selectivity * bufSize * num_nodes)
        break;
    }
  }
  // Optional: the 2-argument registration never sets this, and the GRAPH
  // client no longer needs it either. Calling it unconditionally was a
  // null dereference waiting for the first client that did not supply one.
  if (tram_done)
    tram_done(objPtr);
}
#endif

void HTram::insertValueWPs(datatype value, int dest_pe) {
  insertValueWPsTo(value, dest_pe,
                   (agg == WW) ? dest_pe
                   : nodeOf    ? nodeOf[dest_pe]
                               : dest_pe / nodesize);
}

void HTram::insertValueWPsTo(datatype value, int dest_pe, int destNode) {
  HTramMessage *destMsg = msgBuffers[destNode];
  setItem(destMsg->items()[destMsg->next], value, dest_pe);
  destMsg->next++;
  if (destMsg->next == bufSize) {
    agg_msg_count++;
    tot_send_count += destMsg->next;
#ifdef BUCKETS_BY_DEST
    updates_in_tram[destNode] -= destMsg->next;
    noteFullSend(destNode);
#else
    updates_in_tram_count -= destMsg->next;
#endif
    trim(destMsg);
    if (agg == WW)
      HTRAM_TIMED_SEND(thisProxy[destNode].receiveOnPE(destMsg);)
    else
      HTRAM_TIMED_SEND(nodeGrpProxy[destNode].receive(destMsg);)
    renewBuffer(destNode);
  }
}

// one per node, message, fixed
// Client inserts
void HTram::insertToProcess(datatype value, int destNode) {
  HTramMessage *destMsg = msgBuffers[destNode];
  setItem(destMsg->items()[destMsg->next], value, -1);
  destMsg->next++;
  if (destMsg->next == bufSize) {
    trim(destMsg);
    HTRAM_TIMED_SEND(nodeGrpProxy[destNode].receiveOnProc(destMsg);)
    renewBuffer(destNode);
  }
}

void HTram::insertValue(datatype value, int dest_pe) {
  int destNode = dest_pe / CkNodeSize(0);

  if (agg == PP) {
    int increment = 1;
    int idx_dnode = local_idx[destNode];
    if (idx_dnode <= LOCAL_BUFSIZE - 1) {
      setItem(local_buf[destNode]->items()[idx_dnode], value, dest_pe);
      local_idx[destNode]++;
    }
    bool local_buf_full = false;
    if (local_idx[destNode] == LOCAL_BUFSIZE)
      local_buf_full = true;
    increment = LOCAL_BUFSIZE;
    if (local_buf_full)
      copyToNodeBuf(destNode, increment);
  } else {
    HTramMessage *destMsg = msgBuffers[destNode];
    if (agg == WW)
      destMsg = msgBuffers[dest_pe];

    if (agg == WsP) {
      itemT itm;
      setItem(itm, value, dest_pe);
      localBuffers[dest_pe].push_back(itm);
    } else {
      setItem(destMsg->items()[destMsg->next], value, dest_pe);
    }

    destMsg->next++;
    if (destMsg->next == bufSize) {
      agg_msg_count++;
      if (agg == WW) {
#ifdef BUCKETS_BY_DEST
        updates_in_tram[dest_pe] -= destMsg->next;
        noteFullSend(dest_pe);
#endif
        trim(destMsg);
        HTRAM_TIMED_SEND(thisProxy[dest_pe].receiveOnPE(destMsg);)
        renewBuffer(dest_pe);
      } else if (agg == WsP) {
        shipWsP(destNode);
      } else {
        tot_send_count += destMsg->next;
#ifdef BUCKETS_BY_DEST
        updates_in_tram[destNode] -= destMsg->next;
        noteFullSend(destNode);
#else
        updates_in_tram_count -= destMsg->next;
#endif
        trim(destMsg);
        HTRAM_TIMED_SEND(nodeGrpProxy[destNode].receive(destMsg);)
        renewBuffer(destNode);
      }
    }
  }
}

void HTram::registercb() {
  CcdCallFnAfter(periodic_tflush, (void *)this, flush_time);
}

std::mutex node_mutex;

// PP's shared node buffer. A PE reserves a slot range with get_idx, copies
// its items in, and counts them into done_count; whoever brings done_count to
// bufSize (or a flush, shipSharedPP) ships the buffer, installs a new one and
// resets the counters. With BUCKETS_BY_DEST the mailbox handshake below is
// compiled out, and these atomics are all that orders the copies: each copy
// is released by its done_count increment and acquired by the shipper's, and
// a reservation acquires the reset that installed the buffer it copies into.
// They were relaxed, which x86 forgives (every atomic RMW is a full barrier)
// but ARM does not: a shipper could send items not yet written, and a PE
// could copy into the buffer that had just been sent.
void HTram::copyToNodeBuf(int destnode, int increment) {
  // Reserve with a compare-and-swap. If the buffer is full and being shipped,
  // send this PE's local buffer on its own instead of waiting for it.
  //
  // Waiting is unsafe twice over. This used to retry fetch_add(increment)
  // until it returned a free slot, so every waiting PE kept advancing
  // get_idx, and a ship that stalled in the network let the waiters wrap it
  // past INT_MAX to a negative index (writes ~2^31 items before the buffer).
  // And waiting at all can deadlock: the shipping PE's send may retry until
  // the destination process makes progress, while that process's PEs are
  // waiting here on their own shipper (PP with 16-item buffers on four
  // Stampede3 nodes: every worker spinning here or in an LCI send retry).
  //
  // The buffer's limit, not this PE's bufSize, decides when it is full. It
  // only grows (setBufferSize), so a limit read before another PE installed
  // a new buffer is at most the new one's: this PE may send its local buffer
  // when there was room, never reserve past the limit.
  std::atomic<int> &get_idx = srcNodeGrp->get_idx[destnode];
  std::atomic<int> &limit = srcNodeGrp->pp_limit[destnode];
  int idx = get_idx.load(std::memory_order_acquire);
  do {
    if (idx >= limit.load(std::memory_order_acquire)) {
      local_bypasses++;
      shipLocalPP(destnode);
      return;
    }
  } while (!get_idx.compare_exchange_weak(idx, idx + increment,
                                          std::memory_order_acquire,
                                          std::memory_order_acquire));
  int i;
  for (i = 0; i < increment; i++) {
    srcNodeGrp->msgBuffers[destnode]->items()[idx + i] =
        local_buf[destnode]->items()[i];
  }
  // The shared buffer's minimum key covers these items before they count as
  // done, so no ship of this buffer can reset it in between.
  long long k = min_key_[destnode];
  std::atomic<long long> &shared_min = srcNodeGrp->min_key[destnode];
  long long cur = shared_min.load(std::memory_order_relaxed);
  while (k < cur &&
         !shared_min.compare_exchange_weak(cur, k, std::memory_order_relaxed))
    ;
  int done_count = srcNodeGrp->done_count[destnode].fetch_add(
      increment, std::memory_order_acq_rel);

#ifndef BUCKETS_BY_DEST
  srcNodeGrp->mailbox_receiver[CkMyRank() + (destnode * CkNodeSize(CkMyNode()))].fetch_add(
      increment, std::memory_order_release);
#endif

  // This buffer cannot be replaced before this copy is counted (a shipper
  // waits for every reserved slot), so the limit read here is its own
  const int full = limit.load(std::memory_order_acquire);
  if (done_count + increment == full) {
#ifndef BUCKETS_BY_DEST
    int count = 0;
    while (count < full) {
      count = 0;
      for (int i = 0; i < CkNodeSize(CkMyNode()); ++i) {
        count += srcNodeGrp->mailbox_receiver[i + (destnode * CkNodeSize(CkMyNode()))].load(
            std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
      }
    }
    for (int i = 0; i < CkNodeSize(CkMyNode()); ++i)
      srcNodeGrp->mailbox_receiver[i + (destnode * CkNodeSize(CkMyNode()))].store(
          0, std::memory_order_relaxed);
#endif
    agg_msg_count++;
    srcNodeGrp->msgBuffers[destnode]->next = full;
    trim(srcNodeGrp->msgBuffers[destnode]);
    HTRAM_TIMED_SEND(nodeGrpProxy[destnode].receive(srcNodeGrp->msgBuffers[destnode]);)
    srcNodeGrp->msgBuffers[destnode] = newHTramMessage(BUFSIZE + LOCAL_BUFSIZE);
    srcNodeGrp->min_key[destnode].store(LLONG_MAX, std::memory_order_relaxed);
    // Before get_idx reopens the buffer (its store releases this)
    limit.store(srcNodeGrp->pp_size.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
    srcNodeGrp->done_count[destnode] = 0;
    srcNodeGrp->get_idx[destnode] = 0;
  }
  local_idx[destnode] = 0;
  min_key_[destnode] = LLONG_MAX;
}

void HTram::enableIdleFlush() {
#ifdef IDLE_FLUSH
  CkCallWhenIdle(CkIndex_HTram::idleFlush(), this);
#endif
}

void HTram::tflush(bool idleflush) {
#ifdef BUCKETS_BY_DEST
  if (holds) {
    // Nothing waits in msgBuffers or tram_hold with combining on; the holds
    // are the whole of what is buffered, and flushDest drains them. The idle
    // flag is not consulted: the path below drains the hold and ships partial
    // buffers whatever it says, and this matches that.
    for (int d = 0; d < destCount(); d++)
      flushDest(d);
    if (tram_done)
      tram_done(objPtr);
    return;
  }
#endif
  if (agg == PP) {
    srcNodeGrp->flush_count.fetch_add(1, std::memory_order_seq_cst);
    // Local buffers that hold nothing are no longer sent: each was a message
    // per node per flush carrying zero items.
    for (int i = 0; i < CkNumNodes(); i++)
      shipLocalPP(i);
    for (int i = 0; i < CkNumNodes(); i++)
      shipSharedPP(i);
  } else {
    int buf_count = CkNumNodes();
    if (agg == WW)
      buf_count = CkNumPes();

    for (int i = 0; i < buf_count; i++) {
#ifdef IDLE_FLUSH
      if (!idleflush || msgBuffers[i]->next > bufSize * PARTIAL_FLUSH)
#else
      if (!idleflush && msgBuffers[i]->next)
#endif
      {
        flush_msg_count++;
        HTramMessage *destMsg = msgBuffers[i];
        if (agg == WsP) {
          shipWsP(i);
        } else if (agg == WPs) {
#ifdef BUCKETS_BY_DEST
          // These items are leaving; they are no longer held for this
          // destination. Every other send site decrements, and omitting it
          // here makes the counter drift upward permanently.
          updates_in_tram[i] -= destMsg->next;
#endif
          trim(destMsg);
          HTRAM_TIMED_SEND(nodeGrpProxy[i].receive(destMsg);)
          renewBuffer(i);
        } else if (agg == WW) {
#ifdef BUCKETS_BY_DEST
          updates_in_tram[i] -= destMsg->next;
#endif
          trim(destMsg);
          HTRAM_TIMED_SEND(thisProxy[i].receiveOnPE(destMsg);)
          renewBuffer(i);
        }
      }
    }
  }

#ifdef BUCKETS_BY_DEST
  if (agg == WPs || agg == WW) {
    int num_dest = CkNumNodes();
    if (agg == WW)
      num_dest = CkNumPes();
    // Empty within-threshold buckets
    if (tram_hold)
      for (int dest_node = 0; dest_node < num_dest; dest_node++) {
        HTramMessage *destMsg = msgBuffers[dest_node];
        for (int i = holdNext(dest_node, 0); i <= tram_threshold;
             i = holdNext(dest_node, i + 1)) {
          holdEmptied(dest_node, i); // drained below
          while (!tram_hold[dest_node][i].empty()) {
            datatype item = tram_hold[dest_node][i].front();
            tram_hold[dest_node][i].pop();
            setItem(destMsg->items()[destMsg->next], item, destForItem(item));
            destMsg->next++;
            if (destMsg->next == bufSize) {
              tot_send_count += destMsg->next;
              updates_in_tram[dest_node] -= destMsg->next;
              trim(destMsg);
              if (agg == WW)
                HTRAM_TIMED_SEND(thisProxy[dest_node].receiveOnPE(destMsg);)
              else
                HTRAM_TIMED_SEND(nodeGrpProxy[dest_node].receive(destMsg);)
              renewBuffer(dest_node);
              destMsg = msgBuffers[dest_node];
            }
          }
        }
      }
    // Flush remaining partial buffers
    if (tram_hold)
      for (int node = 0; node < num_dest; node++) {
        HTramMessage *destMsg = msgBuffers[node];
        if (!destMsg->next)
          continue;
        updates_in_tram[node] -= destMsg->next;
#ifdef ADD_FILLERS
        if (destMsg->next < bufSize / 2) {
          for (int i = holdNext(node, tram_threshold + 1); i < histo_bucket_count;
               i = holdNext(node, i + 1)) {
            while (!tram_hold[node][i].empty()) {
              datatype item = tram_hold[node][i].front();
              tram_hold[node][i].pop();
              if (tram_hold[node][i].empty())
                holdEmptied(node, i);
              setItem(destMsg->items()[destMsg->next], item, destForItem(item));
              destMsg->next++;
              if (destMsg->next >= bufSize / 2)
                break;
            }
            if (destMsg->next >= bufSize / 2)
              break;
          }
        }
#endif
        tot_send_count += destMsg->next;
        trim(destMsg);
        if (agg == WW)
          HTRAM_TIMED_SEND(thisProxy[node].receiveOnPE(destMsg);)
        else
          HTRAM_TIMED_SEND(nodeGrpProxy[node].receive(destMsg);)
        renewBuffer(node);
      }
  }
  // Optional: the 2-argument registration never sets this, and the GRAPH
  // client no longer needs it either. Calling it unconditionally was a
  // null dereference waiting for the first client that did not supply one.
  if (tram_done)
    tram_done(objPtr);
#endif
}

#ifdef BUCKETS_BY_DEST
void HTram::enableCombining(const HoldOps *ops, void *client) {
  if (ops->item_size != sizeof(datatype))
    CkAbort("htram: combining ops describe %zu-byte items, but the library "
            "carries %zu-byte items",
            ops->item_size, sizeof(datatype));
  if (agg != WPs && agg != WW)
    CkAbort("htram: combining needs per-destination buffers (WPs or WW)");
  if (holds)
    return;
  for (int d = 0; d < destCount(); d++)
    if (updates_in_tram[d] || msgBuffers[d]->next)
      CkAbort("htram: enableCombining called after items were sent");
  holds = new CombiningHold[CkNumPes()];
  for (int d = 0; d < CkNumPes(); d++)
    holds[d].init(ops, client, histo_bucket_count);
}

// Send msgBuffers[dest] and replace it. `full` is whether it filled on its own
// rather than being flushed, which is what flushStale() keys on.
void HTram::shipBuffer(int dest, bool full) {
  HTramMessage *m = msgBuffers[dest];
  tot_send_count += m->next;
  if (full)
    noteFullSend(dest);
  trim(m);
  if (agg == WW)
    HTRAM_TIMED_SEND(thisProxy[dest].receiveOnPE(m);)
  else
    HTRAM_TIMED_SEND(nodeGrpProxy[dest].receive(m);)
  renewBuffer(dest);
}

// Combining on: while a full buffer's worth of admitted items is held for
// dest, fill a buffer from the lowest buckets and ship it. Items leave the
// hold only here or in a flush, so everything still waiting can be folded.
void HTram::releaseFull(int dest) {
  while (updates_in_tram[dest] >= release_level) {
    HTramMessage *m = msgBuffers[dest];
    holds[dest].release(tram_threshold, bufSize - m->next,
                        [&](void *item) { appendHeld(m, item); });
    if (m->next < bufSize)
      break; // the admitted items ran out first; nothing full to send
    updates_in_tram[dest] -= m->next;
    shipBuffer(dest, true);
  }
}

// Everything tflush() would send to one destination: the held items already
// admitted by the threshold, then the partial buffer. The hold is drained
// first so that both leave in as few messages as possible -- tflush() ships
// the partial buffer before draining, which can cost a second message per
// destination for no reason.
void HTram::flushDest(int dest) {
  if (holds) {
    for (;;) {
      HTramMessage *m = msgBuffers[dest];
      holds[dest].release(tram_threshold, bufSize - m->next,
                          [&](void *item) { appendHeld(m, item); });
      if (m->next < bufSize)
        break;
      updates_in_tram[dest] -= m->next;
      shipBuffer(dest, false);
    }
    HTramMessage *m = msgBuffers[dest];
    if (m->next) {
      updates_in_tram[dest] -= m->next;
#ifdef ADD_FILLERS
      // After the drain above nothing admitted is left, so this takes from
      // above the threshold, as tflush()'s fillers do.
      if (m->next < bufSize / 2)
        holds[dest].release(histo_bucket_count - 1, bufSize / 2 - m->next,
                            [&](void *item) { appendHeld(m, item); });
#endif
      shipBuffer(dest, false);
    }
    return;
  }
  HTramMessage *destMsg = msgBuffers[dest];
  for (int i = holdNext(dest, 0); i <= tram_threshold; i = holdNext(dest, i + 1)) {
    holdEmptied(dest, i); // drained below
    while (!tram_hold[dest][i].empty()) {
      datatype item = tram_hold[dest][i].front();
      tram_hold[dest][i].pop();
      setItem(destMsg->items()[destMsg->next], item, destForItem(item));
      destMsg->next++;
      if (destMsg->next == bufSize) {
        tot_send_count += destMsg->next;
        updates_in_tram[dest] -= destMsg->next;
        trim(destMsg);
        if (agg == WW)
          HTRAM_TIMED_SEND(thisProxy[dest].receiveOnPE(destMsg);)
        else
          HTRAM_TIMED_SEND(nodeGrpProxy[dest].receive(destMsg);)
        renewBuffer(dest);
        destMsg = msgBuffers[dest];
      }
    }
  }
  if (destMsg->next) {
    updates_in_tram[dest] -= destMsg->next;
#ifdef ADD_FILLERS
    // Same padding tflush() applies, so the two flushes differ only in which
    // destinations they reach. Fillers sit above the threshold and were never
    // counted in updates_in_tram, hence the decrement above comes first.
    for (int i = holdNext(dest, tram_threshold + 1);
         i < histo_bucket_count && destMsg->next < bufSize / 2;
         i = holdNext(dest, i + 1)) {
      while (!tram_hold[dest][i].empty() && destMsg->next < bufSize / 2) {
        datatype item = tram_hold[dest][i].front();
        tram_hold[dest][i].pop();
        if (tram_hold[dest][i].empty())
          holdEmptied(dest, i);
        setItem(destMsg->items()[destMsg->next], item, destForItem(item));
        destMsg->next++;
      }
    }
#endif
    tot_send_count += destMsg->next;
    trim(destMsg);
    if (agg == WW)
      HTRAM_TIMED_SEND(thisProxy[dest].receiveOnPE(destMsg);)
    else
      HTRAM_TIMED_SEND(nodeGrpProxy[dest].receive(destMsg);)
    renewBuffer(dest);
  }
}
#endif

void HTram::flushStale() {
#ifdef BUCKETS_BY_DEST
  if (agg == WPs || agg == WW) {
    for (int d = 0; d < destCount(); d++) {
      if (full_sends[d] == 0 && (updates_in_tram[d] > 0 || msgBuffers[d]->next)) {
        flushDest(d);
        stale_flushes++;
      }
      full_sends[d] = 0;
    }
    return;
  }
#endif
  // WsP and PP keep no per-destination fill counts, so "stale" is everything
  // still buffered -- without tflush()'s empty messages.
  if (agg == WsP || agg == PP) {
    flushPending();
    return;
  }
  tflush();
}

void HTram::flushIdle() {
#ifdef BUCKETS_BY_DEST
  if (agg == WPs || agg == WW) {
    // Called on every scheduler pass while the PE is idle, so the common case
    // -- nothing buffered -- has to cost one comparison per destination.
    double now = 0.0;
    if (idle_flush_interval > 0.0) {
      now = CkWallTimer();
      if (now - last_idle_flush < idle_flush_interval)
        return;
    }
    bool sent = false;
    for (int d = 0; d < destCount(); d++)
      if (updates_in_tram[d] > 0 || msgBuffers[d]->next) {
        flushDest(d);
        idle_flushes++;
        sent = true;
      }
    if (sent && idle_flush_interval > 0.0)
      last_idle_flush = now;
    return;
  }
#endif
  // WsP and PP keep no per-destination bookkeeping, but flushPending() sends
  // only what is buffered (tflush() would ship an empty message per node on
  // every idle pass), so it can run here; the interval paces it the same way.
  if (agg == WsP || agg == PP) {
    double now = 0.0;
    if (idle_flush_interval > 0.0) {
      now = CkWallTimer();
      if (now - last_idle_flush < idle_flush_interval)
        return;
    }
    // Counted per call, not per destination, in these modes.
    idle_flushes++;
    flushPending();
    if (idle_flush_interval > 0.0)
      last_idle_flush = now;
  }
}

void HTram::shipWsP(int node) {
  HTramMessage *destMsg = msgBuffers[node];
  const int nsize = CkNodeSize(0);
  int sz = 0;
  for (int k = 0; k < nsize; k++) {
    std::vector<itemT> &v = localBuffers[node * nsize + k];
    std::copy(v.begin(), v.end(), &(destMsg->items()[sz]));
    sz += v.size();
    v.clear();
  }
  CkAssert(sz == destMsg->next);
  trim(destMsg);
  HTRAM_TIMED_SEND(nodeGrpProxy[node].receive_no_sort(destMsg);)
  renewBuffer(node);
}

bool HTram::shipLocalPP(int node) {
  if (!local_idx[node])
    return false;
  HTramLocalMessage *m = local_buf[node];
  m->next = local_idx[node];
  ((envelope *)UsrToEnv(m))->setUsersize(m->usedBytes());
  // Counted with the other sends; tflush() used to leave these out.
  bytes_sent += m->usedBytes();
  bytes_alloc += m->payloadOffset() +
                 ALIGN_DEFAULT(sizeof(itemT) * (size_t)m->cap);
  msgs_sent++;
  HTRAM_TIMED_SEND(nodeGrpProxy[node].receive_small(m);)
  local_buf[node] = newHTramLocalMessage(LOCAL_BUFSIZE);
  local_idx[node] = 0;
  min_key_[node] = LLONG_MAX;
  return true;
}

// The node's shared buffer toward `i`, shipped as tflush() always did: reserve
// the rest of the buffer so no other PE copies into it, wait for the copies
// already reserved to land, and send what they filled. If another PE is
// already shipping it (full, or flushing), that PE owns it and this does
// nothing.
bool HTram::shipSharedPP(int i) {
  if (!srcNodeGrp->done_count[i])
    return false;
  flush_msg_count++;
  // Reserve the rest of the buffer with a CAS, as copyToNodeBuf does, and
  // give up if it is already full or being shipped. A fetch_add here that
  // found it busy left its bufSize added, and idle PEs flushing on every
  // scheduler pass during a stalled ship wrapped get_idx negative. Acquire,
  // as in copyToNodeBuf: the buffer installed last, and the done counts for
  // the items copied into it.
  //
  // Against the buffer's own limit (see copyToNodeBuf). The reservation goes
  // past the largest limit any buffer can have, so no copy can start after
  // it whatever limit that copy read.
  std::atomic<int> &get_idx = srcNodeGrp->get_idx[i];
  std::atomic<int> &limit = srcNodeGrp->pp_limit[i];
  int idx = get_idx.load(std::memory_order_acquire);
  do {
    if (idx >= limit.load(std::memory_order_acquire))
      return false;
  } while (!get_idx.compare_exchange_weak(idx, idx + BUFSIZE + LOCAL_BUFSIZE,
                                          std::memory_order_acquire,
                                          std::memory_order_acquire));
  int done_count = srcNodeGrp->done_count[i].fetch_add(0, std::memory_order_acquire);
  while (idx != done_count) {
    done_count = srcNodeGrp->done_count[i].fetch_add(0, std::memory_order_acquire);
  }
#ifndef BUCKETS_BY_DEST
  if (done_count == idx) {
    int count = 0;
    while (count < done_count) {
      count = 0;
      for (int j = 0; j < CkNodeSize(CkMyNode()); ++j) {
        count += srcNodeGrp->mailbox_receiver[j + (i * CkNodeSize(CkMyNode()))].load(
            std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
      }
    }
    for (int j = 0; j < CkNodeSize(CkMyNode()); ++j)
      srcNodeGrp->mailbox_receiver[j + (i * CkNodeSize(CkMyNode()))].store(
          0, std::memory_order_relaxed);
  }
#endif
  srcNodeGrp->msgBuffers[i]->next = srcNodeGrp->done_count[i];
  trim(srcNodeGrp->msgBuffers[i]);
  HTRAM_TIMED_SEND(nodeGrpProxy[i].receive(srcNodeGrp->msgBuffers[i]);)
  srcNodeGrp->msgBuffers[i] = newHTramMessage(BUFSIZE + LOCAL_BUFSIZE);
  srcNodeGrp->min_key[i].store(LLONG_MAX, std::memory_order_relaxed);
  // Before get_idx reopens the buffer (its store releases this)
  limit.store(srcNodeGrp->pp_size.load(std::memory_order_relaxed),
              std::memory_order_relaxed);
  srcNodeGrp->done_count[i] = 0;
  srcNodeGrp->flush_count = 0;
  srcNodeGrp->get_idx[i] = 0;
  return true;
}

void HTram::insert(const datatype &item, int dest_pe) {
#ifdef BUCKETS_BY_DEST
  if (agg == WPs || agg == WW) {
    if (holds)
      CkAbort("htram: insert() bypasses the combining holds; use "
              "sendItemPrioDeferredDest with combining on");
    // The admitted, below-direct-threshold path of sendItemPrioDeferredDest:
    // count the item in here, and every send site counts it back out.
    const int dest = (agg == WW) ? dest_pe : nodeOf[dest_pe];
    updates_in_tram[dest]++;
    insertValueWPsTo(item, dest_pe, dest);
    return;
  }
#else
  if (agg == WPs || agg == WW)
    CkAbort("htram: insert() under WPs/WW needs a BUCKETS_BY_DEST build");
#endif
  insertValue(item, dest_pe);
}

void HTram::flushTo(int dest_pe) {
  flushIndex(agg == WW ? dest_pe : dest_pe / CkNodeSize(0));
}

void HTram::flushIndex(int dest) {
  switch (agg) {
#ifdef BUCKETS_BY_DEST
  case WPs:
  case WW:
    flushDest(dest);
    break;
#endif
  case WsP:
    if (msgBuffers[dest]->next) {
      flush_msg_count++;
      shipWsP(dest);
    }
    break;
  case PP:
    shipLocalPP(dest);
    break;
  default:
    CkAbort("htram: flushTo() under WPs/WW needs a BUCKETS_BY_DEST build");
  }
}

void HTram::insert(const datatype &item, int dest_pe, long long key) {
  // Before the insert: if it fills and ships the buffer, the reset that
  // follows covers this item too.
  const int d = (agg == WW) ? dest_pe : dest_pe / CkNodeSize(0);
  if (key < min_key_[d])
    min_key_[d] = key;
  insert(item, dest_pe);
}

void HTram::flushDue(long long horizon) {
  for (int d = 0; d < destCount(); d++)
    if (min_key_[d] <= horizon)
      flushIndex(d);
  if (agg == PP)
    for (int i = 0; i < CkNumNodes(); i++)
      if (srcNodeGrp->min_key[i].load(std::memory_order_relaxed) <= horizon)
        shipSharedPP(i);
}

void HTram::flushPending() {
  if (agg == PP) {
    for (int i = 0; i < CkNumNodes(); i++)
      shipLocalPP(i);
    for (int i = 0; i < CkNumNodes(); i++)
      shipSharedPP(i);
    return;
  }
  if (agg == WsP) {
    for (int i = 0; i < CkNumNodes(); i++)
      if (msgBuffers[i]->next) {
        flush_msg_count++;
        shipWsP(i);
      }
    return;
  }
#ifdef BUCKETS_BY_DEST
  for (int d = 0; d < destCount(); d++)
    if (updates_in_tram[d] > 0 || msgBuffers[d]->next)
      flushDest(d);
#else
  tflush();
#endif
}

void HTram::setAggregationMode(int mode) {
  if (mode < WPs || mode > WW)
    CkAbort("htram: unknown aggregation mode %d", mode);
  for (int i = 0; i < destCount(); i++)
    if (msgBuffers[i] && msgBuffers[i]->next)
      CkAbort("htram: setAggregationMode called with items buffered");
  if (msgs_sent)
    CkAbort("htram: setAggregationMode called after items were sent");
  if (mode == PP && bufSize % LOCAL_BUFSIZE)
    CkAbort("htram: PP needs a buffer size that is a multiple of %d, not %d",
            LOCAL_BUFSIZE, bufSize);
#ifdef BUCKETS_BY_DEST
  if (holds && mode != WPs && mode != WW)
    CkAbort("htram: combining needs per-destination buffers (WPs or WW)");
#endif
  for (int i = 0; i < destCount(); i++) {
    delete msgBuffers[i];
    msgBuffers[i] = nullptr;
  }
  agg = mode;
  num_dest = destCount();
  for (int i = 0; i < destCount(); i++) {
#ifdef BUCKETS_BY_DEST
    if (tram_hold && !tram_hold[i])
      tram_hold[i] = new hold_queue[histo_bucket_count];
#endif
    renewBuffer(i);
  }
}

void HTram::flush_everything() {
  tflush();
}

// Called once on PE 0 to initiate htram-aware quiescence.
// Arms Charm's QD; when it fires (no messages in flight), onQD runs on PE 0.
void HTram::htramQuiesce(CkCallback cb) {
  quiesce_cb = cb;
  CkStartQD(CkCallback(CkIndex_HTram::onQD(), thisProxy[0]));
}

// Runs on PE 0 when Charm's QD fires.
// Broadcasts a flush to drain all PE-local buffers into in-flight messages,
// then arms a second QD to wait for those messages to finish processing.
void HTram::onQD() {
  thisProxy.tflush();
  CkStartQD(CkCallback(CkIndex_HTram::countBuffers(), thisProxy));
}

// Runs on every PE after the post-flush QD fires.
// Each PE counts items still sitting in its local buffers and contributes to
// a sum reduction; if any PE has leftover items, another flush-QD cycle runs.
void HTram::countBuffers() {
  int count = 0;
  if (agg == PP) {
    for (int i = 0; i < CkNumNodes(); i++) {
      count += local_idx[i];
      count += srcNodeGrp->done_count[i].load(std::memory_order_relaxed);
    }
  } else {
    int buf_count = (agg == WW) ? CkNumPes() : CkNumNodes();
    for (int i = 0; i < buf_count; i++)
      count += msgBuffers[i]->next;
  }
#ifdef BUCKETS_BY_DEST
  int num_dest = (agg == WW) ? CkNumPes() : CkNumNodes();
  for (int i = 0; i < num_dest; i++)
    count += updates_in_tram[i];
#else
  count += updates_in_tram_count;
#endif
  contribute(sizeof(int), &count, CkReduction::sum_int,
             CkCallback(CkReductionTarget(HTram, onBufferCount), thisProxy[0]));
}

// Reduction target on PE 0.
// If all buffers are empty, fires the user's quiescence callback.
// Otherwise repeats the flush-QD cycle.
void HTram::onBufferCount(int total) {
  if (total == 0)
    quiesce_cb.send();
  else
    CkStartQD(CkCallback(CkIndex_HTram::onQD(), thisProxy[0]));
}

HTramNodeGrp::HTramNodeGrp() {
  msgBuffers = new HTramMessage *[CkNumNodes()];
  get_idx.reset(new std::atomic<int>[CkNumNodes()]);
  done_count.reset(new std::atomic<int>[CkNumNodes()]);
  min_key.reset(new std::atomic<long long>[CkNumNodes()]);
  pp_limit.reset(new std::atomic<int>[CkNumNodes()]);
  for (int i = 0; i < CkNumNodes(); i++) {
    min_key[i] = LLONG_MAX;
    pp_limit[i] = 0;  // set from the first HTram's bufSize (HTram::HTram)
    // Written concurrently by every PE on the node, any of which may have a
    // different bufSize, so these are allocated at the maximum. The extra
    // LOCAL_BUFSIZE is slack for copyToNodeBuf, which claims a slot index
    // below bufSize and then writes LOCAL_BUFSIZE items from it.
    msgBuffers[i] = newHTramMessage(BUFSIZE + LOCAL_BUFSIZE);
    get_idx[i] = 0;
    done_count[i] = 0;
  }
#ifndef BUCKETS_BY_DEST
  num_mailboxes = CkNumPes();
  mailbox_receiver.reset(new std::atomic<int>[num_mailboxes]);
  for (int i = 0; i < num_mailboxes; i++)
    mailbox_receiver[i].store(0, std::memory_order_relaxed);
#endif
}

HTramNodeGrp::HTramNodeGrp(CkMigrateMessage *msg) {}

HTramRecv::HTramRecv() {
  msg_stats[MIN_LATENCY] = 100.0;
  inbox.reset(new HTramInbox[CkNodeSize(CkMyNode())]);
}

void HTramRecv::deliverSorted(HTramNodeMessage *sorted,
                              const std::vector<int> &sizes) {
  const int first = CkNodeFirst(CkMyNode());
  HTram *local = tram_proxy.ckLocalBranch();
  if (local->inbox_delivery) {
    // This PE's own share is delivered last, in place: it is already here
    HTramNodeMessage *own = nullptr;
    for (int r = 0; r < CkNodeSize(CkMyNode()); r++) {
      if (!sizes[r])
        continue;
      HTramNodeMessage *m = (HTramNodeMessage *)CkReferenceMsg(sorted);
      if (first + r == CkMyPe()) {
        own = m;
        continue;
      }
      HTramInbox &box = inbox[r];
      std::lock_guard<std::mutex> guard(box.lock);
      box.msgs.push_back(m);
      box.pending.store((int)box.msgs.size(), std::memory_order_release);
    }
    CkFreeMsg(sorted);
    if (own)
      local->receivePerPE(own);
    return;
  }
  // With skip_empty_deliveries, only the PEs this message carries items for.
  // At 8 nodes a message averages ~70 items over 15 PEs, so most deliveries
  // are a scheduler pass and a handler for nothing, and skipping them made
  // orkut 1.1x faster. It is off by default because those empty deliveries
  // also sit in each PE's FIFO ahead of its heap-pass continuations, and a
  // high-diameter solve depends on that: mesh24 at 2 nodes ran 2.1x slower
  // without them, more improvements arriving only after their vertex had
  // been expanded (step 8b).
  const bool skip = local->skip_empty_deliveries;
  for (int r = 0; r < CkNodeSize(CkMyNode()); r++) {
    if (skip && !sizes[r])
      continue;
    HTramNodeMessage *tmpMsg = (HTramNodeMessage *)CkReferenceMsg(sorted);
    _SET_USED(UsrToEnv(tmpMsg), 0);
    tram_proxy[first + r].receivePerPE(tmpMsg);
  }
  CkFreeMsg(sorted);
}

void HTram::setInboxDelivery(bool on) {
  inbox_delivery = on;
  if (on) {
    skip_empty_deliveries = true;
    recvNodeGrp = nodeGrpProxy.ckLocalBranch();
  }
}

int HTram::drainInbox() {
  if (!inbox_delivery)
    return 0;
  HTramInbox &box = recvNodeGrp->inbox[CkMyRank()];
  if (box.pending.load(std::memory_order_acquire) == 0)
    return 0;
  // Swapped out under the lock and delivered outside it. A delivery never
  // drains (the client calls this from its loop), so one buffer per thread.
  static thread_local std::vector<HTramNodeMessage *> taken;
  taken.clear();
  {
    std::lock_guard<std::mutex> guard(box.lock);
    taken.swap(box.msgs);
    box.pending.store(0, std::memory_order_relaxed);
  }
  for (HTramNodeMessage *m : taken)
    receivePerPE(m);
  return (int)taken.size();
}

HTramRecv::HTramRecv(CkMigrateMessage *msg) {}

void HTramRecv::receive_no_sort(HTramMessage *agg_message) {
  checkHTramEnvelope(agg_message, agg_message->usedBytes(), agg_message->next,
                     "HTramRecv::receive_no_sort");

  for (int i = CkNodeFirst(CkMyNode());
       i < CkNodeFirst(CkMyNode()) + CkNodeSize(CkMyNode()); i++) {
    HTramMessage *tmpMsg = (HTramMessage *)CkReferenceMsg(agg_message);
    _SET_USED(UsrToEnv(tmpMsg), 0);
    tram_proxy[i].receivePerPE(tmpMsg);
  }
  CkFreeMsg(agg_message);
}

void HTram::receivePerPE(HTramMessage *msg) {
  int pe = CkMyPe();
  // Items are pre-sorted by destPe; find this PE's contiguous range.
  int llimit = 0;
  while (llimit < msg->next && itemDestPe(msg->items()[llimit]) < pe)
    llimit++;
  int ulimit = llimit;
  while (ulimit < msg->next && itemDestPe(msg->items()[ulimit]) == pe)
    ulimit++;
  int count = ulimit - llimit;
  if (!ret_list) {
    for (int i = llimit; i < ulimit; i++)
      cb(objPtr, itemPayload(msg->items()[i]));
  } else {
    datatype *buf = new datatype[count];
    for (int i = 0; i < count; i++)
      buf[i] = itemPayload(msg->items()[llimit + i]);
    cb_retarr(objPtr, buf, count);
    delete[] buf;
  }
  tot_recv_count += count;
  CkFreeMsg(msg);
}

void HTram::receiveOnPE(HTramMessage *msg) {
  checkHTramEnvelope(msg, msg->usedBytes(), msg->next,
                     "HTram::receiveOnPE");

  if (!ret_list) {
    for (int i = 0; i < msg->next; i++)
      cb(objPtr, itemPayload(msg->items()[i]));
  } else {
    datatype *buf = new datatype[msg->next];
    for (int i = 0; i < msg->next; i++)
      buf[i] = itemPayload(msg->items()[i]);
    cb_retarr(objPtr, buf, msg->next);
    delete[] buf; // was leaked on every WW batch
  }
  tot_recv_count += msg->next;
  delete msg;
}

#ifndef BUCKETS_BY_DEST
void HTramRecv::set_func_ptr_retarr(void (*func)(void *, datatype *, int),
                                    void *obPtr) {
  cb_retarr = func;
  objPtr = obPtr;
}
#endif

void HTramRecv::receiveOnProc(HTramMessage *agg_message) {
  checkHTramEnvelope(agg_message, agg_message->usedBytes(), agg_message->next,
                     "HTramRecv::receiveOnProc");

#ifndef BUCKETS_BY_DEST
  datatype *buf = new datatype[agg_message->next];
  for (int i = 0; i < agg_message->next; i++)
    buf[i] = itemPayload(agg_message->items()[i]);
  cb_retarr(objPtr, buf, agg_message->next);
  delete[] buf;
  delete agg_message;
#else
  // The entry must exist for def.h linkage, but the node receiver keeps no
  // callback in this build, so the items used to be deleted unread. Nothing
  // sends here (insertToProcess has no caller); fail loudly if that changes.
  CkAbort("htram: receiveOnProc is not supported with BUCKETS_BY_DEST");
#endif
}

// Diagnostic: per-PE time and count of node-level receives (off unless set)
bool htram_time_receives = false;
thread_local double htram_recv_s = 0;
thread_local unsigned long long htram_recv_n = 0;
namespace {
struct RecvTimer {
  double t0;
  RecvTimer() : t0(htram_time_receives ? CkWallTimer() : 0) {}
  ~RecvTimer() {
    if (htram_time_receives) { htram_recv_s += CkWallTimer() - t0; htram_recv_n++; }
  }
};
}

void HTramRecv::receive(HTramMessage *agg_message) {
  RecvTimer timer;
  checkHTramEnvelope(agg_message, agg_message->usedBytes(), agg_message->next,
                     "HTramRecv::receive");

  int rank0PE = CkNodeFirst(thisIndex);
  // Sized to what this message actually carries. It used to be a fixed
  // BUFSIZE payload plus a std::vector member -- and since messages are freed
  // without running a destructor, that vector's allocation leaked on every
  // aggregated message received.
  HTramNodeMessage *sorted_agg_message =
      newHTramNodeMessage(agg_message->next, CkNodeSize(CkMyNode()));
  node_msgs.fetch_add(1, std::memory_order_relaxed);
  node_msg_bytes.fetch_add(sorted_agg_message->allocBytes(),
                           std::memory_order_relaxed);

  std::vector<int> sizes(CkNodeSize(CkMyNode()), 0);

  // Each item's local rank, computed once: carried on the wire, or, under
  // compact wire items, recomputed from the payload by this PE's client.
  static thread_local std::vector<int> ranks;
  const int count = agg_message->next;
  ranks.resize(count);
#if HTRAM_ITEMS_CARRY_DEST
  for (int i = 0; i < count; i++)
    ranks[i] = itemDestPe(agg_message->items()[i]) - rank0PE;
#else
  HTram *local = tram_proxy.ckLocalBranch();
  for (int i = 0; i < count; i++)
    ranks[i] = local->destPeOf(itemPayload(agg_message->items()[i])) - rank0PE;
#endif
  for (int i = 0; i < count; i++)
    sizes[ranks[i]]++;

  sorted_agg_message->offset[0] = 0;
  for (int i = 1; i < CkNodeSize(CkMyNode()); i++)
    sorted_agg_message->offset[i] =
        sorted_agg_message->offset[i - 1] + sizes[i - 1];

  for (int i = 0; i < count; i++)
    sorted_agg_message->items()[sorted_agg_message->offset[ranks[i]]++] =
        itemPayload(agg_message->items()[i]);
  delete agg_message;

  sorted_agg_message->offset[0] = sizes[0];
  for (int i = 1; i < CkNodeSize(CkMyNode()); i++)
    sorted_agg_message->offset[i] =
        sorted_agg_message->offset[i - 1] + sizes[i];

  deliverSorted(sorted_agg_message, sizes);
}

void HTramRecv::receive_small(HTramLocalMessage *agg_message) {
  RecvTimer timer;
  checkHTramEnvelope(agg_message, agg_message->usedBytes(), agg_message->next,
                     "HTramRecv::receive_small");

  int rank0PE = CkNodeFirst(thisIndex);
  HTramNodeMessage *sorted_agg_message =
      newHTramNodeMessage(agg_message->next, CkNodeSize(CkMyNode()));
  node_msgs.fetch_add(1, std::memory_order_relaxed);
  node_msg_bytes.fetch_add(sorted_agg_message->allocBytes(),
                           std::memory_order_relaxed);

  std::vector<int> sizes(CkNodeSize(CkMyNode()), 0);

  for (int i = 0; i < agg_message->next; i++) {
    int rank = itemDestPe(agg_message->items()[i]) - rank0PE;
    sizes[rank]++;
  }

  sorted_agg_message->offset[0] = 0;
  for (int i = 1; i < CkNodeSize(CkMyNode()); i++)
    sorted_agg_message->offset[i] =
        sorted_agg_message->offset[i - 1] + sizes[i - 1];

  for (int i = 0; i < agg_message->next; i++) {
    int rank = itemDestPe(agg_message->items()[i]) - rank0PE;
    sorted_agg_message->items()[sorted_agg_message->offset[rank]++] =
        itemPayload(agg_message->items()[i]);
  }
  delete agg_message;

  sorted_agg_message->offset[0] = sizes[0];
  for (int i = 1; i < CkNodeSize(CkMyNode()); i++)
    sorted_agg_message->offset[i] =
        sorted_agg_message->offset[i - 1] + sizes[i];

  deliverSorted(sorted_agg_message, sizes);
}

void HTramRecv::setTramProxy(CkGroupID tram_gid) {
  tram_proxy = CProxy_HTram(tram_gid);
}

void HTram::receivePerPE(HTramNodeMessage *msg) {
  int llimit = 0;
  int rank = CkMyRank();
  if (rank > 0)
    llimit = msg->offset[rank - 1];
  int ulimit = msg->offset[rank];
  if (!ret_list) {
    for (int i = llimit; i < ulimit; i++)
      cb(objPtr, msg->items()[i]);
  } else
    cb_retarr(objPtr, &msg->items()[llimit], ulimit - llimit);
  CkFreeMsg(msg);
  tot_recv_count += (ulimit - llimit);
}

void HTram::stop_periodic_flush() { enable_flush = false; }

void periodic_tflush(void *htram_obj, double time) {
  HTram *proper_obj = (HTram *)htram_obj;
  proper_obj->tflush();
  if (proper_obj->enable_flush)
    proper_obj->registercb();
}

void HTram::sanityCheck() {
  int tram_h_count = tot_send_count + local_updates;
  contribute(
      sizeof(int), &tot_send_count, CkReduction::sum_int,
      CkCallback(CkReductionTarget(HTram, getTotSendCount), thisProxy[0]));
  contribute(
      sizeof(int), &tot_recv_count, CkReduction::sum_int,
      CkCallback(CkReductionTarget(HTram, getTotRecvCount), thisProxy[0]));
  contribute(
      sizeof(int), &tram_h_count, CkReduction::sum_int,
      CkCallback(CkReductionTarget(HTram, getTotTramHCount), thisProxy[0]));
}

void HTram::getTotTramHCount(int hcount) {
  CkPrintf("Total items remaining in tram_hold = %d\n", hcount);
}

void HTram::getTotSendCount(int scount) {
  CkPrintf("Total items sent via tram library = %d", scount);
}

void HTram::getTotRecvCount(int rcount) {
  CkPrintf("Total items received via tram library = %d\n", rcount);
}

#include "htram_group.def.h"