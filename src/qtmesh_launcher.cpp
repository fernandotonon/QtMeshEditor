// Tiny launcher that re-executes QtMeshEditor.exe in CLI mode.
// On Windows, symlinks require elevated privileges, so we use a
// separate .exe that WinGet can register as a portable command.
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <unistd.h>
#include <libgen.h>
#endif

int main(int argc, char* argv[])
{
#ifdef _WIN32
    // Build path to QtMeshEditor.exe next to this binary
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string dir(exePath);
    auto pos = dir.find_last_of("\\/");
    if (pos != std::string::npos)
        dir = dir.substr(0, pos);
    std::string target = dir + "\\QtMeshEditor.exe";

    // Build argument string: --cli + original args (skip argv[0])
    std::string args = "\"" + target + "\" --cli";
    for (int i = 1; i < argc; ++i) {
        args += " ";
        // Quote args that contain spaces
        std::string arg(argv[i]);
        if (arg.find(' ') != std::string::npos)
            args += "\"" + arg + "\"";
        else
            args += arg;
    }

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
    si.dwFlags = STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(target.c_str(), const_cast<char*>(args.c_str()),
                        nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        fprintf(stderr, "Failed to launch QtMeshEditor.exe (error %lu)\n", GetLastError());
        return 1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(exitCode);
#else
    // On Unix, just exec the main binary with --cli prepended
    char selfPath[4096];
    ssize_t len = readlink("/proc/self/exe", selfPath, sizeof(selfPath) - 1);
    if (len <= 0) {
        // macOS fallback — this launcher is only needed on Windows,
        // but handle gracefully
        fprintf(stderr, "qtmesh launcher: cannot resolve own path\n");
        return 1;
    }
    selfPath[len] = '\0';
    std::string dir(selfPath);
    auto pos = dir.find_last_of('/');
    if (pos != std::string::npos)
        dir = dir.substr(0, pos);
    std::string target = dir + "/QtMeshEditor";

    // Build new argv: target --cli [original args...]
    char** newArgv = new char*[argc + 2];
    newArgv[0] = const_cast<char*>(target.c_str());
    const char* cliFlag = "--cli";
    newArgv[1] = const_cast<char*>(cliFlag);
    for (int i = 1; i < argc; ++i)
        newArgv[i + 1] = argv[i];
    newArgv[argc + 1] = nullptr;

    execv(target.c_str(), newArgv);
    // execv only returns on error
    perror("qtmesh launcher: execv failed");
    delete[] newArgv;
    return 1;
#endif
}
