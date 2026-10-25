/* Real offline steps; never synthesized pictures. See finding 067. */
#include "hle.h"
#include "../include/fps.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <stdio.h>

extern int pads_online(void);
extern int D_0029E3D0, D_0029E3D4, D_0029E3D8, D_0029E3DC;
extern int D_0029E3E0,D_0029E3E8;
extern float D_0029E3FC, D_0029E400;
extern int func_00243CE8(int);
static int requested;
static double phase, anim_phase;
static double step_fields, previous_wall, wall_error;
static int wall_mode;
static unsigned clock_serial;

/* The selected rate limits pictures. Only paced offline updates track wall
   time; deterministic/unpaced diagnostics keep the original rational clock.
   Spread clock error over eight updates instead of feeding frame jitter to
   physics. Retaining the residual makes this a time-conserving filter. */
void port_fps_wall_mode(int enabled) { wall_mode=enabled; previous_wall=0; wall_error=0; }
static double clock_interval(void)
{
    const double nominal=60.0/port_fps();
    double result=nominal;
    if (wall_mode && port_fps_high()) {
        double now=host_wall_seconds();
        if (previous_wall) {
            double elapsed=(now-previous_wall)*60;
            /* Retain late-frame debt even when this step hits the 1/60 cap.
               A later faster stretch repays it without an oversized step. */
            if (elapsed>=0 && isfinite(elapsed)) wall_error+=elapsed-nominal;
            result=nominal+wall_error/8;
            if (result<nominal) result=nominal;
            if (result>1) result=1;
            wall_error-=result-nominal;
        }
        previous_wall=now;
    } else {
        previous_wall=0;
        wall_error=0;
    }
    return result;
}
static int whole;
static uint64_t race_steps;  /* real race updates, for DHD_FPS_TRACE */

static void reset_body_clocks(void);
void port_fps_race_step(void) { race_steps++; }

int port_fps(void)
{
    if (!requested) {
        const char *s = getenv("DHD_FPS");
        requested = s ? atoi(s) : 60;
        if (s && !_stricmp(s, "monitor")) {
            DEVMODEA mode;
            memset(&mode, 0, sizeof mode); mode.dmSize = sizeof mode;
            requested = EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &mode) ? mode.dmDisplayFrequency : 60;
        }
        if (requested < 60) requested = 60;
        if (requested > 144) requested = 144;
        hle_log("fps: requested %d real offline steps/s (online: 60)\n", requested);
    }
    return pads_online() ? 60 : requested;
}
int port_fps_high(void) { return port_fps() > 60; }
double port_frame_period(void) { int n = port_fps(); return n == 60 ? 1001.0 / 60000.0 : 1.0 / n; }
float port_step(void) { return port_fps_high() ? (float)(step_fields ? step_fields : 60.0/port_fps()) : (float)D_0029E3D0; }
int port_whole_step(void) { return port_fps_high() ? whole : D_0029E3D0; }
float port_decay(float f) { return port_fps_high() ? powf(f, port_step()) : f; }
float port_blend(float r) { return port_fps_high() && r > 0 && r < 1 ? (float)(1.0-pow(1.0-(double)r,(wall_mode ? step_fields : 60.0/port_fps()))) : r; }
float port_particle_sum(float q) {
    /* Evaluate the geometric sum in double: float cancellation near q=1
       otherwise makes gently damped particles move systematically too far. */
    return q == 1.0f ? port_step() : (float)((double)q*(1.0-pow((double)q,(wall_mode ? step_fields : 60.0/port_fps())))/(1.0-q));
}
void port_particle_rotation(float *c,float *s)
{
    float angle=atan2f(*s,*c)*port_step();
    *c=cosf(angle);
    *s=sinf(angle);
}
/* The original burst tests age before advancing it, so it retires one full
   legacy field after crossing 1. Preserve that lifetime, including the
   original float accumulation/field quantization, while moving every step. */
int port_particle_dead(float age,float rate)
{
    typedef struct { unsigned bits; int fields; } Life;
    static Life cache[4096];
    union { float f; unsigned u; } key;
    key.f=rate;
    if (!(rate>0) || !isfinite(rate)) return age>=1;
    unsigned start=(key.u*2654435761u)>>20,index=start;
    unsigned probes=0;
    while (cache[index].bits && cache[index].bits!=key.u && probes++<16) {
        index=(index+1)&4095;
    }
    if (probes>16) index=start; /* bounded replacement when the cache fills */
    if (cache[index].bits!=key.u) {
        float legacy=0;
        int fields=1;
        while (legacy<1 && fields<65536) { legacy+=rate; fields++; }
        cache[index].bits=key.u;
        cache[index].fields=fields;
    }
    return (double)age>=(double)rate*(cache[index].fields-(double)port_step());
}
void port_clock_reset(void)
{
    phase = anim_phase = 0;
    whole = 0;
    step_fields=60.0/port_fps();
    previous_wall=0;
    wall_error=0;
    reset_body_clocks();
    port_clock_step();
}
void port_clock_wait(int *out)
{
    D_0029E3E0 += *(volatile int *)0x10000800;
    *(volatile int *)0x10000800=0;
    int result = func_00243CE8(0);
    if (out) *out = result == 0;
    step_fields=clock_interval();
    clock_serial++;
    phase += wall_mode ? step_fields*port_fps() : 60;
    whole = (int)(phase / port_fps());
    phase -= whole*port_fps();
    D_0029E3D8 += whole;
}
void port_clock_step(void)
{
    D_0029E3D0 = whole;
    D_0029E3D4 = wall_mode ? (int)(60.0/step_fields) : port_fps();
    D_0029E3FC = wall_mode ? (float)(60.0/step_fields) : (float)port_fps();
    D_0029E400 = wall_mode ? (float)(step_fields/60.0) : 1.0f / port_fps();
    D_0029E3DC = D_0029E3D8;
    D_0029E3E0 += *(volatile int *)0x10000800;
    *(volatile int *)0x10000800=0;
    D_0029E3E8=D_0029E3E0;
}
/* Stored animation timestamps retain their four-byte integer layout. At high
   rates only, their unit is 1/1024 field; all consumers convert elapsed values. */
int port_animation_step(void)
{
    if (port_animation_scale() == 1) return D_0029E3D0;
    anim_phase += wall_mode && port_fps_high() ? step_fields*1024*port_fps() : 60 * 1024;
    int ticks = (int)(anim_phase / port_fps());
    anim_phase -= ticks*port_fps();
    return ticks;
}
int port_animation_scale(void) { port_fps(); return requested > 60 ? 1024 : 1; }
float port_animation_fields(float t) { return port_animation_scale() > 1 ? t * (1.0f/1024) : t; }

/* Per-body fractional airborne age; no layout changes to retail short x4A4.
   A reset to zero by contact invalidates the residual on the following step. */
typedef struct {
    void *body;
    float age;
    int first,force_field,force_valid;
    double stalled_steps;
    float stall_origin[3];
} Air;
static Air air[32];
static void reset_body_clocks(void)
{
    clock_serial+=2; /* invalidate previous-step Verlet spans across scene/reset boundaries */
    memset(air,0,sizeof air);
}
static Air *air_for(void *body)
{
    for (int i=0; i<32; i++) if (air[i].body == body) return &air[i];
    for (int i=0; i<32; i++) if (!air[i].body) { air[i].body=body; return &air[i]; }
    /* Rider bodies come from a fixed pool of eight; refuse silent aliasing. */
    abort();
}
int port_air_tick(void *body, int previous)
{
    Air *a=air_for(body);
    if (!previous) a->age=0;
    a->first = !previous;
    a->age += port_step();
    /* Integer consumers are elapsed-field timers: ceil made >1 true at
       1.25 fields at 144 Hz, while the original first becomes true at 2.
       Keep a nonzero sentinel until the first complete field so contact and
       the separately tracked first-air-step event remain distinguishable. */
    int fields=(int)a->age;
    return fields>0 ? fields : 1;
}
int port_air_first(void *body, int current) { return current && air_for(body)->first; }
float port_air_age(void *body, int current) { return current ? air_for(body)->age : 0; }
/* Finer collision steps can settle a pedalling bike into a stable wall contact
   where the coarse retail solver falls/rebounds. Its old automatic recovery
   only watches airborne/out-of-world cases (00155208), so this equilibrium
   otherwise lasts indefinitely. Detect sustained immobility in physical time
   and enter the ordinary collision recovery controller, without scripted input
   or direct position/velocity writes. */
void port_fps_contact_recovery(void *body)
{
    if (!port_fps_high()) return;
    unsigned char *b=body,*r=*(unsigned char **)(b+0x158);
    Air *a=air_for(body);
    int slot=*(int *)(r+0x797C),race_phase=*(int *)PS2_HOST(0x43F348);
    float *p=(float *)(b+0x50);
    int contact=b[0x354]!=2 || b[0x3D4]!=2;
    int supported=contact || (a->stalled_steps && port_air_age(body,*(short *)(b+0x4A4))<4);
    int pedal=*(float *)(r+0x7A78)>0 || r[0x7A69];
    extern float func_0012DF48(void *);
    /* AI already has the retail grounded recovery states (001100D0/103B0).
       Preserve their own field timers; this fills the human controller gap. */
    if (*(int *)(r+0x7A5C)!=1 || slot<0 || slot>=10 || *(int *)PS2_HOST(0x29DED8)!=9 || race_phase<2 || race_phase>4 ||
        *(unsigned char *)(PS2_HOST(0x43EF18)+slot) || (r[0x4098]&4) ||
        !supported || !pedal || func_0012DF48(r)>0) {
        a->stalled_steps=0;
        return;
    }
    if (!a->stalled_steps) memcpy(a->stall_origin,p,sizeof a->stall_origin);
    float moved2=0;
    for (int i=0;i<3;i++) { float delta=p[i]-a->stall_origin[i]; moved2+=delta*delta; }
    if (moved2>=1) { a->stalled_steps=0; return; } /* slow climbing makes actual progress */
    a->stalled_steps+=port_step();
    if (a->stalled_steps<120-0.00001) return;
    a->stalled_steps=0;
    extern void func_00155E78(void *,int);
    func_00155E78(r+0x3FD0,2);
}
/* A zero-time impact clears the force accumulator (00231C10) for the rest of
   the retail 1/60 step. At high rates the clear lasts for the rest of that
   legacy field: later updates in the same field also run without force.
   Clock stamps are interval ends: an exact boundary still belongs to the
   preceding integration interval, not the following field. */
void port_fps_force_window(void *body,int clear)
{
    if (!port_fps_high()) return;
    Air *a=air_for(body);
    int field=D_0029E3D8-(phase==0);
    if (clear) {
        a->force_field=field;
        a->force_valid=1;
    } else if (a->force_valid && a->force_field==field) {
        float *force=(float *)((char *)body+0xE0);
        for (int i=0;i<8;i++) force[i]=(i==3 || i==7) ? 1.0f : 0.0f;
    }
}
float port_counter_fields(int counter) { return port_fps_high() ? counter+(float)phase/port_fps() : (float)counter; }

/* prev comes from the preceding real animation/simulation step, including at
   crash entry (001B1C20). Its displacement already has the selected step's
   span: scaling it again would shorten the initial throw. */
void port_ragdoll_step(void *points, int count, int stride, float damping)
{
    float dt=port_step(), d=port_decay(damping), span=1, dt2=dt*dt;
    /* Verlet stores displacement, so variable steps must convert its previous
       span to this step. New/reinitialized groups already span the current dt. */
    typedef struct { void *points; unsigned serial; float dt; } History;
    static History history[128];
    unsigned index=0;
    while (index<128 && history[index].points!=points) index++;
    if (index==128) {
        for (index=0;index<128;index++)
            if (!history[index].points || clock_serial-history[index].serial>1) break;
        if (index==128) abort(); /* no silent history aliasing */
    }
    if (history[index].points==points && history[index].serial+1==clock_serial) {
        float previous=history[index].dt;
        span=dt/previous;
        if (dt!=previous) dt2=dt*(dt+previous)*0.5f;
    }
    history[index]=(History){points,clock_serial,dt};
    for (int i=0;i<count;i++) {
        /* Particle layout checked in 00239A80; points addresses particles[0]. */
        char *p=(char *)points+i*stride;
        float *pos=(float *)p, *prev=(float *)(p+0x10), *force=(float *)(p+0x20);
        for (int k=0;k<3;k++) {
            float old=pos[k];
            pos[k] += (pos[k]-prev[k]) * d * span + force[k] * (dt2*0.0002777778f);
            prev[k]=old;
        }
        prev[3]=pos[3]; /* retail CopyVector also preserves the fourth lane */
    }
}

/* DHD_FPS_TRACE=path: read-only race observer for port/tools/test_fps_race.py. One row per
   rider per race tick, plus every real update through tick 360 in path.steps.csv (the first
   update carrying a tick comes earlier at high rates). DHD_FPS_STOP=N ends the game at tick N.
   Raw offsets are shared with puppet.c and rider.h. The CSVs hold game-derived state. */
void port_fps_observe(void)
{
    static int init, last=-1;
    static FILE *out,*steps_out;
    if (!init) {
        init=1;
        const char *path=getenv("DHD_FPS_TRACE");
        if (path && *path) {
            out=fopen(path,"w");
            if (!out) abort();
            fprintf(out,"tick,hz,slot,x,y,z,speed,checkpoint,race_ticks,finished,finish_ticks,crash,attack,animation,animation_slot,particles,frame,contact_mask,air_age,gauge,boost\n");
            char steps_path[2048];
            snprintf(steps_path,sizeof steps_path,"%s.steps.csv",path);
            steps_out=fopen(steps_path,"w");
            if (!steps_out) abort();
            fprintf(steps_out,"tick,hz,slot,frame,x,y,z,speed\n");
        }
    }
    if (!out || *(int *)PS2_HOST(0x29DED8)!=9 || *(int *)PS2_HOST(0x43F348)<2 || *(int *)PS2_HOST(0x43F348)>4) return;
    int tick=*(int *)PS2_HOST(0x43EEC8);
    if (tick<=360) {
        for (int i=0;i<10;i++) {
            unsigned char *r=(unsigned char *)(PS2_HOST(0x2AF2C0)+i*0x7BD0);
            float *p=(float *)(r+0x4270),*v=(float *)(r+0x4180);
            fprintf(steps_out,"%d,%d,%d,%llu,%.9g,%.9g,%.9g,%.9g\n",tick,port_fps(),i,
                (unsigned long long)race_steps,p[0],p[1],p[2],sqrtf(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]));
        }
        fflush(steps_out);
    }
    if (tick==last) return;
    last=tick;
    int particles=0;
    int index=*(int *)PS2_HOST(0x29E308);
    for (int i=0;i<64 && index>=0 && index<64;i++) {
        int n=*(int *)(PS2_HOST(0x67E180)+index*0x2860+0x2850);
        if (n>0 && n<100000) particles+=n;
        index=*(int *)(PS2_HOST(0x71F980)+index*12+8);
    }
    for (int i=0;i<10;i++) {
        unsigned char *r=(unsigned char *)(PS2_HOST(0x2AF2C0)+i*0x7BD0);
        float *p=(float *)(r+0x4270), *v=(float *)(r+0x4180);
        float speed=sqrtf(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);
        /* Read the owner's active clip, rather than a potentially stopped pedal
           parameter. The latter can stay at zero throughout a valid race. */
        extern int func_0012D300(void *);
        int animation_slot=19+func_0012D300(r);
        unsigned char *track=*(unsigned char **)(r+0x1E40+0x14+animation_slot*16+4);
        unsigned char *owner=*(unsigned char **)(r+0x1E40+0x14+3*16);
        if ((uintptr_t)owner>=PS2_HOST(0) && (uintptr_t)owner<PS2_HOST(PS2_RAM_SIZE)) {
            unsigned char *list=*(unsigned char **)(owner+0x24);
            if ((uintptr_t)list>=PS2_HOST(0) && (uintptr_t)list<PS2_HOST(PS2_RAM_SIZE)) {
                unsigned char *active_track=*(unsigned char **)(list+8);
                if ((uintptr_t)active_track>=PS2_HOST(0) && (uintptr_t)active_track<PS2_HOST(PS2_RAM_SIZE)) {
                    track=active_track;
                    animation_slot=-1;
                }
            }
        }
        float anim=-1;
        if ((uintptr_t)track>=PS2_HOST(0) && (uintptr_t)track<PS2_HOST(PS2_RAM_SIZE)) {
            int now=*(int *)PS2_HOST(0x29E160), start=*(int *)(track+0x14);
            int elapsed=track[0x28] ? now-start : start;
            anim=port_animation_fields((float)elapsed)* *(float *)(track+0x1C)+ *(float *)(track+0x18);
            unsigned char *range=*(unsigned char **)(track+4);
            if ((uintptr_t)range>=PS2_HOST(0) && (uintptr_t)range<PS2_HOST(PS2_RAM_SIZE)) {
                float lo=*(float *)(range+4),hi=*(float *)(range+8);
                if (hi>lo) anim=(anim-lo)/(hi-lo);
            }
        }
        fprintf(out,"%d,%d,%d,%.8g,%.8g,%.8g,%.8g,%u,%d,%u,%d,%u,%u,%.8g,%d,%d,%llu,%u,%d,%.8g,%.8g\n",
                tick,port_fps(),i,p[0],p[1],p[2],speed,r[0x7990],*(int *)(r+0x7994),
                *(unsigned char *)(PS2_HOST(0x43EF18)+i),*(int *)(PS2_HOST(0x43EEDC)+4*i),
                (r[0x4098]>>2)&1,r[0x6390],anim,animation_slot,particles,(unsigned long long)race_steps,
                r[0x4414]|r[0x4494]<<2,*(short *)(r+0x4564),*(float *)(r+0x2E98),*(float *)(r+0x7A78));
    }
    fflush(out);
    const char *stop=getenv("DHD_FPS_STOP");
    if (stop && tick>=atoi(stop)) {
        fclose(out);
        fclose(steps_out);
        out=NULL;
        ExitProcess(0);
    }
}
