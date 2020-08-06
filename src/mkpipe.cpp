/******************************************************************************/
/* Pipe-utils, by LoRd_MuldeR <MuldeR2@GMX.de>                                */
/* This work has been released under the CC0 1.0 Universal license!           */
/******************************************************************************/

#define WIN32_LEAN_AND_MEAN 1
#include <Windows.h>
#include <ShellAPI.h>

#define DEFAULT_PIPE_BUFFER 1048576

#define __MAKE_STR(X) #X
#define _MAKE_STR(X) __MAKE_STR(X)
#define DEFAULT_PIPE_BUFFER_STR _MAKE_STR(DEFAULT_PIPE_BUFFER)

static HANDLE g_stopping = NULL;

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

const WCHAR *const get_env_variable(const WCHAR *const name)
{
	static WCHAR env_buffer[256U];
	const DWORD ret = GetEnvironmentVariableW(name, env_buffer, 256U);
	return ((ret > 0U) && (ret < 256U)) ? env_buffer : NULL;
}

/* ======================================================================= */
/* Help screen                                                             */
/* ======================================================================= */

static void print_help_screen(const HANDLE output)
{
	print_text(output, "mkpipe [" __DATE__ "]\n\n");
	print_text(output, "Usage:\n   mkpipe.exe <command_1> [args] \"|\" <command_2> [args]\n\n");
	print_text(output, "Environment variable MKPIPE_BUFFSIZE overrides buffer size.\n");
	print_text(output, "Default buffer size is " DEFAULT_PIPE_BUFFER_STR " bytes.\n\n");
}

/* ======================================================================= */
/* Math                                                                    */
/* ======================================================================= */

static __inline DWORD add_safe(const DWORD a, const DWORD b)
{
	ULONGLONG t = ((ULONGLONG)a) + ((ULONGLONG)b);
	return (t > MAXDWORD) ? MAXDWORD : ((DWORD)t);
}

static __inline DWORD multiply_safe(const DWORD a, const DWORD b)
{
	ULONGLONG t = ((ULONGLONG)a) * ((ULONGLONG)b);
	return (t > MAXDWORD) ? MAXDWORD : ((DWORD)t);
}

/* ======================================================================= */
/* Parse integer                                                           */
/* ======================================================================= */

static DWORD parse_number(const WCHAR *str)
{
	bool hex_mode = false;
	DWORD value = 0U;
	while((*str) && (*str <= 0x20))
	{
		++str;
	}
	if((str[0U] == L'0') && ((str[1U] == L'x') || (str[1U] == L'X')))
	{
		hex_mode = true;
		str += 2U;
	}
	while(*str)
	{
		if(*str > 0x20)
		{
			value = multiply_safe(value, hex_mode ? 16U : 10U);
			if((*str >= L'0') && (*str <= L'9'))
			{
				value = add_safe(value, (*str - L'0'));
			}
			else if(hex_mode && (*str >= L'a') && (*str <= L'f'))
			{
				value = add_safe(value, (*str - L'a') + 10U);
			}
			else if(hex_mode && (*str >= L'A') && (*str <= L'F'))
			{
				value = add_safe(value, (*str - L'A') + 10U);
			}
			else
			{
				return 0U; /*invalid character!*/
			}
			++str;
		}
		else
		{
			break; /*break at space!*/
		}
	}
	while(*str)
	{
		if(*str > 0x20)
		{
			return 0U; /*character after space!*/
		}
		++str;
	}
	return value;
}

/* ======================================================================= */
/* Command-line parameters                                                 */
/* ======================================================================= */

static bool contains_space(const WCHAR *str)
{
	while(*str)
	{
		if(*str == L' ')
		{
			return true;
		}
		++str;
	}
	return false;
}

static void append_argument(WCHAR *const cmdline, const WCHAR *arg)
{
	int offset = lstrlenW(cmdline);
	if(offset > 0)
	{
		cmdline[offset++] = L' ';
	}
	const bool quoted = contains_space(arg);
	if(quoted)
	{
		cmdline[offset++] = L'"';
	}
	while(*arg)
	{
		if(*arg >= L' ')
		{
			if(*arg == L'"')
			{
				cmdline[offset++] = L'\\';
			}
			cmdline[offset++] = *arg;
		}
		arg++;
	}
	if(quoted)
	{
		cmdline[offset++] = L'"';
	}
	cmdline[offset] = L'\0';
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

static int _main(const WCHAR *const cmdline, const int argc, const LPWSTR *const argv)
{
	int result = 1;
	HANDLE pipe_rd = INVALID_HANDLE_VALUE, pipe_wr = INVALID_HANDLE_VALUE, inherit = INVALID_HANDLE_VALUE;
	WCHAR *command_1 = NULL, *command_2 = NULL;

	const HANDLE std_inp = GetStdHandle(STD_INPUT_HANDLE);
	const HANDLE std_out = GetStdHandle(STD_OUTPUT_HANDLE);
	const HANDLE std_err = GetStdHandle(STD_ERROR_HANDLE);

	STARTUPINFOW startup_info_1, startup_info_2;
	PROCESS_INFORMATION process_info_1, process_info_2;

	SecureZeroMemory(&startup_info_1, sizeof(STARTUPINFOW));
	SecureZeroMemory(&startup_info_2, sizeof(STARTUPINFOW));
	startup_info_1.cb = sizeof(STARTUPINFOW);
	startup_info_2.cb = sizeof(STARTUPINFOW);

	SecureZeroMemory(&process_info_1, sizeof(PROCESS_INFORMATION));
	SecureZeroMemory(&process_info_2, sizeof(PROCESS_INFORMATION));

	DWORD pipe_buffer_size = DEFAULT_PIPE_BUFFER;

	if(argc < 2)
	{
		print_help_screen(std_err);
		goto clean_up;
	}

	/* ---------------------------------------------------------------------- */
	/* Create command-lines                                                   */
	/* ---------------------------------------------------------------------- */

	const DWORD maxlen = lstrlenW(cmdline);

	command_1 = (WCHAR*) LocalAlloc(LPTR, (maxlen + 1U) * sizeof(WCHAR));
	command_2 = (WCHAR*) LocalAlloc(LPTR, (maxlen + 1U) * sizeof(WCHAR));

	if((!command_1) || (!command_2))
	{
		print_text(std_err, "Error: Memory allocation has failed!\n");
		goto clean_up;
	}

	bool flag = false;
	command_1[0U] = command_2[0U] = L'\0';

	for(int i = 1; i < argc; ++i)
	{
		if(lstrcmpW(argv[i], L"|") == 0)
		{
			if(!flag)
			{
				flag = true;
			}
			else
			{
				if(i + 1 < argc)
				{
					print_text(std_err, "Warning: Excess parameters have been ignored!\n\n");
				}
				break;
			}
		}
		else
		{
			append_argument(flag ? command_2 : command_1, argv[i]);
		}
	}

	if(!command_2[0U])
	{
		print_text(std_err, "Error: Second command to be executed is missing!\n\n");
		goto clean_up;
	}

	/* ---------------------------------------------------------------------- */
	/* Create the pipe                                                        */
	/* ---------------------------------------------------------------------- */

	if(const WCHAR *const envstr = get_env_variable(L"MKPIPE_BUFFSIZE"))
	{
		if(envstr[0U])
		{
			const DWORD value = parse_number(envstr);
			if(value > 0U)
			{
				pipe_buffer_size = (value >= 1024U) ? value : 1024U;
			}
			else
			{
				print_text(std_err, "Warning: MKPIPE_BUFFSIZE is invalid -> ignoring!\n");
			}
		}
	}

	if(!CreatePipe(&pipe_rd, &pipe_wr, NULL, pipe_buffer_size))
	{
		print_text(std_err, "Error: Failed to create the pipe!\n");
		goto clean_up;
	}

	if(!(g_stopping = CreateEventW(NULL, TRUE, FALSE, NULL)))
	{
		print_text(std_err, "Error: Failed to create event object!\n");
		goto clean_up;
	}

	/* ---------------------------------------------------------------------- */
	/* Start process #1                                                       */
	/* ---------------------------------------------------------------------- */

	if(!DuplicateHandle(GetCurrentProcess(), pipe_wr, GetCurrentProcess(), &inherit, 0, TRUE, DUPLICATE_SAME_ACCESS))
	{
		print_text(std_err, "Error: Failed to create inheritable handle!\n");
		goto clean_up;
	}

	CloseHandle(pipe_wr);
	pipe_wr = INVALID_HANDLE_VALUE;

	startup_info_1.dwFlags |= STARTF_USESTDHANDLES;
	startup_info_1.hStdInput = std_inp;
	startup_info_1.hStdOutput = inherit;
	startup_info_1.hStdError = std_err;

	if(!CreateProcessW(NULL, command_1, NULL, NULL, TRUE, CREATE_SUSPENDED, NULL, NULL, &startup_info_1, &process_info_1))
	{
		print_text(std_err, "Error: Failed to create process #1!\n");
		goto clean_up;
	}

	CloseHandle(inherit);
	inherit = INVALID_HANDLE_VALUE;

	/* ---------------------------------------------------------------------- */
	/* Start process #2                                                       */
	/* ---------------------------------------------------------------------- */

	if(!DuplicateHandle(GetCurrentProcess(), pipe_rd, GetCurrentProcess(), &inherit, 0, TRUE, DUPLICATE_SAME_ACCESS))
	{
		print_text(std_err, "Error: Failed to create inheritable handle!\n");
		goto clean_up;
	}

	CloseHandle(pipe_rd);
	pipe_rd = INVALID_HANDLE_VALUE;

	startup_info_2.dwFlags |= STARTF_USESTDHANDLES;
	startup_info_2.hStdInput = inherit;
	startup_info_2.hStdOutput = std_out;
	startup_info_2.hStdError = std_err;

	if(!CreateProcessW(NULL, command_2, NULL, NULL, TRUE, CREATE_SUSPENDED, NULL, NULL, &startup_info_2, &process_info_2))
	{
		print_text(std_err, "Error: Failed to create process #2!\n");
		goto clean_up;
	}

	CloseHandle(inherit);
	inherit = INVALID_HANDLE_VALUE;

	/* ---------------------------------------------------------------------- */
	/* Wait for process termination                                           */
	/* ---------------------------------------------------------------------- */

	if((ResumeThread(process_info_1.hThread) == ((DWORD)-1)) || (ResumeThread(process_info_2.hThread) == ((DWORD)-1)))
	{
		print_text(std_err, "Error: Failed to resume threads!\n");
		goto clean_up;
	}

	CloseHandle(process_info_1.hThread);
	CloseHandle(process_info_2.hThread);

	process_info_1.hThread = process_info_2.hThread = NULL;

	const HANDLE wait_handles[] = { process_info_1.hProcess, process_info_2.hProcess, g_stopping };
	WaitForMultipleObjects(3U, wait_handles, TRUE, INFINITE);

	/* ---------------------------------------------------------------------- */
	/* Final clean-up                                                         */
	/* ---------------------------------------------------------------------- */

clean_up:

	if(inherit != INVALID_HANDLE_VALUE)
	{
		CloseHandle(inherit);
	}
	
	if(process_info_1.hThread)
	{
		CloseHandle(process_info_1.hThread);
	}

	if(process_info_2.hThread)
	{
		CloseHandle(process_info_2.hThread);
	}

	if(process_info_1.hProcess)
	{
		if(WaitForSingleObject(process_info_1.hProcess, 2500U) == WAIT_TIMEOUT)
		{
			TerminateProcess(process_info_1.hProcess, 1U);
		}
		CloseHandle(process_info_1.hProcess);
	}

	if(process_info_2.hProcess)
	{
		if(WaitForSingleObject(process_info_2.hProcess, 2500U) == WAIT_TIMEOUT)
		{
			TerminateProcess(process_info_2.hProcess, 1U);
		}
		CloseHandle(process_info_2.hProcess);
	}

	if(pipe_rd != INVALID_HANDLE_VALUE)
	{
		CloseHandle(pipe_rd);
	}

	if(pipe_wr != INVALID_HANDLE_VALUE)
	{
		CloseHandle(pipe_wr);
	}

	if(command_1)
	{
		LocalFree(command_1);
	}

	if(command_2)
	{
		LocalFree(command_2);
	}

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
	const WCHAR *cmdline;

	SetErrorMode(SetErrorMode(0x3) | 0x3);
	SetConsoleCtrlHandler(ctrl_handler_routine, TRUE);

	if(argv = CommandLineToArgvW(cmdline = GetCommandLineW(), &argc))
	{
		result = _main(cmdline, argc, argv);
		LocalFree(argv);
	}

	ExitProcess(result);
}
