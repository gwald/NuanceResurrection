#include "basetypes.h"
#include <time.h>
#include <windows.h>
#include <mmsystem.h>
#include "byteswap.h"
#include "NuonEnvironment.h"
#include "timer.h"
#include "mpe.h"

#define USE_QUEUE_TIMERS // re-test which variant is better in practice, this is at least the newer/recommended way it seems

extern NuonEnvironment nuonEnv;

bool bHighPerformanceTimerAvailable = false;
extern _LARGE_INTEGER tickFrequency;
_LARGE_INTEGER ticksAtBootTime;
#ifdef USE_QUEUE_TIMERS
HANDLE hSysTimer0;
HANDLE hSysTimer1;
HANDLE hSysTimer2;
#else
uint32 hSysTimer0;
uint32 hSysTimer1;
uint32 hSysTimer2;
#endif

#ifdef USE_QUEUE_TIMERS
void CALLBACK SysTimer0Callback(void* lpParameter,BOOLEAN TimerOrWaitFired)
#else
void CALLBACK SysTimer0Callback(uint32 wTimerID, uint32 msg, int32 dwUser, int32 dw1, int32 dw2)
#endif
{ 
  nuonEnv.ScheduleInterrupt(INT_SYSTIMER0);
} 

#ifdef USE_QUEUE_TIMERS
void CALLBACK SysTimer1Callback(void* lpParameter, BOOLEAN TimerOrWaitFired)
#else
void CALLBACK SysTimer1Callback(uint32 wTimerID, uint32 msg, int32 dwUser, int32 dw1, int32 dw2)
#endif
{ 
  nuonEnv.ScheduleInterrupt(INT_SYSTIMER1);
}

// this one is set by InitBios for mpe[3] at ~50 or 60Hz
#ifdef USE_QUEUE_TIMERS
void CALLBACK SysTimer2Callback(void* lpParameter, BOOLEAN TimerOrWaitFired)
#else
void CALLBACK SysTimer2Callback(uint32 wTimerID, uint32 msg, int32 dwUser, int32 dw1, int32 dw2)
#endif
{ 
  //static uint64 cycleCounter[4] = {0,0,0,0};
  //static uint64 max_delta[4] = {0,0,0,0};

//  for(uint32 i = 0; i < 4; i++)
//  {
//    uint64 delta = nuonEnv.mpe[i]->cycleCounter - cycleCounter[i];
//    if(delta > max_delta[i])
//    {
//      max_delta[i] = delta;
//    }
//    cycleCounter[i] = nuonEnv.mpe[i]->cycleCounter;
//  }

  IncrementVideoFieldCounter();
  nuonEnv.TriggerVideoInterrupt();
  nuonEnv.trigger_render_video = true;
}

//

typedef LONG(CALLBACK* NTSETTIMERRESOLUTION)(IN ULONG DesiredTime,
	IN BOOLEAN SetResolution,
	OUT PULONG ActualTime);
static NTSETTIMERRESOLUTION NtSetTimerResolution;

typedef LONG(CALLBACK* NTQUERYTIMERRESOLUTION)(OUT PULONG MaximumTime,
	OUT PULONG MinimumTime,
	OUT PULONG CurrentTime);
static NTQUERYTIMERRESOLUTION NtQueryTimerResolution;

static HMODULE hNtDll = NULL;
static ULONG win_timer_old_period = -1;

static TIMECAPS win_timer_caps;
static MMRESULT win_timer_result = TIMERR_NOCANDO;

static void set_lowest_possible_win_timer_resolution()
{
	// First crank up the multimedia timer resolution to its max
	// this gives the system much finer timeslices (usually 1-2ms)
	win_timer_result = timeGetDevCaps(&win_timer_caps, sizeof(win_timer_caps));
	if (win_timer_result == TIMERR_NOERROR)
		timeBeginPeriod(win_timer_caps.wPeriodMin);

	// Then try the even finer sliced (usually 0.5ms) low level variant
	hNtDll = LoadLibrary("NtDll.dll");
	if (hNtDll) {
		NtQueryTimerResolution = (NTQUERYTIMERRESOLUTION)GetProcAddress(hNtDll, "NtQueryTimerResolution");
		NtSetTimerResolution = (NTSETTIMERRESOLUTION)GetProcAddress(hNtDll, "NtSetTimerResolution");
		if (NtQueryTimerResolution && NtSetTimerResolution) {
			ULONG min_period, tmp;
			NtQueryTimerResolution(&tmp, &min_period, &win_timer_old_period);
			if (min_period < 4500) // just to not screw around too much with the time (i.e. potential timer improvements in future HW/OSs), limit timer period to 0.45ms (picked 0.45 here instead of 0.5 as apparently some current setups can feature values just slightly below 0.5, so just leave them at this native rate then)
				min_period = 5000;
			if (min_period < 10000) // only set this if smaller 1ms, cause otherwise timeBeginPeriod already did the job
				NtSetTimerResolution(min_period, TRUE, &tmp);
			else
				win_timer_old_period = -1;
		}
	}
}

static void restore_win_timer_resolution()
{
	// restore both timer resolutions

	if (hNtDll) {
		if (win_timer_old_period != -1)
		{
			ULONG tmp;
			NtSetTimerResolution(win_timer_old_period, FALSE, &tmp);
			win_timer_old_period = -1;
		}
		FreeLibrary(hNtDll);
		hNtDll = NULL;
	}

	if (win_timer_result == TIMERR_NOERROR)
	{
		timeEndPeriod(win_timer_caps.wPeriodMin);
		win_timer_result = TIMERR_NOCANDO;
	}
}

//

void InitializeTimingMethod(void)
{
  set_lowest_possible_win_timer_resolution();

  hSysTimer0 = 0;
  hSysTimer1 = 0;
  hSysTimer2 = 0;

  if(QueryPerformanceFrequency((_LARGE_INTEGER *)&tickFrequency) == TRUE)
  {
    bHighPerformanceTimerAvailable = true;
    QueryPerformanceCounter((_LARGE_INTEGER *)&ticksAtBootTime);
  }
  else
  {
    bHighPerformanceTimerAvailable = false;
    ticksAtBootTime.QuadPart = time(NULL);
  }
}

void DeInitTimingMethod(void)
{
  restore_win_timer_resolution();
}

void TimeOfDay(MPE &mpe)
{
  time_t currTime;
  tm *pcTime;
  _currenttime *nuonTime;
  uint32 ptrNuonTime, getset;

  ptrNuonTime = mpe.regs[0];
  getset = mpe.regs[1];

  if(getset == 0)
  {
    nuonTime = (_currenttime *)nuonEnv.GetPointerToMemory(mpe,ptrNuonTime,true);

    //Get time and fill time structure

    time(&currTime);
    pcTime = localtime(&currTime);

    nuonTime->sec = pcTime->tm_sec;
    nuonTime->min = pcTime->tm_min;
    nuonTime->hour = pcTime->tm_hour;
    nuonTime->wday = pcTime->tm_wday;
    nuonTime->mday = pcTime->tm_mday;
    nuonTime->month = pcTime->tm_mon + 1; //according to the SDK, january = 1
    nuonTime->year = pcTime->tm_year + 1900; //Nuon year field is the actual year value, not a delta
    nuonTime->isdst = pcTime->tm_isdst;
    nuonTime->timezone = _timezone/60; //Nuon timezone field is in minutes rather than seconds

    //A value of -1 in the Nuon field indicates that the system doesn't keep track of DST.
    if(pcTime->tm_isdst != -1)
    {
      nuonTime->isdst = 1;
    }

    //The pointer in _tzname[1] points to the daylight savings time zone name
    //if available, and is set to NULL if not available.  I am assuming that
    //if windows is not set up to keep track of DST, _tzname[1] is set to NULL.

    //Almost all windows users will allow windows to keep track of DST, so it
    //wont hurt too much if this assumption is incorrect.  This may have to
    //be taken care of differently on other platforms

    if(_tzname[1] == NULL)
    {
      nuonTime->isdst = -1;
    }

    SwapVectorBytes((uint32 *)&nuonTime->sec);
    SwapVectorBytes((uint32 *)&nuonTime->mday);
    SwapScalarBytes((uint32 *)&nuonTime->isdst);
    SwapScalarBytes((uint32 *)&nuonTime->timezone);

    mpe.regs[0] = 0;
  }
  else
  {
    //Set time using values in time structure

    //Not allowed to set the time
    mpe.regs[0] = -1;
  }
}

uint64 useconds_since_start()
{
  if(bHighPerformanceTimerAvailable)
  {
    _LARGE_INTEGER counter;  
    QueryPerformanceCounter((_LARGE_INTEGER *)&counter);

    const unsigned long long cur_tick = (unsigned long long)(counter.QuadPart - ticksAtBootTime.QuadPart);
    return ((unsigned long long)tickFrequency.QuadPart < 100000000ull) ? (cur_tick * 1000000ull / (unsigned long long)tickFrequency.QuadPart)
        : (cur_tick * 1000ull / ((unsigned long long)tickFrequency.QuadPart / 1000ull));
  }
  else
  {
    assert(!"No QueryPerformanceCounter");
    return 0;
  }
}

void TimeElapsed(MPE &mpe)
{
  uint64 seconds, useconds, mseconds;

  if(bHighPerformanceTimerAvailable)
  {
    useconds = useconds_since_start();

    seconds = useconds / 1000000;
    mseconds = useconds / 1000;
    useconds = useconds % 1000000; // returns number of microseconds elapsed within the second
  }
  else
  {
    seconds = time(NULL) - ticksAtBootTime.QuadPart;
    mseconds = seconds*1000;
    useconds = 0;
  }

  const uint32 ptrSecs = mpe.regs[0];
  const uint32 ptrUSecs = mpe.regs[1];

  //Store seconds if pointer is not NULL
  if(ptrSecs)
  {
    uint32 *const memPtr = (uint32 *)nuonEnv.GetPointerToMemory(mpe,ptrSecs,true);
    *memPtr = (uint32)seconds;
    SwapScalarBytes(memPtr);
  }

  //Store microseconds if pointer is not NULL
  if(ptrUSecs)
  {
    uint32 *const memPtr = (uint32 *)nuonEnv.GetPointerToMemory(mpe,ptrUSecs,true);
    *memPtr = (uint32)useconds;
    SwapScalarBytes(memPtr);
  }

  mpe.regs[0] = (uint32)mseconds;
}

void TimerInit(const uint32 whichTimer, const uint32 rate)
{
  if(whichTimer == 0)
  {
#ifdef USE_QUEUE_TIMERS
    if(hSysTimer0)
    {
      DeleteTimerQueueTimer(NULL, hSysTimer0, NULL);
      CloseHandle(hSysTimer0);
      hSysTimer0 = 0;
    }
    if(rate/1000 > 0)
      CreateTimerQueueTimer(&hSysTimer0, NULL, (WAITORTIMERCALLBACK)SysTimer0Callback, 0, 0, rate/1000, WT_EXECUTEINTIMERTHREAD);
#else
    if(hSysTimer0)
    {
      timeKillEvent(hSysTimer0);
      hSysTimer0 = 0;
    }
    if(rate/1000 > 0)
      hSysTimer0 = timeSetEvent(rate/1000,0,(LPTIMECALLBACK)SysTimer0Callback,0,TIME_PERIODIC);
#endif
  }
  else if(whichTimer == 1)
  {
#ifdef USE_QUEUE_TIMERS
    if(hSysTimer1)
    {
      DeleteTimerQueueTimer(NULL, hSysTimer1, NULL);
      CloseHandle(hSysTimer1);
      hSysTimer1 = 0;
    }
    if (rate/1000 > 0)
      CreateTimerQueueTimer(&hSysTimer1, NULL, (WAITORTIMERCALLBACK)SysTimer1Callback, 0, 0, rate/1000, WT_EXECUTEINTIMERTHREAD);
#else
    if(hSysTimer1)
    {
      timeKillEvent(hSysTimer1);
      hSysTimer1 = 0;
    }
    if (rate/1000 > 0)
      hSysTimer1 = timeSetEvent(rate/1000,0,(LPTIMECALLBACK)SysTimer1Callback,0,TIME_PERIODIC);
#endif
  }
  else
  {
    assert(rate/1000 == 1000/VIDEO_HZ); // check if this was set (only by us) to the ~50 or 60Hz to trigger video interrupts at that pace
#ifdef USE_QUEUE_TIMERS
    if(hSysTimer2)
    {
      DeleteTimerQueueTimer(NULL, hSysTimer2, NULL);
      CloseHandle(hSysTimer2);
      hSysTimer2 = 0;
    }
    if (rate/1000 > 0)
      CreateTimerQueueTimer(&hSysTimer2, NULL, (WAITORTIMERCALLBACK)SysTimer2Callback, 0, 0, rate/1000, WT_EXECUTEINTIMERTHREAD);
#else
    if(hSysTimer2)
    {
      timeKillEvent(hSysTimer2);
      hSysTimer2 = 0;
    }
    if (rate/1000 > 0)
      hSysTimer2 = timeSetEvent(rate/1000,0,(LPTIMECALLBACK)SysTimer2Callback,0,TIME_PERIODIC);
#endif
  }
}

void TimerInit(MPE &mpe)
{
  const int32 whichTimer = mpe.regs[0];
  const int32 rate = mpe.regs[1];

  if((whichTimer < 0) || (whichTimer > 1))
  {
    mpe.regs[0] = 0;
  }
  else
  {
    TimerInit(whichTimer,rate);
    mpe.regs[0] = 1;
  }
}
