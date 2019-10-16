/*
Copyright(c) 2019 The MITRE Corporation.All rights reserved.
MITRE Proprietary - Internal Use Only
TLP:Red and NDA Restrictions may apply.
For redistribution, specific permission is needed.
       Contact: infosec@mitre.org

THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED.IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
LIABILITY, OR TORT(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
SUCH DAMAGE.
*/

/*
picaboo

Usage: picaboo [RUN FLAG] [TARGET DLL] [EXPORT FUNCTION]
*/

#include <Windows.h>
#include <tlhelp32.h>
#include <Psapi.h>
#include <stdio.h>
#include <shlwapi.h>
#include <string.h>
#include <direct.h>

#pragma comment(lib, "Shlwapi.lib")

#define CALL_FIRST 1 
#define PAGE_EXECUTE_BACKDOOR 0x51

typedef VOID(*TARGETPROC)();
typedef BOOL(*EXITPROC)(UINT);

TARGETPROC targetProcAdd;
EXITPROC exitProcAdd;

const char* targetFile;
const char* runFlag;
const char* appName = "picaboo";

char dumpDir[FILENAME_MAX];

LONG WINAPI VectoredHandler(struct _EXCEPTION_POINTERS *ExceptionInfo)
{
	UNREFERENCED_PARAMETER(ExceptionInfo);
	PCONTEXT context = ExceptionInfo->ContextRecord;
	DWORD errorCode = 0;
	

	#ifdef _WIN64
	DWORD64 instructionPointer = 0;
	instructionPointer = context->Rip++;
	#else
	DWORD instructionPointer = 0;
	instructionPointer = context->Eip++;
	#endif 

	errorCode = ExceptionInfo->ExceptionRecord->ExceptionCode;
	printf("-----------------------------\n");
	printf("Exception Offset: 0x%p\n", instructionPointer);
	printf("Error Code: 0x%.8X\n", errorCode);

	if (errorCode == EXCEPTION_ACCESS_VIOLATION) {
		char fileName[FILENAME_MAX];
		char fullDumpPath[1024];
		MEMORY_BASIC_INFORMATION memInfo;
		SIZE_T regionSize = 0;
		DWORD dwWritten = 0;
		DWORD lpflOldProtect = 0;
		PVOID regionBase = 0;
		HANDLE hFile;
		
		char* targetFileName = PathFindFileNameA(targetFile);
		PathRemoveExtensionA(targetFileName);

		VirtualQuery(reinterpret_cast<PVOID>(instructionPointer), &memInfo, sizeof(memInfo));
		regionSize = memInfo.RegionSize;
		lpflOldProtect = memInfo.Protect;
		regionBase = memInfo.BaseAddress;

		sprintf_s(fileName, sizeof(fileName), "dump_%s_0x%p_ep_0x%X.bin", targetFileName, regionBase, (instructionPointer - (DWORD)regionBase));
		printf("Writing %d bytes from 0x%p to %s...\n", regionSize, regionBase, fileName);

		
		StrCpyA(fullDumpPath, dumpDir);
		lstrcatA(fullDumpPath, fileName);

		hFile = CreateFileA(fullDumpPath, GENERIC_WRITE, NULL, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		WriteFile(hFile, regionBase, regionSize, &dwWritten, NULL);
		CloseHandle(hFile);
		
		if (_stricmp(runFlag, "break") == 0) {
			exitProcAdd = (EXITPROC)GetProcAddress(GetModuleHandleA("kernel32.dll"), "ExitProcess");

			#ifdef _WIN64
			ExceptionInfo->ContextRecord->Rip = exitProcAdd(0);
			#else
			ExceptionInfo->ContextRecord->Eip = exitProcAdd(0);
			#endif 
		}
		else if(_stricmp(runFlag, "pass") == 0)	{
			printf("Pass through on region 0x%p for instruction pointer 0x%p\n", regionBase, instructionPointer);

			if (VirtualProtect(regionBase, regionSize, PAGE_EXECUTE_BACKDOOR, &lpflOldProtect))	{	
				VirtualQuery(reinterpret_cast<PVOID>(instructionPointer), &memInfo, sizeof(memInfo));
				//printf("Base Addr: 0x%p\n", memInfo.BaseAddress);
				//printf("AllocBase Addr: 0x%p\n", memInfo.AllocationBase);
				//printf("AllocPerms: 0x%X\n", memInfo.AllocationProtect);
				//printf("Mem Perms: 0x%X\n", memInfo.Protect);
				//printf("Mem State: 0x%X\n", memInfo.State);
				//printf("Region Size: 0x%X\n", memInfo.RegionSize);


				printf("Backdoor PAGE_EXECUTE_READWRITE success! Passing control back to 0x%p\n", instructionPointer);

				#ifdef _WIN64
				ExceptionInfo->ContextRecord->Rip = instructionPointer;
				#else
				ExceptionInfo->ContextRecord->Eip = instructionPointer;
				#endif 
			}
			else {
				printf("Backdoor PAGE_EXEC failure! Error 0x%.8X\n", GetLastError());
			}
		}
		printf("-----------------------------\n");
		return EXCEPTION_CONTINUE_EXECUTION;
	}
	printf("-----------------------------\n");
	return EXCEPTION_CONTINUE_SEARCH;
}

bool checkParentProc()
{
	char parentName[FILENAME_MAX];
	DWORD lpdwSize = FILENAME_MAX;
	DWORD pid = 0;
	DWORD crtPid = GetCurrentProcessId();
	PROCESSENTRY32 pe;
	pe.dwSize = sizeof(PROCESSENTRY32);
	HANDLE hSnapShot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	BOOL bContinue = Process32First(hSnapShot, &pe);

	while (bContinue) {
		if (crtPid == pe.th32ProcessID) {
			pid = pe.th32ParentProcessID;
		}
		pe.dwSize = sizeof(PROCESSENTRY32);
		bContinue = !pid && Process32Next(hSnapShot, &pe);
	}

	HANDLE hProcess = OpenProcess(
		SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
		FALSE, pe.th32ParentProcessID);

	if (QueryFullProcessImageNameA(hProcess, 0, parentName, &lpdwSize)) {
		char* parentExe = PathFindFileNameA(parentName);
		
		if (_strnicmp(parentExe, appName, strlen(appName)) == 0) {
			return true;
		}
	}
	else {
		printf("Failed to get parent process. Error 0x%.8X\n", GetLastError());
	}
	return false;
}

void printHelp()
{
	printf("Usage: picaboo [RUN FLAG] [TARGET DLL] [DLL EXPORT FUNCTION]\n");
	printf("[RUN FLAG] : [break|pass]\n");
	printf("\tbreak - Exit on first direct call into allocated memory address space.\n");
	printf("\tpass - Continue execution of target.\n");
	printf("[TARGET] : Path to the target file.\n");
	printf("[EXPORT FUNCTION] : What export function should be called?\n");
	ExitProcess(0);
}

bool createMemDumpDir()
{
	if (!_getcwd(dumpDir, sizeof(dumpDir))) {
		printf("Failed to get current directory. Error 0x%.8X\n", GetLastError());
		return false;
	}

	lstrcatA(dumpDir, "\\memdumps\\");

	if (!CreateDirectoryA(dumpDir, NULL)) {
		if (GetLastError() != ERROR_ALREADY_EXISTS) {
			printf("Failed to create directory %s. Error 0x%.8X\n", dumpDir, GetLastError());
			return false;
		}
	}
	else {
		printf("Created directory %s\n", dumpDir);
	}

	return true;
}

const char* getPeType(const char* fileName)
{
	HANDLE hFile = CreateFileA(fileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
	HANDLE hFileMapping = CreateFileMapping(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
	LPVOID lpFileBase = MapViewOfFile(hFileMapping, FILE_MAP_READ, 0, 0, 0);

	DWORD fileSize = GetFileSize(hFile, NULL);
	if (sizeof(PIMAGE_DOS_HEADER) >= fileSize) {
		return "unknown";
	}

	PIMAGE_DOS_HEADER pDosHeader = (PIMAGE_DOS_HEADER)lpFileBase;
	if (sizeof(PIMAGE_DOS_HEADER) + pDosHeader->e_lfanew + sizeof(PIMAGE_NT_HEADERS) >= fileSize) {
		return "unknown";
	}

	PIMAGE_NT_HEADERS pNTHeader = (PIMAGE_NT_HEADERS)((DWORD)pDosHeader + (DWORD)pDosHeader->e_lfanew);
	if ((pNTHeader->FileHeader.Characteristics & IMAGE_FILE_DLL)) {
		return "dll";
	}
	if ((pNTHeader->FileHeader.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE)) {
		return "exe";
	}
	else {
		return "unknown";
	}
}

void loadDLL(const char* exportName)
{
	// Load our 'hook' libraries for our target DLL.
	#ifdef _WIN64
	HINSTANCE hinstLib = LoadLibraryA("detour64.dll");
	#else
	HINSTANCE hinstLib = LoadLibraryA("detour32.dll");
	#endif

	if (hinstLib == NULL) {
		printf("[*] Failed to load hook DLL \'detour32.dll\' or \'detour64.dll\' . Make sure it is in the same directory as the main program.\n");
		ExitProcess(0);
	}

	/*
	If the string specifies a module name without a path and the file name extension is omitted,
	the function appends the default library extension .dll to the module name.
	To prevent the function from appending .dll to the module name,
	include a trailing point character (.) in the module name string.
	https://msdn.microsoft.com/en-us/library/windows/desktop/ms684175.aspx
	*/
	char t_targetFile[FILENAME_MAX];
	lstrcpyA(t_targetFile, targetFile);
	strcat_s(t_targetFile, FILENAME_MAX, ".");

	printf("Loading %s with target %s...\n", targetFile, exportName);
	HINSTANCE targetLib = LoadLibraryA(t_targetFile);

	if (targetLib != NULL) {
		targetProcAdd = (TARGETPROC)GetProcAddress(targetLib, exportName);
		if (NULL != targetProcAdd) {
			printf("Successfully loaded target at 0x%p...\n", targetProcAdd);
			PVOID h = AddVectoredExceptionHandler(CALL_FIRST, VectoredHandler);
			targetProcAdd();
			RemoveVectoredExceptionHandler(h);
		}
		else {
			printf("Failed to load target function: %s. Error 0x%.8X\n", exportName, GetLastError());
		}
		FreeLibrary(targetLib);
	}
	else {
		printf("Failed to load library: %s. Error 0x%.8X\n", targetFile, GetLastError());
	}
}

void main(int argc, CHAR *argv[])
{
	// Simple check against downstream execution inadvertently 
	// spawning another picaboo process and dumping the help contents.
	if (checkParentProc()) {
		ExitProcess(0);
	}

	if (argc != 4) {
		printf("[*] Invalid number of arguments.\n");
		printHelp();
	}

	runFlag = argv[1];
	if (_stricmp(runFlag, "break") != 0 && _stricmp(runFlag, "pass") != 0) {
		printf("[*] Please specify either break or pass for the [RUN FLAG].\n");
		printHelp();
	}

	targetFile = argv[2];
	if (!PathFileExistsA(targetFile)) {
		printf("[*] Failed to find target file %s! Make sure it exists.\n", targetFile);
		ExitProcess(0);
	}

	if (!createMemDumpDir()) {
		ExitProcess(0);
	}

	const char* peType = getPeType(targetFile);

	if (_stricmp(peType, "dll") != 0) {
		printf("[*] File %s was not found to be a valid DLL.\n", targetFile);
		ExitProcess(0);
	}

	if (_stricmp(peType, "dll") == 0 && argc == 4) {
		loadDLL(argv[3]);
	}
}
