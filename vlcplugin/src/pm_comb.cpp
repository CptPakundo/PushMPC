// pm_comb.cpp: the C interface of pm_comb.h over pushmpc/content_detect.h's CombMeter. Licence: GPL-3.0-or-later.
#include "pm_comb.h"
#include <new>
#include "pushmpc/content_detect.h"

using pushmpc::content::CombMeter;
static_assert(CombMeter::kGiveUp == PM_COMB_GIVE_UP, "pm_comb.h's give-up count is CombMeter's");
static_assert(int(CombMeter::kInterlaced) == PM_COMB_INTERLACED && int(CombMeter::kProgressive) == PM_COMB_PROGRESSIVE, "verdict codes");

extern "C" void *pm_comb_new(void) { return new (std::nothrow) CombMeter(); }
extern "C" int pm_comb_push(void *c, const void *luma, ptrdiff_t stride, int w, int h, int bps, int shift)
{
    auto *m = static_cast<CombMeter *>(c);
    if (!m || !luma) return PM_COMB_MEASURING;
    try {
        return bps > 1 ? int(m->Push(static_cast<const uint16_t *>(luma), stride / 2, w, h, shift)) : int(m->Push(static_cast<const uint8_t *>(luma), stride, w, h, 0));
    } catch (...) { return int(m->verdict()); }
}
extern "C" void pm_comb_counts(void *c, int *measured, int *combed)
{
    auto *m = static_cast<CombMeter *>(c);
    if (measured) *measured = m ? m->measured() : 0;
    if (combed) *combed = m ? m->combed() : 0;
}
extern "C" void pm_comb_free(void *c) { delete static_cast<CombMeter *>(c); }
