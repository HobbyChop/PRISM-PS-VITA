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


#ifndef __SYNTH_H__
#define __SYNTH_H__


#include "control_rom.h"
#include "params.h"
#include "resampler.h"
#include "system_effects.h"
#include "wave_rom.h"

#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>


/* Public API for libEmuSC
 *
 * This Synth class is responsible for receiving MIDI events from client,
 * process the event based on information in Control and Wave ROMS, and
 * finally responding to audio buffer requests.
 *
 * Synth class constructor depends on a valid Control Rom and Wave ROM.
 *
 * MIDI events is sent to the emulator via the midi_input() method using the
 * three bytes from raw MIDI events.
 * 
 * Audio samples are extracted by calling the get_next_frame() method. This
 * is typically done from a callback function triggered by the OS audio
 * driver when the audio buffer is running low.
 *
 * All settings are configured through the Settings class.
 */


namespace EmuSC {

class Part;
class Settings;

/* PRISM: optional host-supplied worker pool. When set, part rendering is dealt
 * across the host's threads: job(arg, k) is called for k in [0, count) and the
 * call returns when every job is done. The host owns the threads. */
extern "C" {
extern void (*emusc_parallel_run)(void (*job)(void *, int), void *arg, int count);
extern int emusc_parallel_count;
}

class Synth
{
public:

  enum class SoundMap {
    GS,                       // Default GS settings
    GS_GM,                    // GS settings in GM mode (available on SC55mk2+)
    MT32                      // MT32 arrangement
  };

  Synth(ControlRom &cRom, WaveRom &pRom, SoundMap map = SoundMap::GS);
  ~Synth();

  // Add start() and stop()? Won't start if sampleRate is not set?

  void midi_input(uint8_t status, uint8_t data1, uint8_t data2);
  void midi_input_sysex(uint8_t *data, uint16_t length);

  int get_next_frame(float &lOut, float &rOut);
  uint32_t get_num_clipped_samples(bool reset = true);
  std::array<int, 16> get_parts_last_peak_sample(void);

  /* PRISM: voice management. The cap counts partials, like the hardware
   * (24 on an SC-55, 28 on a mkII); 0 restores the model's own limit. With
   * stealing on, the oldest or quietest releasing note gives way instead of
   * the new note being dropped. */
  void set_polyphony_cap(int partials);
  void set_voice_stealing(bool on);
  int  get_polyphony_cap(void);
  int  get_num_partials(void);          // partials sounding right now

  // Setting audio properties (default is 44100, 2)
  void set_audio_format(uint32_t sampleRate, uint8_t channels);

  void reset(SoundMap sm, bool resetParts = false);

  void panic(void);

  // Returns libEmuSC version as a string
  static std::string version(void);

  void set_part_instrument(uint8_t partId, uint8_t index, uint8_t bank);

  void add_part_midi_mod_callback(std::function<void(const int)> callback);
  void clear_part_midi_mod_callback(void);

  void add_part_change_callback(std::function<void(const int)> callback);
  void clear_part_change_callback(void);

  void set_part_envelope_callback(int partId,
                                  std::function<void(const float, const float,
                                                     const float, const float,
                                                     const float, const float)> callback);
  void clear_part_envelope_callback(int partId);
  void set_part_lfo_callback(int partId,
                             std::function<void(const int, const int, const int)
                             > callback);
  void clear_part_lfo_callback(int partId);

  // EmuSC clients methods for getting synth paramters
  uint8_t  get_param(enum SystemParam sp);
  uint8_t* get_param_ptr(enum SystemParam sp);
  uint16_t get_param_32nib(enum SystemParam sp);
  uint8_t  get_param(enum PatchParam pp, int8_t part = -1);
  uint8_t* get_param_ptr(enum PatchParam pp, int8_t part = -1);
  uint16_t get_param_uint14(enum PatchParam pp, int8_t part = -1);
  uint8_t  get_param_nib16(enum PatchParam pp, int8_t part = -1);
  uint8_t  get_patch_param(uint16_t address, int8_t part = -1);
  uint8_t  get_param(enum DrumParam, uint8_t map, uint8_t key);
  int8_t* get_param_ptr(enum DrumParam, uint8_t map);

  // EmuSC clients methods for setting synth paramters
  void set_param(enum SystemParam sp, uint8_t value);
  void set_param(enum SystemParam sp, uint32_t value);
  void set_param(enum SystemParam sp, uint8_t *data, uint8_t size = 1);
  void set_param_32nib(enum SystemParam sp, uint16_t value);
  void set_param(enum PatchParam pp, uint8_t value, int8_t part = -1);
  void set_param(enum PatchParam sp, uint8_t *data, uint8_t size = 1,
		 int8_t part = -1);
  void set_param_uint14(enum PatchParam pp, uint16_t value, int8_t part = -1);
  void set_param_nib16(enum PatchParam pp, uint8_t value, int8_t part = -1);
  void set_patch_param(uint16_t address, uint8_t value, int8_t part = 1);
  void set_param(enum DrumParam dp, uint8_t map, uint8_t key, uint8_t value);
  void set_param(enum DrumParam dp, uint8_t map, uint8_t *data, uint8_t length);

  /* End of public API. Below are internal data structures only */

private:
  Settings *_settings;
  
  uint32_t _sampleRate;
  uint8_t _channels;

  std::atomic<uint32_t> _numClippedSamples;

  std::mutex midiMutex;

  struct std::vector<Part> _parts;
  std::vector<std::function<void(const int)>> _partMidiModCallbacks;
  std::vector<std::function<void(const int)>> _partChangeCallbacks;

  ControlRom &_ctrlRom;
  WaveRom &_waveRom;

  float _phase;               // Fractional SC-55 sample position
  float _phaseIncrement;      // SC-55 samples per host sample
  int   _updateCounter;       // Counts SC-55 samples, fires at 256

  std::vector<float> _hostSampleBufL;
  std::vector<float> _hostSampleBufR;
  int _hostSampleBufRIndex;
  int _hostSampleBufWIndex;

  std::array<std::array<float, 256>, 2> _dryBus;
  std::array<std::array<float, 256>, 2> _chorusBus;
  std::array<std::array<float, 256>, 2> _reverbBus;

  std::array<std::array<float, 256>, 2> _chorusOut;
  std::array<std::array<float, 256>, 2> _reverbOut;

  SystemEffects *_systemEffects;
  Resampler *_resampler;

  // MIDI message types
  static const uint8_t midi_NoteOff         = 0x80;
  static const uint8_t midi_NoteOn          = 0x90;
  static const uint8_t midi_PolyKeyPressure = 0xa0;
  static const uint8_t midi_CtrlChange      = 0xb0;
  static const uint8_t midi_PrgChange       = 0xc0;
  static const uint8_t midi_ChPressure      = 0xd0;
  static const uint8_t midi_PitchBend       = 0xe0;

  int _polyCap;                 // 0 = the model's own limit
  bool _steal;
  volatile int _renderNext;     // next part index claimed by a render worker

  void _init_parts(void);
// int _export_sample_24(std::vector<int32_t> &sampleSet, std::string filename);
  void _add_note(uint8_t midiChannel, uint8_t key, uint8_t velocity);
  int _note_partials(Part &p, uint8_t key);   // partials a new note would take
  int _steal_one(void);                       // kill a victim, return its partials
  static void _render_job(void *arg, int k);

  void _midi_input_sysex_DT1(uint8_t model, uint8_t *data, uint16_t length);

  void _process_samples(void);

  Synth();
};

}

#endif  // __SYNTH_H__
