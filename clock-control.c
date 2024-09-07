/*
clock-control.c - part of time-signal
DCF77/JJY/MSF/WWVB radio transmitter for Raspberry Pi

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
#include <string.h>
#include <float.h>
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

// Helper Macros
#define ARRAY_LENGTH(x) (sizeof(x) / sizeof((x)[0]))


static enum RaspberryPiModel get_pi_model();
static void update_clock_source_frequencies();
static uint32_t *map_bcm_register(off_t registerOffset);


enum RaspberryPiModel
{
  PI_MODEL_1 = 1,
  PI_MODEL_2 = 2,
  PI_MODEL_3 = 3,
  PI_MODEL_4 = 4,
  PI_MODEL_5 = 5,
  PI_MODEL_UNKNOWN = -1
};

typedef struct
{
  int clockSource;        // Pi clock source number
  char clockString[10];   // Pi clock source string
  bool enableForUse;      // Enabled for use in this program
  double clockFrequency;  // Clock frequency
} CLOCK_SOURCE;


static enum RaspberryPiModel _piModel;
static volatile uint32_t *_pGpioVirtMem;
static volatile uint32_t *_pClockVirtMem;

// Reference: /sys/kernel/debug/clk/clk_summary
static CLOCK_SOURCE _clockSources[] =
  {
    { 1, "osc",      true,  0 },  // Oscillator (19.2 MHz Pi1-3 / 54.0 MHz Pi4)
    { 4, "plla_per", false, 0 },  // PLLA Per (Changes with audio usage.)
    { 5, "pllc_per", false, 0 },  // PLLC Per (Changes with core clock.)
    { 6, "plld_per", true,  0 },  // PLLD Per (500 MHz Pi1-3 / 750 MHz Pi4)
    { 7, "pllh_aux", true,  0 },  // PLLH Aux / HDMI (216 MHz - Changes with display mode.)
  };


static enum RaspberryPiModel get_pi_model()
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


static void update_clock_source_frequencies()
{
  char buffer[1024];
  FILE *fp;
  char *line = NULL;
  size_t len = 0;
  double freqValue = 0;

  for (size_t i = 0; i < ARRAY_LENGTH(_clockSources); i++)
  {
    strcpy(buffer, "/sys/kernel/debug/clk/");
    strcat(buffer, _clockSources[i].clockString);
    strcat(buffer, "/clk_rate");

    fp = fopen(buffer, "r");
    if (fp == NULL)
    {
      _clockSources[i].clockFrequency = 0;
      continue;
    }

    if (getline(&line, &len, fp) == -1)
    {
      _clockSources[i].clockFrequency = 0;
      fclose(fp);
      free(line);
      continue;
    }

    if (sscanf(line, "%lf", &freqValue) < 1)
    {
      _clockSources[i].clockFrequency = 0;
      fclose(fp);
      free(line);
      continue;
    }

    _clockSources[i].clockFrequency = freqValue;

    fclose(fp);
    free(line);
  }
}


static uint32_t *map_bcm_register(off_t registerOffset)
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


bool gpio_init()
{
  _piModel = get_pi_model();
  if (_piModel == PI_MODEL_UNKNOWN)
  {
    fprintf(stderr, "Error: Raspberry Pi model not supported.\n");
    return false;
  }

  _pGpioVirtMem = map_bcm_register(GPIO_REGISTER_OFFSET);
  if (_pGpioVirtMem == NULL)
  {
    fprintf(stderr, "Failed to map GPIO registers. Ensure program is run with root privileges.\n");
    return false;
  }

  _pClockVirtMem = map_bcm_register(CLOCK_REGISTER_OFFSET);
  if (_pClockVirtMem == NULL)
  {
    fprintf(stderr, "Failed to map clock registers. Ensure program is run with root privileges.\n");
    return false;
  }
  
  return true;
}


double start_clock(double requestedFrequency)
{
  // Reference: https://www.raspberrypi.org/app/uploads/2012/02/BCM2835-ARM-Peripherals.pdf, Page 105

  // Find the best clock source to get closest to the requested frequency (lowest error) with MASH=1.
  // When error is equal, we favor the highest frequency clock in order to have the lowest jitter.

  update_clock_source_frequencies();

  int bestClockSourceIndex = -1;
  double bestError = DBL_MAX;
  double bestSourceFreq = 0;
  int divI = -1;
  int divF = -1;

  printf("Clock Sources:\n");
  for (size_t i = 0; i < ARRAY_LENGTH(_clockSources); i++)
  {
    printf("%-1d - %-8s - %-8s - %9.4lf MHz : ",
           _clockSources[i].clockSource,
           _clockSources[i].clockString,
           _clockSources[i].enableForUse ? "Enabled" : "Disabled",
           _clockSources[i].clockFrequency / 1e6);

    double division = _clockSources[i].clockFrequency / requestedFrequency;
    if (division < 2 || division > 4095)
    {
      printf("Not Suitable\n");
      continue;
    }

    int testDivI = (int)division;
    int testDivF = (division - testDivI) * 1024;
    double resultFreq = _clockSources[i].clockFrequency / (testDivI + testDivF / 1024.0);
    double error = fabsl(requestedFrequency - resultFreq);

    printf("Result = %.4lf Hz, Error = %.4lf Hz\n", resultFreq, error);
    if (error > bestError ||
       (error == bestError && _clockSources[i].clockFrequency <= bestSourceFreq))
    {
      continue;
    }
    
    bestClockSourceIndex = i;
    bestError = error;
    bestSourceFreq = _clockSources[i].clockFrequency;
    divI = testDivI;
    divF = testDivF;
  }
  printf("\n");

  if (bestClockSourceIndex < 0)
    return -1.0;  // Unable to find any suitable clock source

  stop_clock();

  uint32_t src  = _clockSources[bestClockSourceIndex].clockSource;
  uint32_t mash = 1;  // Good approximation, low jitter

  *(_pClockVirtMem + CLK_GP0DIV) = CLK_PASSWD | CLK_DIV_DIVI(divI) | CLK_DIV_DIVF(divF);
  usleep(10);
  *(_pClockVirtMem + CLK_GP0CTL) = CLK_PASSWD | CLK_CTL_MASH(mash) | CLK_CTL_SRC(src);
  usleep(10);
  *(_pClockVirtMem + CLK_GP0CTL) |= CLK_PASSWD | CLK_CTL_ENAB;

  printf("Choose clock %d at %.4lf MHz / %.4lf = %.4lf Hz\n\n",
         _clockSources[bestClockSourceIndex].clockSource,
         _clockSources[bestClockSourceIndex].clockFrequency / 1e6,
         divI + divF / 1024.0,
         _clockSources[bestClockSourceIndex].clockFrequency / (divI + divF / 1024.0));

  return _clockSources[bestClockSourceIndex].clockFrequency / (divI + divF / 1024.0);
}


void stop_clock()
{
  *(_pClockVirtMem + CLK_GP0CTL) = CLK_PASSWD | (*(_pClockVirtMem + CLK_GP0CTL) & ~CLK_CTL_ENAB);

  // Wait until clock confirms not to be busy anymore
  while (*(_pClockVirtMem + CLK_GP0CTL) & CLK_CTL_BUSY)
    usleep(10);

  enable_clock_output(false);
}


void enable_clock_output(bool on)
{
  if (on)
  {
    GPIO_ALT0(4);  // Pinmux GPIO4 into outputting clock
  }
  else
  {
    GPIO_INPUT(4);
  }
}
