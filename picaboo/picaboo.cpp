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

#include <Windows.h>
#include <tlhelp32.h>
#include <Psapi.h>
#include <stdio.h>
#include <shlwapi.h>
#include <string.h>
#include <direct.h>

#pragma comment(lib, "Shlwapi.lib")

struct LibInitParams 
{
	char targetFile[FILENAME_MAX];
	char runFlag[10];
	char dumpDir[FILENAME_MAX];
} initParams;

const char* appName = "picaboo";

typedef VOID(*TARGETPROC)();
TARGETPROC targetProcAdd;

typedef BOOL(*INITIALIZE)(LibInitParams*);
INITIALIZE initProcAdd;

// Load our 'hook' libraries for our target DLL.
#ifdef _WIN64
	HINSTANCE hinstLib = LoadLibraryA("picaboo64.dll");
#else
	HINSTANCE hinstLib = LoadLibraryA("picaboo32.dll");
#endif

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
	printf("Usage: picaboo [RUN FLAG] [TARGET DLL/EXE] [TARGET PARAMETERS]\n");
	printf("[RUN FLAG] : [break|pass]\n");
	printf("\tbreak - Exit on first direct call into allocated memory address space.\n");
	printf("\tpass - Continue execution of target.\n");
	printf("[TARGET] : Path to the target file.\n");
	printf("[TARGET PARAMETERS] : Runtime parameters of the target. Context varies depending on the file type.\n");
	printf("\tdll  - Export entry of target DLL.\n");
	printf("\texe  - Runtime parameters to use for EXE.\n");
	ExitProcess(0);
}

bool getMemDumpDir()
{
	if (!_getcwd(initParams.dumpDir, sizeof(initParams.dumpDir))) {
		printf("Failed to get current directory. Error 0x%.8X\n", GetLastError());
		return false;
	}
	lstrcatA(initParams.dumpDir, "\\memdumps\\");
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

	BOOL isWow64;
	IsWow64Process(GetCurrentProcess(), &isWow64);
	if (pNTHeader->FileHeader.Machine == IMAGE_FILE_MACHINE_I386) {
		if (!isWow64) {
			printf("Target is incompatible with selected picaboo EXE. Use the 32bit version.");
			ExitProcess(0);
		}
	}
	if (pNTHeader->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64) {
		if (isWow64) {
			printf("Target is incompatible with selected picaboo EXE. Use the 64bit version.");
			ExitProcess(0);
		}
	}

	if (pNTHeader->FileHeader.Characteristics & IMAGE_FILE_DLL) {
		return "dll";
	}
	if (pNTHeader->FileHeader.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE) {
		return "exe";
	}
	else {
		return "unknown";
	}
}

void loadDLL(const char* exportName)
{
	initProcAdd = (INITIALIZE)GetProcAddress(hinstLib, "Initialize");
	if (initProcAdd == NULL) {
		printf("[*] Failed to initialize hook library.\n");
		ExitProcess(0);
	}
	if (!initProcAdd(&initParams)) {
		FreeLibrary(hinstLib);
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
	lstrcpyA(t_targetFile, initParams.targetFile);
	strcat_s(t_targetFile, FILENAME_MAX, ".");

	printf("Loading %s with target %s...\n", initParams.targetFile, exportName);
	HINSTANCE targetLib = LoadLibraryA(t_targetFile);

	if (targetLib != NULL) {
		targetProcAdd = (TARGETPROC)GetProcAddress(targetLib, exportName);
		if (targetProcAdd != NULL) {
			printf("Successfully loaded target at 0x%p...\n", targetProcAdd);
			targetProcAdd();
		}
		else {
			printf("Failed to load target function: %s. Error 0x%.8X\n", exportName, GetLastError());
		}
		FreeLibrary(targetLib);
	}
	else {
		printf("Failed to load library: %s. Error 0x%.8X\n", initParams.targetFile, GetLastError());
	}
	FreeLibrary(hinstLib);
}

void loadEXE(char* peArguments)
{
	const char* libName;
	char runString[4096];
	STARTUPINFOA startupInfo = { sizeof(startupInfo) };
	PROCESS_INFORMATION processInfo;
	LPVOID addr;
	DWORD ret;
	HANDLE th;

	StrCpyA(runString, initParams.targetFile);

	if (peArguments != NULL) {
		strcat_s(runString, sizeof(runString), " ");
		strcat_s(runString, sizeof(runString), peArguments);
	}

	printf("Excuting run command: %s\n", runString);
	if (CreateProcessA(NULL, runString, NULL, NULL, TRUE, CREATE_SUSPENDED, NULL, NULL, &startupInfo, &processInfo)) {

		BOOL isChildWow64;
		BOOL isParentWow64;
		IsWow64Process(processInfo.hProcess, &isChildWow64);
		IsWow64Process(GetCurrentProcess(), &isParentWow64);
		if (!isChildWow64) {
			libName = "picaboo64.dll";
		}
		else {
			libName = "picaboo32.dll";
		}

		// Load hook lib into the new process...
		LPVOID loadLibraryFcn = GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
		SIZE_T libNameLen = strlen(libName) + 1;
		addr = VirtualAllocEx(processInfo.hProcess, NULL, libNameLen, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

		if (!WriteProcessMemory(processInfo.hProcess, addr, libName, libNameLen, NULL)) {
			printf("Failed to write inject DLL name into new process. Error 0x%.8X\n", GetLastError());
			TerminateProcess(processInfo.hProcess, 0);
			ExitProcess(0);
		}

		/*
		64-bit versions of Windows use 32-bit handles for interoperability.
		When sharing a handle between 32-bit and 64-bit applications, only the lower 32 bits are significant,
		so it is safe to truncate the handle (when passing it from 64-bit to 32-bit)
		or sign-extend the handle (when passing it from 32-bit to 64-bit).
		https://docs.microsoft.com/en-us/windows/win32/winprog64/interprocess-communication
		*/

		// Load the hook library
		th = CreateRemoteThread(processInfo.hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)loadLibraryFcn, addr, 0, NULL);
		WaitForSingleObject(th, INFINITE);
		ret = 0;
		GetExitCodeThread(th, &ret);
		CloseHandle(th);
		printf("Injected %s into %s...\n", libName, initParams.targetFile);

		// Initialize the hook library with our parameters...
		LPVOID initializeFcn = GetProcAddress(GetModuleHandleA(libName), "Initialize");
		SIZE_T initParamLen = sizeof(LibInitParams) + 1;
		addr = VirtualAllocEx(processInfo.hProcess, NULL, initParamLen, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
		if (!WriteProcessMemory(processInfo.hProcess, addr, &initParams, initParamLen, NULL)) {
			printf("Failed to write initialization parameters into new process. Error 0x%.8X\n", GetLastError());
			TerminateProcess(processInfo.hProcess, 0);
			ExitProcess(0);
		}
		
		// Initialize library
		th = CreateRemoteThread(processInfo.hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)initializeFcn, addr, 0, NULL);
		WaitForSingleObject(th, INFINITE);
		ret = 0;
		GetExitCodeThread(th, &ret);
		CloseHandle(th);
		printf("Initialized %s!\n", libName);

		// Now resume that the hooked library has been initialized within the context of the process...
		ResumeThread(processInfo.hThread);
		WaitForSingleObject(processInfo.hProcess, INFINITE);
		CloseHandle(processInfo.hProcess);
		CloseHandle(processInfo.hThread);
	}
	else {
		printf("Execution failed! Error 0x%.8X\n", GetLastError());
	}
}

void main(int argc, CHAR *argv[])
{
	DEP_SYSTEM_POLICY_TYPE policy = GetSystemDEPPolicy();
	if (policy != DEPPolicyAlwaysOn && policy != DEPPolicyOptOut) {
		printf("[*] You must enforce an 'ALWAYS ON' or 'OPT OUT' policy DEP policy to use this program. Please adjust your settings and reboot.\n");
		ExitProcess(0);
	}

	// Simple check against downstream execution inadvertently 
	// spawning another picaboo process and dumping the help contents.
	if (checkParentProc()) {
		ExitProcess(0);
	}

	if (argc != 4 && argc != 3) {
		printf("[*] Invalid number of arguments.\n");
		printHelp();
	}

	lstrcpyA(initParams.runFlag, argv[1]);
	if (_stricmp(initParams.runFlag, "break") != 0 && _stricmp(initParams.runFlag, "pass") != 0) {
		printf("[*] Please specify either break or pass for the [RUN FLAG].\n");
		printHelp();
	}

	lstrcpyA(initParams.targetFile, argv[2]);
	if (!PathFileExistsA(initParams.targetFile)) {
		printf("[*] Failed to find target file %s! Make sure it exists.\n", initParams.targetFile);
		ExitProcess(0);
	}

	if (!getMemDumpDir()) {
		ExitProcess(0);
	}

	const char* peType = getPeType(initParams.targetFile);
	if (_stricmp(peType, "dll") != 0 && _stricmp(peType, "exe") != 0) {
		printf("[*] File %s was not found to be a valid DLL/EXE.\n", initParams.targetFile);
		ExitProcess(0);
	}

	if (hinstLib == NULL) {
		printf("[*] Failed to load hook DLL \'picaboo32.dll\' or \'picaboo64.dll\'.");
		printf("[*] Make sure it is in the same directory as the main program.\n");
		ExitProcess(0);
	}

	if (_stricmp(peType, "dll") == 0 && argc == 4) {
		loadDLL(argv[3]);
	}

	if (_stricmp(peType, "exe") == 0) {
		if (argc == 4) {
			loadEXE(argv[3]);
		}
		else {
			loadEXE(NULL);
		}
		
	}
}
