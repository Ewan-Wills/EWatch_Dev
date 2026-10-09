#pragma once
#define IRAM_ATTR
#define DRAM_ATTR
#define EXT_RAM_ATTR
#define EXT_RAM_BSS_ATTR
#define RTC_DATA_ATTR
#define RTC_NOINIT_ATTR
#define RTC_FAST_ATTR
#define RTC_SLOW_ATTR
#define RTC_IRAM_ATTR
#define WORD_ALIGNED_ATTR __attribute__((aligned(4)))
#define NOINLINE_ATTR __attribute__((noinline))
#define IRAM_DATA_ATTR
#define DMA_ATTR
#define PSRAM_ATTR
