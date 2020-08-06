/******************************************************************************/
/* Pipe-utils, by LoRd_MuldeR <MuldeR2@GMX.de>                                */
/* This work has been released under the CC0 1.0 Universal license!           */
/******************************************************************************/

#define WIN32_LEAN_AND_MEAN 1
#include <Windows.h>

#define BUFFSIZE (16384U / sizeof(DWORD))
#define BUFFSIZE_BYTES (sizeof(DWORD) * BUFFSIZE)

static DWORD buffer[BUFFSIZE];

static HANDLE g_stopping = NULL;

/* ======================================================================= */
/* Pseduo-random number generator                                          */
/* ======================================================================= */

typedef struct random_t
{
	DWORD a, b, c, d;
	DWORD counter;
}
random_t;

static void random_seed(random_t *const state)
{
	LARGE_INTEGER counter;
	FILETIME time;
	state->a = 65599U * GetCurrentThreadId() + GetCurrentProcessId();
	do
	{
		GetSystemTimeAsFileTime(&time);
		QueryPerformanceCounter(&counter);
		state->b = GetTickCount();
		state->c = 65599U * time.dwHighDateTime + time.dwLowDateTime;
		state->d = 65599U * counter.HighPart + counter.LowPart;
	}
	while((!state->a) && (!state->b) && (!state->c) && (!state->d));
	state->counter = 0U;
}

static __forceinline DWORD random_next(random_t *const state)
{
	DWORD t = state->d;
	const DWORD s = state->a;
	state->d = state->c;
	state->c = state->b;
	state->b = s;
	t ^= t >> 2;
	t ^= t << 1;
	t ^= s ^ (s << 4);
	state->a = t;
	return t + (state->counter += 362437U);
}

/* ======================================================================= */
/* Ctrl+C handler routine                                                  */
/* ======================================================================= */

BOOL WINAPI ctrl_handler_routine(const DWORD type)
{
	switch(type)
	{
	case CTRL_C_EVENT:
	case CTRL_BREAK_EVENT:
	case CTRL_CLOSE_EVENT:
	case CTRL_LOGOFF_EVENT:
	case CTRL_SHUTDOWN_EVENT:
		if(g_stopping)
		{
			SetEvent(g_stopping);
		}
		return TRUE;
	}
	return FALSE;
}

/* ======================================================================= */
/* Main                                                                    */
/* ======================================================================= */

static int _main(void)
{
	random_t state;
	g_stopping = CreateEventW(NULL, TRUE, FALSE, NULL);
	BYTE check = 0U;
	DWORD bytes_written = 0U;

	const HANDLE std_out = GetStdHandle(STD_OUTPUT_HANDLE);
	if (std_out == INVALID_HANDLE_VALUE)
	{
		return 1;
	}


	random_seed(&state);

	for(;;)
	{
		bool fail_flag = false;
		if(!(++check))
		{
			if(WaitForSingleObject(g_stopping, 0U) == WAIT_OBJECT_0)
			{
				goto exit_loop;
			}
		}
		for (DWORD i = 0U; i < BUFFSIZE; ++i)
		{
			buffer[i] = random_next(&state);
		}
		for (DWORD pos = 0U; pos < BUFFSIZE_BYTES; pos += bytes_written)
		{
			if (!WriteFile(std_out, ((BYTE*)buffer) + pos, BUFFSIZE_BYTES - pos, &bytes_written, NULL))
			{
				goto exit_loop;
			}
			if (bytes_written < 1U)
			{
				if(!fail_flag)
				{
					fail_flag = true;
					continue;
				}
				else
				{
					goto exit_loop;
				}
			}
			fail_flag = false;
		}
	}

exit_loop:

	return 0;
}

/* ======================================================================= */
/* Entry point                                                             */
/* ======================================================================= */

void startup(void)
{
	SetErrorMode(SetErrorMode(0x3) | 0x3);
	SetConsoleCtrlHandler(ctrl_handler_routine, TRUE);
	ExitProcess(_main());
}
