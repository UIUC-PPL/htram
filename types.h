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

#endif
