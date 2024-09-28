/*
time-services.c - part of time-signal
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
#include <stdint.h>
#include <time.h>
#include "time-services.h"


static uint64_t to_bcd(int n);
static uint64_t to_padded_bcd(int n);
static uint64_t even_parity(uint64_t data, uint8_t startBit, uint8_t endBit);
static uint64_t odd_parity(uint64_t data, uint8_t startBit, uint8_t endBit);
static uint64_t is_leap_year(int year);


static uint64_t to_bcd(int n)
{
  return (((n / 100) % 10) << 8) | (((n / 10) % 10) << 4) | (n % 10);
}


// JJY and WWVB use BCD, but with a zero bit between the digits.
// We will call this 'padded' BCD.
static uint64_t to_padded_bcd(int n)
{
  return (((n / 100) % 10) << 10) | (((n / 10) % 10) << 5) | (n % 10);
}


static uint64_t even_parity(uint64_t data, uint8_t startBit, uint8_t endBit)
{
  uint8_t count = 0;
  
  for (int i = startBit; i <= endBit; i++)
  {
    if (data & (1LL << i))
      count++;
  }

  return count & 0x01;
}


static uint64_t odd_parity(uint64_t data, uint8_t startBit, uint8_t endBit)
{
  return (even_parity(data, startBit, endBit) == 0);
}


static uint64_t is_leap_year(int year)
{
  return (year % 4 == 0) && (year % 100 != 0 || year % 400 == 0);
}


uint64_t prepare_minute(enum TimeService service, time_t currentTime)
{
  struct tm timeParts, tomorrowParts;
  time_t tomorrowTime;
  uint64_t timeBits = 0;
  uint64_t aBits = 0;
  uint64_t bBits = 0;

  switch (service)
  {
    case DCF77:
      currentTime += 60;  // Time to be transmitted is the following minute

      // Transmitted time is either CET or CEST depending on time of year.
      // When in Germany, this is localtime.
      localtime_r(&currentTime, &timeParts);

      // DCF77 bit order is LSB first.
      // Transmission will start from bit zero of the time bits string.
      timeBits = 0;
      timeBits |= ((timeParts.tm_isdst > 0) ? 1 : 0) << 17;
      timeBits |= ((timeParts.tm_isdst > 0) ? 0 : 1) << 18;
      timeBits |= (1 << 20);  // Start of encoded time - always 1
      timeBits |= to_bcd(timeParts.tm_min) << 21;
      timeBits |= to_bcd(timeParts.tm_hour) << 29;
      timeBits |= to_bcd(timeParts.tm_mday) << 36;
      timeBits |= to_bcd(timeParts.tm_wday ? timeParts.tm_wday : 7) << 42;
      timeBits |= to_bcd(timeParts.tm_mon + 1) << 45;
      timeBits |= to_bcd(timeParts.tm_year % 100) << 50;

      timeBits |= even_parity(timeBits, 21, 27) << 28;
      timeBits |= even_parity(timeBits, 29, 34) << 35;
      timeBits |= even_parity(timeBits, 36, 57) << 58;
      break;


    case JJY:
      // Transmitted time is JST.
      // When in Japan, this is localtime.
      localtime_r(&currentTime, &timeParts);

      // JJY bit order is MSB first.
      // Transmission will start from bit 59 of the time bits string.
      timeBits = 0;
      timeBits |= to_padded_bcd(timeParts.tm_min) << (59 - 8);
      timeBits |= to_padded_bcd(timeParts.tm_hour) << (59 - 18);
      timeBits |= to_padded_bcd(timeParts.tm_yday + 1) << (59 - 33);
      timeBits |= to_bcd(timeParts.tm_year % 100) << (59 - 48);
      timeBits |= to_bcd(timeParts.tm_wday) << (59 - 52);

      timeBits |= even_parity(timeBits, 59 - 18, 59 - 12) << (59 - 36);  // PA1
      timeBits |= even_parity(timeBits, 59 - 8, 59 - 1) << (59 - 37);    // PA2

      // There is a different 'service announcement' encoding in minute 15 and 45,
      // but let's just ignore that for now. Consumer clocks probably don't care.
      break;


    case MSF:
      currentTime += 60;  // Time to be transmitted is the following minute

      // Transmitted time is GMT or BST depending on time of year.
      // When in the UK, this is localtime.
      localtime_r(&currentTime, &timeParts);

      // MSF bit order is MSB first.
      // Transmission will start from bit 59 of the time bits string.
      aBits = 0;
      aBits |= to_bcd(timeParts.tm_year % 100) << (59 - 24);
      aBits |= to_bcd(timeParts.tm_mon + 1) << (59 - 29);
      aBits |= to_bcd(timeParts.tm_mday) << (59 - 35);
      aBits |= to_bcd(timeParts.tm_wday) << (59 - 38);
      aBits |= to_bcd(timeParts.tm_hour) << (59 - 44);
      aBits |= to_bcd(timeParts.tm_min) << (59 - 51);

      bBits = 0;
      bBits |= odd_parity(aBits, 59 - 24, 59 - 17) << (59 - 54);  // P1
      bBits |= odd_parity(aBits, 59 - 35, 59 - 25) << (59 - 55);  // P2
      bBits |= odd_parity(aBits, 59 - 38, 59 - 36) << (59 - 56);  // P3
      bBits |= odd_parity(aBits, 59 - 51, 59 - 39) << (59 - 57);  // P4
      bBits |= ((timeParts.tm_isdst > 0) ? 1 : 0) << (59 - 58);
      // DUT bits (00 - 16) are not supported.
      // STW bit (53) is not supported.

      timeBits = aBits | bBits;
      break;


    case WWVB:
      // Transmitted time is UTC.
      gmtime_r(&currentTime, &timeParts);

      // WWVB bit order is MSB first.
      // Transmission will start from bit 59 of the time bits string.
      timeBits = 0;
      timeBits |= to_padded_bcd(timeParts.tm_min) << (59 - 8);
      timeBits |= to_padded_bcd(timeParts.tm_hour) << (59 - 18);
      timeBits |= to_padded_bcd(timeParts.tm_yday + 1) << (59 - 33);
      timeBits |= to_padded_bcd(timeParts.tm_year % 100) << (59 - 53);
      timeBits |= is_leap_year(timeParts.tm_year + 1900) << (59 - 55);

      // Need local time for now and tomorrow to determine DST status.
      tomorrowTime = currentTime + 86400;
      localtime_r(&currentTime, &timeParts);
      localtime_r(&tomorrowTime, &tomorrowParts);

      timeBits |= ((tomorrowParts.tm_isdst > 0) ? 1 : 0) << (59 - 57);
      timeBits |= ((timeParts.tm_isdst > 0) ? 1 : 0) << (59 - 58);
      break;


    default:
      return ((uint64_t)-1);
  }

  return timeBits;
}


int get_modulation_for_second(enum TimeService service, uint64_t timeBits, int sec)
{
  bool bit;

  switch (service)
  {
    case DCF77:
      if (sec >= 59)
        return 0;

      bit = timeBits & (1LL << sec);
      return (bit ? 200 : 100);


    case JJY:
      if (sec == 0 || sec % 10 == 9 || sec > 59)
        return 200;

      bit = timeBits & (1LL << (59 - sec));
      return (bit ? 500 : 800);


    case MSF:
      if (sec == 0 || sec > 59)
        return 500;

      bit = timeBits & (1LL << (59 - sec));
      return 100 + bit * 100 + ((sec > 52) && (sec < 59)) * 100;


    case WWVB:
      if (sec == 0 || sec % 10 == 9 || sec > 59)
        return 800;

      bit = timeBits & (1LL << (59 - sec));
      return (bit ? 500 : 200);


    default:
      return -1;
  }
}
