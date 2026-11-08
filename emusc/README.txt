libEmuSC 0.7.0 with the PRISM changes.

Upstream: https://github.com/skjelten/emusc (tag v0.7.0), LGPL 2.1 or
later. Only the library (libemusc) is carried; the Qt client is not.

The local changes, also carried as prism/credits/emusc-prism.patch:

 - Parts render into their own buffers and are dealt across the host's
   worker threads (emusc_parallel_run, like the Munt patch). This also
   keeps one part's chorus and reverb sends off the other parts' audio,
   which the upstream bus sharing did not.
 - Voice stealing at the polyphony cap: releasing notes give way first,
   quietest first, then the oldest sounding note - instead of new notes
   being dropped. The cap follows the model (24 or 28 partials) and can
   be raised to 64 or lowered through the new Synth setters.
 - Portamento is completed and per part: the glide source is the part's
   previous note (its current pitch while it still glides) or the CC 84
   key; it works in poly and mono mode, legato accumulates, and gliding
   notes start at the sample's portamento offset. The static cross-part
   state is gone.
 - Poly key pressure is implemented (one value per part, as the Roland
   address map stores it); channel pressure already worked.
 - The ROM lookup tables are verified by shape at the known offsets and
   otherwise located by scanning for the same layout at a shift, so
   firmware builds beyond SC-55 1.21 and mkII 1.01 load - the SC-155
   and SCC-1 ROMs among them. Unknown ROMs are rejected cleanly instead
   of playing garbage (the library no longer calls exit()).
 - Sustain (CC 64) no longer cuts held notes on note-off.
 - Rendering is deterministic: the envelope base class and the TVF, TVA
   and pitch generators initialize every member (several were read before
   first write, so a note's sound depended on recycled heap contents),
   and the over-long loop lengths a few ROM samples carry are healed once
   at ROM load instead of lazily at the first note that used them. Two
   identical MIDI streams now render bit-identical audio, serial or
   threaded (verified with Valgrind and an A/B test on real ROMs).
 - The oscillator inner loop drops its per-fetch bounds checks, and the
   audio-thread logging is reduced to once-only warnings.
 - config.h is a static stand-in for the CMake-generated header.
