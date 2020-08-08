/******************************************************************************/
/* Pipe-utils, by LoRd_MuldeR <MuldeR2@GMX.de>                                */
/* This work has been released under the CC0 1.0 Universal license!           */
/******************************************************************************/

#include "version.h"

#define WIN32_LEAN_AND_MEAN 1
#include <Windows.h>
#include <ShellAPI.h>
#include <intrin.h>

static const double update = 0.3333;

/* ======================================================================= */
/* Global buffer                                                           */
/* ======================================================================= */

#define BUFFSIZE 1048576U
#define SLOT_COUNT 32U

static HANDLE g_stopping = NULL;
static HANDLE g_slots_free = NULL;
static HANDLE g_slots_used = NULL;

static CRITICAL_SECTION lock[SLOT_COUNT];
static DWORD buffer_len[SLOT_COUNT];
static BYTE buffer[SLOT_COUNT][BUFFSIZE];

#define INCREMENT(X) do { (X) = ((X) + 1U) % SLOT_COUNT; } while(0)

static volatile LONG64 g_bytes_transferred = 0ULL;

/* ======================================================================= */
/* Text output                                                             */
/* ======================================================================= */

static __inline BOOL print_text(const HANDLE output, const CHAR *const text)
{
	DWORD bytes_written;
	return WriteFile(output, text, lstrlenA(text), &bytes_written, NULL);
}

static __inline BOOL print_text_fmt(const HANDLE output, const CHAR *const format, ...)
{
	CHAR temp[256U];
	BOOL result = FALSE;
	va_list ap;
	va_start(ap, format);
	if(wvsprintfA(temp, format, ap))
	{
		result = print_text(output, temp);
	}
	va_end(ap);
	return result;
}

/* ======================================================================= */
/* Environment                                                             */
/* ======================================================================= */

const WCHAR *const get_env_variable(const WCHAR *const name)
{
	static WCHAR env_buffer[256U];
	const DWORD ret = GetEnvironmentVariableW(name, env_buffer, 256U);
	return ((ret > 0U) && (ret < 256U)) ? env_buffer : NULL;
}

/* ======================================================================= */
/* Math                                                                    */
/* ======================================================================= */

static __inline LONG round(const double d)
{
	return (d >= double(0.0)) ? LONG(d + double(0.5)) : LONG(d - LONG(d-1) + double(0.5)) + LONG(d-1);
}

static __inline LONG64 round64(const double d)
{
	return (d >= double(0.0)) ? LONG64(d + double(0.5)) : LONG64(d - double(LONG64(d-1)) + double(0.5)) + LONG64(d-1);
}

static __inline DWORD bound(const DWORD min_val, const DWORD val, const DWORD max_val)
{
	return (val < min_val) ? min_val : ((val > max_val) ? max_val : val);
}

/* ======================================================================= */
/* Formatting                                                              */
/* ======================================================================= */

typedef struct number_t
{
	DWORD value;
	DWORD fract;
	DWORD unit;
}
number_t;

static const char *const SIZE_UNITS[] =
{
	"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB", "ZiB", "YiB", NULL
};

static number_t convert(LONG64 value)
{
	number_t result;
	DWORD fract = result.unit = 0U;
	while((!result.unit) || (value >= 1024U))
	{
		++result.unit;
		fract = (DWORD)(value % 1024U);
		value /= 1024U;
	}
	result.value = (DWORD)value;
	result.fract = bound(0U, round((static_cast<double>(fract) / 1024.0) * 1000.0), 999U);
	return result;
}

static CHAR *format(CHAR *const buffer, const LONG64 value)
{
	const number_t number = convert(value);
	if((number.unit > 0U) && (number.value < 1000U))
	{
		if(number.value >= 100U)
		{
			wsprintfA(buffer, "%ld.%01ld %s", number.value, number.fract / 100U, SIZE_UNITS[number.unit]);
		}
		else if (number.value >= 10U)
		{
			wsprintfA(buffer, "%ld.%02ld %s", number.value, number.fract / 10U, SIZE_UNITS[number.unit]);
		}
		else
		{
			wsprintfA(buffer, "%ld.%03ld %s", number.value, number.fract, SIZE_UNITS[number.unit]);
		}
	}
	else
	{
		wsprintfA(buffer, "%ld %s", number.value, SIZE_UNITS[number.unit]);
	}
	return buffer;
}

/* ======================================================================= */
/* I/O functions                                                           */
/* ======================================================================= */

static DWORD read_chunk(const HANDLE handle, const bool is_pipe, BYTE *const data_out)
{
	DWORD bytes_read = 0U, sleep_timeout = 0U;
	for(;;)
	{
		if(ReadFile(handle, data_out, BUFFSIZE, &bytes_read, NULL))
		{
			if(bytes_read > 0U)
			{
				return bytes_read;
			}
			else if(!is_pipe)
			{
				return 0U; /*EOF*/
			}
		}
		else
		{
			const DWORD error = GetLastError();
			if((!is_pipe) || (error != ERROR_NO_DATA))
			{
				return 0U; /*failed*/
			}
		}
		if(sleep_timeout++)
		{
			if(WaitForSingleObject(g_stopping, 0U) == WAIT_OBJECT_0)
			{
				return 0U; /*stop*/
			}
			Sleep(sleep_timeout >> 8);
		}
	}
}

static bool write_chunk(const HANDLE handle, const bool is_pipe, const BYTE *const data, const DWORD data_len)
{
	DWORD bytes_written = 0U, sleep_timeout = 0U;
	for(DWORD offset = 0U; offset < data_len; offset += bytes_written)
	{
		if(!WriteFile(handle, data + offset, data_len - offset, &bytes_written, NULL))
		{
			return false; /*failed*/
		}
		if(bytes_written < 1U)
		{
			if(!is_pipe)
			{
				return false; /*failed*/
			}
			if(sleep_timeout++)
			{
				if(WaitForSingleObject(g_stopping, 0U) == WAIT_OBJECT_0)
				{
					return 0U; /*stop*/
				}
				Sleep(sleep_timeout >> 8);
			}
		}
	}
	return true;
}

/* ======================================================================= */
/* Read thread                                                             */
/* ======================================================================= */

static DWORD __stdcall read_thread(const LPVOID param)
{
	LONG slot_index = 0U;
	const bool is_pipe = (GetFileType((HANDLE)param) == FILE_TYPE_PIPE);

	for(;;)
	{
		const HANDLE handles[] = { g_slots_free, g_stopping };
		const DWORD wait_status = WaitForMultipleObjects(2U, handles, FALSE, INFINITE);
		if(wait_status != WAIT_OBJECT_0)
		{
			if(wait_status != WAIT_OBJECT_0 + 1U)
			{
				SetEvent(g_stopping);
			}
			else
			{
				return 0U;
			}
		}

		EnterCriticalSection(&lock[slot_index]);

		if((buffer_len[slot_index] = read_chunk((HANDLE)param, is_pipe, buffer[slot_index])) < 1U)
		{
			LeaveCriticalSection(&lock[slot_index]);
			SetEvent(g_stopping);
			return 0U;
		}

		LeaveCriticalSection(&lock[slot_index]);
		INCREMENT(slot_index);
		ReleaseSemaphore(g_slots_used, 1U, NULL);
	}
}

/* ======================================================================= */
/* Write thread                                                            */
/* ======================================================================= */

static DWORD __stdcall write_thread(const LPVOID param)
{
	LONG slot_index = 0U;
	const bool is_pipe = (GetFileType((HANDLE)param) == FILE_TYPE_PIPE);

	for(;;)
	{
		const HANDLE handles[] = { g_slots_used, g_stopping };
		const DWORD wait_status = WaitForMultipleObjects(2U, handles, FALSE, INFINITE);
		if(wait_status != WAIT_OBJECT_0)
		{
			if(wait_status == WAIT_OBJECT_0 + 1U)
			{
				if(WaitForSingleObject(g_slots_used, 1U) != WAIT_OBJECT_0)
				{
					return 0U;
				}
			}
			else
			{
				SetEvent(g_stopping);
				return 0U;
			}
		}

		EnterCriticalSection(&lock[slot_index]);

		if(!write_chunk((HANDLE)param, is_pipe, buffer[slot_index], buffer_len[slot_index]))
		{
			LeaveCriticalSection(&lock[slot_index]);
			SetEvent(g_stopping);
			return 0U;
		}

		InterlockedExchangeAdd64(&g_bytes_transferred, buffer_len[slot_index]);

		LeaveCriticalSection(&lock[slot_index]);
		INCREMENT(slot_index);
		ReleaseSemaphore(g_slots_free, 1U, NULL);
	}
}

/* ======================================================================= */
/* Status update                                                           */
/* ======================================================================= */

static void print_status(const HANDLE std_err, LARGE_INTEGER &time_now, LARGE_INTEGER &time_ref, const LARGE_INTEGER &perf_freq, double &average_rate, LONG64 &bytes_total)
{
	char buffer_bytes[32U], buffer_rate[32U];
	const LONG64 bytes_current = InterlockedExchange64(&g_bytes_transferred, 0LL);
	if(QueryPerformanceCounter(&time_now))
	{
		if(time_now.QuadPart > time_ref.QuadPart)
		{
			const double current_rate = static_cast<double>(bytes_current) / (static_cast<double>(time_now.QuadPart - time_ref.QuadPart) / static_cast<double>(perf_freq.QuadPart));
			average_rate = (average_rate < 0.0) ? current_rate : ((current_rate * update) + (average_rate * (1.0 - update)));
			print_text_fmt(std_err, "\r%s [%s/s] ", format(buffer_bytes, bytes_total += bytes_current), format(buffer_rate, round64(average_rate)));
		}
		time_ref.QuadPart = time_now.QuadPart;
	}
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
	print_text(output, "pv v" VERSION_STR " [" __DATE__ "], by LoRd_MuldeR <MuldeR2@GMX.de>\n\n");
	print_text(output, "Measure the throughput of a pipe and the amount of data transferred.\n");
	print_text(output, "Set environment variable PV_FORCE_NOWAIT=1 to force \"async\" mode.\n\n");
}

/* ======================================================================= */
/* Main                                                                    */
/* ======================================================================= */

static int _main(const int argc, const LPWSTR *const argv)
{
	LARGE_INTEGER time_now, time_ref, perf_freq;
	HANDLE thread_read = NULL, thread_write = NULL;
	LONG64 bytes_total = 0U;
	double average_rate = -1.0;

	const HANDLE std_inp = GetStdHandle(STD_INPUT_HANDLE);
	const HANDLE std_out = GetStdHandle(STD_OUTPUT_HANDLE);
	const HANDLE std_err = GetStdHandle(STD_ERROR_HANDLE);

	if ((std_inp == INVALID_HANDLE_VALUE) || (std_out == INVALID_HANDLE_VALUE))
	{
		goto clean_up;
	}

	for(DWORD slot_index = 0; slot_index < SLOT_COUNT; ++slot_index)
	{
		InitializeCriticalSection(&lock[slot_index]);
	}

	if((argc >= 2) && ((lstrcmpW(argv[1], L"-h") == 0) || (lstrcmpW(argv[1], L"-?") == 0) || (lstrcmpW(argv[1], L"/?") == 0)))
	{
		print_help_screen(std_err);
		goto clean_up;
	}

	if(!(QueryPerformanceFrequency(&perf_freq) && QueryPerformanceCounter(&time_ref)))
	{
		print_text(std_err, "Error: Failed to read performance counters!\n");
		goto clean_up;
	}

	if(!(g_stopping = CreateEventW(NULL, TRUE, FALSE, NULL)))
	{
		print_text(std_err, "Error: Failed to create 'stopping' event!\n");
		goto clean_up;
	}

	if(!(g_slots_free = CreateSemaphoreW(NULL, SLOT_COUNT, SLOT_COUNT, NULL)))
	{
		print_text(std_err, "Error: Failed to create 'slots_free' semaphore!\n");
		goto clean_up;
	}

	if(!(g_slots_used = CreateSemaphoreW(NULL, 0U, SLOT_COUNT, NULL)))
	{
		print_text(std_err, "Error: Failed to create 'slots_used' semaphore!\n");
		goto clean_up;
	}

	if(const WCHAR *const envstr = get_env_variable(L"PV_FORCE_NOWAIT"))
	{
		if(envstr[0U] && (GetFileType(std_inp) == FILE_TYPE_PIPE))
		{
			if((lstrcmpiW(envstr, L"1") == 0) || (lstrcmpiW(envstr, L"yes") == 0) || (lstrcmpiW(envstr, L"true") == 0))
			{
				if(GetFileType(std_inp) == FILE_TYPE_PIPE)
				{
					print_text(std_err, "PIPE_NOWAIT\n");
					const DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
					SetNamedPipeHandleState(std_inp, (LPDWORD)&mode, NULL, NULL);
				}
			}
		}
	}

	if(!(thread_read = CreateThread(NULL, 0U, read_thread, std_inp, 0U, NULL)))
	{
		print_text(std_err, "Error: Failed to create 'read' thread!\n");
		goto clean_up;
	}

	if(!(thread_write = CreateThread(NULL, 0U, write_thread, std_out, 0U, NULL)))
	{
		print_text(std_err, "Error: Failed to create 'write' thread!\n");
		goto clean_up;
	}

	SetThreadPriority(thread_read,  THREAD_PRIORITY_ABOVE_NORMAL);
	SetThreadPriority(thread_write, THREAD_PRIORITY_ABOVE_NORMAL);

	const HANDLE wait_handles[] = { thread_read, thread_write, g_stopping };
	while(WaitForMultipleObjects(3U, wait_handles, TRUE, 2500U) == WAIT_TIMEOUT)
	{
		print_status(std_err, time_now, time_ref, perf_freq, average_rate, bytes_total);
	}

	print_status(std_err, time_now, time_ref, perf_freq, average_rate, bytes_total);

clean_up:

	if(thread_read)
	{
		if(WaitForSingleObject(thread_read, 1000U) == WAIT_TIMEOUT)
		{
			TerminateThread(thread_read, 1U);
		}
		CloseHandle(thread_read);
	}

	if(thread_write)
	{
		if(WaitForSingleObject(thread_write, 1000U) == WAIT_TIMEOUT)
		{
			TerminateThread(thread_write, 1U);
		}
		CloseHandle(thread_write);
	}

	if(g_slots_free)
	{
		CloseHandle(g_slots_free);
	}

	if(g_slots_used)
	{
		CloseHandle(g_slots_used);
	}

	if(g_stopping)
	{
		CloseHandle(g_stopping);
	}

	return 0;
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
