#include "env.h"
#include "envmetrics.h"
#include "envmodule.h"
#include "envsecurity.h"
#include "envshortcut.h"
#include "envwindows.h"
#include <log.h>
#include <utility.h>

namespace env
{

using namespace MOBase;

Console::Console()
  : m_hasConsole(false), m_in(nullptr), m_out(nullptr), m_err(nullptr)
{
  // open a console
  if (!AllocConsole()) {
    // failed, ignore
  }

  m_hasConsole = true;

  // redirect stdin, stdout and stderr to it
  freopen_s(&m_in, "CONIN$", "r", stdin);
  freopen_s(&m_out, "CONOUT$", "w", stdout);
  freopen_s(&m_err, "CONOUT$", "w", stderr);
}

Console::~Console()
{
  // close redirected handles and redirect standard stream to NUL in case
  // they're used after this

  if (m_err) {
    std::fclose(m_err);
    freopen_s(&m_err, "NUL", "w", stderr);
  }

  if (m_out) {
    std::fclose(m_out);
    freopen_s(&m_out, "NUL", "w", stdout);
  }

  if (m_in) {
    std::fclose(m_in);
    freopen_s(&m_in, "NUL", "r", stdin);
  }

  // close console
  if (m_hasConsole) {
    FreeConsole();
  }
}


Environment::Environment()
  : m_windows(new WindowsInfo), m_metrics(new Metrics)
{
  m_modules = getLoadedModules();
  m_security = getSecurityProducts();
}

// anchor
Environment::~Environment() = default;

const std::vector<Module>& Environment::loadedModules() const
{
  return m_modules;
}

const WindowsInfo& Environment::windowsInfo() const
{
  return *m_windows;
}

const std::vector<SecurityProduct>& Environment::securityProducts() const
{
  return m_security;
}

const Metrics& Environment::metrics() const
{
  return *m_metrics;
}

void Environment::dump() const
{
  log::debug("windows: {}", m_windows->toString());

  if (m_windows->compatibilityMode()) {
    log::warn("MO seems to be running in compatibility mode");
  }

  log::debug("security products:");
  for (const auto& sp : m_security) {
    log::debug("  . {}", sp.toString());
  }

  log::debug("modules loaded in process:");
  for (const auto& m : m_modules) {
    log::debug(" . {}", m.toString());
  }

  log::debug("displays:");
  for (const auto& d : m_metrics->displays()) {
    log::debug(" . {}", d.toString());
  }
}


struct Process
{
  std::wstring filename;
  DWORD pid;

  Process(std::wstring f, DWORD id)
    : filename(std::move(f)), pid(id)
  {
  }
};


// returns the filename of the given process or the current one
//
std::wstring processFilename(HANDLE process=INVALID_HANDLE_VALUE)
{
  // double the buffer size 10 times
  const int MaxTries = 10;

  DWORD bufferSize = MAX_PATH;

  for (int tries=0; tries<MaxTries; ++tries)
  {
    auto buffer = std::make_unique<wchar_t[]>(bufferSize + 1);
    std::fill(buffer.get(), buffer.get() + bufferSize + 1, 0);

    DWORD writtenSize = 0;

    if (process == INVALID_HANDLE_VALUE) {
      // query this process
      writtenSize = GetModuleFileNameW(0, buffer.get(), bufferSize);
    } else {
      // query another process
      writtenSize = GetModuleBaseNameW(process, 0, buffer.get(), bufferSize);
    }

    if (writtenSize == 0) {
      // hard failure
      const auto e = GetLastError();
      std::wcerr << formatSystemMessage(e) << L"\n";
      break;
    } else if (writtenSize >= bufferSize) {
      // buffer is too small, try again
      bufferSize *= 2;
    } else {
      // if GetModuleFileName() works, `writtenSize` does not include the null
      // terminator
      const std::wstring s(buffer.get(), writtenSize);
      const std::filesystem::path path(s);

      return path.filename().native();
    }
  }

  // something failed or the path is way too long to make sense

  std::wstring what;
  if (process == INVALID_HANDLE_VALUE) {
    what = L"the current process";
  } else {
    what = L"pid " + std::to_wstring(reinterpret_cast<std::uintptr_t>(process));
  }

  std::wcerr << L"failed to get filename for " << what << L"\n";
  return {};
}

std::vector<DWORD> runningProcessesIds()
{
  // double the buffer size 10 times
  const int MaxTries = 10;

  // initial size of 300 processes, unlikely to be more than that
  std::size_t size = 300;

  for (int tries=0; tries<MaxTries; ++tries) {
    auto ids = std::make_unique<DWORD[]>(size);
    std::fill(ids.get(), ids.get() + size, 0);

    DWORD bytesGiven = static_cast<DWORD>(size * sizeof(ids[0]));
    DWORD bytesWritten = 0;

    if (!EnumProcesses(ids.get(), bytesGiven, &bytesWritten))
    {
      const auto e = GetLastError();

      std::wcerr
        << L"failed to enumerate processes, "
        << formatSystemMessage(e) << L"\n";

      return {};
    }

    if (bytesWritten == bytesGiven) {
      // no way to distinguish between an exact fit and not enough space,
      // just try again
      size *= 2;
      continue;
    }

    const auto count = bytesWritten / sizeof(ids[0]);
    return std::vector<DWORD>(ids.get(), ids.get() + count);
  }

  std::cerr << L"too many processes to enumerate";
  return {};
}

std::vector<Process> runningProcesses()
{
  const auto pids = runningProcessesIds();
  std::vector<Process> v;

  for (const auto& pid : pids) {
    if (pid == 0) {
      // the idle process has pid 0 and seems to be picked up by EnumProcesses()
      continue;
    }

    HandlePtr h(OpenProcess(
      PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid));

    if (!h) {
      const auto e = GetLastError();

      if (e != ERROR_ACCESS_DENIED) {
        // don't log access denied, will happen a lot for system processes, even
        // when elevated
        std::wcerr
          << L"failed to open process " << pid << L", "
          << formatSystemMessage(e) << L"\n";
      }

      continue;
    }

    auto filename = processFilename(h.get());
    if (!filename.empty()) {
      v.emplace_back(std::move(filename), pid);
    }
  }

  return v;
}

DWORD findOtherPid()
{
  const std::wstring defaultName = L"ModOrganizer.exe";

  std::wclog << L"looking for the other process...\n";

  // used to skip the current process below
  const auto thisPid = GetCurrentProcessId();
  std::wclog << L"this process id is " << thisPid << L"\n";

  // getting the filename for this process, assumes the other process has the
  // smae one
  auto filename = processFilename();
  if (filename.empty()) {
    std::wcerr
      << L"can't get current process filename, defaulting to "
      << defaultName << L"\n";

    filename = defaultName;
  } else {
    std::wclog << L"this process filename is " << filename << L"\n";
  }

  // getting all running processes
  const auto processes = runningProcesses();
  std::wclog << L"there are " << processes.size() << L" processes running\n";

  // going through processes, trying to find one with the same name and a
  // different pid than this process has
  for (const auto& p : processes) {
    if (p.filename == filename) {
      if (p.pid != thisPid) {
        return p.pid;
      }
    }
  }

  std::wclog
    << L"no process with this filename\n"
    << L"MO may not be running, or it may be running as administrator\n"
    << L"you can try running this again as administrator\n";

  return 0;
}

std::wstring tempDir()
{
  const DWORD bufferSize = MAX_PATH + 1;
  wchar_t buffer[bufferSize + 1] = {};

  const auto written = GetTempPathW(bufferSize, buffer);
  if (written == 0) {
    const auto e = GetLastError();

    std::wcerr
      << L"failed to get temp path, " << formatSystemMessage(e) << L"\n";

    return {};
  }

  // `written` does not include the null terminator
  return std::wstring(buffer, buffer + written);
}

HandlePtr tempFile(const std::wstring dir)
{
  // maximum tries of incrementing the counter
  const int MaxTries = 100;

  // UTC time and date will be in the filename
  const auto now = std::time(0);
  const auto tm = std::gmtime(&now);

  // "ModOrganizer-YYYYMMDDThhmmss.dmp", with a possible "-i" appended, where
  // i can go until MaxTries
  std::wostringstream oss;
  oss
    << L"ModOrganizer-"
    << std::setw(4) << (1900 + tm->tm_year)
    << std::setw(2) << std::setfill(L'0') << (tm->tm_mon + 1)
    << std::setw(2) << std::setfill(L'0') << tm->tm_mday << "T"
    << std::setw(2) << std::setfill(L'0') << tm->tm_hour
    << std::setw(2) << std::setfill(L'0') << tm->tm_min
    << std::setw(2) << std::setfill(L'0') << tm->tm_sec;

  const std::wstring prefix = oss.str();
  const std::wstring ext = L".dmp";

  // first path to try, without counter in it
  std::wstring path = dir + L"\\" + prefix + ext;

  for (int i=0; i<MaxTries; ++i) {
    std::wclog << L"trying file '" << path << L"'\n";

    HandlePtr h (CreateFileW(
      path.c_str(), GENERIC_WRITE, 0, nullptr,
      CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));

    if (h.get() != INVALID_HANDLE_VALUE) {
      // worked
      return h;
    }

    const auto e = GetLastError();

    if (e != ERROR_FILE_EXISTS) {
      // probably no write access
      std::wcerr
        << L"failed to create dump file, " << formatSystemMessage(e) << L"\n";

      return {};
    }

    // try again with "-i"
    path = dir + L"\\" + prefix + L"-" + std::to_wstring(i + 1) + ext;
  }

  std::wcerr << L"can't create dump file, ran out of filenames\n";
  return {};
}

HandlePtr dumpFile()
{
  // try the current directory
  HandlePtr h = tempFile(L".");
  if (h.get() != INVALID_HANDLE_VALUE) {
    return h;
  }

  std::wclog << L"cannot write dump file in current directory\n";

  // try the temp directory
  const auto dir = tempDir();

  if (!dir.empty()) {
    h = tempFile(dir.c_str());
    if (h.get() != INVALID_HANDLE_VALUE) {
      return h;
    }
  }

  return {};
}

bool createMiniDump(HANDLE process, CoreDumpTypes type)
{
  const DWORD pid = GetProcessId(process);

  const HandlePtr file = dumpFile();
  if (!file) {
    std::wcerr << L"nowhere to write the dump file\n";
    return false;
  }

  auto flags = _MINIDUMP_TYPE(
    MiniDumpNormal |
    MiniDumpWithHandleData |
    MiniDumpWithUnloadedModules |
    MiniDumpWithProcessThreadData);

  if (type == CoreDumpTypes::Data) {
    std::wclog << L"writing minidump with data\n";
    flags = _MINIDUMP_TYPE(flags | MiniDumpWithDataSegs);
  } else if (type ==  CoreDumpTypes::Full) {
    std::wclog << L"writing full minidump\n";
    flags = _MINIDUMP_TYPE(flags | MiniDumpWithFullMemory);
  } else {
    std::wclog << L"writing mini minidump\n";
  }

  const auto ret = MiniDumpWriteDump(
    process, pid, file.get(), flags, nullptr, nullptr, nullptr);

  if (!ret) {
    const auto e = GetLastError();

    std::wcerr
      << L"failed to write mini dump, " << formatSystemMessage(e) << L"\n";

    return false;
  }

  std::wclog << L"minidump written correctly\n";
  return true;
}


bool coredump(CoreDumpTypes type)
{
  std::wclog << L"creating minidump for the current process\n";
  return createMiniDump(GetCurrentProcess(), type);
}

bool coredumpOther(CoreDumpTypes type)
{
  std::wclog << L"creating minidump for an running process\n";

  const auto pid = findOtherPid();
  if (pid == 0) {
    std::wcerr << L"no other process found\n";
    return false;
  }

  std::wclog << L"found other process with pid " << pid << L"\n";

  HandlePtr handle(OpenProcess(
    PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid));

  if (!handle) {
    const auto e = GetLastError();

    std::wcerr
      << L"failed to open process " << pid << L", "
      << formatSystemMessage(e) << L"\n";

    return false;
  }

  return createMiniDump(handle.get(), type);
}

} // namespace