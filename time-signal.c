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
#include "macros.h"
#include "clock-control.h"
#include "time-services.h"


#define MINUTES_IN_DAY 1440
#define SECONDS_IN_DAY 86400


static void print_usage(const char *programName);
static void sig_handler(int sigNum);
static bool get_periodic_schedule(bool *buffSched, size_t buffLen, const char *paramString);
static void print_schedule_chart(const bool *buffSched, size_t buffLen);
static bool rt_thread_attr_init(pthread_attr_t *attr);
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
  bool runSchedule[MINUTES_IN_DAY];
  double hourOffset;
  bool disableChecks;
} THREAD_DATA;


static volatile uint8_t _verbosityLevel = 0;
static volatile sig_atomic_t _threadRun = 0;


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
    {"schedule",           required_argument, NULL, 'p'},
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
  char *optSchedule = NULL;
  bool optDisableChecks = false;
  while ((c = getopt_long(argc, argv, "s:cf:p:o:dvh", long_options, NULL)) != -1)
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

      case 'p':
        optSchedule = optarg;
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

  memset(threadData.runSchedule, 1, ARRAY_LENGTH(threadData.runSchedule));
  if (optSchedule != NULL)
    get_periodic_schedule(threadData.runSchedule, ARRAY_LENGTH(threadData.runSchedule), optSchedule);

  threadData.hourOffset = optHourOffset;
  threadData.disableChecks = optDisableChecks;


  printf("time-signal - DCF77/JJY/MSF/WWVB radio transmitter for Raspberry Pi\n");
  printf("Copyright (C) 2024 Steve Matos\n");
  printf("This program comes with ABSOLUTELY NO WARRANTY.\n");
  printf("This is free software, and you are welcome to\n");
  printf("redistribute it under certain conditions.\n\n");
  fflush(stdout);


  pthread_attr_t threadAttr;
  pthread_t threadId;

  if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1)
  {
     perror("Failed to lock memory");
     return EXIT_FAILURE;
  }

  if (!rt_thread_attr_init(&threadAttr))
  {
    fprintf(stderr, "Failed to initialize real-time thread attributes.\n");
    return EXIT_FAILURE;
  }

  _threadRun = 1;
  int pthreadResult =
    pthread_create(&threadId,
                   &threadAttr,
                   optCarrierOnly ? thread_carrier_only : thread_time_signal,
                   (void*)&threadData);

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

  // Block SIGINT and SIGTERM in the main thread. This will force the signals
  // to be sent to the work thread and will allow interruption of any timers.
  sigset_t signalSet;
  sigemptyset(&signalSet);
  sigaddset(&signalSet, SIGINT);
  sigaddset(&signalSet, SIGTERM);
  if (pthread_sigmask(SIG_BLOCK, &signalSet, NULL))
  {
    fprintf(stderr, "Failed to update thread signal mask.\n");
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
         "  -p, --schedule=SCHEDULE        Use SCHEDULE as a run time schedule.\n"
         "                                 SCHEDULE is START:LEN[;START:LEN]...\n"
         "                                 e.g. -p \"2:15;13.5:30\"\n"
         "                                      for 2am for 15min and 1:30pm for 30min\n"
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
    {
      char msg[] = "\nReceived SIGINT signal. Terminating...\n";
      (void)!write(STDOUT_FILENO, msg, strlen(msg));
      _threadRun = 0;  
      break;
    }

    case SIGTERM:
    {
      char msg[] = "\nReceived SIGTERM signal. Terminating...\n";
      (void)!write(STDOUT_FILENO, msg, strlen(msg));
      _threadRun = 0;
      break;
    }

    default:
    {
      char msg[] = "\nReceived unknown signal.\n";
      (void)!write(STDOUT_FILENO, msg, strlen(msg));
      break;
    }
  }
}


static bool get_periodic_schedule(bool *buffSched, size_t buffLen, const char *paramString)
{
  if (buffSched == NULL || buffLen < MINUTES_IN_DAY || paramString == NULL)
    return false;

  char *paramCopy = strdup(paramString);
  if (paramCopy == NULL)
    return false;

  // The parameter string contains schedule entries separated by a ';'.
  // Each schedule entry contains a start hour and run time in minutes
  // separated by a ':'.
  // For example, if the parameter string is "1:3;15.5:15", then the
  // schedule entries are 1am for 3 minutes and 3:30pm for 15 minutes.

  char delimOuter[] = ";";
  char delimInner[] = ":";

  memset(buffSched, 0, buffLen);

  char *spOuter = NULL;
  char *spInner = NULL;
  for(char *schedEntry = strtok_r(paramCopy, delimOuter, &spOuter);
      schedEntry != NULL;
      schedEntry = strtok_r(NULL, delimOuter, &spOuter))
  {

    char *startHourString = strtok_r(schedEntry, delimInner, &spInner);
    if (startHourString == NULL)
      continue;

    double startHour = 0;
    if ((sscanf(startHourString, "%lf", &startHour) < 1) || (!(startHour >= 0 && startHour < 24)))
    {
      fprintf(stderr, "Error: Invalid schedule start hour (%s).\n", startHourString);
      continue;
    }

    char *runMinutesString = strtok_r(NULL, delimInner, &spInner);
    if (runMinutesString == NULL)
      continue;

    uint16_t runMinutes = 0;
    if ((sscanf(runMinutesString, "%" SCNu16, &runMinutes) < 1) || (runMinutes > MINUTES_IN_DAY))
    {
      fprintf(stderr, "Error: Invalid schedule run time minutes (%s).\n", runMinutesString);
      continue;
    }

    uint16_t startMinute = lround(startHour * 60);
    if (startMinute >= MINUTES_IN_DAY)
    {
      fprintf(stderr, "Error: Invalid schedule start minute encountered (%" PRIu16 ").\n", startMinute);
      continue;
    }

    for (int i = 0; i < runMinutes; i++)
    {
      int idx = (startMinute + i) % MINUTES_IN_DAY;
      buffSched[idx] = true;
    }
  }

  free(paramCopy);
  return true;
}


static void print_schedule_chart(const bool *buffSched, size_t buffLen)
{
  if (buffSched == NULL || buffLen < MINUTES_IN_DAY)
    return;

  for (int i = 0; i < MINUTES_IN_DAY; i++)
  {
    if ((i > 0) && (i % 60 == 0))
      printf("\n");

    if (i % 60 == 0)
      printf("%2d:", i / 60);

    if (i % 10 == 0)
      printf(" ");

    printf("%d", buffSched[i]);
  }

  printf("\n");
}


static bool rt_thread_attr_init(pthread_attr_t *attr)
{
  struct sched_param schedParam = { 0 };

  if (pthread_attr_init(attr))
  {
    fprintf(stderr, "Failed to initialize thread attributes object.\n");
    return false;
  }

  if (pthread_attr_setstacksize(attr, PTHREAD_STACK_MIN))
  {
    fprintf(stderr, "Failed to set thread stack size.\n");
    return false;
  }

  if (pthread_attr_setschedpolicy(attr, SCHED_FIFO))
  {
    fprintf(stderr, "Failed to set thread scheduling policy.\n");
    return false;
  }

  if ((schedParam.sched_priority = sched_get_priority_max(SCHED_FIFO)) == -1)
  {
     perror("Failed to get maximum scheduling priority value");
     return false;
  }

  if (pthread_attr_setschedparam(attr, &schedParam))
  {
    fprintf(stderr, "Failed to set thread scheduling parameters.\n");
    return false;
  }

  if (pthread_attr_setinheritsched(attr, PTHREAD_EXPLICIT_SCHED))
  {
    fprintf(stderr, "Failed to set thread inherit-scheduler attribute.\n");
    return false;
  }

  return true;
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

  int32_t minuteOffset = lround(threadData.hourOffset * 60);

  printf("Starting time signal thread...\n");
  printf("Time Service = %s\n", TimeServiceNames[threadData.timeService]);
  printf("Carrier Frequency = %.4lf Hz\n", threadData.carrierFrequency / 1000.0);
  printf("Hour Offset = %.4lf (%d min)\n", threadData.hourOffset, minuteOffset);
  printf("Disable Sanity Checks = %s\n", threadData.disableChecks ? "Yes" : "No");
  printf("\n");
  fflush(stdout);

  if (_verbosityLevel >= 2)
  {
    printf("Run Schedule:\n");
    print_schedule_chart(threadData.runSchedule, ARRAY_LENGTH(threadData.runSchedule));
    printf("\n");
    fflush(stdout);
  }

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
    localtime_r(&minuteStart, &timeParts);
    long tzOffsetSeconds = timeParts.tm_gmtoff;
    time_t offsetMinuteStart = minuteStart + tzOffsetSeconds;
    int minuteOfDay = (offsetMinuteStart % SECONDS_IN_DAY) / 60;

    if (_verbosityLevel >= 2)
    {
      printf("Minute Of Day = %d; Schedule Enabled = %d\n",
             minuteOfDay, threadData.runSchedule[minuteOfDay]);
    }

    // When we aren't scheduled to run, turn off the
    // clock output and wait until the next minute.
    if (!threadData.runSchedule[minuteOfDay])
    {
      enable_clock_output(false);
      minuteStart += 60;

      targetWait.tv_sec = minuteStart;
      targetWait.tv_nsec = 0;
      clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &targetWait, NULL);

      continue;
    }

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
