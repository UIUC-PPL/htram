#include "htram_group.h"
//#define DEBUG 1

void periodic_tflush(void *htram_obj, double time);

HTram::HTram(CkGroupID cgid, int buffer_size, bool enable_buffer_flushing, double time_in_ms, bool ret_item) {
  // TODO: Implement variable buffer sizes and timed buffer flushing
  flush_time = time_in_ms;
  client_gid = cgid;
  enable_flush = enable_buffer_flushing;
  ret_list = !ret_item;
//  cb = delivercb;
  myPE = CkMyPe();
#ifdef PER_DESTPE_BUFFER
  msgBuffers = new HTramMessage*[CkNumPes()];
#else
  msgBuffers = new HTramMessage*[CkNumNodes()];
#endif

#ifdef SRC_GROUPING
  if(thisIndex==0) CkPrintf("\nSource-side grouping enabled\n");
#endif

#ifdef PER_DESTPE_BUFFER
  for(int i=0;i<CkNumPes();i++)
#else
  for(int i=0;i<CkNumNodes();i++)
#endif
    msgBuffers[i] = new HTramMessage();

#ifdef SRC_GROUPING
  localBuffers = new std::vector<itemT>[CkNumPes()];
#endif

  if(enable_flush)
    periodic_tflush((void *) this, flush_time);
}

HTram::HTram(CkGroupID cgid, CkCallback ecb){
  client_gid = cgid;
//  cb = delivercb;
  endCb = ecb;
  myPE = CkMyPe();
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
#ifdef NODE_SRC_BUFFER
  HTramNodeGrp* srcNodeGrp = (HTramNodeGrp*)srcNodeGrpProxy.ckLocalBranch();
  int idx = srcNodeGrp->get_idx[destNode].fetch_add(1, std::memory_order_seq_cst);

  while( idx+1 > BUFSIZE) {
//    CkPrintf("\n[PE-%d]Non-usable idx = %d", thisIndex, idx);
    idx = srcNodeGrp->get_idx[destNode].fetch_add(1, std::memory_order_seq_cst);
  }
  HTramMessage *nodeBuffer = srcNodeGrp->msgBuffers[destNode];
//  CkPrintf("\n[PE-%d] idx = %d", thisIndex, idx);
  int done_idx = -1;
  if(idx < BUFSIZE) {
    nodeBuffer->buffer[idx].payload = value;
    nodeBuffer->buffer[idx].destPe = dest_pe;
    done_idx = srcNodeGrp->done_count[destNode].fetch_add(1, std::memory_order_seq_cst);
  }
#else

#ifdef PER_DESTPE_BUFFER
  HTramMessage *destMsg = msgBuffers[dest_pe];
#else
  HTramMessage *destMsg = msgBuffers[destNode];
#endif

#ifdef SRC_GROUPING
  itemT itm = {value};
  localBuffers[dest_pe].push_back(itm);
#elif defined PER_DESTPE_BUFFER
  destMsg->buffer[destMsg->next].payload = value;
#else
  destMsg->buffer[destMsg->next].payload = value;
  destMsg->buffer[destMsg->next].destPe = dest_pe;
#endif
  destMsg->next++;
#endif

#ifdef NODE_SRC_BUFFER
  if(done_idx+1 == BUFSIZE) {
    nodeBuffer->next = done_idx+1;
/*
    CkPrintf("\n[PE-%d]Sending out data with size = %d", thisIndex, nodeBuffer->next);
    for(int i=0;i<nodeBuffer->next;i++)
    CkPrintf("\nvalue=%d, pe=%d", nodeBuffer->buffer[i].payload, nodeBuffer->buffer[i].destPe);
*/
    nodeGrpProxy[destNode].receive(nodeBuffer);
    srcNodeGrp->msgBuffers[destNode] = new HTramMessage();
    srcNodeGrp->done_count[destNode] = 0;
    srcNodeGrp->get_idx[destNode] = 0;
  }
#else
  if(destMsg->next == BUFSIZE) {
#ifdef SRC_GROUPING
    int sz = 0;
    for(int i=0;i<CkNodeSize(0);i++) {
      std::vector<itemT> localMsg = localBuffers[destNode*CkNodeSize(0)+i];
      std::copy(localMsg.begin(), localMsg.end(), &(destMsg->buffer[sz]));
      sz += localMsg.size();
      destMsg->index[i] = sz;
      localBuffers[destNode*CkNodeSize(0)+i].clear();
    }
#endif
#ifdef PER_DESTPE_BUFFER
    thisProxy[dest_pe].receiveOnPE(destMsg);
    msgBuffers[dest_pe] = new HTramMessage();
#else
    nodeGrpProxy[destNode].receive(destMsg);
    msgBuffers[destNode] = new HTramMessage();
#endif
  }
#endif
}

void HTram::registercb() {
  CcdCallFnAfter(periodic_tflush, (void *) this, flush_time);
}

void HTram::tflush() {
#ifdef NODE_SRC_BUFFER
  HTramNodeGrp* srcNodeGrp = (HTramNodeGrp*)srcNodeGrpProxy.ckLocalBranch();
#endif
#ifdef NODE_SRC_BUFFER
    srcNodeGrp->flush_count++;
    if(srcNodeGrp->flush_count==CkNodeSize(0))
    {
      for(int i=0;i<CkNumNodes();i++) {
        if(srcNodeGrp->done_count[i]) {
//          CkPrintf("\nCalling TFLUSH---\n");
          srcNodeGrp->msgBuffers[i]->next = srcNodeGrp->done_count[i];
/*
          CkPrintf("\n[PE-%d]TF-Sending out data with size = %d", thisIndex, srcNodeGrp->msgBuffers[i]->next);
          for(int j=0;j<srcNodeGrp->msgBuffers[i]->next;j++)
            CkPrintf("\nTFvalue=%d, pe=%d", srcNodeGrp->msgBuffers[i]->buffer[j].payload, srcNodeGrp->msgBuffers[i]->buffer[j].destPe);
*/
          nodeGrpProxy[i].receive(srcNodeGrp->msgBuffers[i]);
          srcNodeGrp->msgBuffers[i] = new HTramMessage();
          srcNodeGrp->get_idx[i] = 0;
          srcNodeGrp->done_count[i] = 0;
          srcNodeGrp->flush_count = 0;
        }
      }
    }
#else
#ifdef PER_DESTPE_BUFFER
  for(int i=0;i<CkNumPes();i++) {
#else
  for(int i=0;i<CkNumNodes();i++) {
#endif
    if(msgBuffers[i]->next)
    {
#ifdef SRC_GROUPING
      int destNode = i;
      HTramMessage *destMsg = msgBuffers[i];
      int sz = 0;
      for(int k=0;k<CkNodeSize(0);k++) {
        std::vector<itemT> localMsg = localBuffers[destNode*CkNodeSize(0)+k];
        std::copy(localMsg.begin(), localMsg.end(), &(destMsg->buffer[sz]));
        sz += localMsg.size();
        destMsg->index[k] = sz;
        localBuffers[destNode*CkNodeSize(0)+k].clear();
      }
#endif
#ifndef PER_DESTPE_BUFFER
      nodeGrpProxy[i].receive(msgBuffers[i]); //only upto next
#else
      thisProxy[i].receiveOnPE(msgBuffers[i]);
#endif
      msgBuffers[i] = new HTramMessage();
    }
  }
#endif
}

HTramNodeGrp::HTramNodeGrp() {
#ifdef NODE_SRC_BUFFER
  msgBuffers = new HTramMessage*[CkNumNodes()];
  for(int i=0;i<CkNumNodes();i++) {
    msgBuffers[i] = new HTramMessage();
    get_idx[i] = 0;
    done_count[i] = 0;
  }
#endif
}

HTramNodeGrp::HTramNodeGrp(CkMigrateMessage* msg) {}


HTramRecv::HTramRecv(){
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

#ifdef SRC_GROUPING
  void HTramRecv::receive(HTramMessage* agg_message) {
    for(int i=CkNodeFirst(CkMyNode()); i < CkNodeFirst(CkMyNode())+CkNodeSize(0);i++) {
      HTramMessage* tmpMsg = (HTramMessage*)CkReferenceMsg(agg_message);
      _SET_USED(UsrToEnv(tmpMsg), 0);
      tram_proxy[i].receivePerPE(tmpMsg);
    } 
  }

  void HTram::receivePerPE(HTramMessage* msg) {
    int llimit = 0;
    int rank = CkMyRank();
    if(rank > 0) llimit = msg->index[rank-1];
    int ulimit = msg->index[rank];
    for(int i=llimit; i<ulimit;i++){
      cb(objPtr, msg->buffer[i].payload);
    }
    CkFreeMsg(msg);
  }

#elif defined PER_DESTPE_BUFFER
  void HTram::receiveOnPE(HTramMessage* msg) {
    for(int i=0;i<msg->next;i++)
      cb(objPtr, msg->buffer[i].payload);
    delete msg;
  }
#else
void HTramRecv::receive(HTramMessage* agg_message) {
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
#endif

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

