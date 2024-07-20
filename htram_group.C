#include "htram_group.h"
//#define DEBUG 1

CkReductionMsg* msgStatsCollection(int nMsg, CkReductionMsg** rdmsgs) {
  double *msg_stats;
  msg_stats = (double*)rdmsgs[0]->getData();
//  CkPrintf("\nInside reducer: %lf, %lf, %lf, %lf", msg_stats[0], msg_stats[1], msg_stats[2], msg_stats[3]);
  for (int i = 1; i < nMsg; i++) {
    CkAssert(rdmsgs[i]->getSize() == STATS_COUNT*sizeof(double));
    if (rdmsgs[i]->getSize() != STATS_COUNT*sizeof(double)) {
      CkPrintf("Error!!! Reduction not correct. Msg size is %d\n",
          rdmsgs[i]->getSize());
      CkAbort("Incorrect Reduction size in MetaBalancer\n");
    }
    
    double* m = (double *)rdmsgs[i]->getData();
    msg_stats[TOTAL_LATENCY] += m[TOTAL_LATENCY];
    msg_stats[MAX_LATENCY] = max(m[MAX_LATENCY], msg_stats[MAX_LATENCY]);
    msg_stats[MIN_LATENCY] = min(m[MIN_LATENCY], msg_stats[MIN_LATENCY]);
    msg_stats[TOTAL_MSGS] += m[TOTAL_MSGS];
  }
  return CkReductionMsg::buildNew(rdmsgs[0]->getSize(), NULL, rdmsgs[0]->getReducer(), rdmsgs[0]);
}

/*global*/ CkReduction::reducerType msgStatsCollectionType;
/*initnode*/ void registerMsgStatsCollection(void) {
  msgStatsCollectionType = CkReduction::addReducer(msgStatsCollection, true, "msgStatsCollection");
}

void periodic_tflush(void *htram_obj, double time);

HTram::HTram(CkGroupID recv_ngid, CkGroupID src_ngid, int buffer_size, bool enable_buffer_flushing, double time_in_ms, bool ret_item, bool req, CkCallback start_cb) {
  request = req;
  // TODO: Implement variable buffer sizes and timed buffer flushing
  flush_time = time_in_ms;
//  client_gid = cgid;
  enable_flush = enable_buffer_flushing;
  msg_stats[MIN_LATENCY] = 100.0;
  agg_msg_count = 0;
  flush_msg_count = 0;
  local_recv_count = 0;
//  if(thisIndex==0) CkPrintf("\nbuf_type = %d, type %d,%d,%d,%d", buf_type, use_src_grouping, use_src_agg, use_per_destpe_agg, use_per_destnode_agg);
/*
  if(use_per_destnode_agg)
    if(thisIndex==0) CkPrintf("\nDest-node side grouping/sorting enabled (1 buffer per src-pe, per dest-node)\n");
*/
  ret_list = !ret_item;
  agg = PNs;//NNs;//PP;
  myPE = CkMyPe();
  msgBuffers = (new HTramMessage*[CkNumPes()]);

  if(thisIndex == 0) {
    if(agg == PNs) CkPrintf("\nAggregation type: PNs with buffer size %d", BUFSIZE);
    else if(agg == NNs) CkPrintf("\nAggregation type: NNs with buffer size %d",BUFSIZE);
    else if(agg == PP) CkPrintf("\nAggregation type: PP with buffer size %d", BUFSIZE);
    else if(agg = PsN)  CkPrintf("\nAggregation type: PsN with buffer size %d", BUFSIZE);
  }

  localMsgBuffer = new HTramMessage();

  for(int i=0;i<CkNumPes();i++)
    msgBuffers[i] = new HTramMessage();


  localBuffers = new std::vector<itemT>[CkNumPes()];

  local_buf = new HTramLocalMessage*[CkNumNodes()];
  for(int i=0;i<CkNumNodes();i++)
  {
    local_buf[i] = new HTramLocalMessage();
    local_idx[i] = 0;
  }

  nodeGrpProxy = CProxy_HTramRecv(recv_ngid);
  srcNodeGrpProxy = CProxy_HTramNodeGrp(src_ngid);

  srcNodeGrp = (HTramNodeGrp*)srcNodeGrpProxy.ckLocalBranch();
  nodeGrp = (HTramRecv*)nodeGrpProxy.ckLocalBranch();

  CkGroupID my_gid = ckGetGroupID();
//  CkPrintf("\nmy_gid = %d", my_gid);
//  if(thisIndex==0)
  nodeGrp->setTramProxy(my_gid);

  if(enable_flush)
    periodic_tflush((void *) this, flush_time);
#ifdef IDLE_FLUSH
  CkCallWhenIdle(CkIndex_HTram::idleFlush(), this);
#endif
  contribute(start_cb);
}

bool HTram::idleFlush() {
//    if(thisIndex==0) CkPrintf("\nCalling idleflush");
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
  if(agg == PP) buf_count = CkNumPes();
  for(int i=0;i<buf_count;i++)
    msgBuffers[i] = new HTramMessage();

  //if(thisIndex==0) CkPrintf("\nbuf_type = %d, size = %d, type = %d", buf_type, BUFSIZE, agg);
}

void HTram::avgLatency(CkCallback cb){
  return_cb = cb;
  msg_stats[TOTAL_LATENCY] /= (2*msg_stats[TOTAL_MSGS]);
//  CkPrintf("\n%lf, %lf, %lf, %lf", msg_stats[0], msg_stats[1], msg_stats[2], msg_stats[3]);
//  double avg = total_latency/total_msg_count;
//  CkCallback cb(CkReductionTarget(MetaBalancer, ReceiveMinStats),
//        thisProxy[0]);
  contribute(STATS_COUNT*sizeof(double), msg_stats, msgStatsCollectionType, cb);
//  CkPrintf("\navg = %lf", avg);
//  contribute(sizeof(double), &avg, CkReduction::sum_double, CkCallback(CkReductionTarget(HTram, printAvgLatency), thisProxy));
}

void HTramRecv::avgLatency(CkCallback cb){
  return_cb = cb;
  msg_stats[TOTAL_LATENCY] /= (2*msg_stats[TOTAL_MSGS]);
//  CkPrintf("\n%lf, %lf, %lf, %lf", msg_stats[0], msg_stats[1], msg_stats[2], msg_stats[3]);
//  double avg = total_latency/total_msg_count;
//  contribute(sizeof(double), &avg, CkReduction::sum_double, CkCallback(CkReductionTarget(HTramRecv, printAvgLatency), thisProxy));
  contribute(STATS_COUNT*sizeof(double), msg_stats, msgStatsCollectionType, cb);
}

HTram::HTram(CkGroupID cgid, CkCallback ecb){
  client_gid = cgid;
//  cb = delivercb;
  endCb = ecb;
  myPE = CkMyPe();
  localMsgBuffer = new HTramMessage();
#ifndef NODE_SRC_BUFFER
  msgBuffers = new HTramMessage*[CkNumNodes()];
  for(int i=0;i<CkNumNodes();i++)
    msgBuffers[i] = new HTramMessage();
#endif
}

void HTram::set_func_ptr(void (*func)(void*, datatype), void* obPtr) {
  cb = func;
  objPtr = obPtr;
}

void HTram::set_func_ptr_retarr(void (*func)(void*, datatype*, int), void* obPtr) {
  cb_retarr = func;
  objPtr = obPtr;
}

HTram::HTram(CkMigrateMessage* msg) {}

//one per node, message, fixed 
//Client inserts
void HTram::insertValue(datatype value, int dest_pe) {
//  CkPrintf("\nInserting on PE-%d", dest_pe);
  int destNode = dest_pe/CkNodeSize(0); //find safer way to find dest node,
  // node size is not always same
  
  if(agg == NNs) {
    int increment = 1;
    int idx = -1;
    int idx_dnode = local_idx[destNode];
    if(idx_dnode<=LOCAL_BUFSIZE-1) {
      local_buf[destNode]->buffer[idx_dnode].payload = value;
      local_buf[destNode]->buffer[idx_dnode].destPe = dest_pe;
      local_idx[destNode]++;
    }
    bool local_buf_full = false;
    if(local_idx[destNode] == LOCAL_BUFSIZE)
      local_buf_full = true;
    increment = LOCAL_BUFSIZE;
    int done_idx = -1;
    if(local_buf_full) {
  //    CkPrintf("\n[PE-%d]Copying for dest node %d, size = %d", CkMyPe(), destNode, increment);
      copyToNodeBuf(destNode, increment);
    }
  }
  else {
    HTramMessage *destMsg = msgBuffers[destNode];
    if(agg == PP)
      destMsg = msgBuffers[dest_pe];

    if(agg == PsN) {
      itemT itm = {dest_pe, value};
      localBuffers[dest_pe].push_back(itm);
    } else if (agg == PP) {//change msg type to not include destPE
      destMsg->buffer[destMsg->next].payload = value;
    } else {
      destMsg->buffer[destMsg->next].payload = value;
      destMsg->buffer[destMsg->next].destPe = dest_pe;
    }
#if 0
    if(*(destMsg->next) == 0 || *(destMsg->next) == BUFSIZE-1) {
      if(*(destMsg->next) == 0)
        destMsg->getTimer()[0] = CkWallTimer();
      else
        destMsg->getTimer()[1] = CkWallTimer();
    }
#endif

    destMsg->next++;
    if(destMsg->next == BUFSIZE) {
       agg_msg_count++;
      if(agg == PsN) {
        int sz = 0;
        for(int i=0;i<CkNodeSize(0);i++) {
          std::vector<itemT> localMsg = localBuffers[destNode*CkNodeSize(0)+i];
          std::copy(localMsg.begin(), localMsg.end(), &(destMsg->buffer[sz]));
          sz += localMsg.size();
//          destMsg->getIndex()[i] = sz;
          localBuffers[destNode*CkNodeSize(0)+i].clear();
        }
      }
//      ((envelope *)UsrToEnv(destMsg))->setUsersize(0);//destMsg->next-20)*4+32);
      int dest_idx = dest_pe;
      if(agg == PP) {
//        CkPrintf("\nmsg size = %d", *destMsg->next);
        thisProxy[dest_pe].receiveOnPE(destMsg);
      } else {
          dest_idx = destNode;
        if(agg == PsN) {
          nodeGrpProxy[destNode].receive_no_sort(destMsg);
//          nodeGrpProxy[destNode].receive_no_sort(destMsg);
        } else {
//          if(request) CkPrintf("\nSending msg from requesting htram");
//          else CkPrintf("\nSending msg from responding htram");
          nodeGrpProxy[destNode].receive(destMsg);
        }
      }
      msgBuffers[dest_idx] = new HTramMessage();
    }
  }
}

void HTram::registercb() {
  CcdCallFnAfter(periodic_tflush, (void *) this, flush_time);
}

void HTram::copyToNodeBuf(int destnode, int increment) {

// Get atomic index
  int idx = srcNodeGrp->get_idx[destnode].fetch_add(increment, std::memory_order_relaxed);
  while(idx >= BUFSIZE) {
    idx = srcNodeGrp->get_idx[destnode].fetch_add(increment, std::memory_order_relaxed);
  }
#if 0
  if(idx==0 || idx == BUFSIZE-increment)
    srcNodeGrp->msgBuffers[destnode]->getTimer()[idx/(BUFSIZE-increment)] = CkWallTimer();
#endif
// Copy data into node buffer from PE-local buffer
  int i;
  for(i=0;i<increment;i++) {
    srcNodeGrp->msgBuffers[destnode]->buffer[idx+i].payload = local_buf[destnode]->buffer[i].payload;
    srcNodeGrp->msgBuffers[destnode]->buffer[idx+i].destPe = local_buf[destnode]->buffer[i].destPe;
  }

  int done_count = srcNodeGrp->done_count[destnode].fetch_add(increment, std::memory_order_relaxed);
  if(done_count+increment == BUFSIZE) {
    agg_msg_count++;
    srcNodeGrp->msgBuffers[destnode]->next = BUFSIZE;
    
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

//Assume to be called only on PE-0
void HTram::global_flush(CkCallback cb) {
  gb_flush_cb = cb;
  for(int i=0;i<CkNumPes();i++)
    thisProxy[i].trackflush();
}

void HTram::trackflush() {
  track_count = true;
  tflush();
}

//Reduction method called on PE-0
void HTram::getTotSends(int s_total) {
  sends = s_total;
  CkPrintf("\nTotal sends = %d", sends);
  thisProxy.getRecvCount();
}

void HTram::getRecvCount() {
  CkCallback _cb(CkReductionTarget(HTram, checkCounts), thisProxy[0]);
  contribute(sizeof(int), &local_recv_count, CkReduction::sum_int, _cb);
}

void HTram::resetCounts(){
  local_sends = 0;
  local_recv_count = 0;
  if(thisIndex == 0) sends = 0;
//  contribute(gb_flush_cb);
}

void HTram::checkCounts(int received) {
//  CkPrintf("\nSends = %d, recvs = %d", sends, received);
  if(sends != received) thisProxy.getRecvCount();
  else thisProxy.resetCounts();
}

void HTram::tflush(bool idleflush, double fraction) {
//    CkPrintf("\nCalling flush on PE-%d", thisIndex); fflush(stdout);
  if(agg == NNs) {
    int flush_count = srcNodeGrp->flush_count.fetch_add(1, std::memory_order_seq_cst);
    //Send your local buffer
    for(int i=0;i<CkNumNodes();i++) {
      local_buf[i]->next = local_idx[i];
      nodeGrpProxy[i].receive_small(local_buf[i]);
      local_buf[i] = new HTramLocalMessage();
      local_idx[i] = 0;
    }
    //If you're last rank on node to flush, then flush your buffer and by setting idx to a high count, node level buffer as well
//    if(flush_count+1==CkNodeSize(0))
    {
      for(int i=0;i<CkNumNodes();i++) {
        if((!idleflush && srcNodeGrp->done_count[i]) || (idleflush && srcNodeGrp->done_count[i] > BUFSIZE*fraction)) {
//        if(srcNodeGrp->done_count[i]) {
          flush_msg_count++;

          int idx = srcNodeGrp->get_idx[i].fetch_add(BUFSIZE, std::memory_order_relaxed);
          int done_count = srcNodeGrp->done_count[i].fetch_add(0, std::memory_order_relaxed);
          if(idx >= BUFSIZE) continue;
          while(idx!=done_count) { done_count = srcNodeGrp->done_count[i].fetch_add(0, std::memory_order_relaxed);}

          srcNodeGrp->msgBuffers[i]->next = srcNodeGrp->done_count[i];
          ((envelope *)UsrToEnv(srcNodeGrp->msgBuffers[i]))->setUsersize(sizeof(int)*2+sizeof(envelope)+sizeof(itemT)*srcNodeGrp->msgBuffers[i]->next);
 /* 
          CkPrintf("\n[PE-%d]TF-Sending out data with size = %d", thisIndex, srcNodeGrp->msgBuffers[i]->next);
          for(int j=0;j<srcNodeGrp->msgBuffers[i]->next;j++)
            CkPrintf("\nTFvalue=%d, pe=%d", srcNodeGrp->msgBuffers[i]->buffer[j].payload, srcNodeGrp->msgBuffers[i]->buffer[j].destPe);
  */
//          *(srcNodeGrp->msgBuffers[i]->getDoTimer()) = 0;
          nodeGrpProxy[i].receive(srcNodeGrp->msgBuffers[i]);
//          srcNodeGrp->msgBuffers[i] = new HTramMessageSmall();//new HTramMessage();//localMsgBuffer;
          srcNodeGrp->msgBuffers[i] = new HTramMessage();
          srcNodeGrp->done_count[i] = 0;
          srcNodeGrp->flush_count = 0;
          srcNodeGrp->get_idx[i] = 0;
        }
//          localMsgBuffer = new HTramMessage();
      }
    }
  }
  else {
    int buf_count = CkNumNodes();
    if(agg == PP) buf_count = CkNumPes();

    for(int i=0;i<buf_count;i++) {
//      if(msgBuffers[i]->next)
//#ifdef IDLE_FLUSH
      if((!idleflush && msgBuffers[i]->next) || (idleflush && msgBuffers[i]->next > BUFSIZE*fraction))
//#else
//      if(!idleflush && msgBuffers[i]->next)
//#endif
      {
        flush_msg_count++;
//        if(idleflush) CkPrintf("\n[PE-%d] flushing buf[%d] at %d", CkMyPe(), i,  msgBuffers[i]->next);
//        else CkPrintf("\nReg[PE-%d] flushing buf[%d] at %d", CkMyPe(), i, msgBuffers[i]->next);
        HTramMessage *destMsg = msgBuffers[i];
//        *destMsg->getDoTimer() = 0;
        if(agg == PsN) {
          int destNode = i;
          int sz = 0;
          for(int k=0;k<CkNodeSize(0);k++) {
            std::vector<itemT> localMsg = localBuffers[destNode*CkNodeSize(0)+k];
            std::copy(localMsg.begin(), localMsg.end(), &(destMsg->buffer[sz]));
            sz += localMsg.size();
//            destMsg->getIndex()[k] = sz;
            localBuffers[destNode*CkNodeSize(0)+k].clear();
          }
          nodeGrpProxy[i].receive_no_sort(destMsg);
        }
        else if(agg == PNs)
        {
          ((envelope *)UsrToEnv(destMsg))->setUsersize(sizeof(int)*2+sizeof(envelope)+sizeof(itemT)*(destMsg->next));
//          nodeGrpProxy[i].receive(destMsg); //todo - Resize only upto next
          if(track_count) {
            local_sends += destMsg->next;
            destMsg->track_count = 1;
          }
          nodeGrpProxy[i].receive(destMsg);
          //count_local = destMsg->next;
        } else if(agg == PP) {
          ((envelope *)UsrToEnv(destMsg))->setUsersize(sizeof(int)*2+sizeof(envelope)+sizeof(itemT)*destMsg->next);
//          CkPrintf("\nmsg size = %d", *destMsg->next);
          thisProxy[i].receiveOnPE(destMsg);
        }
        //msgBuffers[i] = new HTramMessageSmall();//new HTramMessage();
          msgBuffers[i] = new HTramMessage();
      }
    }
    if(agg == PNs && track_count) {
//      CkPrintf("\nPE-%d sending a reduction %d", CkMyPe(), local_sends);
      CkCallback _cb(CkReductionTarget(HTram, getTotSends), thisProxy[0]);
      contribute(sizeof(int), &local_sends, CkReduction::sum_int, _cb);
    }

  }
}

HTramNodeGrp::HTramNodeGrp() {
  msgBuffers = new HTramMessage*[CkNumNodes()];
  for(int i=0;i<CkNumNodes();i++) {
    msgBuffers[i] = new HTramMessage();
    get_idx[i] = 0;
    done_count[i] = 0;
  }
}

HTramNodeGrp::HTramNodeGrp(CkMigrateMessage* msg) {}


HTramRecv::HTramRecv(){
  msg_stats[MIN_LATENCY] = 100.0;
}

#if 0
bool comparePayload(itemT a, itemT b)
{
    return (a.payload > b.payload);
}

bool lower(itemT a, double value) {
  return a.payload < value;
}

bool upper(itemT a, double value) {
  return a.payload > value;
}
#endif

HTramRecv::HTramRecv(CkMigrateMessage* msg) {}

//#ifdef SRC_GROUPING
void HTramRecv::receive_no_sort(HTramMessage* agg_message) {
#if 0
  if(agg_message->do_timer) {
    double time_stamp = CkWallTimer();
    double latency[2];
    for(int i=0;i<2;i++) {
      latency[i] = time_stamp - agg_message->timer[i];
      msg_stats[TOTAL_LATENCY] += latency[i];
    }
    double max = std::max(latency[0], latency[1]);
    if(msg_stats[MAX_LATENCY] < max)
      msg_stats[MAX_LATENCY] = max;
    double min = std::min(latency[0], latency[1]);
    if(msg_stats[MIN_LATENCY] > min)
      msg_stats[MIN_LATENCY] = min;
    msg_stats[TOTAL_MSGS] += 1.0;
  }
#endif

  for(int i=CkNodeFirst(CkMyNode()); i < CkNodeFirst(CkMyNode())+CkNodeSize(0);i++) {
    HTramMessage* tmpMsg = (HTramMessage*)CkReferenceMsg(agg_message);
    _SET_USED(UsrToEnv(tmpMsg), 0);
    tram_proxy[i].receivePerPE(tmpMsg);
  }
  CkFreeMsg(agg_message);
}

  void HTram::receivePerPE(HTramMessage* msg) {
    int llimit = 0;
    int rank = CkMyRank();
    if(rank > 0) llimit = 0;//msg->getIndex()[rank-1];
    int ulimit = 0;//msg->getIndex()[rank];
    for(int i=llimit; i<ulimit;i++){
      cb(objPtr, msg->buffer[i].payload);
    }
    CkFreeMsg(msg);
  }


//#elif defined PER_DESTPE_BUFFER
void HTram::receiveOnPE(HTramMessage* msg) {
//    CkPrintf("\ntotal_latency=%lfs",total_latency);
#if 0
  if(msg->do_timer) {
    double time_stamp = CkWallTimer();
    double latency[2];
    for(int i=0;i<2;i++) {
      latency[i] = time_stamp - msg->timer[i];
      msg_stats[TOTAL_LATENCY] += latency[i];
    }
    double max = std::max(latency[0], latency[1]);
    if(msg_stats[MAX_LATENCY] < max)
      msg_stats[MAX_LATENCY] = max;
    double min = std::min(latency[0], latency[1]);
    if(msg_stats[MIN_LATENCY] > min)
      msg_stats[MIN_LATENCY] = min;
    msg_stats[TOTAL_MSGS] += 1;
  }
#endif

//  CkPrintf("\nrcv-msg size = %d", msg->next);
  for(int i=0;i<msg->next;i++)
    cb(objPtr, msg->buffer[i].payload);
  delete msg;
}

void HTramRecv::receive(HTramMessage* agg_message) {
#if 0
//  CkPrintf("\nReceived msg of size %d on node%d", agg_message->next, thisIndex);
  if(agg_message->do_timer) {
    double time_stamp = CkWallTimer();
    double latency[2];
    for(int i=0;i<2;i++) {
      latency[i] = time_stamp - agg_message->timer[i];
//      if(latency[i] > 0.1) CkPrintf("\nlatency = %lfs (%lf - %lf)", latency[i], time_stamp, agg_message->timer[i]);
      msg_stats[TOTAL_LATENCY] += latency[i];
    } 
    double max = std::max(latency[0], latency[1]);
    if(msg_stats[MAX_LATENCY] < max)
      msg_stats[MAX_LATENCY] = max;
    double min = std::min(latency[0], latency[1]);
    if(msg_stats[MIN_LATENCY] > min)
      msg_stats[MIN_LATENCY] = min;
    msg_stats[TOTAL_MSGS] += 1;
  }
#endif
  
  //broadcast to each PE and decr refcount
  //nodegroup //reference from group
  int rank0PE = CkNodeFirst(thisIndex);
  HTramNodeMessage* sorted_agg_message = new HTramNodeMessage();
  sorted_agg_message->track_count = agg_message->track_count;

  int sizes[PPN_COUNT] = {0};
  
  for(int i=0;i<agg_message->next;i++) {
    int rank = agg_message->buffer[i].destPe - rank0PE;
    sizes[rank]++;
  } 
  
  sorted_agg_message->offset[0] = 0;
  for(int i=1;i<CkNodeSize(0);i++)
    sorted_agg_message->offset[i] = sorted_agg_message->offset[i-1]+sizes[i-1];
    
  for(int i=0;i<agg_message->next;i++) {
    int rank = agg_message->buffer[i].destPe - rank0PE;
    sorted_agg_message->buffer[sorted_agg_message->offset[rank]++] = agg_message->buffer[i].payload;
  } 
  delete agg_message;
  
  sorted_agg_message->offset[0] = sizes[0];
  for(int i=1;i<CkNodeSize(0);i++)
    sorted_agg_message->offset[i] = sorted_agg_message->offset[i-1] + sizes[i];
    
  for(int i=CkNodeFirst(CkMyNode()); i < CkNodeFirst(CkMyNode())+CkNodeSize(0);i++) {
    HTramNodeMessage* tmpMsg = (HTramNodeMessage*)CkReferenceMsg(sorted_agg_message);
    _SET_USED(UsrToEnv(tmpMsg), 0);
    tram_proxy[i].receivePerPE(tmpMsg);
  } 
  CkFreeMsg(sorted_agg_message);
}

void HTramRecv::receive_small(HTramLocalMessage* agg_message) {
  //broadcast to each PE and decr refcount
  //nodegroup //reference from group
  int rank0PE = CkNodeFirst(thisIndex);
  HTramNodeMessage* sorted_agg_message = new HTramNodeMessage();

  int sizes[PPN_COUNT] = {0};

  for(int i=0;i<agg_message->next;i++) {
    int rank = agg_message->buffer[i].destPe - rank0PE;
//    CkPrintf("\nrank=%d, i=%d, next=%d", rank, i, agg_message->next);
    sizes[rank]++;
  }

  sorted_agg_message->offset[0] = 0;
  for(int i=1;i<CkNodeSize(0);i++)
    sorted_agg_message->offset[i] = sorted_agg_message->offset[i-1]+sizes[i-1];

  for(int i=0;i<agg_message->next;i++) {
    int rank = agg_message->buffer[i].destPe - rank0PE;
    sorted_agg_message->buffer[sorted_agg_message->offset[rank]++] = agg_message->buffer[i].payload;
  }
  delete agg_message;

  sorted_agg_message->offset[0] = sizes[0];
  for(int i=1;i<CkNodeSize(0);i++)
    sorted_agg_message->offset[i] = sorted_agg_message->offset[i-1] + sizes[i];

  for(int i=CkNodeFirst(CkMyNode()); i < CkNodeFirst(CkMyNode())+CkNodeSize(0);i++) {
    HTramNodeMessage* tmpMsg = (HTramNodeMessage*)CkReferenceMsg(sorted_agg_message);
    _SET_USED(UsrToEnv(tmpMsg), 0);
    tram_proxy[i].receivePerPE(tmpMsg);
  }
  CkFreeMsg(sorted_agg_message);
}

void HTramRecv::setTramProxy(CkGroupID tram_gid) {
  tram_proxy = CProxy_HTram(tram_gid);
}


void HTram::receivePerPE(HTramNodeMessage* msg) {
  int llimit = 0;
  int rank = CkMyRank();
  if(rank > 0) llimit = msg->offset[rank-1];
  int ulimit = msg->offset[rank];
  if(!ret_list) {
    for(int i=llimit; i<ulimit;i++)
      cb(objPtr, msg->buffer[i]);
  } else
    cb_retarr(objPtr, &msg->buffer[llimit], ulimit-llimit);
  CkFreeMsg(msg);
  if(msg->track_count) local_recv_count += (ulimit-llimit);
}
//#endif

void HTram::stop_periodic_flush() {
  enable_flush = false;
}

void periodic_tflush(void *htram_obj, double time) {
//  CkPrintf("\nIn callback_fn on PE#%d at time %lf",CkMyPe(), CkWallTimer());
  HTram *proper_obj = (HTram *)htram_obj;
  proper_obj->tflush();
  if(proper_obj->enable_flush)
    proper_obj->registercb();
}

#include "htram_group.def.h"

