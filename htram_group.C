#include "htram_group.h"
#include <algorithm>
#include <thread>
#include <mutex>

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
  tram_hold = new std::queue<datatype> *[CkNumPes()];
  updates_in_tram = new int[CkNumPes()];
  full_sends = new int[CkNumPes()];
  for (int i = 0; i < CkNumPes(); i++) {
    tram_hold[i] = nullptr;
    updates_in_tram[i] = 0;
    full_sends[i] = 0;
  }
  for (int i = 0; i < destCount(); i++)
    tram_hold[i] = new std::queue<datatype>[histo_bucket_count];
#else
  nodesize = 0;
  nodeOf = nullptr;
  agg = WsP;
  tram_hold = nullptr;
#endif

  myPE = CkMyPe();
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
//  stale_flushes, items absorbed by the holds, items that entered them}.
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
  unsigned long long values[8] = {msgs_sent, bytes_sent, bytes_alloc, 0, 0,
                                  stale_flushes, absorbed, entered};
  if (CkMyRank() == 0) {
    values[3] = nodeGrp->node_msgs.load(std::memory_order_relaxed);
    values[4] = nodeGrp->node_msg_bytes.load(std::memory_order_relaxed);
  }
  contribute(8 * sizeof(unsigned long long), values,
             CkReduction::sum_ulong_long, cb);
}

void HTram::setBufferSize(int new_size) {
  CkAssert(new_size > 0 && new_size <= BUFSIZE);
  if (new_size == bufSize)
    return;
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
  // agg may have just widened from nodes to PEs, so back-fill anything the
  // constructor did not allocate before refreshing the buffers.
  for (int i = 0; i < destCount(); i++) {
#ifdef BUCKETS_BY_DEST
    if (tram_hold && !tram_hold[i])
      tram_hold[i] = new std::queue<datatype>[histo_bucket_count];
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
  if (_newtramThreshold > tram_threshold) {
    for (int k = 0; k < num_dest; k++)
      for (int i = tram_threshold + 1; i <= _newtramThreshold; i++)
        updates_in_tram[k] += holds ? holds[k].live(i) : tram_hold[k][i].size();
  } else if (tram_threshold > _newtramThreshold) {
    for (int k = 0; k < num_dest; k++)
      for (int i = tram_threshold; i > _newtramThreshold; i--)
        updates_in_tram[k] -= holds ? holds[k].live(i) : tram_hold[k][i].size();
  }
#ifdef DEBUG
  for (int k = 0; k < CkNumNodes(); k++)
    CkPrintf("\nupdates_in_tram[PE-%d] for node#%d = %d", thisIndex, k,
             updates_in_tram[k]);
#endif
  tram_threshold = _newtramThreshold;
  direct_threshold = _directThreshold;
  selectivity = _selectivity;
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
  if (updates_in_tram_count > selectivity * bufSize * CkNumNodes())
    insertBuckets(tram_threshold);
}
#endif

#ifdef BUCKETS_BY_DEST
void HTram::sendItemPrioDeferredDest(datatype new_update, int neighbor_bucket) {
  int dest_proc = get_dest_proc(objPtr, new_update);
  int dest_node = dest_proc / nodesize;
  if (agg == WW)
    dest_node = dest_proc;
  int num_dest = CkNumNodes();
  if (agg == WW)
    num_dest = CkNumPes();
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
    if (updates_in_tram[dest_node] >= selectivity * bufSize)
      releaseFull(dest_node);
    return;
  }
  if (neighbor_bucket > tram_threshold) {
    tram_hold[dest_node][neighbor_bucket].push(new_update);
  } else {
    updates_in_tram[dest_node]++;
    if (neighbor_bucket > direct_threshold) {
      tram_hold[dest_node][neighbor_bucket].push(new_update);
    } else {
      insertValueWPs(new_update, dest_proc);
    }
  }
  if (updates_in_tram[dest_node] > selectivity * bufSize)
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
      for (int i = 0; i <= tram_threshold && i < histo_bucket_count; i++)
        want += tram_hold[d][i].size();
    drift += std::llabs(want - (long long)updates_in_tram[d]);
  }
#endif
  return drift;
}

void HTram::coarsenBuckets(int k) {
  if (k < 2)
    return;
#ifdef BUCKETS_BY_DEST
  if (holds)
    CkAbort("htram: coarsenBuckets is not supported with combining on");
#endif
  const int new_tram = tram_threshold / k;
  const int admit_below = std::min((new_tram + 1) * k, histo_bucket_count);
#ifdef BUCKETS_BY_DEST
  for (int d = 0; d < destCount(); d++) {
    if (!tram_hold[d])
      continue;
    for (int i = tram_threshold + 1; i < admit_below; i++)
      updates_in_tram[d] += tram_hold[d][i].size();
    for (int i = 1; i < histo_bucket_count; i++) {
      std::queue<datatype> &src = tram_hold[d][i];
      std::queue<datatype> &dst = tram_hold[d][i / k];
      while (!src.empty()) {
        dst.push(src.front());
        src.pop();
      }
    }
  }
#else
  for (int i = tram_threshold + 1; i < admit_below; i++)
    updates_in_tram_count += tram_hold[i].size();
  for (int i = 1; i < histo_bucket_count; i++) {
    std::queue<datatype> &src = tram_hold[i];
    std::queue<datatype> &dst = tram_hold[i / k];
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
  for (int i = 0; i <= high; i++) {
    while (!tram_hold[dest_node][i].empty()) {
      datatype item = tram_hold[dest_node][i].front();
      tram_hold[dest_node][i].pop();
      destMsg->items()[destMsg->next].payload = item;
      int dest_proc = get_dest_proc(objPtr, item);
      destMsg->items()[destMsg->next].destPe = dest_proc;
      destMsg->next++;
      if (destMsg->next == bufSize) {
        tot_send_count += destMsg->next;
        updates_in_tram[dest_node] -= destMsg->next;
        noteFullSend(dest_node);
        trim(destMsg);
        if (agg == WW)
          thisProxy[dest_node].receiveOnPE(destMsg);
        else
          nodeGrpProxy[dest_node].receive(destMsg);
        msgBuffers[dest_node] = newHTramMessage(bufSize);
        destMsg = msgBuffers[dest_node];
      }
      if (updates_in_tram[dest_node] < selectivity * bufSize)
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
      destMsg->items()[destMsg->next].payload = item;
      destMsg->items()[destMsg->next].destPe = dest_proc;
      destMsg->next++;
      if (destMsg->next == bufSize) {
        tot_send_count += destMsg->next;
        updates_in_tram_count -= destMsg->next;
        trim(destMsg);
        nodeGrpProxy[dest_node].receive(destMsg);
        msgBuffers[dest_node] = newHTramMessage(bufSize);
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
  int destNode = dest_pe / nodesize;
  if (agg == WW)
    destNode = dest_pe;
  HTramMessage *destMsg = msgBuffers[destNode];
  destMsg->items()[destMsg->next].payload = value;
  destMsg->items()[destMsg->next].destPe = dest_pe;
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
      thisProxy[destNode].receiveOnPE(destMsg);
    else
      nodeGrpProxy[destNode].receive(destMsg);
    msgBuffers[destNode] = newHTramMessage(bufSize);
  }
}

// one per node, message, fixed
// Client inserts
void HTram::insertToProcess(datatype value, int destNode) {
  HTramMessage *destMsg = msgBuffers[destNode];
  destMsg->items()[destMsg->next].payload = value;
  destMsg->next++;
  if (destMsg->next == bufSize) {
    trim(destMsg);
    nodeGrpProxy[destNode].receiveOnProc(destMsg);
    msgBuffers[destNode] = newHTramMessage(bufSize);
  }
}

void HTram::insertValue(datatype value, int dest_pe) {
  int destNode = dest_pe / CkNodeSize(0);

  if (agg == PP) {
    int increment = 1;
    int idx_dnode = local_idx[destNode];
    if (idx_dnode <= LOCAL_BUFSIZE - 1) {
      local_buf[destNode]->items()[idx_dnode].payload = value;
      local_buf[destNode]->items()[idx_dnode].destPe = dest_pe;
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
      itemT itm = {dest_pe, value};
      localBuffers[dest_pe].push_back(itm);
    } else if (agg == WW) {
      destMsg->items()[destMsg->next].payload = value;
    } else {
      destMsg->items()[destMsg->next].payload = value;
      destMsg->items()[destMsg->next].destPe = dest_pe;
    }

    destMsg->next++;
    if (destMsg->next == bufSize) {
      agg_msg_count++;
      if (agg == WsP) {
        int sz = 0;
        for (int i = 0; i < CkNodeSize(0); i++) {
          std::vector<itemT> localMsg =
              localBuffers[destNode * CkNodeSize(0) + i];
          std::copy(localMsg.begin(), localMsg.end(), &(destMsg->items()[sz]));
          sz += localMsg.size();
          localBuffers[destNode * CkNodeSize(0) + i].clear();
        }
      }
      if (agg == WW) {
#ifdef BUCKETS_BY_DEST
        updates_in_tram[dest_pe] -= destMsg->next;
        noteFullSend(dest_pe);
#endif
        trim(destMsg);
        thisProxy[dest_pe].receiveOnPE(destMsg);
        msgBuffers[dest_pe] = newHTramMessage(bufSize);
      } else if (agg == WsP) {
        trim(destMsg);
        nodeGrpProxy[destNode].receive_no_sort(destMsg);
        msgBuffers[destNode] = newHTramMessage(bufSize);
      } else {
        tot_send_count += destMsg->next;
#ifdef BUCKETS_BY_DEST
        updates_in_tram[destNode] -= destMsg->next;
        noteFullSend(destNode);
#else
        updates_in_tram_count -= destMsg->next;
#endif
        trim(destMsg);
        nodeGrpProxy[destNode].receive(destMsg);
        msgBuffers[destNode] = newHTramMessage(bufSize);
      }
    }
  }
}

void HTram::registercb() {
  CcdCallFnAfter(periodic_tflush, (void *)this, flush_time);
}

std::mutex node_mutex;

void HTram::copyToNodeBuf(int destnode, int increment) {
  int idx = srcNodeGrp->get_idx[destnode].fetch_add(increment,
                                                    std::memory_order_relaxed);
  while (idx >= bufSize) {
    idx = srcNodeGrp->get_idx[destnode].fetch_add(increment,
                                                  std::memory_order_relaxed);
  }
  int i;
  for (i = 0; i < increment; i++) {
    srcNodeGrp->msgBuffers[destnode]->items()[idx + i].payload =
        local_buf[destnode]->items()[i].payload;
    srcNodeGrp->msgBuffers[destnode]->items()[idx + i].destPe =
        local_buf[destnode]->items()[i].destPe;
  }
  int done_count = srcNodeGrp->done_count[destnode].fetch_add(
      increment, std::memory_order_relaxed);

#ifndef BUCKETS_BY_DEST
  srcNodeGrp->mailbox_receiver[CkMyRank() + (destnode * CkNodeSize(CkMyNode()))].fetch_add(
      increment, std::memory_order_release);
#endif

  if (done_count + increment == bufSize) {
#ifndef BUCKETS_BY_DEST
    int count = 0;
    while (count < bufSize) {
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
    srcNodeGrp->msgBuffers[destnode]->next = bufSize;
    trim(srcNodeGrp->msgBuffers[destnode]);
    nodeGrpProxy[destnode].receive(srcNodeGrp->msgBuffers[destnode]);
    srcNodeGrp->msgBuffers[destnode] = newHTramMessage(BUFSIZE + LOCAL_BUFSIZE);
    srcNodeGrp->done_count[destnode] = 0;
    srcNodeGrp->get_idx[destnode] = 0;
  }
  local_idx[destnode] = 0;
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
    int flush_count =
        srcNodeGrp->flush_count.fetch_add(1, std::memory_order_seq_cst);
    for (int i = 0; i < CkNumNodes(); i++) {
      local_buf[i]->next = local_idx[i];
      ((envelope *)UsrToEnv(local_buf[i]))
          ->setUsersize(local_buf[i]->usedBytes());
      nodeGrpProxy[i].receive_small(local_buf[i]);
      local_buf[i] = newHTramLocalMessage(LOCAL_BUFSIZE);
      local_idx[i] = 0;
    }
    {
      for (int i = 0; i < CkNumNodes(); i++) {
        if (srcNodeGrp->done_count[i]) {
          flush_msg_count++;
          int idx = srcNodeGrp->get_idx[i].fetch_add(bufSize,
                                                     std::memory_order_relaxed);
          int done_count =
              srcNodeGrp->done_count[i].fetch_add(0, std::memory_order_relaxed);
          if (idx >= bufSize)
            continue;
          while (idx != done_count) {
            done_count = srcNodeGrp->done_count[i].fetch_add(
                0, std::memory_order_relaxed);
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
          nodeGrpProxy[i].receive(srcNodeGrp->msgBuffers[i]);
          srcNodeGrp->msgBuffers[i] = newHTramMessage(BUFSIZE + LOCAL_BUFSIZE);
          srcNodeGrp->done_count[i] = 0;
          srcNodeGrp->flush_count = 0;
          srcNodeGrp->get_idx[i] = 0;
        }
      }
    }
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
          int destNode = i;
          int sz = 0;
          for (int k = 0; k < CkNodeSize(0); k++) {
            std::vector<itemT> localMsg =
                localBuffers[destNode * CkNodeSize(0) + k];
            std::copy(localMsg.begin(), localMsg.end(), &(destMsg->items()[sz]));
            sz += localMsg.size();
            localBuffers[destNode * CkNodeSize(0) + k].clear();
          }
          trim(destMsg);
          nodeGrpProxy[i].receive_no_sort(destMsg);
          msgBuffers[i] = newHTramMessage(bufSize);
        } else if (agg == WPs) {
#ifdef BUCKETS_BY_DEST
          // These items are leaving; they are no longer held for this
          // destination. Every other send site decrements, and omitting it
          // here makes the counter drift upward permanently.
          updates_in_tram[i] -= destMsg->next;
#endif
          trim(destMsg);
          nodeGrpProxy[i].receive(destMsg);
          msgBuffers[i] = newHTramMessage(bufSize);
        } else if (agg == WW) {
#ifdef BUCKETS_BY_DEST
          updates_in_tram[i] -= destMsg->next;
#endif
          trim(destMsg);
          thisProxy[i].receiveOnPE(destMsg);
          msgBuffers[i] = newHTramMessage(bufSize);
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
        for (int i = 0; i <= tram_threshold; i++) {
          while (!tram_hold[dest_node][i].empty()) {
            datatype item = tram_hold[dest_node][i].front();
            int dest_proc = get_dest_proc(objPtr, item);
            tram_hold[dest_node][i].pop();
            destMsg->items()[destMsg->next].payload = item;
            destMsg->items()[destMsg->next].destPe = dest_proc;
            destMsg->next++;
            if (destMsg->next == bufSize) {
              tot_send_count += destMsg->next;
              updates_in_tram[dest_node] -= destMsg->next;
              trim(destMsg);
              if (agg == WW)
                thisProxy[dest_node].receiveOnPE(destMsg);
              else
                nodeGrpProxy[dest_node].receive(destMsg);
              msgBuffers[dest_node] = newHTramMessage(bufSize);
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
          for (int i = tram_threshold + 1; i < histo_bucket_count; i++) {
            while (!tram_hold[node][i].empty()) {
              datatype item = tram_hold[node][i].front();
              tram_hold[node][i].pop();
              int dest_proc = get_dest_proc(objPtr, item);
              destMsg->items()[destMsg->next].payload = item;
              destMsg->items()[destMsg->next].destPe = dest_proc;
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
          thisProxy[node].receiveOnPE(destMsg);
        else
          nodeGrpProxy[node].receive(destMsg);
        msgBuffers[node] = newHTramMessage(bufSize);
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
    thisProxy[dest].receiveOnPE(m);
  else
    nodeGrpProxy[dest].receive(m);
  msgBuffers[dest] = newHTramMessage(bufSize);
}

// Combining on: while a full buffer's worth of admitted items is held for
// dest, fill a buffer from the lowest buckets and ship it. Items leave the
// hold only here or in a flush, so everything still waiting can be folded.
void HTram::releaseFull(int dest) {
  while (updates_in_tram[dest] >= selectivity * bufSize) {
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
  for (int i = 0; i <= tram_threshold; i++) {
    while (!tram_hold[dest][i].empty()) {
      datatype item = tram_hold[dest][i].front();
      tram_hold[dest][i].pop();
      destMsg->items()[destMsg->next].payload = item;
      destMsg->items()[destMsg->next].destPe = get_dest_proc(objPtr, item);
      destMsg->next++;
      if (destMsg->next == bufSize) {
        tot_send_count += destMsg->next;
        updates_in_tram[dest] -= destMsg->next;
        trim(destMsg);
        if (agg == WW)
          thisProxy[dest].receiveOnPE(destMsg);
        else
          nodeGrpProxy[dest].receive(destMsg);
        msgBuffers[dest] = newHTramMessage(bufSize);
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
    for (int i = tram_threshold + 1;
         i < histo_bucket_count && destMsg->next < bufSize / 2; i++) {
      while (!tram_hold[dest][i].empty() && destMsg->next < bufSize / 2) {
        datatype item = tram_hold[dest][i].front();
        tram_hold[dest][i].pop();
        destMsg->items()[destMsg->next].payload = item;
        destMsg->items()[destMsg->next].destPe = get_dest_proc(objPtr, item);
        destMsg->next++;
      }
    }
#endif
    tot_send_count += destMsg->next;
    trim(destMsg);
    if (agg == WW)
      thisProxy[dest].receiveOnPE(destMsg);
    else
      nodeGrpProxy[dest].receive(destMsg);
    msgBuffers[dest] = newHTramMessage(bufSize);
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
  // No per-destination bookkeeping in the other modes, so the best available
  // answer is the whole-library flush.
  tflush();
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
  for (int i = 0; i < CkNumNodes(); i++) {
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

HTramRecv::HTramRecv() { msg_stats[MIN_LATENCY] = 100.0; }

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
  while (llimit < msg->next && msg->items()[llimit].destPe < pe)
    llimit++;
  int ulimit = llimit;
  while (ulimit < msg->next && msg->items()[ulimit].destPe == pe)
    ulimit++;
  int count = ulimit - llimit;
  if (!ret_list) {
    for (int i = llimit; i < ulimit; i++)
      cb(objPtr, msg->items()[i].payload);
  } else {
    datatype *buf = new datatype[count];
    for (int i = 0; i < count; i++)
      buf[i] = msg->items()[llimit + i].payload;
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
      cb(objPtr, msg->items()[i].payload);
  } else {
    datatype *buf = new datatype[msg->next];
    for (int i = 0; i < msg->next; i++)
      buf[i] = msg->items()[i].payload;
    cb_retarr(objPtr, buf, msg->next);
  }
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
    buf[i] = agg_message->items()[i].payload;
  cb_retarr(objPtr, buf, agg_message->next);
  delete agg_message;
#else
  // Not used in GRAPH/BUCKETS_BY_DEST builds; entry must exist for def.h linkage.
  delete agg_message;
#endif
}

void HTramRecv::receive(HTramMessage *agg_message) {
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

  for (int i = 0; i < agg_message->next; i++) {
    int rank = agg_message->items()[i].destPe - rank0PE;
    sizes[rank]++;
  }

  sorted_agg_message->offset[0] = 0;
  for (int i = 1; i < CkNodeSize(CkMyNode()); i++)
    sorted_agg_message->offset[i] =
        sorted_agg_message->offset[i - 1] + sizes[i - 1];

  for (int i = 0; i < agg_message->next; i++) {
    int rank = agg_message->items()[i].destPe - rank0PE;
    sorted_agg_message->items()[sorted_agg_message->offset[rank]++] =
        agg_message->items()[i].payload;
  }
  delete agg_message;

  sorted_agg_message->offset[0] = sizes[0];
  for (int i = 1; i < CkNodeSize(CkMyNode()); i++)
    sorted_agg_message->offset[i] =
        sorted_agg_message->offset[i - 1] + sizes[i];

  for (int i = CkNodeFirst(CkMyNode());
       i < CkNodeFirst(CkMyNode()) + CkNodeSize(CkMyNode()); i++) {
    HTramNodeMessage *tmpMsg =
        (HTramNodeMessage *)CkReferenceMsg(sorted_agg_message);
    _SET_USED(UsrToEnv(tmpMsg), 0);
    tram_proxy[i].receivePerPE(tmpMsg);
  }
  CkFreeMsg(sorted_agg_message);
}

void HTramRecv::receive_small(HTramLocalMessage *agg_message) {
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
    int rank = agg_message->items()[i].destPe - rank0PE;
    sizes[rank]++;
  }

  sorted_agg_message->offset[0] = 0;
  for (int i = 1; i < CkNodeSize(CkMyNode()); i++)
    sorted_agg_message->offset[i] =
        sorted_agg_message->offset[i - 1] + sizes[i - 1];

  for (int i = 0; i < agg_message->next; i++) {
    int rank = agg_message->items()[i].destPe - rank0PE;
    sorted_agg_message->items()[sorted_agg_message->offset[rank]++] =
        agg_message->items()[i].payload;
  }
  delete agg_message;

  sorted_agg_message->offset[0] = sizes[0];
  for (int i = 1; i < CkNodeSize(CkMyNode()); i++)
    sorted_agg_message->offset[i] =
        sorted_agg_message->offset[i - 1] + sizes[i];

  for (int i = CkNodeFirst(CkMyNode());
       i < CkNodeFirst(CkMyNode()) + CkNodeSize(CkMyNode()); i++) {
    HTramNodeMessage *tmpMsg =
        (HTramNodeMessage *)CkReferenceMsg(sorted_agg_message);
    _SET_USED(UsrToEnv(tmpMsg), 0);
    tram_proxy[i].receivePerPE(tmpMsg);
  }
  CkFreeMsg(sorted_agg_message);
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