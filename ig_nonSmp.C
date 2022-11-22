#include "NDMeshStreamer.h"

typedef CmiUInt8 dtype;
#include "packet.h"
#include "ig_nonSmp.decl.h"
#include "TopoManager.h"
#include "tramNonSmp.h"

#include <assert.h>
// Handle to the test driver (chare)
CProxy_TestDriver driverProxy;
/* readonly */ CProxy_tramNonSmp<packet1> tram_request_proxy;
/* readonly */ CProxy_tramNonSmp<packet2> tram_response_proxy;

int ltab_siz = 100000;
int l_num_req  = 1000000;      // number of requests per thread
int l_buffer_size = 1024;
int l_flush_timer = 500;

class TestDriver : public CBase_TestDriver {
private:
  CProxy_Updater  updater_array;
  double starttime;
  CmiInt8 tableSize;

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
    CkPrintf("TRAM Timed Flush enabled with flushes every %f us.\n", static_cast<double>(l_flush_timer)/1000);

    driverProxy = thishandle;
    tram_request_proxy = CProxy_tramNonSmp<packet1>::ckNew(l_buffer_size, static_cast<double>(l_flush_timer)/1000);
    tram_response_proxy = CProxy_tramNonSmp<packet2>::ckNew(l_buffer_size, static_cast<double>(l_flush_timer)/1000);
    // Create the chares storing and updating the global table
    //
    //updater_array = CProxy_Updater::ckNew(CkNumPes() * numElementsPerPe);
    updater_array = CProxy_Updater::ckNew(CkNumPes());
    int dims[2] = {CkNumNodes(), CkNumPes() / CkNumNodes()};
    CkPrintf("Aggregation topology: %d %d\n", dims[0], dims[1]);

    delete args;
  }

  void start() {
    starttime = CkWallTimer();
    CkCallback endCb(CkIndex_TestDriver::startVerificationPhase(), thisProxy);
    updater_array.generateUpdates();
    CkStartQD(endCb);
  }

  void startVerificationPhase() {
    double update_walltime = CkWallTimer() - starttime;
    CkPrintf("  %8.3lf seconds\n", update_walltime);

    // Repeat the update process to verify
    // At the end of the second update phase, check the global table
    //  for errors in Updater::checkErrors()
    CkCallback endCb(CkIndex_Updater::checkErrors(), updater_array);
    //updater_array.generateUpdatesVerify();
    CkStartQD(endCb);
  }

  void reportErrors(CmiInt8 globalNumErrors) {
    CkPrintf("Found %" PRId64 " errors in %" PRId64 " locations (%s).\n", globalNumErrors,
             ltab_siz*CkNumPes(), globalNumErrors <= 0.01 * tableSize ?
             "passed" : "failed");
    CkExit();
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

  // Tram local branches
  tramNonSmp<packet1>* tram_request;
  tramNonSmp<packet2>* tram_response;
public:
  Updater() {
    // Compute table start for this chare
    //globalStartmyProc = thisIndex * localTableSize;
    // CkPrintf("[PE%d] Update (thisIndex=%d) created: ltab_siz = %d, l_num_req =%d\n", CkMyPe(), thisIndex, ltab_siz, l_num_req);

    // Create table;
    table = (CmiInt8*)malloc(sizeof(CmiInt8) * ltab_siz); assert(table != NULL);
    // Initialize
    for(CmiInt8 i = 0; i < ltab_siz; i++) {
      table[i] = (-1)*(i*CkNumPes() + CkMyPe() + 1);
    }
    index   =  (CmiInt8*)malloc(l_num_req * sizeof(CmiInt8)); assert(index != NULL);
    pckindx =  (CmiInt8*)malloc(l_num_req * sizeof(CmiInt8)); assert(pckindx != NULL);

    CmiInt8 indx, lindx, pe;
    CmiInt8 tab_siz = ltab_siz*CkNumPes();
    srand(thisIndex + 5);

    for(CmiInt8 i = 0; i < l_num_req; i++){
      indx = rand() % tab_siz;
      index[i] = indx;
      lindx = indx / CkNumPes();      // the distributed version of indx
      pe  = indx % CkNumPes();
      pckindx[i] = (lindx << 16) | (pe & 0xffff); // same thing stored as (local index, thread) "shmem style"
    }

    tgt  =  (CmiInt8*)calloc(l_num_req, sizeof(CmiInt8)); assert(tgt != NULL);

    tram_request = tram_request_proxy.ckLocalBranch();
    tram_response = tram_response_proxy.ckLocalBranch();

    // Contribute to a reduction to signal the end of the setup phase
    contribute(CkCallback(CkReductionTarget(TestDriver, start), driverProxy));
  }

  Updater(CkMigrateMessage *msg) {}

  // Communication library calls this to deliver each randomly generated key
  inline void myRequest(const packet1& p) {
    packet2 p2;
    p2.val = table[p.val];
    p2.idx = p.idx;

    tram_response->insertValue(p2, p.pe);
    // thisProxy(p.pe).myResponse(p2);
  }

  static void myRequestCaller(void* obj_ptr, packet1 p) {
    ((Updater *)obj_ptr)->myRequest(p);
  }

  inline void myResponse(const packet2& p) {
    tgt[p.idx] = p.val;
  }

  static void myResponseCaller(void* obj_ptr, packet2 p) {
    ((Updater *)obj_ptr)->myResponse(p);
  }

  void generateUpdates() {
    // Generate this chare's share of global updates
    CmiInt8 pe, col;
    tram_request->set_func_ptr(Updater::myRequestCaller, this);
    tram_response->set_func_ptr(Updater::myResponseCaller, this);
    contribute(0, NULL, CkReduction::nop, CkCallbackResumeThread());

    //CkPrintf("[%d] Hi from generateUpdates %d, l_num_ups: %d\n", CkMyPe(),thisIndex, l_num_req);
    packet1 p;
    for(CmiInt8 i = 0; i < l_num_req; i++){
      col = pckindx[i] >> 16;
      pe  = pckindx[i] & 0xffff;
      p.val = col;
      p.idx = i;
      p.pe = CkMyPe();
    //   thisProxy(pe).myRequest(p);
      tram_request->insertValue(p, pe);

        // TODO: Test with something other than % or test with something equal to 2^n
      if  ((i % 10000) == 9999) CthYield();
    }
    tram_request->tflush();
  }

  void checkErrors() {
    CmiInt8 numErrors = 0;
    for(CmiInt8 i=0; i<l_num_req; i++){
      if(tgt[i] != (-1)*(index[i] + 1)){
        numErrors++;
        if(numErrors < 5)  // print first five errors, report all the errors
          fprintf(stderr,"ERROR: model %ld: Thread %d: tgt[%ld] = %ld != %ld)\n",
                  0,  CkMyPe(), i, tgt[i], (-1)*(index[i] + 1));
        //use_model,  MYTHREAD, i, tgt[i],(-1)*(i*THREADS+MYTHREAD + 1) );
      }
      tgt[i] = 0;
    }
    // Sum the errors observed across the entire system
    contribute(sizeof(CmiInt8), &numErrors, CkReduction::sum_long,
               CkCallback(CkReductionTarget(TestDriver, reportErrors),
                          driverProxy));
  }
};


#include "ig_nonSmp.def.h"
