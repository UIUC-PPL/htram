#include "htram_group.h"
//#define NODE_SRC_BUFFER 1
//#define DEBUG 1
HTram::HTram(CkGroupID cgid, int buffer_size, bool enable_buffer_flushing, double time_in_ms) {
  // TODO: Implement variable buffer sizes and timed buffer flushing

  client_gid = cgid;
//  cb = delivercb;
  myPE = CkMyPe();
  msgBuffers = new HTramMessage*[CkNumNodes()];
  for(int i=0;i<CkNumNodes();i++)
    msgBuffers[i] = new HTramMessage();
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

void HTram::set_func_ptr(void (*func)(void*, int), void* obPtr) {
  cb = func;
  objPtr = obPtr;
}

HTram::HTram(CkMigrateMessage* msg) {}

//one per node, message, fixed 
//Client inserts
void HTram::insertValue(int value, int dest_pe) {
  int destNode = dest_pe/CkNodeSize(0); //find safer way to find dest node,
  // node size is not always same
#ifdef NODE_SRC_BUFFER
  HTramNodeGrp* srcNodeGrp = (HTramNodeGrp*)srcNodeGrpProxy.ckLocalBranch();
#endif
  HTramMessage *destMsg = msgBuffers[destNode];
  destMsg->buffer[destMsg->next].payload = value;
  destMsg->buffer[destMsg->next].destPe = dest_pe;
  destMsg->next++;

#ifdef NODE_SRC_BUFFER
  if(destMsg->next == LOCAL_BUFSIZE) {
    //Add to node buffer
    CmiLock(srcNodeGrp->locks[destNode]);
    HTramMessage *nodeBuffer = srcNodeGrp->msgBuffers[destNode];
    std::copy(destMsg->buffer, destMsg->buffer+LOCAL_BUFSIZE, &nodeBuffer->buffer[nodeBuffer->next]);
    nodeBuffer->next+=LOCAL_BUFSIZE;

    if(nodeBuffer->next == BUFSIZE) {
      nodeGrpProxy[destNode].receive(nodeBuffer);
      srcNodeGrp->msgBuffers[destNode] = new HTramMessage();
    }
    CmiUnlock(srcNodeGrp->locks[destNode]);
    msgBuffers[destNode] = new HTramMessage();
  }
#else
  if(destMsg->next == BUFSIZE) {
    nodeGrpProxy[destNode].receive(destMsg);
    msgBuffers[destNode] = new HTramMessage();
  }
#endif
}

void HTram::tflush() {
#ifdef NODE_SRC_BUFFER
  HTramNodeGrp* srcNodeGrp = (HTramNodeGrp*)srcNodeGrpProxy.ckLocalBranch();
#endif
  for(int i=0;i<CkNumNodes();i++) {
#ifdef NODE_SRC_BUFFER
    //if(CkMyRank()==0)
    {
      CmiLock(srcNodeGrp->locks[i]);
      nodeGrpProxy[i].receive(srcNodeGrp->msgBuffers[i]);
      srcNodeGrp->msgBuffers[i] = new HTramMessage();
      CmiUnlock(srcNodeGrp->locks[i]);
    }
#endif
    nodeGrpProxy[i].receive(msgBuffers[i]); //only upto next
    msgBuffers[i] = new HTramMessage();
  }
}

HTramNodeGrp::HTramNodeGrp() {
#ifdef NODE_SRC_BUFFER
  locks = new CmiNodeLock[CkNumNodes()];
  for(int i=0;i<CkNumNodes();i++)
    locks[i] = CmiCreateLock();
#endif
  msgBuffers = new HTramMessage*[CkNumNodes()];
  for(int i=0;i<CkNumNodes();i++)
    msgBuffers[i] = new HTramMessage();
}

HTramNodeGrp::HTramNodeGrp(CkMigrateMessage* msg) {}


HTramRecv::HTramRecv(){
}

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

HTramRecv::HTramRecv(CkMigrateMessage* msg) {}

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
}

void HTram::receivePerPE(HTramNodeMessage* msg) {
  int llimit = 0;
  int rank = CkMyRank();
  if(rank > 0) llimit = msg->offset[rank-1];
  int ulimit = msg->offset[rank];
  for(int i=llimit; i<ulimit;i++){
    cb(objPtr, msg->buffer[i]);
  }
  CkFreeMsg(msg);
}

#include "htram_group.def.h"

