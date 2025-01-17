//**************************************************************************\
//* This file is property of and copyright by the ALICE Project            *\
//* ALICE Experiment at CERN, All rights reserved.                         *\
//*                                                                        *\
//* Primary Authors: Matthias Richter <Matthias.Richter@ift.uib.no>        *\
//*                  for The ALICE HLT Project.                            *\
//*                                                                        *\
//* Permission to use, copy, modify and distribute this software and its   *\
//* documentation strictly for non-commercial purposes is hereby granted   *\
//* without fee, provided that the above copyright notice appears in all   *\
//* copies and that both the copyright notice and this permission notice   *\
//* appear in the supporting documentation. The authors make no claims     *\
//* about the suitability of this software for any purpose. It is          *\
//* provided "as is" without express or implied warranty.                  *\
//**************************************************************************

/// \file GPUTPCCFNoiseSuppression.cxx
/// \author Felix Weiglhofer

#include "GPUTPCCFNoiseSuppression.h"
#include "Array2D.h"
#include "CfConsts.h"
#include "CfUtils.h"
#include "ChargePos.h"

using namespace GPUCA_NAMESPACE::gpu;
using namespace GPUCA_NAMESPACE::gpu::tpccf;

template <>
GPUdii() void GPUTPCCFNoiseSuppression::Thread<GPUTPCCFNoiseSuppression::noiseSuppression>(int nBlocks, int nThreads, int iBlock, int iThread, GPUSharedMemory& smem, processorType& clusterer)
{
  Array2D<PackedCharge> chargeMap(reinterpret_cast<PackedCharge*>(clusterer.mPchargeMap));
  Array2D<uchar> isPeakMap(clusterer.mPpeakMap);
  noiseSuppressionImpl(get_num_groups(0), get_local_size(0), get_group_id(0), get_local_id(0), smem, clusterer.Param().rec, chargeMap, isPeakMap, clusterer.mPpeakPositions, clusterer.mPmemory->counters.nPeaks, clusterer.mPisPeak);
}

template <>
GPUdii() void GPUTPCCFNoiseSuppression::Thread<GPUTPCCFNoiseSuppression::updatePeaks>(int nBlocks, int nThreads, int iBlock, int iThread, GPUSharedMemory& smem, processorType& clusterer)
{
  Array2D<uchar> isPeakMap(clusterer.mPpeakMap);
  updatePeaksImpl(get_num_groups(0), get_local_size(0), get_group_id(0), get_local_id(0), clusterer.mPpeakPositions, clusterer.mPisPeak, clusterer.mPmemory->counters.nPeaks, isPeakMap);
}

GPUdii() void GPUTPCCFNoiseSuppression::noiseSuppressionImpl(int nBlocks, int nThreads, int iBlock, int iThread, GPUSharedMemory& smem,
                                                             const GPUSettingsRec& calibration,
                                                             const Array2D<PackedCharge>& chargeMap,
                                                             const Array2D<uchar>& peakMap,
                                                             const ChargePos* peakPositions,
                                                             const uint peaknum,
                                                             uchar* isPeakPredicate)
{
  SizeT idx = get_global_id(0);

  ChargePos pos = peakPositions[CAMath::Min(idx, (SizeT)(peaknum - 1))];
  Charge charge = chargeMap[pos].unpack();

  ulong minimas, bigger, peaksAround;
  findMinimaAndPeaks(
    chargeMap,
    peakMap,
    calibration,
    charge,
    pos,
    smem.posBcast,
    smem.buf,
    &minimas,
    &bigger,
    &peaksAround);

  peaksAround &= bigger;

  bool keepMe = keepPeak(minimas, peaksAround);

  bool iamDummy = (idx >= peaknum);
  if (iamDummy) {
    return;
  }

  isPeakPredicate[idx] = keepMe;
}

GPUd() void GPUTPCCFNoiseSuppression::updatePeaksImpl(int nBlocks, int nThreads, int iBlock, int iThread,
                                                      const ChargePos* peakPositions,
                                                      const uchar* isPeak,
                                                      const uint peakNum,
                                                      Array2D<uchar>& peakMap)
{
  SizeT idx = get_global_id(0);

  if (idx >= peakNum) {
    return;
  }

  ChargePos pos = peakPositions[idx];

  uchar peak = isPeak[idx];

  peakMap[pos] = 0b10 | peak; // if this positions was marked as peak at some point, then its charge must exceed the charge threshold.
                              // So we can just set the bit and avoid rereading the charge
}

GPUdi() void GPUTPCCFNoiseSuppression::checkForMinima(
  float q,
  float epsilon,
  PackedCharge other,
  int pos,
  ulong* minimas,
  ulong* bigger)
{
  float r = other.unpack();

  ulong isMinima = (q - r > epsilon);
  *minimas |= (isMinima << pos);

  ulong lq = (r > q);
  *bigger |= (lq << pos);
}

GPUdi() void GPUTPCCFNoiseSuppression::findMinima(
  const PackedCharge* buf,
  const ushort ll,
  const int N,
  int pos,
  const float q,
  const float epsilon,
  ulong* minimas,
  ulong* bigger)
{
  GPUCA_UNROLL(U(), U())
  for (int i = 0; i < N; i++, pos++) {
    PackedCharge other = buf[N * ll + i];

    checkForMinima(q, epsilon, other, pos, minimas, bigger);
  }
}

GPUdi() void GPUTPCCFNoiseSuppression::findPeaks(
  const uchar* buf,
  const ushort ll,
  const int N,
  int pos,
  ulong* peaks)
{
  GPUCA_UNROLL(U(), U())
  for (int i = 0; i < N; i++, pos++) {
    ulong p = CfUtils::isPeak(buf[N * ll + i]);

    *peaks |= (p << pos);
  }
}

GPUdi() bool GPUTPCCFNoiseSuppression::keepPeak(
  ulong minima,
  ulong peaks)
{
  bool keepMe = true;

  GPUCA_UNROLL(U(), U())
  for (int i = 0; i < NOISE_SUPPRESSION_NEIGHBOR_NUM; i++) {
    bool otherPeak = (peaks & (ulong(1) << i));
    bool minimaBetween = (minima & cfconsts::NoiseSuppressionMinima[i]);

    keepMe &= (!otherPeak || minimaBetween);
  }

  return keepMe;
}

GPUd() void GPUTPCCFNoiseSuppression::findMinimaAndPeaks(
  const Array2D<PackedCharge>& chargeMap,
  const Array2D<uchar>& peakMap,
  const GPUSettingsRec& calibration,
  float q,
  const ChargePos& pos,
  ChargePos* posBcast,
  PackedCharge* buf,
  ulong* minimas,
  ulong* bigger,
  ulong* peaks)
{
  ushort ll = get_local_id(0);

  posBcast[ll] = pos;
  GPUbarrier();

  ushort wgSizeHalf = (SCRATCH_PAD_WORK_GROUP_SIZE + 1) / 2;

  bool inGroup1 = ll < wgSizeHalf;
  ushort llhalf = (inGroup1) ? ll : (ll - wgSizeHalf);

  *minimas = 0;
  *bigger = 0;
  *peaks = 0;

  /**************************************
   * Look for minima
   **************************************/

  CfUtils::blockLoad(
    chargeMap,
    SCRATCH_PAD_WORK_GROUP_SIZE,
    SCRATCH_PAD_WORK_GROUP_SIZE,
    ll,
    16,
    2,
    cfconsts::NoiseSuppressionNeighbors,
    posBcast,
    buf);

  findMinima(
    buf,
    ll,
    2,
    16,
    q,
    calibration.tpc.cfNoiseSuppressionEpsilon,
    minimas,
    bigger);

  CfUtils::blockLoad(
    chargeMap,
    wgSizeHalf,
    SCRATCH_PAD_WORK_GROUP_SIZE,
    ll,
    0,
    16,
    cfconsts::NoiseSuppressionNeighbors,
    posBcast,
    buf);

  if (inGroup1) {
    findMinima(
      buf,
      llhalf,
      16,
      0,
      q,
      calibration.tpc.cfNoiseSuppressionEpsilon,
      minimas,
      bigger);
  }

  CfUtils::blockLoad(
    chargeMap,
    wgSizeHalf,
    SCRATCH_PAD_WORK_GROUP_SIZE,
    ll,
    18,
    16,
    cfconsts::NoiseSuppressionNeighbors,
    posBcast,
    buf);

  if (inGroup1) {
    findMinima(
      buf,
      llhalf,
      16,
      18,
      q,
      calibration.tpc.cfNoiseSuppressionEpsilon,
      minimas,
      bigger);
  }

#if defined(GPUCA_GPUCODE)
  CfUtils::blockLoad(
    chargeMap,
    wgSizeHalf,
    SCRATCH_PAD_WORK_GROUP_SIZE,
    ll,
    0,
    16,
    cfconsts::NoiseSuppressionNeighbors,
    posBcast + wgSizeHalf,
    buf);

  if (!inGroup1) {
    findMinima(
      buf,
      llhalf,
      16,
      0,
      q,
      calibration.tpc.cfNoiseSuppressionEpsilon,
      minimas,
      bigger);
  }

  CfUtils::blockLoad(
    chargeMap,
    wgSizeHalf,
    SCRATCH_PAD_WORK_GROUP_SIZE,
    ll,
    18,
    16,
    cfconsts::NoiseSuppressionNeighbors,
    posBcast + wgSizeHalf,
    buf);

  if (!inGroup1) {
    findMinima(
      buf,
      llhalf,
      16,
      18,
      q,
      calibration.tpc.cfNoiseSuppressionEpsilon,
      minimas,
      bigger);
  }
#endif

  uchar* bufp = (uchar*)buf;

  /**************************************
     * Look for peaks
     **************************************/

  CfUtils::blockLoad(
    peakMap,
    SCRATCH_PAD_WORK_GROUP_SIZE,
    SCRATCH_PAD_WORK_GROUP_SIZE,
    ll,
    0,
    16,
    cfconsts::NoiseSuppressionNeighbors,
    posBcast,
    bufp);

  findPeaks(
    bufp,
    ll,
    16,
    0,
    peaks);

  CfUtils::blockLoad(
    peakMap,
    SCRATCH_PAD_WORK_GROUP_SIZE,
    SCRATCH_PAD_WORK_GROUP_SIZE,
    ll,
    18,
    16,
    cfconsts::NoiseSuppressionNeighbors,
    posBcast,
    bufp);

  findPeaks(
    bufp,
    ll,
    16,
    18,
    peaks);
}
