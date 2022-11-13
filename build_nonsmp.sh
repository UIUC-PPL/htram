#!/bin/bash

set -x

N=100000
T=10000

while [[ $# -gt 0 ]]; do
    case $1 in
        --charm-dir)
            CHARMDIR="$2"
            shift
            shift
            ;;
        -n)
            N="$2"
            shift
            shift
            ;;
        -T)
            T="$2"
            shift
            shift
            ;;
        *)
            shift
            ;;
    esac
done

CHARMC=${CHARMDIR}/bin/charmc
CHARMRUN=${CHARMDIR}/bin/charmrun

${CHARMC} tramNonSmp.ci
${CHARMC} tramNonSmp.C -o libtramnonsmp.a -language charm++ -I$(pwd)

${CHARMC} histo_nonSmp.ci
${CHARMC} histo_nonSmp.C libtramnonsmp.a -o histo_nonsmp.out -O3 -language charm++ -I$(pwd)
# ${CHARMRUN} +p2 ./histo_nonsmp.out -n${N} -T${T}

${CHARMC} ig_nonSmp.ci
${CHARMC} ig_nonSmp.C libtramnonsmp.a -o ig_nonsmp.out -O3 -language charm++ -I$(pwd)
# ${CHARMRUN} +p2 ./ig_nonsmp.out -n${N} -T${T}
