/*
 *  This file is part of libEmuSC, a Sound Canvas emulator library
 *  Copyright (C) 2022-2026  Håkon Skjelten
 *
 *  libEmuSC is free software: you can redistribute it and/or modify it
 *  under the terms of the GNU Lesser General Public License as published
 *  by the Free Software Foundation, either version 2.1 of the License, or
 *  (at your option) any later version.
 *
 *  libEmuSC is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with libEmuSC. If not, see <http://www.gnu.org/licenses/>.
 */


#include "wave_oscillator.h"

#include <algorithm>
#include <iostream>


namespace EmuSC {


WaveOscillator::WaveOscillator(ControlRom::Sample *ctrlSample,
                               std::vector<float> *pcmSamples,
                               std::function<void(void)> cb, bool portamento)
  : _pcmSamples(pcmSamples),
    _phase(0.0f),
    _loopMode{ctrlSample->loopMode},
    _firstRunCompleteCallback(cb),
    _firstRunComplete(false)
{
  // A gliding (portamento / legato) note skips the attack transient and
  // starts at the offset stored with the sample control data
  _sampleStart = 0;
  if (portamento && ctrlSample->portaOffset < ctrlSample->sampleLen)
    _sampleStart = ctrlSample->portaOffset;

  _sampleEnd = ctrlSample->sampleLen;
  _loopStart = _sampleEnd - ctrlSample->loopLen;
  _loopLength = ctrlSample->loopLen;

  if (_loopMode == LoopMode::PingPong) {
    _sampleEnd = ctrlSample->sampleLen + _loopLength + 1;
    _loopStart = _sampleEnd - 2 * ctrlSample->loopLen - 1;
  }

  if (_loopStart < 0)
    _loopStart = 0;

  _index = _sampleStart;
}


void WaveOscillator::get_sample_set(Pitch *pitch, float pitchBend,
                                    std::array<float, 256> &dryBus)
{
  /* Reads stay within the decoded sample set by construction: indexes wrap
   * to _loopStart before they pass _sampleEnd, which is the last entry. */
  const float *smp = _pcmSamples->data();
  const int end = _sampleEnd, loop = _loopStart;
  int idx = _index;
  float phase = _phase;

  for (int i = 0; i < 256; i++) {
    // Interpolation based on information from the Nuked-SC55 project:
    // the hardware uses the top 7 bits of the fractional phase
    int i1 = idx + 1 > end ? loop : idx + 1;
    int i2 = i1 + 1 > end ? loop : i1 + 1;
    int i3 = i2 + 1 > end ? loop : i2 + 1;
    float s0 = smp[idx], s1 = smp[i1], s2 = smp[i2], s3 = smp[i3];

    int r = static_cast<int>(phase * 128.0f) & 127;
    constexpr float q = 1.0f / 4096.0f;
    float c0 = _interpolationLUT[0][r] * q;
    float c1 = _interpolationLUT[1][r] * q;
    float c2 = _interpolationLUT[2][r] * q;

    dryBus[i] = s0 + c0 * (s1 - s0) + c1 * (s2 - s1) + c2 * (s3 - s2);

    phase += pitchBend * pitch->get_phase_increment() / 16384.0f;
    while (phase >= 1.0f) {
      phase -= 1.0f;

      if (!_firstRunComplete && idx >= end) {
        _firstRunComplete = true;
        if (_firstRunCompleteCallback) _firstRunCompleteCallback();
      }

      idx++;
      if (idx > end)
	idx = loop;
    }
  }

  _index = idx;
  _phase = phase;
}


} // namespace EmuSC
