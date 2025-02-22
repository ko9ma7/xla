/* Copyright 2021 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/stream_executor/tpu/tpu_initializer_helper.h"

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/ascii.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "absl/synchronization/mutex.h"
#include "xla/stream_executor/tpu/libtftpu.h"
#include "xla/stream_executor/tpu/tpu_api_dlsym_set_fn.h"
#include "xla/stream_executor/tpu/tpu_executor_c_api.h"
#include "xla/stream_executor/tpu/tpu_ops_c_api.h"
#include "tsl/platform/errors.h"
#include "tsl/platform/logging.h"

#if !defined(PLATFORM_GOOGLE)
#include "xla/stream_executor/tpu/tpu_api.h"
#include "xla/stream_executor/tpu/tpu_platform.h"
#include "tsl/platform/env.h"
#elif defined(LIBTPU_STATIC)
#include "xla/stream_executor/tpu/tpu_api.h"
#include "xla/stream_executor/tpu/tpu_platform.h"
#endif  // PLATFORM_GOOGLE

namespace tensorflow {
namespace tpu {
namespace {

static std::string GetEnvVar(const char* name) {
  // Constructing a std::string directly from nullptr is undefined behavior so
  // we can return empty string in that case
  const char* env_value = getenv(name);
  if (!env_value) return "";
  return std::string(env_value);
}

bool GetEnvBool(const char* name, bool defval) {
  const char* env = getenv(name);
  if (env == nullptr) {
    return defval;
  }
  if (std::strcmp(env, "true") == 0) {
    return true;
  }
  if (std::strcmp(env, "false") == 0) {
    return false;
  }
  int int_env;
  bool has_int = absl::SimpleAtoi(env, &int_env);
  return has_int && int_env != 0;
}

const char* GetTpuDriverFile() {
  static const char* tpu_dev_path = []() {
    struct stat sb;
    if (stat("/dev/accel0", &sb) == 0) {
      return "/dev/accel0";
    } else {
      return "/dev/vfio/0";
    }
  }();
  return tpu_dev_path;
}

// This function gets pid of a process and checks if that process is using tpu.
// It is not able to check processes that are owned by another user.
bool IsTpuUsed(int64_t pid) {
  std::string path = absl::StrCat("/proc/", pid, "/fd");
  DIR* raw_fd_dir = opendir(path.c_str());
  if (!raw_fd_dir) {
    return false;
  }
  std::unique_ptr<DIR, int (*)(DIR*)> fd_dir(raw_fd_dir, closedir);
  struct dirent* ent;
  std::string line;
  std::string tpu_dev_path = GetTpuDriverFile();
  line.resize(tpu_dev_path.size());
  while ((ent = readdir(raw_fd_dir))) {
    if (!absl::ascii_isdigit(*ent->d_name)) continue;
    int64_t fd;
    if (!absl::SimpleAtoi(ent->d_name, &fd)) continue;
    path = absl::StrCat("/proc/", pid, "/fd/", fd);
    if (!readlink(path.c_str(), &line[0], line.size())) continue;
    if (line != tpu_dev_path) continue;
    return true;
  }
  return false;
}

// This function iterates through all the processes in /proc and finds out if
// any process it was able to check is using the TPU. It does not have
// permission to processes owned by another user.
// TODO (shahrokhi) use tensorflow/core/platform/filesystem (GetChildren) for
// this.
tsl::StatusOr<int64_t> FindLibtpuProcess() {
  DIR* proc = opendir("/proc");

  if (proc == nullptr) {
    return tsl::errors::Unavailable("was not able to open /proc");
  }
  std::unique_ptr<DIR, int (*)(DIR*)> proc_dir(proc, closedir);
  struct dirent* ent;
  while ((ent = readdir(proc))) {
    if (!absl::ascii_isdigit(*ent->d_name)) continue;

    int64_t pid;
    if (!absl::SimpleAtoi(ent->d_name, &pid)) continue;
    if (IsTpuUsed(pid)) {
      return pid;
    }
  }
  return tsl::errors::NotFound("did not find which pid uses the libtpu.so");
}

}  // namespace

tsl::Status TryAcquireTpuLock() {
  static absl::Mutex* mu = new absl::Mutex();
  absl::MutexLock l(mu);

  std::string load_library_override = absl::StrCat(getenv("TPU_LOAD_LIBRARY"));

  if (load_library_override == "1") {
    VLOG(1) << "TPU_LOAD_LIBRARY=1, force loading libtpu";
    return ::tsl::OkStatus();
  } else if (load_library_override == "0") {
    return tsl::errors::FailedPrecondition(
        "TPU_LOAD_LIBRARY=0, not loading libtpu");
  }

  bool allow_multiple_libtpu_load =
      GetEnvBool("ALLOW_MULTIPLE_LIBTPU_LOAD", false);

  if (allow_multiple_libtpu_load) {
    VLOG(1) << "ALLOW_MULTIPLE_LIBTPU_LOAD is set to True, "
               "allowing multiple concurrent libtpu.so loads.";
    return ::tsl::OkStatus();
  }

  std::string chips_per_process_bounds =
      GetEnvVar("TPU_CHIPS_PER_PROCESS_BOUNDS");
  if (chips_per_process_bounds.empty()) {
    // TODO(skyewm): remove this when TPU_CHIPS_PER_HOST_BOUNDS is fully
    // deprecated
    chips_per_process_bounds = GetEnvVar("TPU_CHIPS_PER_HOST_BOUNDS");
  }

  // TODO(b/291278826): make per-chip lock files and look at TPU_VISIBLE_DEVICES
  // to make TPU process mutex separation more accurate.
  bool use_all_tpus =
      chips_per_process_bounds.empty() || chips_per_process_bounds == "2,2,1";
  if (!use_all_tpus) {
    VLOG(1) << "TPU_CHIPS_PER_PROCESS_BOUNDS is a subset of host's TPU "
               "devices, allowing multiple libtpu.so loads.";
    return ::tsl::OkStatus();
  }

  static constexpr char libtpu_lockfn[] = "/tmp/libtpu_lockfile";

  // Clean-up call to remove user owned libtpu lockfile on proc exit.
  atexit([]() {
    // Ignores any lockfile removal error at proc exit.
    remove(libtpu_lockfn);
  });

  int fd = open(libtpu_lockfn, O_CREAT | O_RDWR, 0600);
  if (fd == -1) {
    // File open permission locks multi-user access by default.
    return tsl::errors::Aborted(
        "The TPU is already in use by another process probably owned by "
        "another user. Run \"$ sudo lsof -w ",
        GetTpuDriverFile(),
        "\" to figure out which process is using the TPU. If you still get "
        "this message, run \"$ sudo rm /tmp/libtpu_lockfile\".");
  }

  // lockf() holds the lock until the process exits to guard the underlying
  // TPU devices throughout process lifetime.
  if (lockf(fd, F_TLOCK, 0) != 0) {
    auto pid = FindLibtpuProcess();
    if (pid.ok()) {
      return tsl::errors::Aborted(absl::StrCat(
          "The TPU is already in use by process with pid ", pid.value(),
          ". Not attempting to load libtpu.so in this process."));
    } else {
      return tsl::errors::Aborted(
          "Internal error when accessing libtpu multi-process lockfile. "
          "Run \"$ sudo rm /tmp/libtpu_lockfile\".");
    }
  }
  return ::tsl::OkStatus();
}

#if !defined(PLATFORM_GOOGLE)
#include "xla/stream_executor/tpu/tpu_library_init_fns.inc"

tsl::Status InitializeTpuLibrary(void* library_handle) {
  tsl::Status s = InitializeTpuStructFns(library_handle);

  // Retrieve arguments from environment if applicable
  std::pair<std::vector<std::string>, std::vector<const char*>> args =
      GetLibTpuInitArguments();

  // TPU platform registration must only be performed after the library is
  // loaded. We do not want to register a TPU platform in XLA without the
  // supporting library providing the necessary APIs.
  if (s.ok()) {
    void (*initialize_fn)(bool init_library, int num_args, const char** args);
    initialize_fn = reinterpret_cast<decltype(initialize_fn)>(
        dlsym(library_handle, "TfTpu_Initialize"));
    (*initialize_fn)(/*init_library=*/true, args.second.size(),
                     args.second.data());

    RegisterTpuPlatform();
  }

  return s;
}

// TODO(b/261484192): refactor this function to align with supporting different
// PJRT plugins.
tsl::Status FindAndLoadTpuLibrary() {
  const char* env_value = getenv("TPU_LIBRARY_PATH");
  const char* libtpu_path =
      env_value && strlen(env_value) > 0 ? env_value : "libtpu.so";
  LOG(INFO) << "Libtpu path is: " << libtpu_path;
  void* library = dlopen(libtpu_path, RTLD_LAZY);
  if (library) {
    // We can open the shared library which means we are in a TPU environment.
    // Try to acquire exclusive access.
    TF_RETURN_IF_ERROR(TryAcquireTpuLock());
    TF_RETURN_IF_ERROR(InitializeTpuLibrary(library));
  } else {
    LOG(INFO) << "Failed to open libtpu: " << dlerror();
  }

  return ::tsl::OkStatus();
}

#elif defined(LIBTPU_STATIC)

#include "xla/stream_executor/tpu/tpu_library_init_fns.inc"

tsl::Status InitializeTpuLibrary() {
  // Retrieve arguments from environment if applicable
  std::pair<std::vector<std::string>, std::vector<const char*>> args =
      GetLibTpuInitArguments();

  TfTpu_Initialize(/*init_library*/ true, args.second.size(),
                   args.second.data());

  RegisterTpuPlatform();
  return ::tsl::OkStatus();
}

tsl::Status FindAndLoadTpuLibrary() {
  // We can open the shared library which means we are in a TPU environment.
  // Try to acquire exclusive access.
  TF_RETURN_IF_ERROR(TryAcquireTpuLock());
  TF_RETURN_IF_ERROR(InitializeTpuLibrary());
  return ::tsl::OkStatus();
}

#else   // PLATFORM_GOOGLE
tsl::Status InitializeTpuLibrary(void* library_handle) {
  return tsl::errors::Unimplemented(
      "You must statically link in a TPU library.");
}
#endif  // PLATFORM_GOOGLE
std::pair<std::vector<std::string>, std::vector<const char*>>
GetLibTpuInitArguments() {
  // We make copies of the arguments returned by getenv because the memory
  // returned may be altered or invalidated by further calls to getenv.
  std::vector<std::string> args;
  std::vector<const char*> arg_ptrs;

  // Retrieve arguments from environment if applicable.
  char* env = getenv("LIBTPU_INIT_ARGS");
  if (env != nullptr) {
    // TODO(frankchn): Handles quotes properly if necessary.
    args = absl::StrSplit(env, ' ');
  }

  arg_ptrs.reserve(args.size());
  for (int i = 0; i < args.size(); ++i) {
    arg_ptrs.push_back(args[i].data());
  }

  return {std::move(args), std::move(arg_ptrs)};
}

}  // namespace tpu
}  // namespace tensorflow
