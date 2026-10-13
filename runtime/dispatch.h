/* Address -> recompiled function lookup for indirect calls. */
#ifndef DISPATCH_H
#define DISPATCH_H

#include <stdint.h>

typedef struct {
    uint32_t addr;
    void (*fn)(void);
} FuncEntry;

extern const FuncEntry func_table[];
extern const int func_table_len;

void (*find_function(uint32_t addr))(void);

#endif
