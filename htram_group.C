#include "htram_group.h"
//#define DEBUG 1
HTram::HTram(CkGroupID cgid, CkCallback ecb){
  client_gid = cgid;
//  cb = delivercb;
  endCb = ecb;
  myPE = CkMyPe();
  msgBuffers = new HTramMessage*[CkNumNodes()];
  for(int i=0;i<CkNumNodes();i++)
    msgBuffers[i] = new HTramMessage();
}

void HTram::setCb(void (*func)(CkGroupID, void*, int), void* obPtr) {
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
    nodeGrpProxy[destNode].receive(destMsg);
    msgBuffers[destNode] = new HTramMessage();
  }
}

void HTram::tflush() {
  for(int i=0;i<CkNumNodes();i++) {
    nodeGrpProxy[i].receive(msgBuffers[i]); //only upto next
    msgBuffers[i] = new HTramMessage();
  }
}


HTramRecv::HTramRecv(){
}

HTramRecv::HTramRecv(CkMigrateMessage* msg) {}

void HTramRecv::receive(HTramMessage* agg_message) {
  //broadcast to each PE and decr refcount
  //nodegroup //reference from group
  HTramNodeMessage* sorted_agg_message = new HTramNodeMessage();

  int sizes[PPN_COUNT] = {0};

  int rank0PE = CkNodeFirst(thisIndex);

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

  sorted_agg_message->offset[0] = sizes[0];
  for(int i=1;i<CkNodeSize(0);i++)
    sorted_agg_message->offset[i] = sorted_agg_message->offset[i-1] + sizes[i];

  for(int i=CkNodeFirst(CkMyNode()); i < CkNodeFirst(CkMyNode())+CkNodeSize(0);i++) {
#if 0
    HTramMessage* tmpMsg = new HTramMessage(agg_message->next, agg_message->buffer);
    htramProxy[i].receivePerPE(tmpMsg);//converse message?
#elif 0
    HTramMessage* tmpMsg = (HTramMessage*)CkReferenceMsg(agg_message);
    _SET_USED(UsrToEnv(tmpMsg), 0);
     htramProxy[i].receivePerPE(tmpMsg);//agg_message);
#else
    
    HTramNodeMessage* tmpMsg = (HTramNodeMessage*)CkReferenceMsg(sorted_agg_message);
    _SET_USED(UsrToEnv(tmpMsg), 0);
    htramProxy[i].receivePerPE(tmpMsg);
#endif
  }
}

void HTram::receivePerPE(HTramNodeMessage* msg) {
  int llimit = 0;
  int rank = CkMyRank();
  if(rank > 0) llimit = msg->offset[rank-1];
  int ulimit = msg->offset[rank];
  for(int i=llimit; i<ulimit;i++){
    cb(client_gid, objPtr, msg->buffer[i]);
  }
  CkFreeMsg(msg);
}

#include "htram_group.def.h"

