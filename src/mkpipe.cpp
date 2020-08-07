/******************************************************************************/
/* Pipe-utils, by LoRd_MuldeR <MuldeR2@GMX.de>                                */
/* This work has been released under the CC0 1.0 Universal license!           */
/******************************************************************************/

#define WIN32_LEAN_AND_MEAN 1
#include <Windows.h>
#include <ShellAPI.h>

#define MAX_CMDLINE_LEN 32768
#define MAX_PROCESSES 16U
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
/* Help screen                                                             */
/* ======================================================================= */

static void print_help_screen(const HANDLE output)
{
	print_text(output, "mkpipe [" __DATE__ "]\n\n");
	print_text(output, "Usage:\n");
	print_text(output, "   mkpipe.exe mkpipe.exe <command_1> \"|\" <command_2> \"|\" ... \"|\" <command_n>\n\n");
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
/* Handles                                                                */
/* ======================================================================= */

static HANDLE create_inheritable_handle(HANDLE &original)
{
	HANDLE duplicate = INVALID_HANDLE_VALUE;
	if(DuplicateHandle(GetCurrentProcess(), original, GetCurrentProcess(), &duplicate, 0, TRUE, DUPLICATE_SAME_ACCESS))
	{
		CloseHandle(original);
		original = INVALID_HANDLE_VALUE;
		return duplicate;
	}
	return INVALID_HANDLE_VALUE;
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

static bool append_argument(WCHAR *const cmdline, const WCHAR *arg)
{
	const bool quoted = contains_space(arg);
	int offset = lstrlenW(cmdline);
	if(((offset > 0U) ? offset + 1U : offset) + lstrlenW(arg) + (quoted ? 3L : 1L) > MAX_CMDLINE_LEN)
	{
		return false;
	}
	if(offset > 0)
	{
		cmdline[offset++] = L' ';
	}
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
	return true;
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

static int _main(const int argc, const LPWSTR *const argv)
{
	int result = 1;
	DWORD pipe_buffer_size = DEFAULT_PIPE_BUFFER, command_count = 0U;
	WCHAR *command[MAX_PROCESSES];
	HANDLE pipe_rd[MAX_PROCESSES - 1U], pipe_wr[MAX_PROCESSES - 1U];
	STARTUPINFOW startup_info[MAX_PROCESSES];
	PROCESS_INFORMATION process_info[MAX_PROCESSES];

	SecureZeroMemory(startup_info, MAX_PROCESSES * sizeof(STARTUPINFOW));
	SecureZeroMemory(process_info, MAX_PROCESSES * sizeof(PROCESS_INFORMATION));

	for(DWORD command_index = 0; command_index < MAX_PROCESSES; ++command_index)
	{
		command[command_index] = NULL;
		startup_info[command_index].cb = sizeof(STARTUPINFOW);
		if(command_index < MAX_PROCESSES - 1U)
		{
			pipe_rd[command_index] = pipe_wr[command_index] = INVALID_HANDLE_VALUE;
		}
	}

	const HANDLE std_inp = GetStdHandle(STD_INPUT_HANDLE);
	const HANDLE std_out = GetStdHandle(STD_OUTPUT_HANDLE);
	const HANDLE std_err = GetStdHandle(STD_ERROR_HANDLE);

	if(argc < 2)
	{
		print_help_screen(std_err);
		goto clean_up;
	}

	if((std_inp == INVALID_HANDLE_VALUE) || (std_out == INVALID_HANDLE_VALUE))
	{
		print_text(std_err, "Error: Invalid standard handles!\n");
		goto clean_up;
	}

	/* ---------------------------------------------------------------------- */
	/* Create command-lines                                                   */
	/* ---------------------------------------------------------------------- */

	if(command[0U] = (WCHAR*) LocalAlloc(LPTR, MAX_CMDLINE_LEN * sizeof(WCHAR)))
	{
		command[0U][0U] = L'\0';
	}
	else
	{
		print_text(std_err, "Error: Memory allocation has failed!\n");
		goto clean_up;
	}

	for(int i = 1; i < argc; ++i)
	{
		if(lstrcmpW(argv[i], L"|") == 0)
		{
			if(!command[command_count][0U])
			{
				print_text_fmt(std_err, "Error: Command #%ld is incomplete!\n", command_count + 1U);
				goto clean_up;
			}
			if(i + 1 < argc)
			{
				if(command_count >= MAX_PROCESSES - 1U)
				{
					print_text_fmt(std_err, "Error: Too many commands specified!\n", command_count);
					goto clean_up;
				}
				if(!(command[++command_count] = (WCHAR*) LocalAlloc(LPTR, MAX_CMDLINE_LEN * sizeof(WCHAR))))
				{
					print_text(std_err, "Error: Memory allocation has failed!\n");
					goto clean_up;
				}
				command[command_count][0U] = L'\0';
			}
			else
			{
				break; /*no more args*/
			}
		}
		else
		{
			if(!append_argument(command[command_count], argv[i]))
			{
				print_text(std_err, "Error: Command-line length exceeds the limit!\n");
				goto clean_up;
			}
		}
	}

	if(!command[command_count++][0U])
	{
		print_text_fmt(std_err, "Error: Command #%ld is incomplete!\n", command_count);
		goto clean_up;
	}

	if(command_count < 2U)
	{
		print_text_fmt(std_err, "Error: Must specify at least two commands!\n", command_count);
		goto clean_up;
	}

	if(command_count > MAX_PROCESSES)
	{
		print_text_fmt(std_err, "Error: Too many commands specified!\n", command_count);
		goto clean_up;
	}

	/* ---------------------------------------------------------------------- */
	/* Create the pipes                                                       */
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

	for(DWORD command_index = 0U; command_index < command_count - 1U; ++command_index)
	{
		if(!CreatePipe(&pipe_rd[command_index], &pipe_wr[command_index], NULL, pipe_buffer_size))
		{
			pipe_rd[command_index] = pipe_wr[command_index] = INVALID_HANDLE_VALUE;
			print_text(std_err, "Error: Failed to create the pipe!\n");
			goto clean_up;
		}
	}

	if(!(g_stopping = CreateEventW(NULL, TRUE, FALSE, NULL)))
	{
		print_text(std_err, "Error: Failed to create event object!\n");
		goto clean_up;
	}

	/* ---------------------------------------------------------------------- */
	/* Start processes                                                        */
	/* ---------------------------------------------------------------------- */

	for(DWORD command_index = 0U; command_index < command_count; ++command_index)
	{
		startup_info[command_index].dwFlags |= STARTF_USESTDHANDLES;
		startup_info[command_index].hStdError  = std_err;
		startup_info[command_index].hStdInput  = (command_index > 0U) ? create_inheritable_handle(pipe_rd[command_index - 1U]) : std_inp;
		startup_info[command_index].hStdOutput = (command_index < command_count - 1U) ? create_inheritable_handle(pipe_wr[command_index]) : std_out;
		
		if((startup_info[command_index].hStdInput == INVALID_HANDLE_VALUE) || (startup_info[command_index].hStdOutput == INVALID_HANDLE_VALUE))
		{
			print_text(std_err, "Error: Failed to create inheritable handle!\n");
			goto clean_up;
		}

		const BOOL success = CreateProcessW(NULL, command[command_index], NULL, NULL, TRUE, CREATE_SUSPENDED, NULL, NULL, &startup_info[command_index], &process_info[command_index]);

		if(startup_info[command_index].hStdInput != std_inp)
		{
			CloseHandle(startup_info[command_index].hStdInput);
			
		}

		if(startup_info[command_index].hStdOutput != std_out)
		{
			CloseHandle(startup_info[command_index].hStdOutput);
		}

		startup_info[command_index].hStdInput = startup_info[command_index].hStdOutput = NULL;

		if(!success)
		{
			process_info[command_index].hProcess = process_info[command_index].hThread = NULL;
			print_text_fmt(std_err, "Error: Failed to create process #%ld!\n", command_index + 1U);
			goto clean_up;
		}
	}

	/* ---------------------------------------------------------------------- */
	/* Resume processes                                                       */
	/* ---------------------------------------------------------------------- */

	for(DWORD command_index = 0U; command_index < command_count; ++command_index)
	{
		if(ResumeThread(process_info[command_index].hThread) == ((DWORD)-1))
		{
			print_text_fmt(std_err, "Error: Failed to resume process #%ld!\n", command_index + 1U);
			goto clean_up;
		}
		CloseHandle(process_info[command_index].hThread);
		process_info[command_index].hThread = NULL;
	}

	/* ---------------------------------------------------------------------- */
	/* Wait for process termination                                           */
	/* ---------------------------------------------------------------------- */

	for(DWORD command_index = 0U; command_index < command_count; ++command_index)
	{
		const HANDLE wait_handles[] =
		{
			process_info[command_index].hProcess, g_stopping
		};
		if(WaitForMultipleObjects(2U, wait_handles, FALSE, INFINITE) != WAIT_OBJECT_0)
		{
			goto clean_up;
		}
		CloseHandle(process_info[command_index].hProcess);
		process_info[command_index].hProcess = NULL;
	}

	result = 0;

	/* ---------------------------------------------------------------------- */
	/* Final clean-up                                                         */
	/* ---------------------------------------------------------------------- */

clean_up:

	for(DWORD command_index = 0U; command_index < MAX_PROCESSES; ++command_index)
	{
		if(process_info[command_index].hThread)
		{
			CloseHandle(process_info[command_index].hThread);
		}
		if(process_info[command_index].hProcess)
		{
			if(WaitForSingleObject(process_info[command_index].hProcess, 1000U) == WAIT_TIMEOUT)
			{
				TerminateProcess(process_info[command_index].hProcess, 1U);
			}
			CloseHandle(process_info[command_index].hProcess);
		}
	}

	for(DWORD command_index = 0U; command_index < MAX_PROCESSES - 1U; ++command_index)
	{
		if(pipe_rd[command_index] != INVALID_HANDLE_VALUE)
		{
			CloseHandle(pipe_rd[command_index]);
		}
		if(pipe_wr[command_index] != INVALID_HANDLE_VALUE)
		{
			CloseHandle(pipe_wr[command_index]);
		}
	}

	for(DWORD command_index = 0U; command_index < command_count; ++command_index)
	{
		if(command[command_index])
		{
			LocalFree(command[command_index]);
		}
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

	SetErrorMode(SetErrorMode(0x3) | 0x3);
	SetConsoleCtrlHandler(ctrl_handler_routine, TRUE);

	if(argv = CommandLineToArgvW(GetCommandLineW(), &argc))
	{
		result = _main(argc, argv);
		LocalFree(argv);
	}

	ExitProcess(result);
}
