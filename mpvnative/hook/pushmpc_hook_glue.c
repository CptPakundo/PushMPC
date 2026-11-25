/*
 * pushmpc_hook_glue.c: compiled into the hook's private libavfilter (one per FFmpeg branch, so the public struct
 * layouts below are the system's own). Exported under the avfilter_ prefix, which libavfilter's version script keeps.
 *   avfilter_pushmpc_parse2   avfilter_graph_parse2 for graph strings that name pushmpc ("lavfi=[...]"): the system's
 *                             segment parser, our filter pre-created for the "pushmpc" entries (create_filters skips
 *                             entries whose filter is set), then the system's apply. Only the system's public API runs
 *                             on the system's graph, through the function table the hook passes in.
 *   avfilter_pushmpc_avutil_built  the libavutil version this copy was compiled against (the hook checks the system's)
 */
#include <stdio.h>
#include <string.h>
#include "libavutil/error.h"
#include "libavutil/mem.h"
#include "libavutil/version.h"
#include "avfilter.h"

#if LIBAVFILTER_VERSION_MAJOR >= 9
typedef struct PushmpcSysApi {
    int (*segment_parse)(AVFilterGraph *graph, const char *graph_str, int flags, AVFilterGraphSegment **seg);
    int (*segment_apply)(AVFilterGraphSegment *seg, int flags, AVFilterInOut **inputs, AVFilterInOut **outputs);
    void (*segment_free)(AVFilterGraphSegment **seg);
    AVFilterContext *(*alloc_filter)(AVFilterGraph *graph, const AVFilter *filter, const char *name);
    void (*filter_free)(AVFilterContext *filter);
} PushmpcSysApi;
#else
typedef struct PushmpcSysApi PushmpcSysApi;   /* graph strings are not hooked below FFmpeg 6.0 */
#endif

unsigned avfilter_pushmpc_avutil_built(void);
unsigned avfilter_pushmpc_avutil_built(void) { return LIBAVUTIL_VERSION_INT; }

int avfilter_pushmpc_parse2(AVFilterGraph *graph, const char *filters, AVFilterInOut **inputs, AVFilterInOut **outputs,
                            const PushmpcSysApi *api, const AVFilter *pushmpc);
int avfilter_pushmpc_parse2(AVFilterGraph *graph, const char *filters, AVFilterInOut **inputs, AVFilterInOut **outputs,
                            const PushmpcSysApi *api, const AVFilter *pushmpc)
{
#if LIBAVFILTER_VERSION_MAJOR < 9   /* no segment API before FFmpeg 6.0: graph strings are not hooked there */
    (void)graph; (void)filters; (void)inputs; (void)outputs; (void)api; (void)pushmpc;
    return AVERROR(ENOSYS);
#else
    AVFilterGraphSegment *seg = NULL;
    int ret = api->segment_parse(graph, filters, 0, &seg);
    size_t c, f, n = 0;
    if (ret < 0)
        return ret;
    for (c = 0; c < seg->nb_chains; c++)
        for (f = 0; f < seg->chains[c]->nb_filters; f++) {
            AVFilterParams *p = seg->chains[c]->filters[f];
            char name[64];
            if (p->filter || !p->filter_name || strcmp(p->filter_name, "pushmpc"))
                continue;
            snprintf(name, sizeof(name), "Parsed_pushmpc_%zu", n++);
            p->filter = api->alloc_filter(graph, pushmpc, p->instance_name ? p->instance_name : name);
            av_freep(&p->filter_name);
            if (!p->filter) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }
        }
    ret = api->segment_apply(seg, 0, inputs, outputs);
fail:
    api->segment_free(&seg);
    if (ret < 0) {   /* as upstream avfilter_graph_parse2 */
        while (graph->nb_filters)
            api->filter_free(graph->filters[0]);
        av_freep(&graph->filters);
    }
    return ret;
#endif
}
