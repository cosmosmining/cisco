#include "core/stats.h"

static const char *const stat_names[] = {
#define X(name) #name,
    PF_STATS_MAP(X)
#undef X
};

size_t pf_stats_count(void)
{
    return PF_ARRAY_LEN(stat_names);
}

const char *pf_stats_name(size_t i)
{
    PF_ASSERT(i < PF_ARRAY_LEN(stat_names));
    return stat_names[i];
}

uint64_t pf_stats_get(const struct pf_stats *st, size_t i)
{
    PF_ASSERT(i < PF_ARRAY_LEN(stat_names));
    const uint64_t *base = (const uint64_t *)st;
    return base[i];
}
