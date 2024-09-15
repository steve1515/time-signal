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
#include <inttypes.h>
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


static const char * const TimeServiceNames[] =
{
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
  bool disableChecks;
} THREAD_DATA;


volatile uint8_t _verbosityLevel = 0;
volatile sig_atomic_t _threadRun = 0;


int main(int argc, char *argv[])
{
  struct sigaction sigAction = { 0 };
  sigAction.sa_handler = sig_handler;
  sigaction(SIGINT, &sigAction, NULL);
  sigaction(SIGTERM, &sigAction, NULL);


  static struct option long_options[] =
  {
    {"time-service",       required_argument, NULL, 's'},
    {"carrier-only",       no_argument,       NULL, 'c'},
    {"frequency-override", required_argument, NULL, 'f'},
    {"time-offset",        required_argument, NULL, 'o'},
    {"disable-checks",     no_argument,       NULL, 'd'},
    {"verbose",            no_argument,       NULL, 'v'},
    {"help",               no_argument,       NULL, 'h'},
    {0, 0, 0, 0}
  };

  int c;
  char *optTimeService = "";
  bool optCarrierOnly = false;
  uint32_t optFreqOverride = 0;
  double optHourOffset = 0.0;
  bool optDisableChecks = false;
  while ((c = getopt_long(argc, argv, "s:cf:o:dvh", long_options, NULL)) != -1)
  {
    switch (c)
    {
      case 's':
        optTimeService = optarg;
        break;

      case 'c':
        optCarrierOnly = true;
        break;

      case 'f':
        if (sscanf(optarg, "%" SCNu32, &optFreqOverride) < 1 || optFreqOverride <= 0)
        {
          fprintf(stderr, "Error: Carrier frequency override value must be greater than zero.\n");
          print_usage(argv[0]);
          return EXIT_FAILURE;
        }
        break;

      case 'o':
        if (sscanf(optarg, "%lf", &optHourOffset) < 1)
        {
          fprintf(stderr, "Error: Invalid hour offset.\n");
          print_usage(argv[0]);
          return EXIT_FAILURE;
        }
        break;

      case 'd':
        optDisableChecks = true;
        break;

      case 'v':
        _verbosityLevel++;
        break;

      case 'h':
      case '?':
      default:
        print_usage(argv[0]);
        return EXIT_SUCCESS;
    }
  }


  THREAD_DATA threadData = { 0 };
  if      (!strcasecmp(optTimeService, "DCF77")) { threadData.timeService = DCF77; threadData.carrierFrequency = 77500; }
  else if (!strcasecmp(optTimeService, "JJY40")) { threadData.timeService = JJY;   threadData.carrierFrequency = 40000; }
  else if (!strcasecmp(optTimeService, "JJY60")) { threadData.timeService = JJY;   threadData.carrierFrequency = 60000; }
  else if (!strcasecmp(optTimeService, "MSF"))   { threadData.timeService = MSF;   threadData.carrierFrequency = 60000; }
  else if (!strcasecmp(optTimeService, "WWVB"))  { threadData.timeService = WWVB;  threadData.carrierFrequency = 60000; }
  else
  {
    fprintf(stderr, "Invalid time service selected.\n\n");
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }

  if (optFreqOverride > 0)
    threadData.carrierFrequency = optFreqOverride;

  threadData.hourOffset = optHourOffset;
  threadData.disableChecks = optDisableChecks;


  printf("time-signal - DCF77/JJY/MSF/WWVB radio transmitter for Raspberry Pi\n");
  printf("Copyright (C) 2024 Steve Matos\n");
  printf("This program comes with ABSOLUTELY NO WARRANTY.\n");
  printf("This is free software, and you are welcome to\n");
  printf("redistribute it under certain conditions.\n\n");
  fflush(stdout);


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
  fflush(stdout);
}


static void print_usage(const char *programName)
{
  printf("Usage: %s [OPTION]...\n\n"
         "Mandatory arguments to long options are mandatory for short options too.\n"
         "  -s, --time-service={DCF77|JJY40|JJY60|MSF|WWVB}\n"
         "                                 Time service to transmit.\n"
         "  -c, --carrier-only             Output carrier wave only.\n"
         "  -f, --frequency-override=NUM   Set carrier frequency to NUM Hz.\n"
         "  -o, --time-offset=NUM          Offset transmitted time by NUM hours.\n"
         "  -d, --disable-checks           Disable sanity checks.\n"
         "  -v, --verbose                  Enable verbose output.\n"
         "                                 Add multiple times for more output. e.g. -vv\n"
         "  -h, --help                     Print this message and exit.\n",
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

  fflush(stdout);
}


static void *thread_carrier_only(void *arg)
{
  THREAD_DATA threadData = *(THREAD_DATA*)arg;

  printf("Starting carrier only thread...\n");
  printf("Time Service = %s\n", TimeServiceNames[threadData.timeService]);
  printf("Carrier Frequency = %.4lf kHz\n", threadData.carrierFrequency / 1000.0);
  printf("\n");
  fflush(stdout);

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
  struct tm timeParts;
  char dateString[] = "1970-01-01 00:00:00";
  uint64_t minuteBits;
  int modulation;
  struct timespec targetWait;

  int32_t minuteOffset = round(threadData.hourOffset * 60);

  printf("Starting time signal thread...\n");
  printf("Time Service = %s\n", TimeServiceNames[threadData.timeService]);
  printf("Carrier Frequency = %.4lf Hz\n", threadData.carrierFrequency / 1000.0);
  printf("Hour Offset = %.4lf (%d min)\n", threadData.hourOffset, minuteOffset);
  printf("Disable Sanity Checks = %s\n", threadData.disableChecks ? "Yes" : "No");
  printf("\n");
  fflush(stdout);

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

  gmtime_r(&currentTime, &timeParts);
  if (!threadData.disableChecks && (timeParts.tm_year + 1900) < 2020)
  {
    fprintf(stderr, "Sanity check failed: System clock year must be >= 2020.\n");
    strftime(dateString, sizeof(dateString), "%Y-%m-%d %H:%M:%S", &timeParts);
    fprintf(stderr, "Current system clock (UTC): %s\n", dateString);
    _threadRun = 0;
  }

  while (_threadRun)
  {
    if (_verbosityLevel >= 1)
    {
      localtime_r(&minuteStart, &timeParts);
      strftime(dateString, sizeof(dateString), "%Y-%m-%d %H:%M:%S", &timeParts);
      printf("%s", dateString);

      if (minuteOffset != 0)
      {
        time_t t = minuteStart + (minuteOffset * 60);
        localtime_r(&t, &timeParts);
        strftime(dateString, sizeof(dateString), "%Y-%m-%d %H:%M:%S", &timeParts);
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
