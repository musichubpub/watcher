#include "watcher_plus.h"

// Debug logging function
void LOG_DEBUG(const char *fmt, ...)
{
  if (!_debug_mode)
    return;
  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args); // Output to standard error
  va_end(args);
}

// Signal handler for graceful shutdown
void signal_handler(int sig)
{
  running = false;
}

// Get entity type: 0=unknown, 1=file, 2=dir, 3=link
int32_t getEntityType(const char *fullPath)
{
  int32_t type = 0;
  struct stat path_stat;
  if (stat(fullPath, &path_stat) == 0)
  {
    if (S_ISREG(path_stat.st_mode))
    {
      type = 1;
    }
    else if (S_ISDIR(path_stat.st_mode))
    {
      type = 2;
    }
    else
    {
      type = 3;
    }
  }
  return type;
}

// Convert dmon_action to action code
static int32_t getActionType(dmon_action action)
{
  int32_t action_code;
  switch (action)
  {
  case DMON_ACTION_CREATE:
    action_code = 0; // Created
    break;
  case DMON_ACTION_DELETE:
    action_code = 1; // Deleted
    break;
  case DMON_ACTION_MODIFY:
    action_code = 2; // Modified
    break;
  case DMON_ACTION_MOVE:
    action_code = 3; // Moved
    break;
  default:
    action_code = -1; // Unknown
  }
  return action_code;
}

// Traverse directory to process files and subdirectories
#ifdef _WIN32
void traverse_directory(dmon_action action, const char *new_dir_path, const char *old_dir_path)
{
  WIN32_FIND_DATA findFileData;
  HANDLE hFind;
  char searchPath[MAX_PATH];
  snprintf(searchPath, MAX_PATH, "%s/*", new_dir_path); // Use new_dir_path
  hFind = FindFirstFile(searchPath, &findFileData);
  if (hFind == INVALID_HANDLE_VALUE)
    return;
  do
  {
    if (findFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    {
      // Process directory
      LOG_DEBUG("Dir: %s\n", findFileData.cFileName);
      char sub_path[MAX_PATH];
      snprintf(sub_path, MAX_PATH, "%s/%s", new_dir_path, findFileData.cFileName);
      if (strcmp(findFileData.cFileName, ".") != 0 && strcmp(findFileData.cFileName, "..") != 0)
      {
        char sub_old_path[MAX_PATH];
        if (old_dir_path)
          snprintf(sub_old_path, MAX_PATH, "%s/%s", old_dir_path, findFileData.cFileName);
        sendEventToDart(action, sub_path, old_dir_path ? sub_old_path : NULL);
        traverse_directory(action, sub_path, old_dir_path ? sub_old_path : NULL); // Recursively traverse subdirectory
      }
    }
    else
    {
      // Process file
      LOG_DEBUG("File: %s\n", findFileData.cFileName);
      char file_path[MAX_PATH];
      snprintf(file_path, MAX_PATH, "%s/%s", new_dir_path, findFileData.cFileName);
      char file_old_path[MAX_PATH];
      if (old_dir_path)
        snprintf(file_old_path, MAX_PATH, "%s/%s", old_dir_path, findFileData.cFileName);
      sendEventToDart(action, file_path, old_dir_path ? file_old_path : NULL);
    }
  } while (FindNextFile(hFind, &findFileData));
  FindClose(hFind);
}
#else
void traverse_directory(dmon_action action, const char *new_dir_path, const char *old_dir_path)
{
  DIR *dir = opendir(new_dir_path);
  if (!dir)
  {
    LOG_DEBUG("Cannot open directory: %s\n", new_dir_path);
    return;
  }
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL)
  {
    // Skip . and ..
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
    {
      continue;
    }

    // Construct full path
    char new_full_path[DMON_MAX_PATH];
    snprintf(new_full_path, sizeof(new_full_path), "%s/%s", new_dir_path, entry->d_name);
    LOG_DEBUG("new_full_path: %s\n", new_full_path);

    // Construct old path if provided
    char old_full_path[DMON_MAX_PATH];
    if (old_dir_path)
      snprintf(old_full_path, sizeof(old_full_path), "%s/%s", old_dir_path, entry->d_name);
    LOG_DEBUG("old_full_path: %s\n", old_full_path);

    sendEventToDart(action, new_full_path, old_full_path);

    // If it's a directory, send event and recurse
    if (entry->d_type == DT_DIR)
    {
      // Recursively traverse subdirectory
      traverse_directory(action, new_full_path, old_full_path);
    }
  }

  closedir(dir);
}
#endif

// Callback for directory watch events
static void watch_callback(dmon_watch_id watch_id, dmon_action action,
                           const char *rootdir, const char *filepath,
                           const char *oldfilepath, void *user)
{
  (void)watch_id; // Unused
  (void)user;     // Unused

  if (!rootdir || !filepath)
  {
    fprintf(stderr, "Invalid callback parameters: rootdir=%p, filepath=%p\n",
            (void *)rootdir, (void *)filepath);
    return;
  }

  // If directory-related operation, traverse directory and send events
  char full_path[DMON_MAX_PATH];
  snprintf(full_path, sizeof(full_path), "%s%s", rootdir, filepath);
  char old_full_path[DMON_MAX_PATH];
  if (oldfilepath)
  {
    snprintf(old_full_path, sizeof(old_full_path), "%s%s", rootdir, oldfilepath);
  }

  sendEventToDart(action, full_path, old_full_path);

  // If the path is a directory, traverse it for create or move actions
  if (getEntityType(full_path) == 2)
  {
    if (action == DMON_ACTION_CREATE || action == DMON_ACTION_MOVE)
    {
      traverse_directory(action, full_path, old_full_path);
    }
  }
}

// Send file system event to Dart
void sendEventToDart(dmon_action action, const char *full_path, const char *old_full_path)
{
  Dart_CObject cobj;
  cobj.type = Dart_CObject_kArray;
  static uint8_t length = 4;
  Dart_CObject *elements[4];

  int32_t type = getEntityType(full_path);
  char *full_path_copy = strdup(full_path);
  char *old_full_path_copy = old_full_path ? strdup(old_full_path) : NULL;

  // Check memory allocation
  if (!full_path_copy || (old_full_path && !old_full_path_copy))
  {
    fprintf(stderr, "Memory allocation failed in sendEventToDart\n");
    free(full_path_copy);
    free(old_full_path_copy);
    return;
  }

  for (int i = 0; i < length; i++)
  {
    elements[i] = (Dart_CObject *)malloc(sizeof(Dart_CObject));
    if (!elements[i])
    {
      fprintf(stderr, "Memory allocation failed for element %d\n", i);
      free(full_path_copy);
      free(old_full_path_copy);
      for (int j = 0; j < i; j++)
        free(elements[j]);
      return;
    }
  }

  elements[0]->type = Dart_CObject_kInt32;
  elements[0]->value.as_int32 = getActionType(action);

  elements[1]->type = Dart_CObject_kInt32;
  elements[1]->value.as_int32 = type;

  elements[2]->type = Dart_CObject_kString;
  elements[2]->value.as_string = full_path_copy;

  if (old_full_path)
  {
    elements[3]->type = Dart_CObject_kString;
    elements[3]->value.as_string = old_full_path_copy;
  }
  else
  {
    elements[3]->type = Dart_CObject_kNull;
  }

  cobj.value.as_array.length = length;
  cobj.value.as_array.values = elements;

  // Send message and check result
  if (!Dart_PostCObject(_dart_port, &cobj))
  {
    fprintf(stderr, "Failed to send message to Dart port\n");
  }
  else
  {
    LOG_DEBUG("Message sent\n");
  }

  // Clean up memory
  for (int i = 0; i < length; i++)
  {
    free(elements[i]);
  }
  free(full_path_copy);
  free(old_full_path_copy);
}

// Start monitoring a directory
int start_monitor(const char *watch_dir, int64_t dart_port, int32_t recursive, bool debug_mode)
{
  _debug_mode = debug_mode;
  _dart_port = dart_port;
  _watch_dir = watch_dir;
  _recursive = recursive;

  if (!_watch_dir || !*_watch_dir)
  {
    fprintf(stderr, "Invalid directory: %s\n", _watch_dir ? _watch_dir : "NULL");
    return 1;
  }
  if (_dart_port == 0)
  {
    fprintf(stderr, "Invalid Dart port\n");
    return 1;
  }
  if (strlen(_watch_dir) >= DMON_MAX_PATH)
  {
    fprintf(stderr, "Directory path too long: %s\n", _watch_dir);
    return 1;
  }

  dmon_init();

  id = dmon_watch(_watch_dir, watch_callback, recursive, NULL);

  if (id.id == 0)
  {
    fprintf(stderr, "Failed to monitor directory: %s\n", _watch_dir);
    dmon_deinit();
    return 1;
  }

  LOG_DEBUG("Monitoring %s (press Ctrl+C to stop)\n", _watch_dir);
#ifdef _WIN32
#else
  signal(SIGINT, signal_handler);
#endif
  return 0;
}

// Stop monitoring the directory
void stop_monitor()
{
  running = false;
  dmon_unwatch(id);
  dmon_deinit();
  printf("Stopped monitoring %s\n", _watch_dir);
}