#ifndef ANYGBA_CODEGEN_FIXTURE_H
#define ANYGBA_CODEGEN_FIXTURE_H

#include <anygba/cpu.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct anygba_fixture_entry {
    uint32_t raw;
    uint32_t addr;
    uint32_t thumb;
    anygba_fn fn;
} anygba_fixture_entry;

extern const anygba_fixture_entry anygba_fixture[];
extern const unsigned anygba_fixture_count;

#ifdef __cplusplus
}
#endif

#endif
