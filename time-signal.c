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
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <signal.h>
#include <sched.h>
#include <sys/mman.h>
#include <pthread.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include <getopt.h>
#include "clock-control.h"
#include "time-services.h"


static void print_usage(const char *programName);
static void sig_handler(int sigNum);
static void *thread_carrier_only(void *arg);
static void *thread_time_signal(void *arg);


static const char * const TimeServiceNames[] = {
  [DCF77] = "DCF77",
  [JJY]   = "JJY",
  [MSF]   = "MSF",
  [WWVB]  = "WWVB"
};

typedef struct
{
  enum TimeService timeService;
  uint32_t carrierFrequency;
  double hourOffset;
} THREAD_DATA;


volatile uint8_t _verbosityLevel = 0;
volatile sig_atomic_t _threadRun = 0;


int main(int argc, char *argv[])
{
  struct sigaction sigAction = { 0 };
  sigAction.sa_handler = sig_handler;
  sigaction(SIGINT, &sigAction, NULL);
  sigaction(SIGTERM, &sigAction, NULL);


  int c;
  char *optTimeSource = "";
  bool optCarrierOnly = false;
  double optHourOffset = 0.0;
  while ((c = getopt(argc, argv, "s:co:vh")) != -1)
  {
    switch (c)
    {
      case 's':
        optTimeSource = optarg;
        break;

      case 'c':
        optCarrierOnly = true;
        break;

      case 'o':
        if (sscanf(optarg, "%lf", &optHourOffset) < 1)
        {
          fprintf(stderr, "Error: Invalid hour offset.\n");
          print_usage(argv[0]);
          return EXIT_FAILURE;
        }
        break;

      case 'v':
        _verbosityLevel++;
        break;

      case 'h':
      default:
        print_usage(argv[0]);
        return EXIT_SUCCESS;
    }
  }


  THREAD_DATA threadData = { 0 };
  if      (!strcasecmp(optTimeSource, "DCF77")) { threadData.timeService = DCF77; threadData.carrierFrequency = 77500; }
  else if (!strcasecmp(optTimeSource, "JJY40")) { threadData.timeService = JJY;   threadData.carrierFrequency = 40000; }
  else if (!strcasecmp(optTimeSource, "JJY60")) { threadData.timeService = JJY;   threadData.carrierFrequency = 60000; }
  else if (!strcasecmp(optTimeSource, "MSF"))   { threadData.timeService = MSF;   threadData.carrierFrequency = 60000; }
  else if (!strcasecmp(optTimeSource, "WWVB"))  { threadData.timeService = WWVB;  threadData.carrierFrequency = 60000; }
  else
  {
    fprintf(stderr, "Invalid time service selected.\n\n");
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }

  threadData.hourOffset = optHourOffset;


  printf("time-signal - DCF77/JJY/MSF/WWVB radio transmitter for Raspberry Pi\n");
  printf("Copyright (C) 2024 Steve Matos\n");
  printf("This program comes with ABSOLUTELY NO WARRANTY.\n");
  printf("This is free software, and you are welcome to\n");
  printf("redistribute it under certain conditions.\n\n");


  struct sched_param schedParam = { 0 };
  pthread_attr_t threadAttr;
  pthread_t threadId;

  if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1)
  {
     perror("Failed to lock memory");
     return EXIT_FAILURE;
  }

  if (pthread_attr_init(&threadAttr))
  {
    fprintf(stderr, "Failed to initialize thread attributes object.\n");
    return EXIT_FAILURE;
  }

  if (pthread_attr_setstacksize(&threadAttr, PTHREAD_STACK_MIN))
  {
    fprintf(stderr, "Failed to set thread stack size.\n");
    return EXIT_FAILURE;
  }

  if (pthread_attr_setschedpolicy(&threadAttr, SCHED_FIFO))
  {
    fprintf(stderr, "Failed to set thread scheduling policy.\n");
    return EXIT_FAILURE;
  }

  if ((schedParam.sched_priority = sched_get_priority_max(SCHED_FIFO)) == -1)
  {
     perror("Failed to get maximum scheduling priority value");
     return EXIT_FAILURE;
  }

  if (pthread_attr_setschedparam(&threadAttr, &schedParam))
  {
    fprintf(stderr, "Failed to set thread scheduling parameters.\n");
    return EXIT_FAILURE;
  }

  if (pthread_attr_setinheritsched(&threadAttr, PTHREAD_EXPLICIT_SCHED))
  {
    fprintf(stderr, "Failed to set thread inherit-scheduler attribute.\n");
    return EXIT_FAILURE;
  }

  _threadRun = 1;
  int pthreadResult = 0;
  if (optCarrierOnly)
    pthreadResult = pthread_create(&threadId, &threadAttr, thread_carrier_only, (void*)&threadData);
  else
    pthreadResult = pthread_create(&threadId, &threadAttr, thread_time_signal, (void*)&threadData);

  if (pthreadResult)
  {
    fprintf(stderr, "Failed to create execution thread. Ensure program is run with root privileges.\n");
    return EXIT_FAILURE;
  }

  if (pthread_attr_destroy(&threadAttr))
  {
    fprintf(stderr, "Failed to destroy thread attributes object.\n");
    return EXIT_FAILURE;
  }

  if (pthread_join(threadId, NULL))
  {
    fprintf(stderr, "Failed to join thread.\n");
    return EXIT_FAILURE;
  }

  if (munlockall() == -1)
  {
     perror("Failed to unlock memory");
     return EXIT_FAILURE;
  }

  printf("Program terminated.\n");
}


static void print_usage(const char *programName)
{
  printf("Usage: %s [options]\n"
         "Options:\n"
         "  -s <service>   Time service. ('DCF77', 'JJY40', 'JJY60', 'MSF', or 'WWVB')\n"
         "  -c             Output carrier wave only.\n"
         "  -o <hours>     Hours to offset.\n"
         "  -v             Verbose. (Add multiple times for more verbosity. e.g. -vv)\n"
         "  -h             Print this message and exit.\n",
         programName);
}


static void sig_handler(int sigNum)
{
  switch (sigNum)
  {
    case SIGINT:
      printf("\nReceived SIGINT signal. Terminating...\n");
      _threadRun = 0;
      break;

    case SIGTERM:
      printf("\nReceived SIGTERM signal. Terminating...\n");
      _threadRun = 0;
      break;

    default:
      printf("\nReceived unknown signal (%d).\n", sigNum);
      break;
  }
}


static void *thread_carrier_only(void *arg)
{
  THREAD_DATA threadData = *(THREAD_DATA*)arg;

  printf("Starting carrier only thread...\n");
  printf("Time Service = %s\n", TimeServiceNames[threadData.timeService]);
  printf("Carrier Frequency = %.4lf kHz\n", threadData.carrierFrequency / 1000.0);
  printf("\n");

  if (!gpio_init())
  {
    fprintf(stderr, "Failed to initialize GPIO.\n");
    _threadRun = 0;
    pthread_exit(NULL);
  }

  if (start_clock(threadData.carrierFrequency) <= 0)
  {
    fprintf(stderr, "Failed to start clock.\n");
    _threadRun = 0;
    pthread_exit(NULL);
  }

  enable_clock_output(true);

  while (_threadRun)
  {
    usleep(100);
  }

  printf("Stopping thread...\n");
  enable_clock_output(false);
  stop_clock();

  pthread_exit(NULL);
}


static void *thread_time_signal(void *arg)
{
  THREAD_DATA threadData = *(THREAD_DATA*)arg;
  time_t currentTime;
  time_t minuteStart;
  uint64_t minuteBits;
  int modulation;
  struct timespec targetWait;

  int32_t minuteOffset = round(threadData.hourOffset * 60);

  printf("Starting time signal thread...\n");
  printf("Time Service = %s\n", TimeServiceNames[threadData.timeService]);
  printf("Carrier Frequency = %.4lf Hz\n", threadData.carrierFrequency / 1000.0);
  printf("Hour Offset = %.4lf (%d min)\n", threadData.hourOffset, minuteOffset);
  printf("\n");

  if (!gpio_init())
  {
    fprintf(stderr, "Failed to initialize GPIO.\n");
    _threadRun = 0;
    pthread_exit(NULL);
  }

  if (start_clock(threadData.carrierFrequency) <= 0)
  {
    fprintf(stderr, "Failed to start clock.\n");
    _threadRun = 0;
    pthread_exit(NULL);
  }

  enable_clock_output(false);

  currentTime = time(NULL);
  minuteStart = currentTime - (currentTime % 60);  // Round down to start of minute

  while (_threadRun)
  {
    if (_verbosityLevel >= 1)
    {
      struct tm timeValue;
      char dateString[] = "1970-01-01 00:00:00";
      localtime_r(&minuteStart, &timeValue);
      strftime(dateString, sizeof(dateString), "%Y-%m-%d %H:%M:%S", &timeValue);
      printf("%s", dateString);

      if (minuteOffset != 0)
      {
        time_t t = minuteStart + (minuteOffset * 60);
        localtime_r(&t, &timeValue);
        strftime(dateString, sizeof(dateString), "%Y-%m-%d %H:%M:%S", &timeValue);
        printf(" --> %s", dateString);
      }

      printf("\n");
      fflush(stdout);
    }

    minuteBits = prepare_minute(threadData.timeService, minuteStart + (minuteOffset * 60));
    if (minuteBits == (uint64_t)-1)
    {
      fprintf(stderr, "Error preparing minute bits.\n");
      _threadRun = 0;
      break;
    }

    for (int second = 0; second < 60; second++)
    {
      if (!_threadRun)
        break;

      modulation = get_modulation_for_second(threadData.timeService, minuteBits, second);
      if (modulation < 0)
      {
        fprintf(stderr, "Error getting modulation time.\n");
        _threadRun = 0;
        break;
      }

      // Wait until we reach the beginning of the current second
      targetWait.tv_sec = minuteStart + second;
      targetWait.tv_nsec = 0;
      clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &targetWait, NULL);

      if (threadData.timeService == JJY)
        enable_clock_output(true);
      else
        enable_clock_output(false);

      if (_verbosityLevel >= 2)
      {
        printf("%03d ", modulation);

        if ((second + 1) % 15 == 0)
          printf("\n");

        fflush(stdout);
      }

      targetWait.tv_nsec = modulation * 1e6;
      clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &targetWait, NULL);

      if (threadData.timeService == JJY)
        enable_clock_output(false);
      else
        enable_clock_output(true);
    }

    minuteStart += 60;
  }

  printf("Stopping thread...\n");
  enable_clock_output(false);
  stop_clock();

  pthread_exit(NULL);
}
