/*
clock-control.c - part of time-signal
JJY/MSF/WWVB/DCF77 radio transmitter for Raspberry Pi

Copyright (C) 2024 Steve Matos
Source: https://github.com/steve1515/time-signal

Parts of this code are based on time-signal code written by Pierre Brial
Source: https://github.com/harlock974/time-signal
Copyright (C) 2023 Pierre Brial <p.brial@tethys.re>

Parts of this code are based on txtempus code written by Henner Zeller
Source: https://github.com/hzeller/txtempus
Copyright (C) 2018 Henner Zeller <h.zeller@acm.org>
Licensed under the GNU General Public License, version 3 or later

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/


#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <inttypes.h>
#include <math.h>
#include <fcntl.h>
#include <sys/mman.h>
#include "clock-control.h"

// Peripheral Base Addresses
#define BCM2708_PERI_BASE     0x20000000    // BCM2835 - Model 1
#define BCM2709_PERI_BASE     0x3f000000    // BCM2836 - Model 2
#define BCM2710_PERI_BASE     0x3f000000    // BCM2837 - Model 3
#define BCM2711_PERI_BASE     0xfe000000    // Model 4
#define BCM2712_PERI_BASE     0x1f000d0000  // Model 5

#define GPIO_REGISTER_OFFSET  0x00200000
#define CLOCK_REGISTER_OFFSET 0x00101000

// GPIO Register Word Offsets
#define GPIO_GPFSEL_OFFSET 0
#define GPIO_GPSET_OFFSET  7
#define GPIO_GPCLR_OFFSET  10

// Clock Control Register Word Offsets
#define CLK_GP0CTL 28
#define CLK_GP0DIV 29

#define CLK_GP1CTL 30
#define CLK_GP1DIV 31

#define CLK_GP2CTL 32
#define CLK_GP2DIV 33

// GPIO Setup Macros
// Note: Always call GPIO_INPUT() to clear register bits
//       before calling GPIO_OUTPUT() or GPIO_ALT0().
#define GPIO_INPUT(x)  *(_pGpioVirtMem + GPIO_GPFSEL_OFFSET + ((x) / 10)) &= ~(7 << (((x) % 10) * 3))
#define GPIO_OUTPUT(x) *(_pGpioVirtMem + GPIO_GPFSEL_OFFSET + ((x) / 10)) |=  (1 << (((x) % 10) * 3))
#define GPIO_ALT0(x)   *(_pGpioVirtMem + GPIO_GPFSEL_OFFSET + ((x) / 10)) |=  (4 << (((x) % 10) * 3))

// GPIO Set/Clear Macros
#define GPIO_SET(x)   *(_pGpioVirtMem + GPIO_GPSET_OFFSET + ((x) / 32)) = (1 << ((x) % 32))
#define GPIO_CLEAR(x) *(_pGpioVirtMem + GPIO_GPCLR_OFFSET + ((x) / 32)) = (1 << ((x) % 32))

// Clock Control Macros
#define CLK_PASSWD      (0x5a << 24)

#define CLK_CTL_MASH(x) ((x) << 9)
#define CLK_CTL_FLIP      (1 << 8)
#define CLK_CTL_BUSY      (1 << 7)
#define CLK_CTL_KILL      (1 << 5)
#define CLK_CTL_ENAB      (1 << 4)
#define CLK_CTL_SRC(x)  ((x) << 0)

#define CLK_DIV_DIVI(x) ((x) << 12)
#define CLK_DIV_DIVF(x) ((x) << 0)


static enum RaspberryPiModel GetPiModel();
static uint32_t *MapBcmRegister(off_t registerOffset);


enum RaspberryPiModel
{
  PI_MODEL_1,
  PI_MODEL_2,
  PI_MODEL_3,
  PI_MODEL_4,
  PI_MODEL_5,
  PI_MODEL_UNKNOWN
};


enum RaspberryPiModel _piModel;
volatile uint32_t *_pGpioVirtMem;
volatile uint32_t *_pClockVirtMem;


bool GpioInit()
{
  _piModel = GetPiModel();
  if (_piModel == PI_MODEL_UNKNOWN)
  {
    fprintf(stderr, "Error: Raspberry Pi model not supported.\n");
    return false;
  }

  _pGpioVirtMem = MapBcmRegister(GPIO_REGISTER_OFFSET);
  if (_pGpioVirtMem == NULL)
  {
    fprintf(stderr, "Failed to map GPIO registers. Ensure program is run with root privileges.\n");
    return false;
  }

  _pClockVirtMem = MapBcmRegister(CLOCK_REGISTER_OFFSET);
  if (_pClockVirtMem == NULL)
  {
    fprintf(stderr, "Failed to map clock registers. Ensure program is run with root privileges.\n");
    return false;
  }
  
  return true;
}


double StartClock(double requested_freq)
{
  // Reference: https://www.raspberrypi.org/app/uploads/2012/02/BCM2835-ARM-Peripherals.pdf, Page 105

  // Figure out best clock source to get closest to the requested frequency with MASH=1.
  // We check starting from the highest frequency to find lowest jitter opportunity first.
  struct { int src; double frequency; } kClockSources[] =
    {
      { 7, _piModel == PI_MODEL_4 ? 0       : 216.0e6 },  // HDMI  <- this can be problematic if monitor connected
      //{ 5, _piModel == PI_MODEL_4 ? 750.0e6 : 500.0e6 },  // PLLD
      { 1, _piModel == PI_MODEL_4 ? 54.0e6  : 19.2e6  },  // regular oscillator
    };

  int divI = -1;
  int divF = -1;
  int best_clock_source = -1;
  double smallest_error_so_far = 1e9;

  printf("Clock Sources:\n");
  for (size_t i = 0; i < sizeof(kClockSources) / sizeof(kClockSources[0]); ++i)
  {
    printf("%d : ", kClockSources[i].src);

    double division = kClockSources[i].frequency / requested_freq;
    if (division < 2 || division > 4095)
    {
      puts("");
      continue;
    }

    int test_divi = (int)division;
    int test_divf = (division - test_divi) * 1024;
    double freq = kClockSources[i].frequency / (test_divi + test_divf / 1024.0);
    double error = fabsl(requested_freq - freq);

    printf("freq = %.4f error = %.4f\n", freq, error);
    if (error >= smallest_error_so_far)
      continue;
    
    smallest_error_so_far = error;
    best_clock_source = i;
    divI = test_divi;
    divF = test_divf;
  }

  if (divI < 0)
    return -1.0;  // Couldn't find any suitable clock.

  StopClock();

  const uint32_t ctl  = CLK_GP0CTL;
  const uint32_t div  = CLK_GP0DIV;
  const uint32_t src  = kClockSources[best_clock_source].src;
  const uint32_t mash = 1;  // Good approximation, low jitter.

  _pClockVirtMem[div] = CLK_PASSWD | CLK_DIV_DIVI(divI) | CLK_DIV_DIVF(divF);
  usleep(10);
  _pClockVirtMem[ctl] = CLK_PASSWD | CLK_CTL_MASH(mash) | CLK_CTL_SRC(src);
  usleep(10);
  _pClockVirtMem[ctl] |= CLK_PASSWD | CLK_CTL_ENAB;

  // EnableClockOutput(true);

  // There have been reports of different clock source frequencies.
  // This helps figuring out which source was picked.
  printf("\nChoose clock %d at %gHz / %.3f = %.3f\n\n",
         kClockSources[best_clock_source].src,
         kClockSources[best_clock_source].frequency,
         divI + divF / 1024.0,
         kClockSources[best_clock_source].frequency / (divI + divF / 1024.0));

  return kClockSources[best_clock_source].frequency / (divI + divF / 1024.0);
}


void StopClock()
{
  *(_pClockVirtMem + CLK_GP0CTL) = CLK_PASSWD | (*(_pClockVirtMem + CLK_GP0CTL) & ~CLK_CTL_ENAB);

  // Wait until clock confirms not to be busy anymore
  while (*(_pClockVirtMem + CLK_GP0CTL) & CLK_CTL_BUSY)
    usleep(10);

  EnableClockOutput(false);
}


void EnableClockOutput(bool on)
{
  if (on)
    GPIO_ALT0(4);  // Pinmux GPIO4 into outputting clock.
  else
    GPIO_INPUT(4);
}


static enum RaspberryPiModel GetPiModel()
{
  FILE *fp;
  char *line = NULL;
  size_t len = 0;
  unsigned int revisionCode = 0;

  fp = fopen("/proc/cpuinfo", "r");
  if (fp == NULL)
    return PI_MODEL_UNKNOWN;

  while (getline(&line, &len, fp) != -1)
  {
    if (sscanf(line, "Revision : %x", &revisionCode) >= 1)
      break;
  }

  fclose(fp);
  free(line);

  if (revisionCode == 0)
    return PI_MODEL_UNKNOWN;

  // Reference: https://github.com/raspberrypi/documentation/blob/develop/documentation/asciidoc/computers/raspberry-pi/revision-codes.adoc
  uint8_t newFlag = (revisionCode >> 23) & 0x01;
  uint8_t modelType = (revisionCode >> 4) & 0xff;

  // Handle old style revision codes
  if (!newFlag)
  {
    if ((revisionCode >= 0x0002 && revisionCode <= 0x0009) ||
        (revisionCode >= 0x000d && revisionCode <= 0x0015))
    {
      return PI_MODEL_1;
    }
    else
    {
      return PI_MODEL_UNKNOWN;
    }
  }

  switch(modelType)
  {
    case 0x00:  // A
    case 0x01:  // B
    case 0x02:  // A+
    case 0x03:  // B+
    case 0x06:  // CM1
    case 0x09:  // Zero
    case 0x0c:  // Zero W
      return PI_MODEL_1;

    case 0x04:  // 2B
      return PI_MODEL_2;

    case 0x08:  // 3B
    case 0x0a:  // CM3
    case 0x0d:  // 3B+
    case 0x0e:  // 3A+
    case 0x10:  // CM3+
    case 0x12:  // Zero 2 W
      return PI_MODEL_3;

    case 0x11:  // 4B
    case 0x13:  // 400
    case 0x14:  // CM4
    case 0x15:  // CM4S
      return PI_MODEL_4;

    case 0x17:  // 5
      return PI_MODEL_5;

    case 0x05:  // Alpha (early prototype)
    case 0x0f:  // Internal use only
    case 0x16:  // Internal use only
    default:
      return PI_MODEL_UNKNOWN;
  }

  return PI_MODEL_UNKNOWN;
}


static uint32_t *MapBcmRegister(off_t registerOffset)
{
  off_t baseAddress = 0;
  switch (_piModel)
  {
    case PI_MODEL_1:
      baseAddress = BCM2708_PERI_BASE;
      break;

    case PI_MODEL_2:
      baseAddress = BCM2709_PERI_BASE;
      break;

    case PI_MODEL_3:
      baseAddress = BCM2710_PERI_BASE;
      break;

    case PI_MODEL_4:
      baseAddress = BCM2711_PERI_BASE;
      break;

    default:
      fprintf(stderr, "Error: Raspberry Pi model not supported. (%d)\n", _piModel);
      return NULL;
  }

  int fd = open("/dev/mem", O_RDWR | O_SYNC);
  if (fd < 0)
  {
    perror("Failed to open /dev/mem");
    return NULL;
  }

  uint32_t *pVirtMem = (uint32_t*)mmap(NULL,                         // Kernel chooses mapped address
                                       getpagesize(),                // Length of mapping
                                       PROT_READ | PROT_WRITE,       // Enable reading and writing to mapped memory
                                       MAP_SHARED,                   // Shared with other processes
                                       fd,                           // File to map
                                       baseAddress + registerOffset  // Offset to BCM peripheral
  );

  close(fd);

  if (pVirtMem == MAP_FAILED)
  {
    perror("Failed to perform mmap");
    return NULL;
  }

  return pVirtMem;
}
