/*
 * SPDX-FileCopyrightText: 2021 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/* ESP32 Linker Script Memory Layout

   This file describes the memory layout (memory blocks) as virtual
   memory addresses.

   esp32.project.ld contains output sections to link compiler output
   into these memory blocks.

   ***

   This linker script is passed through the C preprocessor to include
   configuration options.

   Please use preprocessor features sparingly! Restrict
   to simple macros with numeric values, and/or #if/#endif blocks.
*/
/*

 * Automatically generated file. DO NOT EDIT.

 * Espressif IoT Development Framework (ESP-IDF) 6.1.0 Configuration Header

 */
       
/* List of deprecated options */
/*
 * SPDX-FileCopyrightText: 2021-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/* CPU instruction prefetch padding size for flash mmap scenario */
/*
 * PMP region granularity size
 * Software may determine the PMP granularity by writing zero to pmp0cfg, then writing all ones
 * to pmpaddr0, then reading back pmpaddr0. If G is the index of the least-significant bit set,
 * the PMP granularity is 2^G+2 bytes.
 */
/* CPU instruction prefetch padding size for memory protection scenario */
/* Memory alignment size for PMS */
    /* rtc timer data (s_rtc_timer_retain_mem, see esp_clk.c files). For rtc_timer_data_in_rtc_mem section. */
/*
 * APP_BUILD_MINIMIZE_BINARY_CHANGES_PADDING
 *
 * Aligns the current location (.) to CONFIG_APP_BUILD_MINIMIZE_BINARY_CHANGES_PADDING.
 * If more than half of the alignment block is already consumed,
 * extra padding is applied to skip to the next boundary.
 * For example, if the current location (.) is 0x1900 and the padding is 4 KB,
 * the location will be moved to 0x3000 instead of 0x2000 because less than
 * half of the current 4 KB block remains (0x2000 - 0x1900 < 2 KB).
 */
/* If BT is not built at all */
MEMORY
{
  /* All these values assume the flash cache is on, and have the blocks this uses subtracted from the length
  of the various regions. The 'data access port' dram/drom regions map to the same iram/irom regions but
  are connected to the data port of the CPU and eg allow bytewise access. */
  /* IRAM for PRO cpu. Not sure if happy with this, this is MMU area... */
  iram0_0_seg (RX) : org = 0x40080000, len = 0x20000 + 0x0
  /* Even though the segment name is iram, it is actually mapped to flash
  */
  iram0_2_seg (RX) : org = 0x400D0020, len = 0x330000-0x20
  /*
    (0x20 offset above is a convenience for the app binary image generation.
    Flash cache has 64KB pages. The .bin file which is flashed to the chip
    has a 0x18 byte file header, and each segment has a 0x08 byte segment
    header. Setting this offset makes it simple to meet the flash cache MMU's
    constraint that (paddr % 64KB == vaddr % 64KB).)
  */
  /* Shared data RAM, excluding memory reserved for ROM bss/data/stack.

     Enabling Bluetooth & Trace Memory features in menuconfig will decrease
     the amount of RAM available.

     Note: Length of this section *should* be 0x50000, and this extra DRAM is available
     in heap at runtime. However due to static ROM memory usage at this 176KB mark, the
     additional static memory temporarily cannot be used.
  */
  dram0_0_seg (RW) : org = 0x3FFB0000 + 0,
                                     len = 0x2c200 - 0
  /* Flash mapped constant data */
  drom0_0_seg (R) : org = 0x3F400020, len = 0x400000-0x20
  /* (See iram0_2_seg for meaning of 0x20 offset in the above.) */
/**
 *  RTC Memory Layout for ESP32
 *
 *  ESP32 has two separate RTC memory regions:
 *  1. RTC FAST Memory: 8KB, accessible by PRO_CPU only
 *  2. RTC SLOW Memory: 8KB, accessible by both CPUs and ULP coprocessor
 *
 *  ==================== RTC FAST Memory ====================
 *  Size: 8KB (0x2000)
 *  IRAM View: 0x400C0000 - 0x400C2000 (executable)
 *  DRAM View: 0x3FF80000 - 0x3FF82000 (data access)
 *
 *  +-----------------------------+ 0x400C2000 / 0x3FF82000 (RTC FAST memory end)
 *  |   rtc_fast_reserved_seg     |
 *  | (ESP_BOOTLOADER_RESERVE_RTC)|
 *  |- - - - - - - - - - - - - - -|
 *  | bootloader_data_rtc_mem     | s_bootloader_retain_mem
 *  +-----------------------------+ 0x400C2000 - ESP_BOOTLOADER_RESERVE_RTC
 *  |     rtc_iram_seg /          |
 *  |     rtc_data_seg            |
 *  | .rtc.text                   | RTC wake stub code
 *  |- - - - - - - - - - - - - - -|
 *  | .rtc.data / .rtc.rodata     | (when CONFIG_ESP32_RTCDATA_IN_FAST_MEM)
 *  |- - - - - - - - - - - - - - -|
 *  | .rtc.bss                    |
 *  +-----------------------------+ 0x400C0000 / 0x3FF80000 (RTC FAST memory start)
 *
 *  ==================== RTC SLOW Memory ====================
 *  Size: 8KB (0x2000)
 *  Address: 0x50000000 - 0x50002000
 *
 *  +-----------------------------+ 0x50002000 (RTC SLOW memory end)
 *  |   rtc_slow_reserved_seg     |
 *  |    (RESERVE_RTC_MEM = 24)   |
 *  |- - - - - - - - - - - - - - -|
 *  | rtc_timer_data_in_rtc_mem   | s_rtc_timer_retain_mem (24 bytes)
 *  | (RTC_TIMER_RESERVE_RTC)     |
 *  +-----------------------------+ 0x50002000 - RESERVE_RTC_MEM
 *  |      rtc_slow_seg           |
 *  |- - - - - - - - - - - - - - -|
 *  | .rtc.data / .rtc.rodata     | (when !CONFIG_ESP32_RTCDATA_IN_FAST_MEM)
 *  | .rtc.bss                    |
 *  +-----------------------------+ 0x50000000 + CONFIG_ULP_COPROC_RESERVE_MEM (if ULP enabled)
 *  | ULP Coprocessor Reserve     | (Optional, CONFIG_ULP_COPROC_RESERVE_MEM)
 *  | (code + data)               |
 *  +-----------------------------+ 0x50000000 (RTC SLOW memory start)
 *
 *  ESP_BOOTLOADER_RESERVE_RTC = ALIGN_UP(CONFIG_BOOTLOADER_RESERVE_RTC_SIZE, 8)
 *  RESERVE_RTC_MEM = RTC_TIMER_RESERVE_RTC (24 bytes) for ESP32
 *
 *  Note: RTC data can be placed in either FAST or SLOW memory based on
 *        CONFIG_ESP32_RTCDATA_IN_FAST_MEM configuration option.
 */
  /* RTC fast memory (executable). Persists over deep sleep. */
  rtc_iram_seg(RWX) : org = 0x400C0000, len = 0x2000 - 0
  /* RTC fast memory (same block as above, rtc_iram_seg), viewed from data bus */
  rtc_data_seg(RW) : org = 0x3ff80000, len = 0x2000 - 0
  /* We reduced the size of rtc_iram_seg and rtc_data_seg by ESP_BOOTLOADER_RESERVE_RTC value.
     It reserves the amount of RTC fast memory that we use for this memory segment.
     This segment is intended for keeping bootloader rtc data (s_bootloader_retain_mem, when a Kconfig option is on).
     The aim of this is to keep data that will not be moved around and have a fixed address.
     org = 0x3ff80000 + 0x2000 - ESP_BOOTLOADER_RESERVE_RTC == SOC_RTC_DRAM_HIGH - sizeof(rtc_retain_mem_t)
  */
  rtc_fast_reserved_seg(RW) : org = 0x3ff80000 + 0x2000 - 0, len = 0
  /* RTC slow memory (data accessible). Persists over deep sleep.

     Start of RTC slow memory is reserved for ULP co-processor code + data, if enabled.
  */
  rtc_slow_seg(RW) : org = 0x50000000, len = 0x2000 - ((24))
  /* We reduced the size of rtc_slow_seg by RESERVE_RTC_MEM value.
     It reserves the amount of RTC slow memory that we use for this memory segment.
     This segment is intended for keeping rtc timer data (s_rtc_timer_retain_mem, see esp_clk.c files).
     The aim of this is to keep data that will not be moved around and have a fixed address.
     org = 0x50000000 + 0x2000 - RESERVE_RTC_MEM
  */
  rtc_slow_reserved_seg(RW) : org = 0x50000000 + 0x2000 - ((24)), len = ((24))
  /* external memory */
  extern_ram_seg(RW) : org = 0x3F800000,
                                     len = 0x400000
}
_heap_start = _heap_low_start;
_sram1_iram_start = 0x400A0000;
_sram1_iram_len = ( _iram_end > _sram1_iram_start) ? (_iram_end - _sram1_iram_start) : 0;
_heap_end = ALIGN(0x40000000 - _sram1_iram_len - 3, 4);
/* The lines below define location alias for .rtc.data section based on Kconfig option.
   When the option is not defined then use slow memory segment
   else the data will be placed in fast memory segment */
REGION_ALIAS("rtc_data_location", rtc_slow_seg );
  REGION_ALIAS("default_code_seg", iram0_2_seg);
  REGION_ALIAS("default_rodata_seg", drom0_0_seg);
/**
 *  If rodata default segment is placed in `drom0_0_seg`, then flash's first rodata section must
 *  also be first in the segment.
 */
  ASSERT(_rodata_start == ORIGIN(default_rodata_seg),
         ".flash.appdesc section must be placed at the beginning of the rodata segment.")
