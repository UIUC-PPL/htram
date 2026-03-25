#include "htram_group.h"
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
  bufSize = BUFSIZE; //set bufSize to BUFSIZE by default

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
  tram_hold = new std::queue<datatype> *[CkNumPes()];
  updates_in_tram = new int[CkNumPes()];
  for (int i = 0; i < CkNumPes(); i++) {
    tram_hold[i] = new std::queue<datatype>[histo_bucket_count];
    updates_in_tram[i] = 0;
  }
#else
  nodesize = 0;
  nodeOf = nullptr;
  agg = WW;
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

  localMsgBuffer = new HTramMessage();
  for (int i = 0; i < CkNumPes(); i++)
    msgBuffers[i] = new HTramMessage();

  localBuffers = new std::vector<itemT>[CkNumPes()];

  local_buf = new HTramLocalMessage *[CkNumNodes()];
  for (int i = 0; i < CkNumNodes(); i++) {
    local_buf[i] = new HTramLocalMessage();
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

void HTram::setBufferSize(int new_size) {
  CkAssert(new_size > 0 && new_size <= BUFSIZE);
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
  int buf_count = CkNumNodes();
  if (agg == WW)
    buf_count = CkNumPes();
  for (int i = 0; i < buf_count; i++)
    msgBuffers[i] = new HTramMessage();
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
  localMsgBuffer = new HTramMessage();
#ifndef NODE_SRC_BUFFER
  msgBuffers = new HTramMessage *[CkNumNodes()];
  for (int i = 0; i < CkNumNodes(); i++)
    msgBuffers[i] = new HTramMessage();
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

void HTram::shareArrayOfBuckets(std::vector<datatype> *new_tram_hold,
                                int bucket_count) {
  histo_bucket_count = bucket_count;
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
  if (_newtramThreshold > tram_threshold) {
    for (int k = 0; k < num_dest; k++)
      for (int i = tram_threshold + 1; i <= _newtramThreshold; i++)
        updates_in_tram[k] += tram_hold[k][i].size();
  } else if (tram_threshold > _newtramThreshold) {
    for (int k = 0; k < num_dest; k++)
      for (int i = tram_threshold; i > _newtramThreshold; i--)
        updates_in_tram[k] -= tram_hold[k][i].size();
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
    if (updates_in_tram[dest_node] > selectivity * bufSize)
      insertBucketsByDest(tram_threshold, dest_node);
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

#ifdef BUCKETS_BY_DEST
void HTram::insertBucketsByDest(int high, int dest_node) {
  HTramMessage *destMsg = msgBuffers[dest_node];
  for (int i = 0; i <= high; i++) {
    while (!tram_hold[dest_node][i].empty()) {
      datatype item = tram_hold[dest_node][i].front();
      tram_hold[dest_node][i].pop();
      destMsg->buffer[destMsg->next].payload = item;
      int dest_proc = get_dest_proc(objPtr, item);
      destMsg->buffer[destMsg->next].destPe = dest_proc;
      destMsg->next++;
      if (destMsg->next == bufSize) {
        tot_send_count += destMsg->next;
        updates_in_tram[dest_node] -= destMsg->next;
        if (agg == WW)
          thisProxy[dest_node].receiveOnPE(destMsg);
        else
          nodeGrpProxy[dest_node].receive(destMsg);
        msgBuffers[dest_node] = new HTramMessage();
        destMsg = msgBuffers[dest_node];
      }
      if (updates_in_tram[dest_node] < selectivity * bufSize)
        break;
    }
  }
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
      destMsg->buffer[destMsg->next].payload = item;
      destMsg->buffer[destMsg->next].destPe = dest_proc;
      destMsg->next++;
      if (destMsg->next == bufSize) {
        tot_send_count += destMsg->next;
        updates_in_tram_count -= destMsg->next;
        nodeGrpProxy[dest_node].receive(destMsg);
        msgBuffers[dest_node] = new HTramMessage();
      }
      if (updates_in_tram_count < selectivity * bufSize * num_nodes)
        break;
    }
  }
  tram_done(objPtr);
}
#endif

void HTram::insertValueWPs(datatype value, int dest_pe) {
  int destNode = dest_pe / nodesize;
  if (agg == WW)
    destNode = dest_pe;
  HTramMessage *destMsg = msgBuffers[destNode];
  destMsg->buffer[destMsg->next].payload = value;
  destMsg->buffer[destMsg->next].destPe = dest_pe;
  destMsg->next++;
  if (destMsg->next == bufSize) {
    agg_msg_count++;
    tot_send_count += destMsg->next;
#ifdef BUCKETS_BY_DEST
    updates_in_tram[destNode] -= destMsg->next;
#else
    updates_in_tram_count -= destMsg->next;
#endif
    if (agg == WW)
      thisProxy[destNode].receiveOnPE(destMsg);
    else
      nodeGrpProxy[destNode].receive(destMsg);
    msgBuffers[destNode] = new HTramMessage();
  }
}

// one per node, message, fixed
// Client inserts
void HTram::insertToProcess(datatype value, int destNode) {
  HTramMessage *destMsg = msgBuffers[destNode];
  destMsg->buffer[destMsg->next].payload = value;
  destMsg->next++;
  if (destMsg->next == bufSize) {
    nodeGrpProxy[destNode].receiveOnProc(destMsg);
    msgBuffers[destNode] = new HTramMessage();
  }
}

void HTram::insertValue(datatype value, int dest_pe) {
  int destNode = dest_pe / CkNodeSize(0);

  if (agg == PP) {
    int increment = 1;
    int idx_dnode = local_idx[destNode];
    if (idx_dnode <= LOCAL_BUFSIZE - 1) {
      local_buf[destNode]->buffer[idx_dnode].payload = value;
      local_buf[destNode]->buffer[idx_dnode].destPe = dest_pe;
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
      destMsg->buffer[destMsg->next].payload = value;
    } else {
      destMsg->buffer[destMsg->next].payload = value;
      destMsg->buffer[destMsg->next].destPe = dest_pe;
    }

    destMsg->next++;
    if (destMsg->next == bufSize) {
      agg_msg_count++;
      if (agg == WsP) {
        int sz = 0;
        for (int i = 0; i < CkNodeSize(0); i++) {
          std::vector<itemT> localMsg =
              localBuffers[destNode * CkNodeSize(0) + i];
          std::copy(localMsg.begin(), localMsg.end(), &(destMsg->buffer[sz]));
          sz += localMsg.size();
          localBuffers[destNode * CkNodeSize(0) + i].clear();
        }
      }
      if (agg == WW) {
#ifdef BUCKETS_BY_DEST
        updates_in_tram[dest_pe] -= destMsg->next;
#endif
        thisProxy[dest_pe].receiveOnPE(destMsg);
        msgBuffers[dest_pe] = new HTramMessage();
      } else if (agg == WsP) {
        nodeGrpProxy[destNode].receive_no_sort(destMsg);
        msgBuffers[destNode] = new HTramMessage();
      } else {
        tot_send_count += destMsg->next;
#ifdef BUCKETS_BY_DEST
        updates_in_tram[destNode] -= destMsg->next;
#else
        updates_in_tram_count -= destMsg->next;
#endif
        nodeGrpProxy[destNode].receive(destMsg);
        msgBuffers[destNode] = new HTramMessage();
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
    srcNodeGrp->msgBuffers[destnode]->buffer[idx + i].payload =
        local_buf[destnode]->buffer[i].payload;
    srcNodeGrp->msgBuffers[destnode]->buffer[idx + i].destPe =
        local_buf[destnode]->buffer[i].destPe;
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
    nodeGrpProxy[destnode].receive(srcNodeGrp->msgBuffers[destnode]);
    srcNodeGrp->msgBuffers[destnode] = new HTramMessage();
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
  if (agg == PP) {
    int flush_count =
        srcNodeGrp->flush_count.fetch_add(1, std::memory_order_seq_cst);
    for (int i = 0; i < CkNumNodes(); i++) {
      local_buf[i]->next = local_idx[i];
      nodeGrpProxy[i].receive_small(local_buf[i]);
      local_buf[i] = new HTramLocalMessage();
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
          ((envelope *)UsrToEnv(srcNodeGrp->msgBuffers[i]))
              ->setUsersize(sizeof(int) +
                            sizeof(itemT) * srcNodeGrp->msgBuffers[i]->next);
          nodeGrpProxy[i].receive(srcNodeGrp->msgBuffers[i]);
          srcNodeGrp->msgBuffers[i] = new HTramMessage();
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
            std::copy(localMsg.begin(), localMsg.end(), &(destMsg->buffer[sz]));
            sz += localMsg.size();
            localBuffers[destNode * CkNodeSize(0) + k].clear();
          }
          nodeGrpProxy[i].receive_no_sort(destMsg);
          msgBuffers[i] = new HTramMessage();
        } else if (agg == WPs) {
          ((envelope *)UsrToEnv(destMsg))->setUsersize(
              sizeof(int) + sizeof(itemT) * (destMsg->next));
          nodeGrpProxy[i].receive(destMsg);
          msgBuffers[i] = new HTramMessage();
        } else if (agg == WW) {
          ((envelope *)UsrToEnv(destMsg))->setUsersize(
              sizeof(int) + sizeof(envelope) + sizeof(itemT) * destMsg->next);
          thisProxy[i].receiveOnPE(destMsg);
          msgBuffers[i] = new HTramMessage();
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
            destMsg->buffer[destMsg->next].payload = item;
            destMsg->buffer[destMsg->next].destPe = dest_proc;
            destMsg->next++;
            if (destMsg->next == bufSize) {
              tot_send_count += destMsg->next;
              updates_in_tram[dest_node] -= destMsg->next;
              if (agg == WW)
                thisProxy[dest_node].receiveOnPE(destMsg);
              else
                nodeGrpProxy[dest_node].receive(destMsg);
              msgBuffers[dest_node] = new HTramMessage();
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
              destMsg->buffer[destMsg->next].payload = item;
              destMsg->buffer[destMsg->next].destPe = dest_proc;
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
        ((envelope *)UsrToEnv(destMsg))
            ->setUsersize(sizeof(int) + sizeof(itemT) * (destMsg->next));
        if (agg == WW)
          thisProxy[node].receiveOnPE(destMsg);
        else
          nodeGrpProxy[node].receive(destMsg);
        msgBuffers[node] = new HTramMessage();
      }
  }
  tram_done(objPtr);
#endif
}

void HTram::flush_everything() {
  tflush();
}

HTramNodeGrp::HTramNodeGrp() {
  msgBuffers = new HTramMessage *[CkNumNodes()];
  for (int i = 0; i < CkNumNodes(); i++) {
    msgBuffers[i] = new HTramMessage();
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
  for (int i = CkNodeFirst(CkMyNode());
       i < CkNodeFirst(CkMyNode()) + CkNodeSize(CkMyNode()); i++) {
    HTramMessage *tmpMsg = (HTramMessage *)CkReferenceMsg(agg_message);
    _SET_USED(UsrToEnv(tmpMsg), 0);
    tram_proxy[i].receivePerPE(tmpMsg);
  }
  CkFreeMsg(agg_message);
}

void HTram::receivePerPE(HTramMessage *msg) {
  int llimit = 0;
  int rank = CkMyRank();
  if (rank > 0)
    llimit = 0;
  int ulimit = 0;
  for (int i = llimit; i < ulimit; i++) {
    cb(objPtr, msg->buffer[i].payload);
  }
  CkFreeMsg(msg);
}

void HTram::receiveOnPE(HTramMessage *msg) {
  if (!ret_list) {
    for (int i = 0; i < msg->next; i++)
      cb(objPtr, msg->buffer[i].payload);
  } else {
    datatype *buf = new datatype[msg->next];
    for (int i = 0; i < msg->next; i++)
      buf[i] = msg->buffer[i].payload;
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
#ifndef BUCKETS_BY_DEST
  datatype *buf = new datatype[agg_message->next];
  for (int i = 0; i < agg_message->next; i++)
    buf[i] = agg_message->buffer[i].payload;
  cb_retarr(objPtr, buf, agg_message->next);
  delete agg_message;
#else
  // Not used in GRAPH/BUCKETS_BY_DEST builds; entry must exist for def.h linkage.
  delete agg_message;
#endif
}

void HTramRecv::receive(HTramMessage *agg_message) {
  int rank0PE = CkNodeFirst(thisIndex);
  HTramNodeMessage *sorted_agg_message = new HTramNodeMessage();

  std::vector<int> sizes(CkNodeSize(CkMyNode()), 0);

  for (int i = 0; i < agg_message->next; i++) {
    int rank = agg_message->buffer[i].destPe - rank0PE;
    sizes[rank]++;
  }

  sorted_agg_message->offset[0] = 0;
  for (int i = 1; i < CkNodeSize(CkMyNode()); i++)
    sorted_agg_message->offset[i] =
        sorted_agg_message->offset[i - 1] + sizes[i - 1];

  for (int i = 0; i < agg_message->next; i++) {
    int rank = agg_message->buffer[i].destPe - rank0PE;
    sorted_agg_message->buffer[sorted_agg_message->offset[rank]++] =
        agg_message->buffer[i].payload;
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
  int rank0PE = CkNodeFirst(thisIndex);
  HTramNodeMessage *sorted_agg_message = new HTramNodeMessage();

  std::vector<int> sizes(CkNodeSize(CkMyNode()), 0);

  for (int i = 0; i < agg_message->next; i++) {
    int rank = agg_message->buffer[i].destPe - rank0PE;
    sizes[rank]++;
  }

  sorted_agg_message->offset[0] = 0;
  for (int i = 1; i < CkNodeSize(CkMyNode()); i++)
    sorted_agg_message->offset[i] =
        sorted_agg_message->offset[i - 1] + sizes[i - 1];

  for (int i = 0; i < agg_message->next; i++) {
    int rank = agg_message->buffer[i].destPe - rank0PE;
    sorted_agg_message->buffer[sorted_agg_message->offset[rank]++] =
        agg_message->buffer[i].payload;
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
      cb(objPtr, msg->buffer[i]);
  } else
    cb_retarr(objPtr, &msg->buffer[llimit], ulimit - llimit);
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