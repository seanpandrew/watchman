/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "make_unique.h"
#include "watchman.h"

#ifdef HAVE_PORT_CREATE

#define WATCHMAN_PORT_EVENTS \
  FILE_MODIFIED | FILE_ATTRIB | FILE_NOFOLLOW

struct PortFSWatcher : public Watcher {
  int port_fd;
  /* map of file name to watchman_port_file */
  w_ht_t *port_files;
  /* protects port_files */
  pthread_mutex_t lock;
  port_event_t portevents[WATCHMAN_BATCH_LIMIT];

  PortFSWatcher() : Watcher("portfs", 0) {}
  ~PortFSWatcher();

  bool initNew(w_root_t* root, char** errmsg) override;

  struct watchman_dir_handle* startWatchDir(
      struct write_locked_watchman_root* lock,
      struct watchman_dir* dir,
      struct timeval now,
      const char* path) override;

  bool startWatchFile(struct watchman_file* file) override;

  bool consumeNotify(w_root_t* root, struct watchman_pending_collection* coll)
      override;

  bool waitNotify(int timeoutms) override;
  bool do_watch(w_string_t* name, struct stat* st);
};

struct watchman_port_file {
  file_obj_t port_file;
  w_string_t *name;
};

static const struct flag_map pflags[] = {
    {FILE_ACCESS, "FILE_ACCESS"},
    {FILE_MODIFIED, "FILE_MODIFIED"},
    {FILE_ATTRIB, "FILE_ATTRIB"},
    {FILE_DELETE, "FILE_DELETE"},
    {FILE_RENAME_TO, "FILE_RENAME_TO"},
    {FILE_RENAME_FROM, "FILE_RENAME_FROM"},
    {UNMOUNTED, "UNMOUNTED"},
    {MOUNTEDOVER, "MOUNTEDOVER"},
    {0, nullptr},
};

static struct watchman_port_file *make_port_file(w_string_t *name,
    struct stat *st) {
  struct watchman_port_file *f;

  f = calloc(1, sizeof(*f));
  if (!f) {
    return nullptr;
  }
  f->name = name;
  w_string_addref(name);
  f->port_file.fo_name = (char*)name->buf;
  f->port_file.fo_atime = st->st_atim;
  f->port_file.fo_mtime = st->st_mtim;
  f->port_file.fo_ctime = st->st_ctim;

  return f;
}

static void free_port_file(struct watchman_port_file *f) {
  w_string_delref(f->name);
  free(f);
}

static void portfs_del_port_file(w_ht_val_t key) {
  free_port_file(w_ht_val_ptr(key));
}

const struct watchman_hash_funcs port_file_funcs = {
    w_ht_string_copy,
    w_ht_string_del,
    w_ht_string_equal,
    w_ht_string_hash,
    nullptr, // copy_val
    portfs_del_port_file,
};

bool PortFSWatcher::initNew(w_root_t* root, char** errmsg) {
  auto watcher = watchman::make_unique<PortFSWatcher>();

  if (!watcher) {
    *errmsg = strdup("out of memory");
    return false;
  }

  pthread_mutex_init(&watcher->lock, nullptr);
  watcher->port_files = w_ht_new(
      root->config.getInt(CFG_HINT_NUM_DIRS, HINT_NUM_DIRS), &port_file_funcs);

  watcher->port_fd = port_create();
  if (watcher->port_fd == -1) {
    ignore_result(asprintf(
        errmsg,
        "watch(%s): port_create() error: %s",
        root->root_path.c_str(),
        strerror(errno)));
    w_log(W_LOG_ERR, "%s\n", *errmsg);
    return false;
  }
  w_set_cloexec(watcher->port_fd);

  root->inner.watcher = std::move(watcher);
  return true;
}

PortFSWatcher::~PortFSWatcher() {
  close(port_fd);
  port_fd = -1;
  w_ht_free(port_files);
  pthread_mutex_destroy(&lock);
}

bool PortFSWatcher::do_watch(w_string_t* name, struct stat* st) {
  struct watchman_port_file *f;
  bool success = false;

  pthread_mutex_lock(&lock);
  if (w_ht_get(port_files, w_ht_ptr_val(name))) {
    // Already watching it
    success = true;
    goto out;
  }

  f = make_port_file(name, st);
  if (!f) {
    goto out;
  }

  if (!w_ht_set(port_files, w_ht_ptr_val(name), w_ht_ptr_val(f))) {
    free_port_file(f);
    goto out;
  }

  w_log(W_LOG_DBG, "watching %s\n", name->buf);
  errno = 0;
  if (port_associate(
          port_fd,
          PORT_SOURCE_FILE,
          (uintptr_t)&f->port_file,
          WATCHMAN_PORT_EVENTS,
          (void*)f)) {
    w_log(W_LOG_ERR, "port_associate %s %s\n",
        f->port_file.fo_name, strerror(errno));
    w_ht_del(port_files, w_ht_ptr_val(name));
    goto out;
  }

  success = true;

out:
  pthread_mutex_unlock(&lock);
  return success;
}

bool PortFSWatcher::startWatchFile(struct watchman_file* file) {
  w_string_t *name;
  bool success = false;

  name = w_string_path_cat(file->parent->path, file->name);
  if (!name) {
    return false;
  }
  success = do_watch(name, &file->st);
  w_string_delref(name);

  return success;
}

struct watchman_dir_handle* PortFSWatcher::startWatchDir(
    struct write_locked_watchman_root* lock,
    struct watchman_dir* dir,
    struct timeval now,
    const char* path) {
  struct watchman_dir_handle *osdir;
  struct stat st;
  w_string_t *dir_name;

  osdir = w_dir_open(path);
  if (!osdir) {
    handle_open_errno(lock, dir, now, "opendir", errno, nullptr);
    return nullptr;
  }

  if (fstat(dirfd(osdir), &st) == -1) {
    // whaaa?
    w_log(W_LOG_ERR, "fstat on opened dir %s failed: %s\n", path,
        strerror(errno));
    w_root_schedule_recrawl(root, "fstat failed");
    w_dir_close(osdir);
    return nullptr;
  }

  dir_name = w_dir_copy_full_path(dir);
  if (!do_watch(dir_name, &st)) {
    w_dir_close(osdir);
    w_string_delref(dir_name);
    return nullptr;
  }

  w_string_delref(dir_name);
  return osdir;
}

bool PortFSWatcher::consumeNotify(
    w_root_t* root,
    struct watchman_pending_collection* coll) {
  uint_t i, n;
  struct timeval now;

  errno = 0;

  n = 1;
  if (port_getn(
          port_fd,
          portevents,
          sizeof(portevents) / sizeof(portevents[0]),
          &n,
          nullptr)) {
    if (errno == EINTR) {
      return false;
    }
    w_log(W_LOG_FATAL, "port_getn: %s\n",
        strerror(errno));
  }

  w_log(W_LOG_DBG, "port_getn: n=%u\n", n);

  if (n == 0) {
    return false;
  }

  pthread_mutex_lock(&lock);

  for (i = 0; i < n; i++) {
    struct watchman_port_file *f;
    uint32_t pe = portevents[i].portev_events;
    char flags_label[128];

    f = (struct watchman_port_file*)portevents[i].portev_user;
    w_expand_flags(pflags, pe, flags_label, sizeof(flags_label));
    w_log(W_LOG_DBG, "port: %s [0x%x %s]\n",
        f->port_file.fo_name,
        pe, flags_label);

    if ((pe & (FILE_RENAME_FROM|UNMOUNTED|MOUNTEDOVER|FILE_DELETE))
        && w_string_equal(f->name, root->root_path)) {
      w_log(
          W_LOG_ERR,
          "root dir %s has been (re)moved (code 0x%x %s), canceling watch\n",
          root->root_path.c_str(),
          pe,
          flags_label);

      w_root_cancel(root);
      pthread_mutex_unlock(&lock);
      return false;
    }
    w_pending_coll_add(coll, f->name, now,
        W_PENDING_RECURSIVE|W_PENDING_VIA_NOTIFY);

    // It was port_dissociate'd implicitly.  We'll re-establish a
    // watch later when portfs_root_start_watch_(file|dir) are called again
    w_ht_del(port_files, w_ht_ptr_val(f->name));
  }
  pthread_mutex_unlock(&lock);

  return true;
}

bool PortFSWatcher::waitNotify(int timeoutms) {
  int n;
  struct pollfd pfd;

  pfd.fd = port_fd;
  pfd.events = POLLIN;

  n = poll(&pfd, 1, timeoutms);

  return n == 1;
}

static PortFSWatcher watcher;
Watcher* portfs_watcher = &watcher;

#endif // HAVE_INOTIFY_INIT

/* vim:ts=2:sw=2:et:
 */
