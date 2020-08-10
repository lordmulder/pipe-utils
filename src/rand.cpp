/******************************************************************************/
/* Pipe-utils, by LoRd_MuldeR <MuldeR2@GMX.de>                                */
/* This work has been released under the CC0 1.0 Universal license!           */
/******************************************************************************/

#include "version.h"

#define WIN32_LEAN_AND_MEAN 1
#include <Windows.h>
#include <ShellAPI.h>

#define BUFFSIZE (16384U / sizeof(DWORD))
#define BUFFSIZE_BYTES (sizeof(DWORD) * BUFFSIZE)

static DWORD buffer[BUFFSIZE];
static HANDLE g_stopping = NULL;

/* ======================================================================= */
/* Text output                                                             */
/* ======================================================================= */

static __inline BOOL print_text(const HANDLE output, const CHAR *const text)
{
	DWORD bytes_written;
	return WriteFile(output, text, lstrlenA(text), &bytes_written, NULL);
}

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
/* Help screen                                                             */
/* ======================================================================= */

#define __VERSION_STR(X, Y, Z) #X "." #Y "." #Z
#define _VERSION_STR(X, Y, Z) __VERSION_STR(X, Y, Z)
#define VERSION_STR _VERSION_STR(PIPEUTILS_VERSION_MAJOR, PIPEUTILS_VERSION_MINOR, PIPEUTILS_VERSION_PATCH)

static void print_help_screen(const HANDLE output)
{
	print_text(output, "rand v" VERSION_STR " [" __DATE__ "], by LoRd_MuldeR <MuldeR2@GMX.de>\n\n");
	print_text(output, "Fast generator of pseudo-random bytes, using the \"xorwow\" method.\n");
	print_text(output, "Output has been verified to pass the Dieharder test suite.\n\n");
}

/* ======================================================================= */
/* Main                                                                    */
/* ======================================================================= */

static UINT _main(const int argc, const LPWSTR *const argv)
{
	random_t state;
	g_stopping = CreateEventW(NULL, TRUE, FALSE, NULL);
	UINT result = 1U;
	DWORD bytes_written = 0U;
	BYTE check = 0U;

	const HANDLE std_err = GetStdHandle(STD_ERROR_HANDLE);
	const HANDLE std_out = GetStdHandle(STD_OUTPUT_HANDLE);

	if((argc >= 2) && ((lstrcmpW(argv[1], L"-h") == 0) || (lstrcmpW(argv[1], L"-?") == 0) || (lstrcmpW(argv[1], L"/?") == 0)))
	{
		print_help_screen(std_err);
		goto exit_loop;
	}

	if (std_out == INVALID_HANDLE_VALUE)
	{
		print_text(std_err, "Error: Failed to initialize output stream!\n");
		goto exit_loop;
	}

	const bool is_pipe = (GetFileType(std_out) == FILE_TYPE_PIPE);

	result = 0U;
	random_seed(&state);

	for(;;)
	{
		DWORD bytes_written = 0U, sleep_timeout = 0U;
		if(!(++check))
		{
			if(WaitForSingleObject(g_stopping, 0U) == WAIT_OBJECT_0)
			{
				result = 130U;
				goto exit_loop;
			}
		}
		for (DWORD offset = 0U; offset < BUFFSIZE; ++offset)
		{
			buffer[offset] = random_next(&state);
		}
		for (DWORD offset = 0U; offset < BUFFSIZE_BYTES; offset += bytes_written)
		{
			if (!WriteFile(std_out, ((BYTE*)buffer) + offset, BUFFSIZE_BYTES - offset, &bytes_written, NULL))
			{
				goto exit_loop; /*failed*/
			}
			if(bytes_written < 1U)
			{
				if(!is_pipe)
				{
					goto exit_loop; /*failed*/
				}
				if(sleep_timeout++)
				{
					if(WaitForSingleObject(g_stopping, 0U) == WAIT_OBJECT_0)
					{
						result = 130U;
						goto exit_loop; /*stop*/
					}
					Sleep(sleep_timeout >> 8);
				}
			}
		}
	}

exit_loop:

	return result;
}

/* ======================================================================= */
/* Entry point                                                             */
/* ======================================================================= */

void startup(void)
{
	int argc;
	UINT result = (UINT)(-1);
	LPWSTR *argv;

	SetErrorMode(SetErrorMode(0x3) | 0x3);
	SetConsoleCtrlHandler(ctrl_handler_routine, TRUE);

	if(argv = CommandLineToArgvW(GetCommandLineW(), &argc))
	{
		result = _main(argc, argv);
		LocalFree(argv);
	}

	ExitProcess(result);
}
