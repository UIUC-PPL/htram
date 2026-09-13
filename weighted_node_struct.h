#include "NDMeshStreamer.h"


class Edge{
        public:
                int end;
                int distance;
        Edge(){}
        void pup(PUP::er &p) {
        p | end;
        p | distance;
        }
};

class Node{
	public:
		int index;
		int home_process;
		int distance;
		CkVec<Edge> adjacent;
	
	Node(){}
	explicit operator bool() const
      	{ return index != NULL; }
    	void pup(PUP::er &p) {
      	p | index;
      	p | home_process;
	p | distance;
      	p | adjacent;
    	}
};

class LongEdge
{
	public:
		int begin;
                int end;
                int distance;
	LongEdge(){}
        void pup(PUP::er &p) {
        p | begin;
	p | end;
        p | distance;
        }
};


template <>
struct is_PUPbytes<std::pair<int,int>> {
  static const bool value = true;
};

