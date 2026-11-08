/* SC-55 mode. Wraps libEmuSC (LGPL), a Sound Canvas emulator that plays the
 * control, CPU and wave ROM images of a real SC-55 or SC-55mkII. ROMs are
 * read from ux0:data/prism/sc55 and are not shipped. The audio thread owns
 * the synth once it is open; the panel reads a snapshot the audio thread
 * refreshes. */
#ifndef SC55_H
#define SC55_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int open;                    /* synth open and ready */
    char model[20];              /* "SC-55", "SC-55mkII", ... */
    char version[8], date[12];   /* firmware version and date from the ROM */
    char status[80];             /* status line */
    int roms_found;              /* ROM files seen in the folder */
    /* how the lookup tables were found: 0 stock offsets of a known build,
     * 1 known offsets fitted this build, 2 located by shape at a shift */
    int lut_prog, lut_cpu;
    int lut_dprog, lut_dcpu;     /* the shift, when scanned */
    char prog_file[64], cpu_file[64], wave_files[96];   /* source files */
    int soundmap;                /* 0 GS, 1 GM, 2 MT-32; applied at reset */
    int polycap;                 /* partial cap in use */
    int poly_model;              /* the model's own limit (24 or 28) */
    int polycap_set;             /* requested cap; 0 = the model's own */
    int steal;                   /* voice stealing on */
    int gain;                    /* output, 0..127, 100 = unity */
    int cores, cores_used;       /* render threads, 1..3 */
    int voices;                  /* partials sounding */
    int bench[4];                /* full load on 1, 2, 3 cores, then with
                                    stealing; percent of the block budget */
    /* Part state for the panel, refreshed by sc55_snapshot. */
    unsigned char chan[16];      /* receive channel 0..15, >=16 off */
    unsigned char prog[16];      /* program (tone number) */
    unsigned char bank[16];      /* variation bank (CC 0) */
    unsigned char level[16], pan[16];
    unsigned char rev[16], cho[16];
    unsigned char drum[16];      /* nonzero: rhythm part */
    unsigned char bars[16];      /* bar display, 0..16 like the real LCD */
    char pname[16][13];          /* instrument name per part */
    char iname[128][13];         /* capital tone names (variation bank 0) */
    char dname[128][13];         /* drum set name per program; "" = invalid */
} Sc55State;

extern Sc55State g_sc55;

/* Main thread. Scans the folder, loads the ROMs, opens the synth. 0 = ok. */
int  sc55_open(const char *dir);
void sc55_close(void);
int  sc55_is_open(void);

/* Audio thread. */
void sc55_midi_packet(const unsigned char *pkt);   /* one USB-MIDI packet */
void sc55_render(short *stereo, int frames);
void sc55_snapshot(void);                          /* refresh g_sc55 */
void sc55_all_off(void);

/* Settings. All apply immediately; the sound map silences and resets. */
void sc55_set_map(int m);            /* 0 GS, 1 GM, 2 MT-32 */
void sc55_set_polycap(int partials); /* 0 = the model's own limit, else 8..64 */
void sc55_set_steal(int on);
void sc55_set_gain(int v127);
void sc55_set_cores(int n);          /* 1..3 */

/* Part control, audio thread only. The panel sends events. */
void sc55_part_channel(int part, int ch);      /* ch 0..15, 16 = off */
void sc55_part_program(int part, int prog);
void sc55_part_bank(int part, int bank);       /* variation bank */
void sc55_part_cc(int part, int cc, int v);    /* 7, 10, 91, 93 */
void sc55_part_note(int part, int note, int vel);   /* vel 0 = note off */

#ifdef __cplusplus
}
#endif

#endif
