/* SC-55 mode: PRISM's wrapper around libEmuSC. The emulator plays the real
 * ROM images (control + CPU + wave) through sixteen GS parts. Parts render
 * on the helper cores through the same worker pool the other engines use. */
#include "sc55.h"
extern "C" {
#include "plat.h"
}
#include "../emusc/libemusc/src/control_rom.h"
#include "../emusc/libemusc/src/wave_rom.h"
#include "../emusc/libemusc/src/synth.h"
#include "../emusc/libemusc/src/params.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <string>
#include <vector>

using namespace EmuSC;

Sc55State g_sc55;

static ControlRom *s_rom = NULL;
static WaveRom *s_wave = NULL;
static Synth *s_syn = NULL;

static int s_map = 0;                  /* 0 GS, 1 GM, 2 MT-32 */
static int s_polycap = 0;              /* 0 = the model's own limit */
static int s_steal = 1;
static float s_gain = 1.0f;
static int s_cores = 3;
static volatile int s_pending_map = -1;   /* sound map change, taken by the
                                             audio thread between blocks */

static unsigned char s_syx[8192];
static int s_syx_n = 0;

int sc55_is_open(void) { return s_syn != NULL && g_sc55.open; }

static void set_status(const char *s) { snprintf(g_sc55.status, sizeof g_sc55.status, "%s", s); }

static Synth::SoundMap soundmap_of(int m)
{
    return m == 1 ? Synth::SoundMap::GS_GM
         : m == 2 ? Synth::SoundMap::MT32 : Synth::SoundMap::GS;
}

/* instrument index for a program, walking down the variation banks the way
 * the hardware resolves an empty slot */
static int resolve_instrument(int bank, int prog)
{
    if (!s_rom) return -1;
    if (bank < 0) bank = 0;
    if (bank > 127) bank = 127;
    uint16_t idx = s_rom->variation(bank)[prog & 127];
    while (idx == 0xffff && bank > 0)
        idx = s_rom->variation(--bank)[prog & 127];
    return idx == 0xffff ? -1 : idx;
}

/* drum set index for a program; -1 when the program has no set */
static int resolve_drum_set(int prog)
{
    if (!s_rom) return -1;
    const std::array<uint8_t, 128> &lut = s_rom->get_drum_sets_LUT();
    if (lut[prog & 127] == 0xff) return -1;
    int index = 0;
    for (int i = 0; i < (prog & 127); i++)
        if (lut[i] != 0xff) index++;
    return index;
}

static void fill_name_tables(void)
{
    for (int p = 0; p < 128; p++) {
        int idx = resolve_instrument(0, p);
        snprintf(g_sc55.iname[p], sizeof g_sc55.iname[p], "%.12s",
                 idx >= 0 ? s_rom->instrument(idx).name.c_str() : "-");
        int ds = resolve_drum_set(p);
        if (ds >= 0)
            snprintf(g_sc55.dname[p], sizeof g_sc55.dname[p], "%.12s",
                     s_rom->drumSet(ds).name.c_str());
        else
            g_sc55.dname[p][0] = 0;
    }
}

/* Benchmark at open: the voice cap saturated by a stream of notes, rendered
 * on one, two and three cores, then with a note stolen every block. Percent
 * of the block budget. The caller runs on the audio core to measure. */
static int bench_blocks(short *buf, int blocks)
{
    const unsigned long long blk_us = OUT_FRAMES * 1000000ull / OUT_RATE;
    unsigned int t0 = plat_time_us();
    for (int b = 0; b < blocks; b++) sc55_render(buf, OUT_FRAMES);
    unsigned int dt = plat_time_us() - t0;
    return (int)((unsigned long long)dt * 100ull / ((unsigned long long)blocks * blk_us));
}

static void bench(void)
{
    static short buf[OUT_FRAMES * 2];
    int saved = emusc_parallel_count, have = 1 + plat_par_workers();
    plat_bench_core(1);
    for (int ch = 0; ch < 6; ch++) {
        s_syn->midi_input(0xC0 | ch, 48, 0);            /* strings: 2 partials */
        for (int i = 0; i < 6; i++)
            s_syn->midi_input(0x90 | ch, 40 + ch * 3 + i * 5, 100);
    }
    bench_blocks(buf, 8);   /* let the attacks settle */
    for (int c = 1; c <= 3; c++) {
        emusc_parallel_count = c > have ? have : c;
        g_sc55.bench[c - 1] = bench_blocks(buf, 16);
    }
    emusc_parallel_count = 3 > have ? have : 3;
    {   /* voice stealing: a new note every block on top of the full load */
        const unsigned long long blk_us = OUT_FRAMES * 1000000ull / OUT_RATE;
        unsigned int t0 = plat_time_us();
        for (int b = 0; b < 16; b++) {
            s_syn->midi_input(0x90 | (b % 6), 72 + b, 100);
            sc55_render(buf, OUT_FRAMES);
        }
        unsigned int dt = plat_time_us() - t0;
        g_sc55.bench[3] = (int)((unsigned long long)dt * 100ull / (16ull * blk_us));
    }
    for (int ch = 0; ch < 16; ch++) {
        s_syn->midi_input(0xB0 | ch, 120, 0);
        if (ch < 6) s_syn->midi_input(0xC0 | ch, 0, 0);
    }
    bench_blocks(buf, 4);
    emusc_parallel_count = saved;
    plat_bench_core(0);
    plat_log("SC-55 bench: 1 core %d%%, 2 cores %d%%, 3 cores %d%%, stealing %d%%",
             g_sc55.bench[0], g_sc55.bench[1], g_sc55.bench[2], g_sc55.bench[3]);
}

int sc55_open(const char *dir)
{
    sc55_close();
    memset(&g_sc55, 0, sizeof g_sc55);
    g_sc55.soundmap = s_map; g_sc55.steal = s_steal; g_sc55.cores = s_cores;
    g_sc55.polycap_set = s_polycap;
    g_sc55.gain = (int)(s_gain * 100.0f);

    static char names[32][64];
    int n = plat_list_files(dir, ".rom", names, 16);
    n += plat_list_files(dir, ".bin", names + n, 16);
    g_sc55.roms_found = n;
    if (n == 0) { set_status("No ROM files in ux0:data/prism/sc55"); return -1; }

    /* classify by size: 32 kB = CPU ROM, whole megabytes = wave ROMs,
     * anything from 128 kB to 768 kB = control (program) ROM candidate */
    int cpu[4], ncpu = 0, prog[4], nprog = 0, wave[4], nwave = 0;
    for (int i = 0; i < n; i++) {
        char path[400];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        long sz = plat_file_size(path);
        if (sz == 32768 && ncpu < 4) cpu[ncpu++] = i;
        else if (sz >= 0x100000 && sz <= 0x400000 && (sz % 0x100000) == 0 && nwave < 4) wave[nwave++] = i;
        else if (sz >= 0x20000 && sz <= 0xC0000 && nprog < 4) prog[nprog++] = i;
    }
    if (!ncpu || !nprog || !nwave) {
        char b[80];
        snprintf(b, sizeof b, "Need control + CPU + wave ROMs (have %d/%d/%d)", nprog, ncpu, nwave);
        set_status(b);
        return -2;
    }

    /* wave ROMs concatenate in name order: name them so they sort by bank */
    for (int a = 0; a < nwave; a++)
        for (int b = a + 1; b < nwave; b++)
            if (strcasecmp(names[wave[a]], names[wave[b]]) > 0) { int t = wave[a]; wave[a] = wave[b]; wave[b] = t; }

    std::string err;
    for (int p = 0; p < nprog && !s_rom; p++)
        for (int c = 0; c < ncpu && !s_rom; c++) {
            char pp[400], cp[400];
            snprintf(pp, sizeof pp, "%s/%s", dir, names[prog[p]]);
            snprintf(cp, sizeof cp, "%s/%s", dir, names[cpu[c]]);
            try {
                s_rom = new ControlRom(pp, cp);
                snprintf(g_sc55.prog_file, sizeof g_sc55.prog_file, "%s", names[prog[p]]);
                snprintf(g_sc55.cpu_file, sizeof g_sc55.cpu_file, "%s", names[cpu[c]]);
            } catch (std::string e) {
                err = e;
                delete s_rom; s_rom = NULL;
            }
        }
    if (!s_rom) {
        snprintf(g_sc55.status, sizeof g_sc55.status, "%.79s",
                 err.empty() ? "Control ROM not recognised" : err.c_str());
        return -3;
    }

    {
        std::vector<std::string> wf;
        char wl[96]; wl[0] = 0;
        for (int w = 0; w < nwave; w++) {
            char path[400];
            snprintf(path, sizeof path, "%s/%s", dir, names[wave[w]]);
            wf.push_back(path);
            if (w) strncat(wl, " + ", sizeof wl - strlen(wl) - 1);
            strncat(wl, names[wave[w]], sizeof wl - strlen(wl) - 1);
        }
        snprintf(g_sc55.wave_files, sizeof g_sc55.wave_files, "%s", wl);
        try {
            s_wave = new WaveRom(wf, *s_rom);
        } catch (std::string e) {
            snprintf(g_sc55.status, sizeof g_sc55.status, "%.79s", e.c_str());
            delete s_rom; s_rom = NULL;
            return -4;
        }
    }

    s_syn = new Synth(*s_rom, *s_wave, soundmap_of(s_map));
    s_syn->set_audio_format(OUT_RATE, 2);
    s_syn->set_polyphony_cap(s_polycap);
    s_syn->set_voice_stealing(s_steal != 0);
    emusc_parallel_run = plat_par_run;
    sc55_set_cores(s_cores);

    snprintf(g_sc55.model, sizeof g_sc55.model, "%.19s", s_rom->model().c_str());
    snprintf(g_sc55.version, sizeof g_sc55.version, "%.7s", s_rom->version().c_str());
    snprintf(g_sc55.date, sizeof g_sc55.date, "%.11s", s_rom->date().c_str());
    g_sc55.lut_prog = (int)s_rom->lut_source_prog();
    g_sc55.lut_cpu = (int)s_rom->lut_source_cpu();
    g_sc55.lut_dprog = s_rom->lut_delta_prog();
    g_sc55.lut_dcpu = s_rom->lut_delta_cpu();
    g_sc55.poly_model = s_rom->max_polyphony();
    g_sc55.polycap = s_syn->get_polyphony_cap();
    fill_name_tables();
    {
        char b[80];
        snprintf(b, sizeof b, "%s v%s (%s)", g_sc55.model, g_sc55.version, g_sc55.date);
        set_status(b);
    }
    s_syx_n = 0;
    s_pending_map = -1;
    bench();
    sc55_snapshot();
    g_sc55.open = 1;
    return 0;
}

void sc55_close(void)
{
    g_sc55.open = 0;
    delete s_syn; s_syn = NULL;
    delete s_wave; s_wave = NULL;
    delete s_rom; s_rom = NULL;
}

void sc55_midi_packet(const unsigned char *pkt)
{
    if (!s_syn || !g_sc55.open) return;
    int cin = pkt[0] & 0x0F;
    switch (cin) {
    case 0x4:   /* sysex start or continuation, three bytes */
        if (s_syx_n + 3 <= (int)sizeof s_syx) { s_syx[s_syx_n++] = pkt[1]; s_syx[s_syx_n++] = pkt[2]; s_syx[s_syx_n++] = pkt[3]; }
        break;
    case 0x5: case 0x6: case 0x7: {   /* sysex end with one, two or three bytes */
        int k = cin - 4;
        for (int i = 0; i < k && s_syx_n < (int)sizeof s_syx; i++) s_syx[s_syx_n++] = pkt[1 + i];
        if (s_syx_n >= 2 && s_syx[0] == 0xF0) s_syn->midi_input_sysex(s_syx, (uint16_t)s_syx_n);
        s_syx_n = 0;
        break;
    }
    case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: case 0xD: case 0xE:
        s_syn->midi_input(pkt[1], pkt[2], pkt[3]);
        break;
    default: break;
    }
}

void sc55_render(short *stereo, int frames)
{
    if (!s_syn) { memset(stereo, 0, (size_t)frames * 2 * sizeof(short)); return; }
    if (s_pending_map >= 0) {   /* sound map change between blocks */
        int m = s_pending_map;
        s_pending_map = -1;
        s_syn->panic();
        s_syn->reset(soundmap_of(m), true);
    }
    float g = s_gain * 32767.0f;
    for (int i = 0; i < frames; i++) {
        float l = 0, r = 0;
        s_syn->get_next_frame(l, r);
        float lv = l * g, rv = r * g;
        stereo[i * 2] = (short)(lv > 32767.0f ? 32767 : lv < -32768.0f ? -32768 : (int)lv);
        stereo[i * 2 + 1] = (short)(rv > 32767.0f ? 32767 : rv < -32768.0f ? -32768 : (int)rv);
    }
}

void sc55_snapshot(void)
{
    if (!s_syn) return;
    std::array<int, 16> peaks = s_syn->get_parts_last_peak_sample();
    for (int p = 0; p < 16; p++) {
        g_sc55.chan[p] = s_syn->get_param(PatchParam::RxChannel, p);
        g_sc55.prog[p] = s_syn->get_param(PatchParam::ToneNumber2, p);
        g_sc55.bank[p] = s_syn->get_param(PatchParam::ToneNumber, p);
        g_sc55.level[p] = s_syn->get_param(PatchParam::PartLevel, p);
        g_sc55.pan[p] = s_syn->get_param(PatchParam::PartPanpot, p);
        g_sc55.rev[p] = s_syn->get_param(PatchParam::ReverbSendLevel, p);
        g_sc55.cho[p] = s_syn->get_param(PatchParam::ChorusSendLevel, p);
        g_sc55.drum[p] = s_syn->get_param(PatchParam::UseForRhythm, p);
        int bars = peaks[p] < 0 ? 0 : peaks[p];   /* already 0..16 bar units */
        g_sc55.bars[p] = (unsigned char)(bars > 16 ? 16 : bars);
        if (g_sc55.drum[p]) {
            const char *dn = g_sc55.dname[g_sc55.prog[p] & 127];
            snprintf(g_sc55.pname[p], sizeof g_sc55.pname[p], "%s", dn[0] ? dn : "Drums");
        } else {
            int idx = resolve_instrument(g_sc55.bank[p], g_sc55.prog[p]);
            snprintf(g_sc55.pname[p], sizeof g_sc55.pname[p], "%.12s",
                     idx >= 0 ? s_rom->instrument(idx).name.c_str() : "-");
        }
    }
    g_sc55.voices = s_syn->get_num_partials();
    g_sc55.polycap = s_syn->get_polyphony_cap();
}

void sc55_all_off(void)
{
    if (!s_syn || !g_sc55.open) return;
    for (int ch = 0; ch < 16; ch++) {
        s_syn->midi_input(0xB0 | ch, 123, 0);
        s_syn->midi_input(0xB0 | ch, 120, 0);
    }
}

void sc55_set_map(int m)
{
    s_map = m < 0 ? 0 : m > 2 ? 2 : m;
    g_sc55.soundmap = s_map;
    if (s_syn && g_sc55.open) s_pending_map = s_map;
}

void sc55_set_polycap(int partials)
{
    if (partials != 0) { if (partials < 8) partials = 8; if (partials > 64) partials = 64; }
    s_polycap = partials;
    g_sc55.polycap_set = s_polycap;
    if (s_syn) { s_syn->set_polyphony_cap(s_polycap); g_sc55.polycap = s_syn->get_polyphony_cap(); }
}

void sc55_set_steal(int on)
{
    s_steal = on ? 1 : 0;
    g_sc55.steal = s_steal;
    if (s_syn) s_syn->set_voice_stealing(s_steal != 0);
}

void sc55_set_gain(int v127)
{
    if (v127 < 0) v127 = 0;
    if (v127 > 127) v127 = 127;
    s_gain = v127 / 100.0f;
    g_sc55.gain = v127;
}

void sc55_set_cores(int n)
{
    s_cores = n < 1 ? 1 : n > 3 ? 3 : n;
    g_sc55.cores = s_cores;
    int have = 1 + plat_par_workers();
    emusc_parallel_count = s_cores > have ? have : s_cores;
    g_sc55.cores_used = emusc_parallel_count;
}

void sc55_part_channel(int part, int ch)
{
    if (!s_syn || part < 0 || part > 15) return;
    unsigned char v = (unsigned char)(ch < 0 ? 0 : ch > 16 ? 16 : ch);
    s_syn->set_param(PatchParam::RxChannel, v, part);
    g_sc55.chan[part] = v;
}

void sc55_part_program(int part, int prog)
{
    if (!s_syn || part < 0 || part > 15) return;
    if (prog < 0) prog = 0;
    if (prog > 127) prog = 127;
    s_syn->set_part_instrument(part, (uint8_t)prog,
                               s_syn->get_param(PatchParam::ToneNumber, part));
    g_sc55.prog[part] = (unsigned char)prog;
}

void sc55_part_bank(int part, int bank)
{
    if (!s_syn || part < 0 || part > 15) return;
    if (bank < 0) bank = 0;
    if (bank > 127) bank = 127;
    s_syn->set_part_instrument(part, s_syn->get_param(PatchParam::ToneNumber2, part),
                               (uint8_t)bank);
    g_sc55.bank[part] = (unsigned char)bank;
}

void sc55_part_cc(int part, int cc, int v)
{
    if (!s_syn || part < 0 || part > 15) return;
    if (v < 0) v = 0;
    if (v > 127) v = 127;
    switch (cc) {
    case 7: s_syn->set_param(PatchParam::PartLevel, (uint8_t)v, part); g_sc55.level[part] = (unsigned char)v; break;
    case 10: s_syn->set_param(PatchParam::PartPanpot, (uint8_t)v, part); g_sc55.pan[part] = (unsigned char)v; break;
    case 91: s_syn->set_param(PatchParam::ReverbSendLevel, (uint8_t)v, part); g_sc55.rev[part] = (unsigned char)v; break;
    case 93: s_syn->set_param(PatchParam::ChorusSendLevel, (uint8_t)v, part); g_sc55.cho[part] = (unsigned char)v; break;
    default: break;
    }
}

void sc55_part_note(int part, int note, int vel)
{
    if (!s_syn || part < 0 || part > 15) return;
    int ch = s_syn->get_param(PatchParam::RxChannel, part);
    if (ch > 15) return;
    s_syn->midi_input((vel ? 0x90 : 0x80) | ch, note & 0x7F, vel & 0x7F);
}
