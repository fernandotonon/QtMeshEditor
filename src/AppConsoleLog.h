#ifndef APPCONSOLELOG_H
#define APPCONSOLELOG_H

class MainWindow;

namespace AppConsoleLog {

/// Install global Qt message handler (call once after QApplication exists).
/// Buffers lines until a MainWindow attaches.
void install();

/// Redirect process stdout/stderr into the same in-app console (GUI builds only).
/// Returns false if the pipe could not be created (app continues with terminal I/O).
bool installStdioCapture();

/// Restore stdout/stderr and stop the reader thread (call before exiting GUI main()).
void shutdown();

/// Drains captured stdio chunks on the GUI thread (internal; queued from reader thread).
void flushStdioChunks();

/// Route log lines to this window's console and flush any buffered startup lines.
void attachMainWindow(MainWindow* window);

/// Stop routing to `window` (e.g. MainWindow destructor).
void detachMainWindow(MainWindow* window);

} // namespace AppConsoleLog

#endif
