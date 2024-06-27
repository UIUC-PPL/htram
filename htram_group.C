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

HTram::HTram(CkGroupID cgid, int buffer_size, bool enable_buffer_flushing, double time_in_ms, bool ret_item) {
  // TODO: Implement variable buffer sizes and timed buffer flushing
  flush_time = time_in_ms;
  client_gid = cgid;
  enable_flush = enable_buffer_flushing;
  msg_stats[MIN_LATENCY] = 100.0;
  buf_type = 0;
  bufSize = SIZE_LIST[buf_type];
  prevBufSize = bufSize;
//  if(thisIndex==0) CkPrintf("\nbuf_type = %d, type %d,%d,%d,%d", buf_type, use_src_grouping, use_src_agg, use_per_destpe_agg, use_per_destnode_agg);
/*
  if(use_per_destnode_agg)
    if(thisIndex==0) CkPrintf("\nDest-node side grouping/sorting enabled (1 buffer per src-pe, per dest-node)\n");
*/
  ret_list = !ret_item;

  myPE = CkMyPe();
  msgBuffers = new HTramMessage*[CkNumPes()];

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
  srcNodeGrp = (HTramNodeGrp*)srcNodeGrpProxy.ckLocalBranch();
  nodeGrp = (HTramRecv*)nodeGrpProxy.ckLocalBranch();

  if(enable_flush)
    periodic_tflush((void *) this, flush_time);
}

void HTram::reset_stats(int btype, int buf_size, int agtype) {
  prevBufSize = bufSize;
  std::fill_n(msg_stats, STATS_COUNT, 0.0);
  msg_stats[MIN_LATENCY] = 100.0;
  std::fill_n(nodeGrp->msg_stats, STATS_COUNT, 0.0);
  nodeGrp->msg_stats[MIN_LATENCY] = 100.0;
  buf_type = btype;
  bufSize = buf_size;
  agg = agtype;
  int buf_count = CkNumNodes();
  if(agg == PP) buf_count = CkNumPes();
  for(int i=0;i<buf_count;i++) {
    if(buf_type == 0)
      msgBuffers[i] = new HTramMessage();
    else if(buf_type == 1)
      msgBuffers[i] = new HTramMessageSmall();
    else if(buf_type == 2)
      msgBuffers[i] = new HTramMessageLarge();
  }

  //if(thisIndex==0) CkPrintf("\nbuf_type = %d, size = %d, type = %d", buf_type, bufSize, agg);
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
    BaseMsg *destMsg = msgBuffers[destNode];
    if(agg == PP)
      destMsg = msgBuffers[dest_pe];

    if(agg == PsN) {
      itemT itm = {dest_pe, value};
      localBuffers[dest_pe].push_back(itm);
    } else if (agg == PP) {//change msg type to not include destPE
      destMsg->getBuffer()[*(destMsg->getNext())].payload = value;
    } else {
      destMsg->getBuffer()[*(destMsg->getNext())].payload = value;
      destMsg->getBuffer()[*(destMsg->getNext())].destPe = dest_pe;
    }

    if(*(destMsg->getNext()) == 0 || *(destMsg->getNext()) == bufSize-1) {
      if(*(destMsg->getNext()) == 0)
        destMsg->getTimer()[0] = CkWallTimer();
      else
        destMsg->getTimer()[1] = CkWallTimer();
    }

    *(destMsg->getNext()) = *(destMsg->getNext())+1;
    if(*(destMsg->getNext()) == bufSize) {
      if(agg == PsN) {
        int sz = 0;
        for(int i=0;i<CkNodeSize(0);i++) {
          std::vector<itemT> localMsg = localBuffers[destNode*CkNodeSize(0)+i];
          std::copy(localMsg.begin(), localMsg.end(), &(destMsg->getBuffer()[sz]));
          sz += localMsg.size();
          destMsg->getIndex()[i] = sz;
          localBuffers[destNode*CkNodeSize(0)+i].clear();
        }
      }
#if 1
      //((envelope *)UsrToEnv(destMsg))->setUsersize(0);//destMsg->next-20)*4+32);
      int dest_idx = dest_pe;
      if(agg == PP) {
//        CkPrintf("\nmsg size = %d", *destMsg->getNext());
        if(buf_type == 0)
            thisProxy[dest_pe].receiveOnPE(destMsg);
          else if(buf_type == 1)
            thisProxy[dest_pe].receiveOnPESmall(destMsg);
          else if(buf_type == 2)
            thisProxy[dest_pe].receiveOnPELarge(destMsg);
      } else {
          dest_idx = destNode;
        if(agg == PsN) {
          if(buf_type == 0)
            nodeGrpProxy[destNode].receive_no_sort(destMsg);
          else if(buf_type == 1)
            nodeGrpProxy[destNode].receive_no_sortSmall(destMsg);
          else if(buf_type == 2)
            nodeGrpProxy[destNode].receive_no_sortLarge(destMsg);
//          nodeGrpProxy[destNode].receive_no_sort(destMsg);
        } else {
          if(buf_type == 0)
            nodeGrpProxy[destNode].receive(destMsg);
          else if(buf_type == 1)
            nodeGrpProxy[destNode].receiveSmall(destMsg);
          else if(buf_type == 2)
            nodeGrpProxy[destNode].receiveLarge(destMsg);
        }
      }
      if(buf_type == 0)
          msgBuffers[dest_idx] = new HTramMessage();
        else if(buf_type == 1)
          msgBuffers[dest_idx] = new HTramMessageSmall();
        else if(buf_type == 2)
          msgBuffers[dest_idx] = new HTramMessageLarge();
#endif
    }
  }
}

void HTram::registercb() {
  CcdCallFnAfter(periodic_tflush, (void *) this, flush_time);
}

void HTram::copyToNodeBuf(int destnode, int increment) {

// Get atomic index
  int idx = srcNodeGrp->get_idx[destnode].fetch_add(increment, std::memory_order_relaxed);
  while(idx >= bufSize) {
    idx = srcNodeGrp->get_idx[destnode].fetch_add(increment, std::memory_order_relaxed);
  }

  if(idx==0 || idx == bufSize-increment)
    srcNodeGrp->msgBuffers[destnode]->getTimer()[idx/(bufSize-increment)] = CkWallTimer();

// Copy data into node buffer from PE-local buffer
  int i;
  for(i=0;i<increment;i++) {
    srcNodeGrp->msgBuffers[destnode]->getBuffer()[idx+i].payload = local_buf[destnode]->buffer[i].payload;
    srcNodeGrp->msgBuffers[destnode]->getBuffer()[idx+i].destPe = local_buf[destnode]->buffer[i].destPe;
  }

  int done_count = srcNodeGrp->done_count[destnode].fetch_add(increment, std::memory_order_relaxed);
  if(done_count+increment == bufSize) {
    *(srcNodeGrp->msgBuffers[destnode]->getNext()) = bufSize;
    
    if(buf_type == 0) {
      nodeGrpProxy[destnode].receive(srcNodeGrp->msgBuffers[destnode]);
      srcNodeGrp->msgBuffers[destnode] = new HTramMessage();
    } else if(buf_type == 1) {
      nodeGrpProxy[destnode].receiveSmall(srcNodeGrp->msgBuffers[destnode]);
      srcNodeGrp->msgBuffers[destnode] = new HTramMessageSmall();
    } else if(buf_type == 2) {
      nodeGrpProxy[destnode].receiveLarge(srcNodeGrp->msgBuffers[destnode]);
      srcNodeGrp->msgBuffers[destnode] = new HTramMessageLarge();
    }
    srcNodeGrp->done_count[destnode] = 0;
    srcNodeGrp->get_idx[destnode] = 0;
  }
  local_idx[destnode] = 0;
}

void HTram::tflush() {
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
    if(flush_count+1==CkNodeSize(0))
    {
      for(int i=0;i<CkNumNodes();i++) {
        if(srcNodeGrp->done_count[i]) {
  //          CkPrintf("\nCalling TFLUSH---\n");
          *(srcNodeGrp->msgBuffers[i]->getNext()) = srcNodeGrp->done_count[i];
  /*
          CkPrintf("\n[PE-%d]TF-Sending out data with size = %d", thisIndex, srcNodeGrp->msgBuffers[i]->next);
          for(int j=0;j<srcNodeGrp->msgBuffers[i]->next;j++)
            CkPrintf("\nTFvalue=%d, pe=%d", srcNodeGrp->msgBuffers[i]->buffer[j].payload, srcNodeGrp->msgBuffers[i]->buffer[j].destPe);
  */
          *(srcNodeGrp->msgBuffers[i]->getDoTimer()) = 0;
          if(buf_type == 0)
            nodeGrpProxy[i].receive(srcNodeGrp->msgBuffers[i]);
          else if(buf_type == 1)
            nodeGrpProxy[i].receiveSmall(srcNodeGrp->msgBuffers[i]);
          else if(buf_type == 2)
            nodeGrpProxy[i].receiveLarge(srcNodeGrp->msgBuffers[i]);
        }
//          srcNodeGrp->msgBuffers[i] = new HTramMessageSmall();//new HTramMessage();//localMsgBuffer;
          if((buf_type+1)%3 == 0)
            srcNodeGrp->msgBuffers[i] = new HTramMessage();
          else if((buf_type+1)%3 == 1)
            srcNodeGrp->msgBuffers[i] = new HTramMessageSmall();
          else if((buf_type+1)%3 == 2)
            srcNodeGrp->msgBuffers[i] = new HTramMessageLarge();
          srcNodeGrp->done_count[i] = 0;
          srcNodeGrp->flush_count = 0;
          srcNodeGrp->get_idx[i] = 0;
//          localMsgBuffer = new HTramMessage();
        
      }
    }
  }
  else {
    int buf_count = CkNumNodes();
    if(agg == PP) buf_count = CkNumPes();

    for(int i=0;i<buf_count;i++) {
      if(msgBuffers[i]->getNext())
      {
        BaseMsg *destMsg = msgBuffers[i];
        *destMsg->getDoTimer() = 0;
        if(agg == PsN) {
          int destNode = i;
          int sz = 0;
          for(int k=0;k<CkNodeSize(0);k++) {
            std::vector<itemT> localMsg = localBuffers[destNode*CkNodeSize(0)+k];
            std::copy(localMsg.begin(), localMsg.end(), &(destMsg->getBuffer()[sz]));
            sz += localMsg.size();
            destMsg->getIndex()[k] = sz;
            localBuffers[destNode*CkNodeSize(0)+k].clear();
          }
          nodeGrpProxy[i].receive_no_sort(destMsg);
        }
        else if(agg == PNs) {
//          nodeGrpProxy[i].receive(destMsg); //todo - Resize only upto next
          if(buf_type == 0)
            nodeGrpProxy[i].receive(destMsg);
          else if(buf_type == 1)
            nodeGrpProxy[i].receiveSmall(destMsg);
          else if(buf_type == 2)
            nodeGrpProxy[i].receiveLarge(destMsg);
        } else if(agg == PP) {
//          CkPrintf("\nmsg size = %d", *destMsg->getNext());
          if(buf_type == 0)
            thisProxy[i].receiveOnPE(destMsg);
          else if(buf_type == 1)
            thisProxy[i].receiveOnPESmall(destMsg);
          else if(buf_type == 2)
            thisProxy[i].receiveOnPELarge(destMsg);
        }
      }
#if 0
        //msgBuffers[i] = new HTramMessageSmall();//new HTramMessage();
        if((buf_type+1)%3 == 0)
          msgBuffers[i] = new HTramMessage();
        else if((buf_type+1)%3 == 1)
          msgBuffers[i] = new HTramMessageSmall();
        else if((buf_type+1)%3 == 2)
          msgBuffers[i] = new HTramMessageLarge();
#endif      
    }
  }
#if 0
  buf_type = (buf_type+1)%3;
  bufSize = SIZE_LIST[buf_type];
#endif
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

  for(int i=CkNodeFirst(CkMyNode()); i < CkNodeFirst(CkMyNode())+CkNodeSize(0);i++) {
    HTramMessage* tmpMsg = (HTramMessage*)CkReferenceMsg(agg_message);
    _SET_USED(UsrToEnv(tmpMsg), 0);
    tram_proxy[i].receivePerPE(tmpMsg);
  }
  CkFreeMsg(agg_message);
}

void HTramRecv::receive_no_sortSmall(HTramMessageSmall* agg_message) {
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

  for(int i=CkNodeFirst(CkMyNode()); i < CkNodeFirst(CkMyNode())+CkNodeSize(0);i++) {
    HTramMessage* tmpMsg = (HTramMessage*)CkReferenceMsg(agg_message);
    _SET_USED(UsrToEnv(tmpMsg), 0);
    tram_proxy[i].receivePerPE(tmpMsg);
  }
  CkFreeMsg(agg_message);
}

void HTramRecv::receive_no_sortLarge(HTramMessageLarge* agg_message) {
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
    if(rank > 0) llimit = msg->getIndex()[rank-1];
    int ulimit = msg->getIndex()[rank];
    for(int i=llimit; i<ulimit;i++){
      cb(objPtr, msg->buffer[i].payload);
    }
    CkFreeMsg(msg);
  }


//#elif defined PER_DESTPE_BUFFER
void HTram::receiveOnPE(HTramMessage* msg) {
//    CkPrintf("\ntotal_latency=%lfs",total_latency);
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
//  CkPrintf("\nrcv-msg size = %d", msg->next);
  for(int i=0;i<msg->next;i++)
    cb(objPtr, msg->buffer[i].payload);
  delete msg;
}

void HTram::receiveOnPESmall(HTramMessageSmall* msg) {
//    CkPrintf("\ntotal_latency=%lfs",total_latency);
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
//  CkPrintf("\nrcv-msg size = %d", msg->next);
  for(int i=0;i<msg->next;i++)
    cb(objPtr, msg->buffer[i].payload);
  delete msg;
}

void HTram::receiveOnPELarge(HTramMessageLarge* msg) {
//    CkPrintf("\ntotal_latency=%lfs",total_latency);
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
//  CkPrintf("\nrcv-msg size = %d", msg->next);
  for(int i=0;i<msg->next;i++)
    cb(objPtr, msg->buffer[i].payload);
  delete msg;
}
//#else
void HTramRecv::receiveSmall(HTramMessageSmall* agg_message) {
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

  //broadcast to each PE and decr refcount
  //nodegroup //reference from group
  int rank0PE = CkNodeFirst(thisIndex);
  HTramNodeMessage* sorted_agg_message = new HTramNodeMessage();

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
void HTramRecv::receive(HTramMessage* agg_message) {
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
  
  //broadcast to each PE and decr refcount
  //nodegroup //reference from group
  int rank0PE = CkNodeFirst(thisIndex);
  HTramNodeMessage* sorted_agg_message = new HTramNodeMessage();
  
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

void HTramRecv::receiveLarge(HTramMessageLarge* agg_message) {
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

  //broadcast to each PE and decr refcount
  //nodegroup //reference from group
  int rank0PE = CkNodeFirst(thisIndex);
  HTramNodeMessage* sorted_agg_message = new HTramNodeMessage();

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

