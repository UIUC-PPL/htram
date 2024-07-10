#include "NDMeshStreamer.h"

typedef CmiUInt8 dtype;
#include "TopoManager.h"
#include "smp_ig.decl.h"

#include <assert.h>
#define SIZES 3
#define PHASE_COUNT 1//12
// Handle to the test driver (chare)
CProxy_TestDriver driverProxy;

int ltab_siz = 100000;
int l_num_req = 1000000;     // per thread number of requests (updates)
int lnum_counts = 1000;       // per thread size of the table
int l_buffer_size = 1024;
bool enable_buffer_flushing = false;
int l_flush_timer = 500;
bool return_item = true;

#include "htram_group.h"

using tram_proxy_t = CProxy_HTram;
using tram_t = HTram;

class TestDriver : public CBase_TestDriver {
private:
  CProxy_Updater  updater_array;
  tram_proxy_t tram_req_proxy;
  tram_proxy_t tram_resp_proxy;

  CProxy_HTramRecv nodeGrpReqProxy;
  CProxy_HTramNodeGrp srcNodeGrpReqProxy;
  CProxy_HTramRecv nodeGrpRespProxy;
  CProxy_HTramNodeGrp srcNodeGrpRespProxy;

  double starttime;

public:
  TestDriver(CkArgMsg* args) {
    int64_t printhelp = 0;
    int opt;
    while( (opt = getopt(args->argc, args->argv, "hn:T:S:t:")) != -1 ) {
      switch(opt) {
      case 'h': printhelp = 1; break;
      case 'n': sscanf(optarg,"%d" ,&l_num_req);   break;
      case 'T': sscanf(optarg,"%d" ,&ltab_siz);   break;
      case 'S': sscanf(optarg, "%d", &l_buffer_size); break;
      case 't': sscanf(optarg, "%d", &l_flush_timer); break;
      default:  break;
      }
    }
    assert(sizeof(CmiInt8) == sizeof(int64_t));
    CkPrintf("Running ig on %d PEs\n", CkNumPes());
    CkPrintf("Number of Request / PE           (-n)= %ld\n", l_num_req );
    CkPrintf("Table size / PE                  (-T)= %ld\n", ltab_siz);
//    CkPrintf("TRAM Timed Flush enabled with flushes every %f us.\n", static_cast<double>(l_flush_timer)/1000);
 
    driverProxy = thishandle;

    int dims[2] = {CkNumNodes(), CkNumPes() / CkNumNodes()};
    CkPrintf("Aggregation topology: %d %d\n", dims[0], dims[1]);

    nodeGrpReqProxy = CProxy_HTramRecv::ckNew();
    srcNodeGrpReqProxy = CProxy_HTramNodeGrp::ckNew();

    nodeGrpRespProxy = CProxy_HTramRecv::ckNew();
    srcNodeGrpRespProxy = CProxy_HTramNodeGrp::ckNew();

    CkCallback start_cb(CkReductionTarget(TestDriver, start), driverProxy);
    tram_req_proxy = tram_proxy_t::ckNew(nodeGrpReqProxy.ckGetGroupID(), srcNodeGrpReqProxy.ckGetGroupID(), l_buffer_size, enable_buffer_flushing, static_cast<double>(l_flush_timer)/1000, return_item, true, start_cb);
    tram_resp_proxy = tram_proxy_t::ckNew(nodeGrpRespProxy.ckGetGroupID(), srcNodeGrpRespProxy.ckGetGroupID(), l_buffer_size, enable_buffer_flushing, static_cast<double>(l_flush_timer)/1000, return_item, false, start_cb);

    updater_array = CProxy_Updater::ckNew(tram_req_proxy.ckGetGroupID(), tram_resp_proxy.ckGetGroupID());
    
    delete args;
  }

  int count = 0;
  void start() {
    if(++count == 3)
    {
//      CkPrintf("\nStarting updates"); fflush(stdout);
      starttime = CkWallTimer();
    
      CkCallback endCb(CkIndex_TestDriver::startVerificationPhase(), thisProxy);
//      CkCallback endCb(CkIndex_Updater::lastFlush(),updater_array);
      if(phase < PHASE_COUNT) updater_array.preGenerateUpdates(phase%SIZES, SIZE_LIST[phase%SIZES], phase/SIZES);
      CkStartQD(endCb);
    }
  }
  int phase = 0;
  double update_walltime;

//#define VERIFY
  void startVerificationPhase() {
//    update_walltime = CkWallTimer() - starttime;
    
//    CkPrintf("   %8.3lf seconds\n", update_walltime);

    //updater_array.getAvgLatency();
    CkCallback endCb(CkIndex_TestDriver::completeRun(), thisProxy);
   // CkCallback endCb(CkIndex_Updater::checkErrors(), updater_array);
    //updater_array.generateUpdatesVerify();
    CkStartQD(endCb);
    updater_array.lastFlush();
  }

  void completeRun() {
    update_walltime = CkWallTimer() - starttime;
    CkPrintf("   %8.3lf seconds\n", update_walltime);
    updater_array.getAvgLatency();
    CkCallback endCb(CkIndex_Updater::checkErrors(), updater_array);
    CkStartQD(endCb);

  }

  void printLatency(double latency_sum) {
//    CkPrintf("\nAvg latency = %lf/(%d*%d/128.0) = %lf s", latency_sum, l_num_req, CkNumPes(), latency_sum/((double)l_num_req*(double)CkNumPes()/128.0));
    CkPrintf("\nAvg latency = %lf s", latency_sum/((double)l_num_req*(double)CkNumPes()/128.0));
//    CkExit();
  }

  void ReceiveMsgStats(double* stats, int n) {
  }

  void reportErrors(CmiInt8 globalNumErrors) {
    CkPrintf("Found %" PRId64 " errors in %" PRId64 " locations (%s).\n", globalNumErrors,
             lnum_counts*CkNumPes(), globalNumErrors == 0 ?
             "passed" : "failed");
    start();
#ifndef VERIFY
    CkExit();
#endif
  }
};

// Chare Array with multiple chares on each PE
// Each chare: owns a portion of the global table
//             performs updates on its portion
//             generates random keys and sends them to the appropriate chares
class Updater : public CBase_Updater {
private:
  CmiInt8 *counts;
  CmiInt8 *table;
  CmiInt8 *index;
  CmiInt8 *pckindx;
  CmiInt8 *tgt;
  tram_proxy_t tram_req_proxy;
  tram_proxy_t tram_resp_proxy;
  tram_t* tram_req;
  tram_t* tram_resp;
  double* local_timestamps;
  double latency;
  CmiInt8 *count;

public:
  Updater(CkGroupID req_gid, CkGroupID resp_gid) {
    tram_req_proxy = CProxy_HTram(req_gid);
    tram_resp_proxy = CProxy_HTram(resp_gid);
    count = (CmiInt8*)malloc(l_num_req * sizeof(CmiInt8));
    // Compute table start for this chare
    //globalStartmyProc = thisIndex * localTableSize;
    // CkPrintf("[PE%d] Update (thisIndex=%d) created: ltab_siz = %d, l_num_req =%d\n", CkMyPe(), thisIndex, ltab_siz, l_num_req);

    // Create table;
    table = (CmiInt8*)malloc(sizeof(CmiInt8) * ltab_siz); assert(table != NULL);
    // Initialize
    for(CmiInt8 i = 0; i < ltab_siz; i++) {
      int index_k = thisIndex*ltab_siz + i;
      table[i] = (-1)*(index_k + 1);
      //CkPrintf("\n[PE-%d] table[%d = %d] = %d", thisIndex, i, index_k, table[i]);
    }
    index   =  (CmiInt8*)malloc(l_num_req * sizeof(CmiInt8)); assert(index != NULL);
    pckindx =  (CmiInt8*)malloc(l_num_req * sizeof(CmiInt8)); assert(pckindx != NULL);

    CmiInt8 indx, lindx, pe;
    CmiInt8 tab_siz = ltab_siz*CkNumPes();
    srand(thisIndex + 5);

    for(CmiInt8 i = 0; i < l_num_req; i++){
      indx = rand() % tab_siz;
      index[i] = indx;
      count[i] = 0;
//      if(index[i] < 0 || index[i] >= tab_siz) CkPrintf("\njunk");
//      CkPrintf("\nPE[%d] index[%d] = %d", thisIndex, i, index[i]);
      lindx = indx / CkNumPes();      // the distributed version of indx
      pe  = indx % CkNumPes();
      pckindx[i] = (lindx << 16) | (pe & 0xffff); // same thing stored as (local index, thread) "shmem style"
    }

    tgt  =  (CmiInt8*)calloc(l_num_req, sizeof(CmiInt8)); assert(tgt != NULL);
    local_timestamps = new double[4+ l_num_req/128];
    latency = 0.0;
    // Contribute to a reduction to signal the end of the setup phase
    contribute(CkCallback(CkReductionTarget(TestDriver, start), driverProxy));
  }

  Updater(CkMigrateMessage *msg) {}
  void lastFlush() {
    tram_resp->tflush();
  }

  inline void insertData2(const CmiInt8& key) {
    counts[key]--;
  }

  // Communication library calls this to deliver each randomly generated key
  inline void requestData(const packet1& p){//const CmiInt8& key) {
    packet1 p2;
    p2.val = table[p.val];
    
    p2.idx = p.idx;
    p2.pe = p.pe;
    //CkPrintf("\n[PE-%d]Responding with %d val for idx %d, the %dth request", thisIndex, p2.val, p.val, p2.idx);
//    CkPrintf("\nReceived request"); fflush(stdout);
    tram_resp->insertValue(p2, p.pe);
//    thisProxy[p.pe].responseData(p2);//myResponse(p2);
  }

  inline void responseData(const packet1& p){//const CmiInt8& key) {
    if(p.idx%128==0) latency += (CkWallTimer()-local_timestamps[p.idx/128]);
    tgt[p.idx] = p.val;
    packet1 p2;
    p2.val = pckindx[p.idx] >> 16;;
    p2.idx = p.idx;
    p2.pe = CkMyPe();
    int pe = pckindx[p.idx] & 0xffff;
    if(++count[p.idx]<3) tram_req->insertValue(p2, pe);
    //CkPrintf("\n[PE-%d]Received value tgt[%d] =  %d", thisIndex, p.idx, p.val);
    //if(p.idx%256==255) tram_resp->tflush();
  }

  static void requestDataCaller(void* p, packet1 key) {
    ((Updater *)p)->requestData(key);
  }

  static void responseDataCaller(void* p, packet1 key) {
    ((Updater *)p)->responseData(key);
  }
#if 0
  static void insertDataArrCaller(void* p, int* keys, int count) {
    for(int i=0;i<count;i++) {
      ((Updater *)p)->requesttData(keys[i]);
    }
  }
#endif
  
  void preGenerateUpdates(int buf_type, int buf_size, int agtype) {
    tram_req = tram_req_proxy.ckLocalBranch();
    tram_resp = tram_resp_proxy.ckLocalBranch();

    tram_req->set_func_ptr(Updater::requestDataCaller, this); //requestData
    tram_resp->set_func_ptr(Updater::responseDataCaller, this);
//    tram_req->reset_stats(2, 2048, PNs);
//    tram_resp->reset_stats(2, 2048, PNs);
//    CkPrintf("\nDone w preGen");
    //respondWData
//    tram->reset_stats(buf_type, buf_size, agtype);
#if 0//def RETURN_ITEMLIST
    tram->set_func_ptr_retarr(Updater::insertDataArrCaller, this);
#endif
    int idx = 0;
    contribute(sizeof(int), &idx, CkReduction::sum_int, CkCallback(CkReductionTarget(Updater, generateUpdates), thisProxy));
  }

  void getAvgLatency() {
    CkCallback cb(CkReductionTarget(TestDriver, printLatency), driverProxy);
    contribute(sizeof(double),&latency,CkReduction::sum_double, cb); 
  }


  void break_loop(int idx) {
    thisProxy[thisIndex].generateUpdates(idx);
  }

  void generateUpdates(int idx) {

  // Generate this chare's share of global updates
    CmiInt8 pe, col;

    //CkPrintf("[%d] Hi from generateUpdates %d, l_num_req: %d\n", CkMyPe(),thisIndex, l_num_req);
    packet1 p;
    for(CmiInt8 i = idx; i < l_num_req; i++){
      col = pckindx[i] >> 16;
      pe  = pckindx[i] & 0xffff;
      p.val = col;
      p.idx = i;
      p.pe = CkMyPe();
      if(i%128==0) local_timestamps[i/128] = CkWallTimer();
      // thisProxy[pe].requestData(p);//myRequest(p);
      //CkPrintf("\n[PE-%d] request for index %d from pe %d, as %dth request", thisIndex, col, pe, i);
      tram_req->insertValue(p, pe);
//      if(i%128==0) tram_req->tflush();

        // TODO: Test with something other than % or test with something equal to 2^n
      if  ((i % 5000) == 4999) CthYield();
//      if(i == l_num_req*0.75) tram_resp->enableIdleFlush();
    }
    //tram_resp->enableIdleFlush();
    tram_req->tflush();
//    CkPrintf("\n[PE-%d] Done sending latency = %lf/8 = %lf/# of msgs\n", thisIndex, latency, latency/8);
  }

  void generateUpdatesVerify() {
    // Generate this chare's share of global updates
    CmiInt8 pe, col;
    
    for(CmiInt8 i = 0; i < l_num_req; i++) {
      col = pckindx[i] >> 16;
      pe  = pckindx[i] & 0xffff;
      // Submit generated key to chare owning that portion of the table
      thisProxy[pe].insertData2(col);

      if  ((i % 8192) == 8191) CthYield();
    }
  }

  void checkErrors() {
    CmiInt8 numErrors = 0;
    for(CmiInt8 i=0; i<l_num_req; i++){
      int index_k = index[i];
      int l_indx =  index_k/CkNumPes();
      int _pe = index_k%CkNumPes();
      int index_i = _pe*ltab_siz + l_indx;
//      CkPrintf("\nEndPE[%d] index[%d] = %d", thisIndex, i, index[i]);
#if 1
      if(tgt[i] != (-1*index_i)-1){
        numErrors++;
        if(numErrors < 5)  // print first five errors, report all the errors
          fprintf(stderr,"ERROR: model %ld: Thread %d: tgt[%ld] = %ld != (idx=%d,%d) %ld)\n",
                  0,  CkMyPe(), i, tgt[i],index[i],index_i, ((-1)*index_i)-1);
        //use_model,  MYTHREAD, i, tgt[i],(-1)*(i*THREADS+MYTHREAD + 1) );
      }
#endif
      tgt[i] = 0;
    }
#if 0
    int flush_count = tram_req->flush_msg_count + tram_resp->flush_msg_count;
    int agg_count = tram_req->agg_msg_count + tram_req->agg_msg_count;
    if(flush_count || agg_count)
      CkPrintf("\nPE-%d, msg count = %d(agg), %d(flush)", thisIndex, agg_count, flush_count);
#endif
    // Sum the errors observed across the entire system
    contribute(sizeof(CmiInt8), &numErrors, CkReduction::sum_long,
               CkCallback(CkReductionTarget(TestDriver, reportErrors),
                          driverProxy));
  }
};

#include "smp_ig.def.h"
