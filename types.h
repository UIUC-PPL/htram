/*Defines the unionfind data structures for tramlib*/
#ifndef TYPES_H
#define TYPES_H

struct findBossData {
    uint64_t arrIdx;
    uint64_t partnerOrBossID;
    uint64_t senderID;
    uint64_t isFBOne;

    void pup(PUP::er &p) {
        p|arrIdx;
        p|partnerOrBossID;
        p|senderID;
        p|isFBOne;
    }
};

#ifdef ANCHOR_ALGO
struct anchorData {
    uint64_t arrIdx;
    uint64_t v;

    void pup(PUP::er &p) {
        p|arrIdx;
        p|v;
    }
};
#endif

struct shortCircuitData {
    uint64_t arrIdx;
    uint64_t grandparentID;

    void pup(PUP::er &p) {
        p|arrIdx;
        p|grandparentID;
    }
};

typedef long cost;

class Edge{
	public:
		long end;
		cost distance;
	Edge(){}
	void pup(PUP::er &p) 
	{
		p | end;
		p | distance;
	}
};

class Update{
	public:
		long dest_vertex;
		cost distance;
	Update(){}
	void pup(PUP::er &p) 
	{
		p | dest_vertex;
		p | distance;
	}
};

class Node{
	public:
		int home_process;
		cost distance;
		std::vector<Edge> adjacent;
	
	Node(){}
	void pup(PUP::er &p) 
	{
		p | home_process;
		p | distance;
		p | adjacent;
	}
};

class LongEdge
{
	public:
		long begin;
        long end;
        cost distance;
	LongEdge(){}
	void pup(PUP::er &p) 
	{
		p | begin;
		p | end;
		p | distance;
	}
};


#endif
