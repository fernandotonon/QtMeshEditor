#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    DWORD parentPid;
    char installRoot[MAX_PATH];
    char newDir[MAX_PATH];
    char oldDir[MAX_PATH];
    char executablePath[MAX_PATH];
} InstallManifest;

static void trim(char* value)
{
    if (!value) {
        return;
    }
    size_t len = strlen(value);
    while (len > 0 && (value[len - 1] == '\r' || value[len - 1] == '\n' || value[len - 1] == ' ')) {
        value[--len] = '\0';
    }
}

static int parse_manifest(const char* path, InstallManifest* manifest)
{
    FILE* file = fopen(path, "r");
    if (!file) {
        return 0;
    }

    char line[1024];
    while (fgets(line, sizeof(line), file)) {
        char* equals = strchr(line, '=');
        if (!equals) {
            continue;
        }
        *equals = '\0';
        char* key = line;
        char* value = equals + 1;
        trim(value);
        if (strcmp(key, "parent_pid") == 0) {
            manifest->parentPid = (DWORD)strtoul(value, NULL, 10);
        } else if (strcmp(key, "install_root") == 0) {
            strncpy(manifest->installRoot, value, sizeof(manifest->installRoot) - 1);
        } else if (strcmp(key, "new_dir") == 0) {
            strncpy(manifest->newDir, value, sizeof(manifest->newDir) - 1);
        } else if (strcmp(key, "old_dir") == 0) {
            strncpy(manifest->oldDir, value, sizeof(manifest->oldDir) - 1);
        } else if (strcmp(key, "executable_path") == 0) {
            strncpy(manifest->executablePath, value, sizeof(manifest->executablePath) - 1);
        }
    }

    fclose(file);
    return manifest->installRoot[0] != '\0' && manifest->newDir[0] != '\0'
        && manifest->executablePath[0] != '\0';
}

static int remove_directory_tree(const char* path)
{
    char command[MAX_PATH * 2 + 32];
    _snprintf(command, sizeof(command), "cmd /C rmdir /S /Q \"%s\"", path);
    return system(command) == 0;
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        return 1;
    }

    InstallManifest manifest;
    ZeroMemory(&manifest, sizeof(manifest));
    if (!parse_manifest(argv[1], &manifest)) {
        return 2;
    }

    if (manifest.parentPid != 0) {
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, manifest.parentPid);
        if (process) {
            WaitForSingleObject(process, INFINITE);
            CloseHandle(process);
        }
    }

    if (manifest.oldDir[0] != '\0') {
        remove_directory_tree(manifest.oldDir);
    }

    if (!MoveFileExA(manifest.installRoot, manifest.oldDir, MOVEFILE_WRITE_THROUGH)) {
        return 3;
    }
    if (!MoveFileExA(manifest.newDir, manifest.installRoot, MOVEFILE_WRITE_THROUGH)) {
        MoveFileExA(manifest.oldDir, manifest.installRoot, MOVEFILE_WRITE_THROUGH);
        return 4;
    }

    STARTUPINFOA startupInfo;
    PROCESS_INFORMATION processInfo;
    ZeroMemory(&startupInfo, sizeof(startupInfo));
    ZeroMemory(&processInfo, sizeof(processInfo));
    startupInfo.cb = sizeof(startupInfo);

    if (!CreateProcessA(manifest.executablePath,
                        NULL,
                        NULL,
                        NULL,
                        FALSE,
                        0,
                        NULL,
                        manifest.installRoot,
                        &startupInfo,
                        &processInfo)) {
        MoveFileExA(manifest.oldDir, manifest.installRoot, MOVEFILE_WRITE_THROUGH);
        return 5;
    }

    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    remove_directory_tree(manifest.oldDir);
    return 0;
}
