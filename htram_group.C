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
#if SORT //Sort
  std::sort(agg_message->buffer, agg_message->buffer + agg_message->next, comparePayload);
  std::fill(agg_message->lrange, agg_message->lrange+PPN_COUNT, 0);
  std::fill(agg_message->urange, agg_message->urange+PPN_COUNT, 0);
  for(int i=0;i<agg_message->next;i++) {
    int rank = agg_message->buffer[i].destPe - rank0PE;
    if(agg_message->lrange[rank] > agg_message->buffer[i].payload) agg_message->lrange[rank] = agg_message->buffer[i].payload;
    if(agg_message->urange[rank] < agg_message->buffer[i].payload) agg_message->urange[rank] = agg_message->buffer[i].payload;
  }
  for(int i=CkNodeFirst(CkMyNode()); i < CkNodeFirst(CkMyNode())+CkNodeSize(0);i++) {
    HTramMessage* tmpMsg = (HTramMessage*)CkReferenceMsg(agg_message);
    _SET_USED(UsrToEnv(tmpMsg), 0);
    htramProxy[i].receivePerPE(tmpMsg);
  }
#else
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
    htramProxy[i].receivePerPE(tmpMsg);
  }

#endif
}

#if SORT
void HTram::receivePerPE(HTramMessage* msg) {
#else
void HTram::receivePerPE(HTramNodeMessage* msg) {
#endif
#if SORT
//  int llimit = std::lower_bound(msg->buffer, msg->buffer+msg->next, msg->lrange[CkMyRank()], lower);
  auto llimit_val = std::lower_bound(msg->buffer, msg->buffer+msg->next, msg->lrange[CkMyRank()],
            [](const itemT& info, double value)
            {
                return info.payload < value;
            });
  int llimit = llimit_val - msg->buffer;
  auto ulimit_val = std::upper_bound(msg->buffer, msg->buffer+msg->next, msg->urange[CkMyRank()],
	    [](double value, const itemT& info)
            {
                return info.payload > value;
            });
  int ulimit = ulimit_val - msg->buffer;
  int myPE = CkMyPe();
  for(int i=llimit;i<ulimit;i++) {
    if(msg->buffer[i].destPe == myPE)
      cb(client_gid, objPtr, msg->buffer[i].payload);
  }
#else
  int llimit = 0;
  int rank = CkMyRank();
  if(rank > 0) llimit = msg->offset[rank-1];
  int ulimit = msg->offset[rank];
  for(int i=llimit; i<ulimit;i++){
    cb(client_gid, objPtr, msg->buffer[i]);
  }
#endif
  CkFreeMsg(msg);
}

#include "htram_group.def.h"

