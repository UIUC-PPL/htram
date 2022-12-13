#include "htram_group.h"
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
  msgBuffers = new HTramMessage*[CkNumNodes()];
  for(int i=0;i<CkNumNodes();i++)
    msgBuffers[i] = new HTramMessage();
}

void HTramRecv::set_func_ptr(void (*func)(void*, int), void* obPtr) {
  cb = func;
  objPtr = obPtr;
}

HTram::HTram(CkMigrateMessage* msg) {}

//one per node, message, fixed 
//Client inserts
void HTram::insertValue(int value, int dest_pe) {
  int destNode = dest_pe/CkNodeSize(0); //find safer way to find dest node,
  // node size is not always same
  HTramMessage *destMsg = msgBuffers[destNode];
  destMsg->buffer[destMsg->next].payload = value;
  destMsg->buffer[destMsg->next].destPe = dest_pe;
  destMsg->next++;

#ifdef DEBUG
  if(destMsg->next%1000 == 0) CkPrintf("\nPE-%d, BufSize = %d\n", CkMyPe(), destMsg->next);
#endif

  if(destMsg->next == BUFSIZE) {
#ifdef DEBUG
    CkPrintf("\nPE-%d, Flushing", CkMyPe());
#endif
    int i = CkMyRank();//0
//    for(;i<CkNodeSize(0);i++) 
    {
//	HTramMessage* tmpMsg = (HTramMessage*)CkReferenceMsg(destMsg);
//	_SET_USED(UsrToEnv(tmpMsg), 0);
    	nodeGrpProxy[CkNodeSize(0)*destNode+i/*CkMyRank()*/].receive(destMsg);
    }
//    delete destMsg;
    msgBuffers[destNode] = new HTramMessage();
  }
}

void HTram::tflush() {
#if 1
  for(int i=0;i<CkNumNodes();i++) {
    nodeGrpProxy[CkNodeSize(0)*i+CkMyRank()].receive(msgBuffers[i]); //only upto next
    msgBuffers[i] = new HTramMessage();
  }
#endif
}


HTramRecv::HTramRecv(){
  for(int i=0;i<POOL_SIZE;i++) {
    msg_pool[i] = new HTramNodeMessage();
    in_use[i] = 0;
  }
//  sorted_agg_message = new HTramNodeMessage();
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
//HTramNodeMessage*  sorted_agg_message = new HTramNodeMessage();
  //broadcast to each PE and decr refcount
  //nodegroup //reference from group
  int rank0PE = CkMyNode()*CkNodeSize(0);//CkNodeFirst(thisIndex);
  int sizes[PPN_COUNT] = {0};

  int index = -1;
  for(int i=0;i<POOL_SIZE;i++) {
    if(in_use[i] == 0) {
      index = i;
      in_use[i] = 1;
//      CkPrintf("\nUsing msg index %d from pool on PE %d\n", i, thisIndex);
      break;
    }
  }

  if(index == -1) {
    index = 0;
    CkPrintf("\nDid not find msg in pool on PE=%d", thisIndex);
  }

  sorted_agg_message = msg_pool[index];
#if 1
  for(int i=0;i<agg_message->next;i++) {
    int rank = agg_message->buffer[i].destPe - rank0PE;
//    if(rank < PPN_COUNT && rank>0)
    sizes[rank]++;
//    else CkPrintf("\nrank = %d\n", rank);
  }
#endif
#if 1
  sorted_agg_message->offset[0] = 0;
  for(int i=1;i<CkNodeSize(0);i++)
    sorted_agg_message->offset[i] = sorted_agg_message->offset[i-1]+sizes[i-1];

  for(int i=0;i<agg_message->next;i++) {
    int rank = agg_message->buffer[i].destPe - rank0PE;
    sorted_agg_message->buffer[sorted_agg_message->offset[rank]++] = agg_message->buffer[i].payload;
  }
#endif
  delete agg_message;
//  CkFreeMsg(agg_message);
#if 1
  sorted_agg_message->offset[0] = sizes[0];
  for(int i=1;i<CkNodeSize(0);i++)
    sorted_agg_message->offset[i] = sorted_agg_message->offset[i-1] + sizes[i];
#endif
#if 1
  sorted_agg_message->refCount = CkNodeSize(0);
  sorted_agg_message->poolIndex = index;
  sorted_agg_message->pool_index_use = &in_use[index];
  for(int i=CkMyNode()*CkNodeSize(0); i < CkMyNode()*CkNodeSize(0)+CkNodeSize(0);i++) {
#if 0
    HTramNodeMessage* tmpMsg = (HTramNodeMessage*)CkReferenceMsg(sorted_agg_message);
    _SET_USED(UsrToEnv(tmpMsg), 0);
#endif
    thisProxy[i].receivePerPE(sorted_agg_message);
  }
//sorted_agg_message = new HTramNodeMessage();
#endif
}

void HTramRecv::receivePerPE(HTramNodeMessage* msg) {
#if 1
  int llimit = 0;
  int rank = CkMyRank();
  if(rank > 0) llimit = msg->offset[rank-1];
  int ulimit = msg->offset[rank];
  for(int i=llimit; i<ulimit;i++){
    cb(objPtr, msg->buffer[i]);
  }
  msg->refCount--;
  if(msg->refCount==0) {
    msg->pool_index_use = 0;
//    in_use[msg->poolIndex] = 0;
//    CkPrintf("\nReleasing msg to pool");
  }
#endif
#if 1
//  CkFreeMsg(msg);
#endif
}

#include "htram_group.def.h"

