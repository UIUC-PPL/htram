#!/bin/bash
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=12
#SBATCH --cpus-per-task=1
#SBATCH --output=histo_nonsmp.out
#SBATCH --partition=cpu
#SBATCH --account=mzu-delta-cpu
#SBATCH --job-name=histo_nonsmp
#SBATCH --exclusive
#SBATCH --time=00:10:00      # hh:mm:ss for the job

module load PrgEnv-cray
module load craype-x86-milan
module load cray-pmi
module load cray-fftw
srun -n 12 ./histo_nonSmp -n 10000000 -T 100000