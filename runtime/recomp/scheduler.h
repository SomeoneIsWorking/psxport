#ifndef SCHEDULER_H
#define SCHEDULER_H
#include <stdint.h>
struct Core;
#define TASKBASE 0x801fe000u   // task obj table base (slot i at +i*0x70)
#define TASKSTRIDE 0x70u
#define CUR_TASK 0x1f800138u   // DAT_1f800138: scheduler current-task ptr
void scheduler_yield(Core* c);              // FUN_80080880 ChangeThread override — the universal task-switch/yield primitive
void native_scheduler_step(Core* c);  // one scheduler pass over the 3 task slots (replaces FUN_80051e60)
#endif
