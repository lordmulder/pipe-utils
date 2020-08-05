#define WIN32_LEAN_AND_MEAN 1
#include <Windows.h>
#include <math.h>
#include <intrin.h>

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
	result.fract = (fract * 1000U) / 1024U;
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

static __inline LONG64 round64(const double d)
{
	return d >= 0.0 ? LONG64(d + 0.5) : LONG64(d - LONG64(LONG64(d-1)) + 0.5) + LONG64(d-1);
}

/* ======================================================================= */
/* I/O functions                                                           */
/* ======================================================================= */

static DWORD read_chunk(const HANDLE handle, const bool is_pipe, BYTE *const data_out)
{
	WORD sleep = 0U;
	DWORD bytes_read = 0U;
	for(;;)
	{
		if(ReadFile(handle, data_out, BUFFSIZE, &bytes_read, NULL))
		{
			sleep = 0U;
			if(bytes_read > 0U)
			{
				return bytes_read;
			}
			else
			{
				if(!is_pipe)
				{
					return 0U;
				}
			}
		}
		else
		{
			const DWORD error = GetLastError();
			if(error != ERROR_NO_DATA)
			{
				return 0U;
			}
			if(++sleep > 32U)
			{
				Sleep((sleep > 1024U) ? 1U : 0U);
			}
		}
	}
}

static bool write_chunk(const HANDLE handle, const BYTE *const data, const DWORD data_len)
{
	DWORD bytes_written = 0U;
	for(DWORD offset = 0U; offset < data_len; offset += bytes_written)
	{
		bool success = false;
		if(WriteFile(handle, data + offset, data_len - offset, &bytes_written, NULL))
		{
			success = (bytes_written > 0U);
		}
		if(!success)
		{
			return false;
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

		if(!write_chunk((HANDLE)param, buffer[slot_index], buffer_len[slot_index]))
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
/* Main                                                                    */
/* ======================================================================= */

static int _main(void)
{
	LARGE_INTEGER time_now, time_ref, perf_freq;
	char buffer_bytes[32U], buffer_rate[32U];
	HANDLE thread_read = NULL, thread_write = NULL;
	LONG64 bytes_total = 0U;
	DWORD time_start = GetTickCount();
	double average_rate = -1.0;


	const HANDLE std_in  = GetStdHandle(STD_INPUT_HANDLE);
	const HANDLE std_out = GetStdHandle(STD_OUTPUT_HANDLE);
	const HANDLE std_err = GetStdHandle(STD_ERROR_HANDLE);

	if (std_in == INVALID_HANDLE_VALUE)
	{
		return 1;
	}

	for(DWORD slot_index = 0; slot_index < SLOT_COUNT; ++slot_index)
	{
		InitializeCriticalSection(&lock[slot_index]);
	}

	if(!(QueryPerformanceFrequency(&perf_freq) && QueryPerformanceCounter(&time_ref)))
	{
		goto clean_up;
	}

	if(!(g_stopping = CreateEventW(NULL, TRUE, FALSE, NULL)))
	{
		goto clean_up;
	}

	if(!(g_slots_free = CreateSemaphoreW(NULL, SLOT_COUNT, SLOT_COUNT, NULL)))
	{
		goto clean_up;
	}

	if(!(g_slots_used = CreateSemaphoreW(NULL, 0U, SLOT_COUNT, NULL)))
	{
		goto clean_up;
	}

	if(GetFileType(std_in) == FILE_TYPE_PIPE)
	{
		const DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
		SetNamedPipeHandleState(std_in, (LPDWORD)&mode, NULL, NULL);
	}

	if(!(thread_read = CreateThread(NULL, 0U, read_thread, std_in, 0U, NULL)))
	{
		goto clean_up;
	}

	if(!(thread_write = CreateThread(NULL, 0U, write_thread, std_out, 0U, NULL)))
	{
		goto clean_up;
	}

	SetThreadPriority(thread_read,  THREAD_PRIORITY_ABOVE_NORMAL);
	SetThreadPriority(thread_write, THREAD_PRIORITY_ABOVE_NORMAL);

	const HANDLE handles[] = { thread_read, thread_write };
	while(WaitForMultipleObjects(2U, handles, TRUE, 2500U) == WAIT_TIMEOUT)
	{
		if(QueryPerformanceCounter(&time_now))
		{
			const LONG64 bytes_current = InterlockedExchange64(&g_bytes_transferred, 0LL);
			const double current_rate = static_cast<double>(bytes_current) / (static_cast<double>(time_now.QuadPart - time_ref.QuadPart) / static_cast<double>(perf_freq.QuadPart));
			average_rate = (average_rate < 0.0) ? current_rate : ((current_rate * 0.25) + (average_rate * 0.75));
			print_text_fmt(std_err, "\r%s [%s/s] ", format(buffer_bytes, bytes_total += bytes_current), format(buffer_rate, round64(average_rate)));
			time_ref.QuadPart = time_now.QuadPart;
		}
	}

clean_up:

	if(thread_read)
	{
		if(WaitForSingleObject(thread_read, 250U) == WAIT_TIMEOUT)
		{
			TerminateThread(thread_read, 0U);
		}
		CloseHandle(thread_read);
	}

	if(thread_write)
	{
		if(WaitForSingleObject(thread_write, 250U) == WAIT_TIMEOUT)
		{
			TerminateThread(thread_write, 0U);
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

void startup(void)
{
	SetErrorMode(SetErrorMode(0x3) | 0x3);
	ExitProcess(_main());
}
