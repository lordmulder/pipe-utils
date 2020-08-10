/******************************************************************************/
/* Pipe-utils, by LoRd_MuldeR <MuldeR2@GMX.de>                                */
/* This work has been released under the CC0 1.0 Universal license!           */
/******************************************************************************/

#include "version.h"

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

#define ARGV_IS_VALID(N) \
	(((N) < argc) && \
	argv[(N)][0U] && \
	(lstrcmpW(argv[(N)], L"|") != 0) && \
	(lstrcmpW(argv[(N)], L"<") != 0) && \
	(lstrcmpW(argv[(N)], L">") != 0))

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

#define __VERSION_STR(X, Y, Z) #X "." #Y "." #Z
#define _VERSION_STR(X, Y, Z) __VERSION_STR(X, Y, Z)
#define VERSION_STR _VERSION_STR(PIPEUTILS_VERSION_MAJOR, PIPEUTILS_VERSION_MINOR, PIPEUTILS_VERSION_PATCH)

static void print_help_screen(const HANDLE output)
{
	print_text(output, "mkpipe v" VERSION_STR " [" __DATE__ "], by LoRd_MuldeR <MuldeR2@GMX.de>\n\n");
	print_text(output, "Connect N processes via pipe(s), with configurable pipe buffer size.\n\n");
	print_text(output, "Usage:\n");
	print_text(output, "   mkpipe.exe <command_1> \"|\" <command_2> \"|\" ... \"|\" <command_n>\n");
	print_text(output, "   mkpipe.exe \"<\" <input_file> [commands 1...n] \">\" <output_file>\n\n");
	print_text(output, "Environment variable MKPIPE_BUFFSIZE can be used to override buffer size.\n");
	print_text(output, "Default buffer size is " DEFAULT_PIPE_BUFFER_STR " bytes.\n\n");
	print_text(output, "Operators '|', '<' and '>' must be *quoted* when running from the shell!\n\n");
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
/* I/O functions                                                           */
/* ======================================================================= */

static HANDLE create_inheritable_handle(const HANDLE std_stream_1, const HANDLE std_stream_2, HANDLE &original)
{
	HANDLE duplicate = INVALID_HANDLE_VALUE;
	if(DuplicateHandle(GetCurrentProcess(), original, GetCurrentProcess(), &duplicate, 0, TRUE, DUPLICATE_SAME_ACCESS))
	{
		if((original != std_stream_1) && (original != std_stream_2))
		{
			CloseHandle(original);
			original = INVALID_HANDLE_VALUE;
		}
		return duplicate;
	}
	return INVALID_HANDLE_VALUE;
}

static const HANDLE open_file(const WCHAR *const file_name, const BOOL write_mode)
{
	HANDLE handle = INVALID_HANDLE_VALUE;
	DWORD retry;
	for(retry = 0U; retry < 32U; ++retry)
	{
		if(retry > 0U)
		{
			Sleep(retry); /*delay before retry*/
		}
		if((handle = CreateFileW(file_name, write_mode ? GENERIC_WRITE : GENERIC_READ, write_mode ? 0U: FILE_SHARE_READ, NULL, write_mode ? CREATE_ALWAYS : OPEN_EXISTING, 0, NULL)) == INVALID_HANDLE_VALUE)
		{
			const DWORD error = GetLastError();
			if(((!write_mode) && (error == ERROR_FILE_NOT_FOUND)) || (error == ERROR_PATH_NOT_FOUND) || (error == ERROR_INVALID_NAME))
			{
				break;
			}
		}
		else
		{
			return handle;
		}
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

static int cmdline_required_size(int offset, const bool quoted, const WCHAR *arg)
{
	if(offset > 0)
	{
		++offset;
	}
	if(quoted)
	{
		offset += 2U;
	}
	while(*arg)
	{
		if(*arg >= L' ')
		{
			if(*arg == L'"')
			{
				++offset;
			}
			++offset;
		}
		arg++;
	}
	return offset;
}

static void cmdline_force_append(WCHAR *const cmdline, int offset, const bool quoted, const WCHAR *arg)
{
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
}

static bool append_argument(WCHAR *const cmdline, const WCHAR *const arg)
{
	const bool quoted = contains_space(arg);
	const int offset = lstrlenW(cmdline);
	if(cmdline_required_size(offset, quoted, arg) < MAX_CMDLINE_LEN)
	{
		cmdline_force_append(cmdline, offset, quoted, arg);
		return true;
	}
	return false; /*too long*/
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

static UINT _main(const int argc, const LPWSTR *const argv)
{
	UINT result = 1U;
	DWORD pipe_buffer_size = DEFAULT_PIPE_BUFFER, command_count = 0U;
	WCHAR *command[MAX_PROCESSES], *input_file = NULL, *output_file = NULL;
	HANDLE pipe_rd[MAX_PROCESSES - 1U], pipe_wr[MAX_PROCESSES - 1U];
	HANDLE stream_inp = INVALID_HANDLE_VALUE, stream_out = INVALID_HANDLE_VALUE;
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

	if((argc < 2) || (lstrcmpW(argv[1], L"-h") == 0) || (lstrcmpW(argv[1], L"-?") == 0) || (lstrcmpW(argv[1], L"/?") == 0))
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

	for(int i = 1; i < argc; ++i)
	{
		if(lstrcmpW(argv[i], L"|") == 0)
		{
			if(command_count >= MAX_PROCESSES - 1U)
			{
				print_text_fmt(std_err, "Error: Too many commands specified! (Limit: %ld)\n", MAX_PROCESSES);
				goto clean_up;
			}
			++command_count;
		}
		else if(lstrcmpW(argv[i], L"<") == 0)
		{
			if(ARGV_IS_VALID(i + 1))
			{
				if(input_file && input_file[0U])
				{
					print_text(std_err, "Error: Input file was specified more than once!\n");
					goto clean_up;
				}
				input_file = argv[++i];
			}
			else
			{
				print_text(std_err, "Error: Input file name is missing!\n");
				goto clean_up;
			}
		}
		else if(lstrcmpW(argv[i], L">") == 0)
		{
			if(ARGV_IS_VALID(i + 1))
			{
				if(output_file && output_file[0U])
				{
					print_text(std_err, "Error: Output file was specified more than once!\n");
					goto clean_up;
				}
				output_file = argv[++i];
			}
			else
			{
				print_text(std_err, "Error: Output file name is missing!\n");
				goto clean_up;
			}
		}
		else
		{
			if(!command[command_count])
			{
				if(!(command[command_count] = (WCHAR*) LocalAlloc(LPTR, MAX_CMDLINE_LEN * sizeof(WCHAR))))
				{
					print_text(std_err, "Error: Memory allocation has failed!\n");
					goto clean_up;
				}
				command[command_count][0U] = L'\0';
			}
			if(!append_argument(command[command_count], argv[i]))
			{
				print_text(std_err, "Error: Command-line length exceeds the allowable limit!\n");
				goto clean_up;
			}
		}
	}

	++command_count;

	/* ---------------------------------------------------------------------- */
	/* Validate command-lines                                                 */
	/* ---------------------------------------------------------------------- */

	for(DWORD command_index = 0U; command_index < command_count; ++command_index)
	{
		if((!command[command_index]) || (!command[command_index][0U]))
		{
			print_text_fmt(std_err, "Error: Command #%ld is incomplete!\n", command_index + 1U);
			goto clean_up;
		}
	}

	if(command_count < 2U)
	{
		print_text(std_err, "Error: Must specify *at least* two commands!\n");
		goto clean_up;
	}

	/* ---------------------------------------------------------------------- */
	/* Open input/output files                                                */
	/* ---------------------------------------------------------------------- */

	stream_inp = (input_file) ? open_file(input_file, false) : std_inp;
	if(stream_inp == INVALID_HANDLE_VALUE)
	{
		print_text(std_err, "Error: Failed to open the input file for reading!\n");
		goto clean_up;
	}

	stream_out = (output_file) ? open_file(output_file, true) : std_out;
	if(stream_out == INVALID_HANDLE_VALUE)
	{
		print_text(std_err, "Error: Failed to open the output file for writing!\n");
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
		startup_info[command_index].hStdInput  = create_inheritable_handle(std_inp, std_out, (command_index > 0U) ? pipe_rd[command_index - 1U] : stream_inp);
		startup_info[command_index].hStdOutput = create_inheritable_handle(std_inp, std_out, (command_index < command_count - 1U) ? pipe_wr[command_index] : stream_out);
		
		if((startup_info[command_index].hStdInput == INVALID_HANDLE_VALUE) || (startup_info[command_index].hStdOutput == INVALID_HANDLE_VALUE))
		{
			print_text(std_err, "Error: Failed to create inheritable handle!\n");
			goto clean_up;
		}

		const BOOL success = CreateProcessW(NULL, command[command_index], NULL, NULL, TRUE, CREATE_SUSPENDED, NULL, NULL, &startup_info[command_index], &process_info[command_index]);
		const DWORD error_code = success ? ERROR_SUCCESS : GetLastError();

		CloseHandle(startup_info[command_index].hStdInput);
		startup_info[command_index].hStdInput = NULL;
		CloseHandle(startup_info[command_index].hStdOutput);
		startup_info[command_index].hStdOutput = NULL;

		if(!success)
		{
			process_info[command_index].hProcess = process_info[command_index].hThread = NULL;
			print_text_fmt(std_err, "Error: Failed to create process #%ld! [Error: %ld]\n", command_index + 1U, error_code);
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

	result = 0U;

	for(DWORD command_index = 0U; command_index < command_count; ++command_index)
	{
		DWORD exit_code;
		const HANDLE wait_handles[] =
		{
			process_info[command_index].hProcess, g_stopping
		};
		if(WaitForMultipleObjects(2U, wait_handles, FALSE, INFINITE) != WAIT_OBJECT_0)
		{
			result = 130U;
			goto clean_up;
		}
		if(GetExitCodeProcess(process_info[command_index].hProcess, &exit_code))
		{
			result = max(result, exit_code);
		}
	}

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

	if((stream_inp != INVALID_HANDLE_VALUE) && (stream_inp != std_inp))
	{
		CloseHandle(stream_inp);
	}

	if((stream_out != INVALID_HANDLE_VALUE) && (stream_out != std_out))
	{
		CloseHandle(stream_out);
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
