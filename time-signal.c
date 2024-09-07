/*
time-signal.c - DCF77/JJY/MSF/WWVB radio transmitter for Raspberry Pi

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
#include <unistd.h>
#include <sched.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <getopt.h>
#include "time-services.h"
#include "clock-control.h"


int usage(const char *msg, const char *progname)
{
  fprintf(stderr,
          "%susage: %s [options]\n"
          "Options:\n"
          "\t-s <service>          : Service; one of "
          "'DCF77', 'WWVB', 'JJY40', 'JJY60', 'MSF'\n"
          "\t-v                    : Verbose.\n"
          "\t-c                    : Carrier wave only.\n"
          "\t-h                    : This help.\n",
          msg,
          progname);
  return 1;
}


void signaux(int sigtype)
{
  switch (sigtype)
  {
  case SIGINT:
    printf("\nSIGINT");
    break;

  case SIGTERM:
    printf("\nSIGTERM");
    break;

  default:
    printf("\nUnknow %d", sigtype);
  }

  stop_clock();
  printf(" signal received - Program terminated\n");
  exit(0);
}


int main(int argc, char *argv[])
{
  bool verbose = false;
  bool carrier_only = false;

  int modulation;
  int frequency = 60000;
  int opt;

  uint64_t minute_bits;
  char *time_source  = "";
  char date_string[] = "1969-07-21 00:00:00";

  enum TimeService service;
  time_t now, minute_start;
  struct timespec target_wait;
  struct tm tv;

  signal(SIGINT, signaux);
  signal(SIGTERM, signaux);

  puts("time-signal - JJY/MSF/WWVB/DCF77 radio transmitter");
  puts("Copyright (C) 2024 Steve Matos");
  puts("This program comes with ABSOLUTELY NO WARRANTY.");
  puts("This is free software, and you are welcome to");
  puts("redistribute it under certain conditions.\n");

  while ((opt = getopt(argc, argv, "vs:hc")) != -1)
  {
    switch (opt)
    {
    case 'v':
      verbose = true;
      break;

    case 's':
      time_source = optarg;
      break;

    case 'c':
      carrier_only = true;
      break;

    default:
      return usage("", argv[0]);
    }
  }

  if (strcasecmp(time_source, "DCF77") == 0)
  {
    frequency = 77500;
    service   = DCF77;
  }
  else if (strcasecmp(time_source, "WWVB") == 0)
  {
    service = WWVB;
  }
  else if (strcasecmp(time_source, "JJY40") == 0)
  {
    frequency = 40000;
    service   = JJY;
  }
  else if (strcasecmp(time_source, "JJY60") == 0)
  {
    service = JJY;
  }
  else if (strcasecmp(time_source, "MSF") == 0)
  {
    service = MSF;
  }
  else
  {
    return usage("Please choose a service name with -s option\n", argv[0]);
  }

  gpio_init();
  start_clock(frequency);

  if (carrier_only)
    enable_clock_output(1);

  // Give max priority to this programm
  struct sched_param sp;
  sp.sched_priority = 99;
  sched_setscheduler(0, SCHED_FIFO, &sp);

  now = time(NULL);
  minute_start = now - now % 60;  // round to minute

  while (1)
  {
    if (carrier_only)
    {
      usleep(100);
      continue;
    }

    localtime_r(&minute_start, &tv);
    strftime(date_string, sizeof(date_string), "%Y-%m-%d %H:%M:%S", &tv);
    printf("%s\n", date_string);

    minute_bits = prepare_minute(service, minute_start);
    if (minute_bits == ((uint64_t)-1))
    {
      exit(0);
    }

    for (int second = 0; second < 60; ++second)
    {
      modulation = get_modulation_for_second(service, minute_bits, second);
      if (modulation < 0)
      {
        exit(0);
      }

      // First, let's wait until we reach the beginning of that second
      target_wait.tv_sec = minute_start + second;
      target_wait.tv_nsec = 0;
      clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &target_wait, NULL);

      if (service == JJY)
        enable_clock_output(1);  // Set signal to HIGH
      else
        enable_clock_output(0);  // Set signal to LOW

      if (verbose)
      {
        fprintf(stderr, "%03d ", modulation);

        if ((second + 1) % 15 == 0)
          fprintf(stderr, "\n");
      }

      target_wait.tv_nsec = modulation * 1e6;
      clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &target_wait, NULL);

      if (service == JJY)
        enable_clock_output(0);  // signal to LOW
      else
        enable_clock_output(1);  // Set signal to HIGH
    }

    minute_start += 60;
  }
}
