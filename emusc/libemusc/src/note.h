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


#ifndef __NOTE_H__
#define __NOTE_H__


#include "control_rom.h"
#include "partial.h"
#include "settings.h"
#include "wave_generator.h"
#include "wave_rom.h"

#include <stdint.h>

#include <array>


namespace EmuSC {

class Note
{
public:
  /* portaSrc: per-partial glide source pitch (key * 1000 scale) or INT32_MIN
   * for no glide. May be NULL. */
  Note(uint8_t key, uint8_t velocity, ControlRom &ctrlRom, WaveRom &waveRom,
       Settings *settings, int8_t partId, const int *portaSrc = 0);
  ~Note();

  void stop(void);
  void stop(uint8_t key);
  void sustain(bool state);

  void update(void);

  bool get_sample_set(std::array<std::array<float, 256>, 2> &dryBus);

  int get_num_partials(void);

  int get_current_pitch(bool partial);
  int get_current_tvf(bool partial);
  int get_current_tva(bool partial);
  int get_current_lfo(int lfo);

  /* PRISM: voice stealing and portamento helpers */
  inline uint32_t seq(void) const { return _seq; }
  bool releasing(void);          // partials released (note off reached them)
  int  tva_max(void);            // loudest partial's current TVA level
  int  porta_base(int partial);  // partial's base pitch; INT32_MIN if absent
  int  porta_now(int partial);   // base plus any glide still in progress

private:
  uint8_t _key;

  bool _sustain;
  bool _stopped;
  bool _noteOff;                 // stop() reached the partials
  uint32_t _seq;                 // creation order, for last-note priority

  const double _7bScale;     // Constant: 1 / 127

  WaveGenerator *_LFO1;

  struct Partial *_partial[2];

  Settings *_settings;
  int8_t _partId;
};

}

#endif  // __NOTE_H__
