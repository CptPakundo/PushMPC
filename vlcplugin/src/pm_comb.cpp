// pm_comb.cpp: the C interface of pm_comb.h over pushmpc/content_detect.h's CombMeter (field combing) and
// pushmpc/cadence_detect.h's CadenceMeter (2:3 pulldown), fed by the same frames. Licence: GPL-3.0-or-later.
#include "pm_comb.h"
#include <new>
#include "pushmpc/content_detect.h"
#include "pushmpc/cadence_detect.h"

using pushmpc::content::CadenceMeter;
using pushmpc::content::CombMeter;
static_assert(CombMeter::kGiveUp == PM_COMB_GIVE_UP, "pm_comb.h's give-up count is CombMeter's");
static_assert(int(CombMeter::kInterlaced) == PM_COMB_INTERLACED && int(CombMeter::kProgressive) == PM_COMB_PROGRESSIVE, "verdict codes");
static_assert(int(CadenceMeter::kTelecine) == PM_CADENCE_TELECINE && int(CadenceMeter::kNone) == PM_CADENCE_NONE, "cadence codes");

namespace {
struct State { CombMeter comb; CadenceMeter cad; int pushed = 0; };   // pushed: every frame, also after the cadence verdict
}

extern "C" void *pm_comb_new(void) { return new (std::nothrow) State(); }
extern "C" int pm_comb_push(void *c, const void *luma, ptrdiff_t stride, int w, int h, int bps, int shift)
{
    auto *s = static_cast<State *>(c);
    if (!s || !luma) return PM_COMB_MEASURING;
    s->pushed++;   // the cadence phase of the current frame stays (pushed - 1) % 5 when the comb verdict comes later
    try {
        if (bps > 1) {
            const auto *y = static_cast<const uint16_t *>(luma);
            if (s->cad.verdict() == CadenceMeter::kMeasuring) s->cad.Push(y, stride / 2, w, h, shift);
            return s->comb.verdict() == CombMeter::kMeasuring ? int(s->comb.Push(y, stride / 2, w, h, shift)) : int(s->comb.verdict());
        }
        const auto *y = static_cast<const uint8_t *>(luma);
        if (s->cad.verdict() == CadenceMeter::kMeasuring) s->cad.Push(y, stride, w, h, 0);
        return s->comb.verdict() == CombMeter::kMeasuring ? int(s->comb.Push(y, stride, w, h, 0)) : int(s->comb.verdict());
    } catch (...) { return int(s->comb.verdict()); }
}
extern "C" void pm_comb_counts(void *c, int *measured, int *combed)
{
    auto *s = static_cast<State *>(c);
    if (measured) *measured = s ? s->comb.measured() : 0;
    if (combed) *combed = s ? s->comb.combed() : 0;
}
extern "C" int pm_comb_cadence(void *c, int *measured, int *combed, int *phase, int *tff, int *frames)
{
    auto *s = static_cast<State *>(c);
    if (measured) *measured = s ? s->cad.measured() : 0;
    if (combed) *combed = s ? s->cad.combed() : 0;
    if (phase) *phase = s ? s->cad.phase() : 0;
    if (tff) *tff = s ? (s->cad.topFieldFirst() ? 1 : 0) : 1;
    if (frames) *frames = s ? s->pushed : 0;
    return s ? int(s->cad.verdict()) : PM_CADENCE_MEASURING;
}
extern "C" void pm_comb_free(void *c) { delete static_cast<State *>(c); }
