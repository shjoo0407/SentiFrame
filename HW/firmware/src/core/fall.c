/* SPDX-License-Identifier: MIT */
#include "sf_core.h"
#include <math.h>
#include <string.h>

void sf_fall_reset(struct sf_fall *f) { memset(f,0,sizeof(*f)); }
static float norm(const float *v) { return sqrtf(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]); }
enum sf_event sf_fall_push(struct sf_fall *f, const struct sf_sample *s) {
    if (f->count && ((uint32_t)(s->time_ms-f->last_time)>100u || s->flags&SF_DATA_GAP))
        sf_fall_reset(f); /* missing time is not immobility */
    f->last_time=s->time_ms;
    float a[3];
    for (unsigned i=0;i<3;i++) a[i]=(float)s->a[i]/2048.0f; /* configured +/-16g */
    float g=norm(a), mean=f->count?(float)(f->sum/f->count):1.0f;
    double var=f->count?f->sum2/f->count-(double)mean*(double)mean:0.0;
    float sd=sqrtf((float)fmax(var,0.0025));
    f->last_z=(g-mean)/sd; /* computed BEFORE admitting the current sample */
    enum sf_event event=0;
    bool impact=g>=3.0f || (f->count>=25&&g>=2.0f&&f->last_z>=6.0f);
    if (!f->pending&&f->count>=25&&impact) {
        memcpy(f->baseline,f->gravity,sizeof(f->baseline));
        f->impact_time=s->time_ms; f->peak_g=g; f->pending=true; f->still=false;
        event=SF_IMPACT;
    } else if (f->pending) {
        if (g>f->peak_g) f->peak_g=g;
        float gyro=0;
        for (unsigned i=0;i<3;i++) gyro+=(float)s->g[i]*(float)s->g[i];
        bool quiet=g>0.7f&&g<1.3f&&(!(s->flags&SF_GYRO_VALID)||gyro<491.0f*491.0f);
        if (!quiet) f->still=false;
        else if (!f->still) { f->still=true; f->still_time=s->time_ms; }
        float dot=0, bn=norm(f->baseline), an=norm(a);
        for (unsigned i=0;i<3;i++) dot+=a[i]*f->baseline[i];
        bool tilted=bn>0.5f&&an>0.5f&&dot/(bn*an)<0.7071f;
        if (quiet&&tilted&&f->still&&(uint32_t)(s->time_ms-f->still_time)>=750) {
            event=SF_FALL_SUSPECTED; f->pending=false;
        } else if ((uint32_t)(s->time_ms-f->impact_time)>3000) f->pending=false;
    }
    if (g>0.7f&&g<1.3f) for (unsigned i=0;i<3;i++)
        f->gravity[i]=f->count?0.95f*f->gravity[i]+0.05f*a[i]:a[i];
    if (f->count==SF_HISTORY_SIZE) {
        double old=f->magnitude[f->pos]; f->sum-=old; f->sum2-=old*old;
    } else f->count++;
    f->history[f->pos]=*s; f->magnitude[f->pos]=g;
    f->sum+=(double)g; f->sum2+=(double)g*(double)g;
    f->pos=(f->pos+1)%SF_HISTORY_SIZE;
    if (f->pos==0) { /* bounded rebase controls subtraction roundoff */
        f->sum=0; f->sum2=0;
        for (size_t i=0;i<f->count;i++) { double x=f->magnitude[i]; f->sum+=x; f->sum2+=x*x; }
    }
    return event;
}
size_t sf_fall_history(const struct sf_fall *f, struct sf_sample *out, size_t cap) {
    size_t n=f->count<cap?f->count:cap;
    size_t start=(f->pos+SF_HISTORY_SIZE-n)%SF_HISTORY_SIZE;
    for (size_t i=0;i<n;i++) out[i]=f->history[(start+i)%SF_HISTORY_SIZE];
    return n;
}
