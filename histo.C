#include "NDMeshStreamer.h"

typedef CmiUInt8 dtype;
#include "TopoManager.h"
#include "histo.decl.h"

#include <assert.h>
#define SIZES 3
#define PHASE_COUNT 1//12
// Handle to the test driver (chare)
CProxy_TestDriver driverProxy;

int l_num_ups = 1000000;     // per thread number of requests (updates)
int lnum_counts = 1000;       // per thread size of the table
int l_buffer_size = 1024;
bool enable_buffer_flushing = false;
int l_flush_timer = 500;
bool return_item = true;

#ifdef TRAM_SMP
#if GROUPBY
#include "htram_group.h"
#elif SORTBY
#include "htram_sort.h"
#else
#include "htram.h"
#endif

using tram_proxy_t = CProxy_HTram;
using tram_t = HTram;

/* readonly */ CProxy_HTramRecv nodeGrpProxy;
/* readonly */ CProxy_HTramNodeGrp srcNodeGrpProxy;
#elif TRAM_NON_SMP
#include "tramNonSmp.h"

using tram_proxy_t = CProxy_tramNonSmp<int>;
using tram_t = tramNonSmp<int>;
#endif


class TestDriver : public CBase_TestDriver {
private:
  CProxy_Updater  updater_array;
  tram_proxy_t tram_proxy;
  double starttime;

public:
  TestDriver(CkArgMsg* args) {
    int64_t printhelp = 0;
    int opt;

    while( (opt = getopt(args->argc, args->argv, "hen:T:S:t:")) != -1 ) {
      switch(opt) {
      case 'h': printhelp = 1; break;
      case 'e': enable_buffer_flushing = true; break;
      case 'n': sscanf(optarg,"%d" ,&l_num_ups);  break;
      case 'T': sscanf(optarg,"%d" ,&lnum_counts);  break;
      case 'S': sscanf(optarg, "%d", &l_buffer_size); break;
      case 't': sscanf(optarg, "%d", &l_flush_timer); break;
      default:  break;
      }
    }
    assert(sizeof(CmiInt8) == sizeof(int64_t));
    CkPrintf("Running histo on %d PEs\n", CkNumPes());
    CkPrintf("Number updates / PE              (-n)= %d\n", l_num_ups);
    CkPrintf("Table size / PE                  (-T)= %d\n", lnum_counts);
    CkPrintf("TRAM Buffer Size                 (-S)= %d\n", l_buffer_size);
    if (enable_buffer_flushing) {
      CkPrintf("TRAM Timed Flush enabled with flushes every %f us.\n", static_cast<double>(l_flush_timer)/1000);
    }

    driverProxy = thishandle;
//    updater_array = CProxy_Updater::ckNew();

    int dims[2] = {CkNumNodes(), CkNumPes() / CkNumNodes()};
    CkPrintf("Aggregation topology: %d %d\n", dims[0], dims[1]);

    // Initialize TRAM with appropriate arguments
//    CkGroupID updater_array_gid;
//    updater_array_gid = updater_array.ckGetGroupID();

#ifdef TRAM_SMP
    nodeGrpProxy = CProxy_HTramRecv::ckNew();
    srcNodeGrpProxy = CProxy_HTramNodeGrp::ckNew();
    CkCallback start_cb(CkReductionTarget(TestDriver, start), driverProxy);
    tram_proxy = tram_proxy_t::ckNew(nodeGrpProxy.ckGetGroupID(), srcNodeGrpProxy.ckGetGroupID(), l_buffer_size, enable_buffer_flushing, static_cast<double>(l_flush_timer)/1000, return_item,true, start_cb);
#endif
    updater_array = CProxy_Updater::ckNew(tram_proxy.ckGetGroupID(), 42);
    delete args;
  }

  int count = 0;
  void start() {
    count++;
    if(count == 2) {
      updater_array.preGenerateUpdates();
    } else if(count == 3) {
      starttime = CkWallTimer();
      CkCallback endCb(CkIndex_TestDriver::startVerificationPhase(), thisProxy);
      updater_array.generateUpdates();
      CkStartQD(endCb);
    }
  }
  int phase = 0;
  double update_walltime;

//#define VERIFY
  void startVerificationPhase() {
    update_walltime = CkWallTimer() - starttime;
    
    CkPrintf("   %8.3lf seconds\n", update_walltime);
      CkCallback endCb(CkIndex_Updater::checkErrors(), updater_array);
      updater_array.generateUpdatesVerify();
      CkStartQD(endCb);
  }


  void reportErrors(CmiInt8 globalNumErrors) {
    CkPrintf("Found %" PRId64 " errors in %" PRId64 " locations (%s).\n", globalNumErrors,
             lnum_counts*CkNumPes(), globalNumErrors == 0 ?
             "passed" : "failed");
//    start();
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
  CmiInt8 *index;
  CmiInt8 *pckindx;
  CmiInt8 num_counts;
  tram_proxy_t tram_proxy;
  tram_t* tram;
  int count;
public:
  Updater(CkGroupID tram_id, int k) {
    count = 0;
    tram_proxy = CProxy_HTram(tram_id);
  
    // Compute table start for this chare
    // CkPrintf("[PE%d] Update (thisIndex=%d) created: lnum_counts = %d, l_num_ups =%d\n", CkMyPe(), thisIndex, lnum_counts, l_num_ups);

    srand(thisIndex + 120348);
    // Create table;
    counts = (CmiInt8*)malloc(sizeof(CmiInt8) * lnum_counts); assert(counts != NULL);
    // Initialize
    for(CmiInt8 i = 0; i < lnum_counts; i++) {
      counts[i] = 0;
    }
    index = (CmiInt8 *) malloc(l_num_ups * sizeof(CmiInt8)); assert(index != NULL);
    pckindx = (CmiInt8 *) malloc(l_num_ups * sizeof(CmiInt8)); assert(pckindx != NULL);
  
    num_counts = lnum_counts * CkNumPes();
    CmiInt8 indx, lindx, pe;
    for(CmiInt8 i = 0; i < l_num_ups; i++) {
      //indx = i % num_counts;          //might want to do this for debugging
      indx = rand() % num_counts;
      index[i] = indx;
      lindx = indx / CkNumPes();
      pe  = indx % CkNumPes();
      pckindx[i]  =  (lindx << 16L) | (pe & 0xffff);
    }
    // Contribute to a reduction to signal the end of the setup phase
    contribute(CkCallback(CkReductionTarget(TestDriver, start), driverProxy));
  }

  Updater(CkMigrateMessage *msg) {}

  // Communication library calls this to deliver each randomly generated key
  inline void insertData(const CmiInt8& key) {
    counts[key]++;
#if 0
    CmiInt8 indx = rand() % num_counts;
    CmiInt8 lindx = indx / CkNumPes();
    CmiInt8 pe  = indx % CkNumPes();

    if(++count < l_num_ups*8) tram->insertValue(lindx, pe);
    if(count%l_num_ups==0) tram->tflush();
#endif
  }

  inline void insertData2(const CmiInt8& key) {
    counts[key]--;
  }

  static void insertDataCaller(void* p, int key) {
    ((Updater *)p)->insertData(key);
  }

  static void insertDataArrCaller(void* p, int* keys, int count) {
    for(int i=0;i<count;i++) {
      ((Updater *)p)->insertData(keys[i]);
    }
  }

  void preGenerateUpdates() {
    tram = tram_proxy.ckLocalBranch();
    tram->set_func_ptr(Updater::insertDataCaller, this);
//    tram->reset_stats(buf_type, buf_size, agtype);
#ifdef RETURN_ITEMLIST
    tram->set_func_ptr_retarr(Updater::insertDataArrCaller, this);
#endif

    contribute(CkCallback(CkReductionTarget(TestDriver, start), driverProxy));
    //contribute(CkCallback(CkReductionTarget(Updater, generateUpdates), thisProxy));
  }

  void generateUpdates() {
    // Generate this chare's share of global updates
    CmiInt8 pe, col;

    for(CmiInt8 i = 0; i < l_num_ups; i++) {
      col = pckindx[i] >> 16;
      pe  = pckindx[i] & 0xffff;
      // Submit generated key to chare owning that portion of the table
      tram->insertValue(col, pe);

      if  ((i % 2048) == 2047) {/*tram->tflush();*/ CthYield();}
    }
    tram->tflush();
  }

  void generateUpdatesVerify() {
    // Generate this chare's share of global updates
    CmiInt8 pe, col;
    
    for(CmiInt8 i = 0; i < l_num_ups; i++) {
      col = pckindx[i] >> 16;
      pe  = pckindx[i] & 0xffff;
      // Submit generated key to chare owning that portion of the table
      thisProxy[pe].insertData2(col);

      if  ((i % 8192) == 8191) CthYield();
    }
  }

  void checkErrors() {
    CmiInt8 numErrors = 0;
#if 1
    for(CmiInt8 i = 0; i < lnum_counts; i++) {
      if(counts[i] != 0L) {
        numErrors++;
        if(numErrors < 5)  // print first five errors, report number of errors below
          fprintf(stderr,"ERROR: Thread %d error at %ld (= %ld)\n", CkMyPe(), i, counts[i]);
      }
    }
#endif
#if 0
    int flush_count = tram->flush_msg_count;
    int agg_count = tram->agg_msg_count;
    if(flush_count || agg_count)
      CkPrintf("\nPE-%d, msg count = %d(agg), %d(flush), data sent = %d", thisIndex, agg_count, flush_count, count);
#endif
    // Sum the errors observed across the entire system
    contribute(sizeof(CmiInt8), &numErrors, CkReduction::sum_long,
               CkCallback(CkReductionTarget(TestDriver, reportErrors),
                          driverProxy));
  }
};

#include "histo.def.h"
