/*
   Copyright (c) 2007, 2019, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

#include <portlib/NdbSleep.h>
#include <portlib/NdbDir.hpp>
#include "atrt.hpp"

static bool generate_my_cnf(BaseString mycnf, atrt_config& config);
static bool create_directory(const char* path);
static bool delete_file_if_exists(const char* path);
static bool copy_file(const char* src, const char* dst);
static bool generate_mysql_init_file_configurations(const char* fileName);

bool setup_directories(atrt_config& config, int setup) {
  /**
   * 0 = validate
   * 1 = setup
   * 2 = setup+clean
   */
  for (unsigned i = 0; i < config.m_clusters.size(); i++) {
    atrt_cluster& cluster = *config.m_clusters[i];
    for (unsigned j = 0; j < cluster.m_processes.size(); j++) {
      atrt_process& proc = *cluster.m_processes[j];
      const char* dir = proc.m_proc.m_cwd.c_str();
      struct stat sbuf;
      int exists = 0;
      if (lstat(dir, &sbuf) == 0) {
        if (S_ISDIR(sbuf.st_mode))
          exists = 1;
        else
          exists = -1;
      }

      switch (setup) {
        case 0:
          switch (exists) {
            case 0:
              g_logger.error("Could not find directory: %s", dir);
              return false;
            case -1:
              g_logger.error("%s is not a directory!", dir);
              return false;
          }
          break;
        case 1:
          if (exists == -1) {
            g_logger.error("%s is not a directory!", dir);
            return false;
          }
          break;
        case 2:
          if (exists == 1) {
            if (!remove_dir(dir)) {
              g_logger.error("Failed to remove %s!", dir);
              return false;
            }
            exists = 0;
            break;
          } else if (exists == -1) {
            if (!unlink(dir)) {
              g_logger.error("Failed to remove %s!", dir);
              return false;
            }
            exists = 0;
          }
      }
      if (exists != 1) {
        if (!create_directory(dir)) {
          return false;
        }
      }
    }
  }
  return true;
}

static void printfile(FILE* out, Properties& props, const char* section, ...)
    ATTRIBUTE_FORMAT(printf, 3, 4);

static void printfile(FILE* out, Properties& props, const char* section, ...) {
  Properties::Iterator it(&props);
  const char* name = it.first();
  if (name) {
    va_list ap;
    va_start(ap, section);
    /* const int ret = */ vfprintf(out, section, ap);
    va_end(ap);
    fprintf(out, "\n");

    for (; name; name = it.next()) {
      const char* val;
      props.get(name, &val);
      fprintf(out, "%s %s\n", name + 2, val);
    }
    fprintf(out, "\n");
  }
  fflush(out);
}

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

static bool generate_my_cnf(BaseString mycnf, atrt_config& config) {
  FILE* out = fopen(mycnf.c_str(), "a+");
  if (out == NULL) {
    g_logger.error("Failed to open %s for append", mycnf.c_str());
    return false;
  }

  time_t now = time(0);
  fprintf(out, "#\n# Generated by atrt\n");
  fprintf(out, "# %s\n", ctime(&now));

  for (unsigned i = 0; i < config.m_clusters.size(); i++) {
    atrt_cluster& cluster = *config.m_clusters[i];
    printfile(out, cluster.m_options.m_generated, "[mysql_cluster%s]",
              cluster.m_name.c_str());

    for (unsigned j = 0; j < cluster.m_processes.size(); j++) {
      atrt_process& proc = *cluster.m_processes[j];

      switch (proc.m_type) {
        case atrt_process::AP_NDB_MGMD:
          printfile(out, proc.m_options.m_generated,
                    "[cluster_config.ndb_mgmd.%d%s]", proc.m_index,
                    proc.m_cluster->m_name.c_str());
          break;
        case atrt_process::AP_NDBD:
          printfile(out, proc.m_options.m_generated,
                    "[cluster_config.ndbd.%d%s]", proc.m_index,
                    proc.m_cluster->m_name.c_str());
          break;
        case atrt_process::AP_MYSQLD:
          printfile(out, proc.m_options.m_generated, "[mysqld.%d%s]",
                    proc.m_index, proc.m_cluster->m_name.c_str());
          break;
        case atrt_process::AP_CLIENT:
          printfile(out, proc.m_options.m_generated, "[client.%d%s]",
                    proc.m_index, proc.m_cluster->m_name.c_str());
          break;
        case atrt_process::AP_CUSTOM:
          printfile(out, proc.m_options.m_generated, "[%s.%d%s]",
                    proc.m_name.c_str(), proc.m_index,
                    proc.m_cluster->m_name.c_str());
          break;
        case atrt_process::AP_NDB_API:
        // fall-through
        case atrt_process::AP_ALL:
        // fall-through
        case atrt_process::AP_CLUSTER:
          break;
      }
    }
  }

  fclose(out);
  return true;
}

bool exists_file(const char* path) {
  struct stat sbuf;
  int ret = lstat(path, &sbuf);
  return ret == 0;
}

static bool delete_file_if_exists(const char* path) {
  if (!exists_file(path)) {
    return true;
  }

  if (unlink(path) != 0) {
    g_logger.error("Failed to remove %s", path);
    return false;
  }

  return true;
}

static bool copy_file(const char* src, const char* dst) {
  BaseString cp;
  cp.assfmt("cp %s %s", src, dst);
  to_fwd_slashes(cp);
  if (sh(cp.c_str()) != 0) {
    g_logger.error("Failed to '%s'", cp.c_str());
    return false;
  }

  return true;
}

bool setup_files(atrt_config& config, int setup, int sshx) {
  /**
   * 0 = validate
   * 1 = setup
   * 2 = setup+clean
   */
  BaseString mycnf;
  mycnf.assfmt("%s/my.cnf", g_basedir);
  to_native(mycnf);

  if (!create_directory(g_basedir)) {
    return false;
  }

  if (mycnf != g_my_cnf) {
    if (!delete_file_if_exists(mycnf.c_str())) {
      return false;
    }

    BaseString aux(g_my_cnf);
    to_native(aux);
    if (!copy_file(aux.c_str(), mycnf.c_str())) {
      return false;
    }
  }

  if (config.m_config_type == atrt_config::INI) {
    for (unsigned i = 0; i < config.m_clusters.size(); i++) {
      atrt_cluster& cluster = *config.m_clusters[i];
      const char* cluster_name = cluster.m_name.c_str();

      if (strcmp(cluster_name, ".atrt") == 0) {
        continue;
      }

      BaseString dst_config_ini;
      dst_config_ini.assfmt("%s/config%s.ini", g_basedir, cluster_name);
      to_native(dst_config_ini);

      if (!delete_file_if_exists(dst_config_ini.c_str())) {
        return false;
      }

      BaseString src_config_ini;
      src_config_ini.assfmt("%s/config%s.ini", g_cwd, cluster_name);
      to_native(src_config_ini);

      if (!copy_file(src_config_ini.c_str(), dst_config_ini.c_str())) {
        return false;
      }
    }
  }

  if (setup == 2 || config.m_generated) {
    bool use_mysqld =
        (g_resources.getExecutableFullPath(g_resources.MYSQL_INSTALL_DB) == "");
    if (!use_mysqld) {
      // Even if mysql_install_db exists, prefer use of mysqld if possible
      BaseString mysqld_bin_path =
          g_resources.getExecutableFullPath(g_resources.MYSQLD).c_str();
      BaseString tmp;
      tmp.assfmt("%s --help --verbose", mysqld_bin_path.c_str());
      FILE* f = popen(tmp.c_str(), "re");
      char buf[1000];
      while (NULL != fgets(buf, sizeof(buf), f)) {
        if (strncmp(buf, "initialize-insecure ", 20) == 0) {
          use_mysqld = true;
        }
      }
      pclose(f);
    }
    /**
     * Do mysql_install_db
     */
    for (unsigned i = 0; i < config.m_clusters.size(); i++) {
      atrt_cluster& cluster = *config.m_clusters[i];
      for (unsigned j = 0; j < cluster.m_processes.size(); j++) {
        atrt_process& proc = *cluster.m_processes[j];
        if (proc.m_type == atrt_process::AP_MYSQLD)
#ifndef _WIN32
        {
          const char* val;
          require(proc.m_options.m_loaded.get("--datadir=", &val));
          BaseString tmp;
          if (use_mysqld) {
            BaseString initFile;
            initFile.assfmt("%s/mysqld-init-file.sql", g_basedir);
            if (!generate_mysql_init_file_configurations(initFile.c_str())) {
              g_logger.error(
                  "Failed to generate the mysql configuration init-file: %s",
                  initFile.c_str());
              return false;
            }

            BaseString mysqld_bin_path =
                g_resources.getExecutableFullPath(g_resources.MYSQLD).c_str();
            tmp.assfmt(
                "%s --defaults-file=%s/my.cnf --basedir=%s "
                "--datadir=%s --initialize-insecure --init-file=%s"
                "> %s/mysqld-initialize.log 2>&1",
                mysqld_bin_path.c_str(), g_basedir, g_prefix, val,
                initFile.c_str(), proc.m_proc.m_cwd.c_str());
          } else {
            BaseString mysql_install_db_bin_path =
                g_resources.getExecutableFullPath(g_resources.MYSQL_INSTALL_DB)
                    .c_str();
            assert(mysql_install_db_bin_path != "");
            tmp.assfmt(
                "%s --defaults-file=%s/my.cnf --basedir=%s "
                "--datadir=%s > %s/mysql_install_db.log 2>&1",
                mysql_install_db_bin_path.c_str(), g_basedir, g_prefix0, val,
                proc.m_proc.m_cwd.c_str());
          }
          to_fwd_slashes(tmp);
          if (sh(tmp.c_str()) != 0) {
            if (use_mysqld) {
              g_logger.error(
                  "Failed to mysqld --initialize-insecure for "
                  "%s, cmd: '%s'",
                  proc.m_proc.m_cwd.c_str(), tmp.c_str());
            } else {
              g_logger.error("Failed to mysql_install_db for %s, cmd: '%s'",
                             proc.m_proc.m_cwd.c_str(), tmp.c_str());
            }
            return false;
          } else {
            if (use_mysqld) {
              g_logger.info("mysqld --initialize-insecure for %s",
                            proc.m_proc.m_cwd.c_str());
            } else {
              g_logger.info("mysql_install_db for %s",
                            proc.m_proc.m_cwd.c_str());
            }
          }
        }
#else
        {
          g_logger.info(
              "not running mysqld --initialize-insecure nor "
              "mysql_install_db for %s",
              proc.m_proc.m_cwd.c_str());
        }
#endif
      }
    }
  }

  bool skip_my_cnf_generation =
      config.m_config_type == atrt_config::INI || config.m_generated == false;
  if (skip_my_cnf_generation) {
    g_logger.info("Skipping my.cnf generation...");
  } else {
    bool ok = generate_my_cnf(mycnf, config);
    if (!ok) return false;
  }

  for (unsigned i = 0; i < config.m_clusters.size(); i++) {
    atrt_cluster& cluster = *config.m_clusters[i];
    for (unsigned j = 0; j < cluster.m_processes.size(); j++) {
      atrt_process& proc = *cluster.m_processes[j];

      /**
       * Create env.sh
       */
      BaseString tmp;
      tmp.assfmt("%s/env.sh", proc.m_proc.m_cwd.c_str());
      to_native(tmp);
      char** env = BaseString::argify(0, proc.m_proc.m_env.c_str());
      if (env[0] || proc.m_proc.m_path.length()) {
        Vector<BaseString> keys;
        FILE* fenv = fopen(tmp.c_str(), "w+");
        if (fenv == 0) {
          g_logger.error("Failed to open %s for writing", tmp.c_str());
          return false;
        }
        for (size_t k = 0; env[k]; k++) {
          tmp = env[k];
          ssize_t pos = tmp.indexOf('=');
          require(pos > 0);
          env[k][pos] = 0;
          fprintf(fenv, "%s=\"%s\"\n", env[k], env[k] + pos + 1);
          keys.push_back(env[k]);
          free(env[k]);
        }
        if (proc.m_proc.m_path.length()) {
          fprintf(fenv, "CMD=\"%s", proc.m_proc.m_path.c_str());
          if (proc.m_proc.m_args.length()) {
            fprintf(fenv, " %s", proc.m_proc.m_args.c_str());
          }
          fprintf(fenv, "\"\nexport CMD\n");
        }

        fprintf(fenv, "PATH=");
        for (int i = 0; g_search_path[i] != 0; i++) {
          fprintf(fenv, "%s/%s:", g_prefix0, g_search_path[i]);
        }
        fprintf(fenv, "$PATH\n");
        keys.push_back("PATH");

        {
        /**
         * In 5.5...binaries aren't compiled with rpath
         * So we need an explicit LD_LIBRARY_PATH
         *
         * Use path from libmysqlclient.so
         */
#if defined(__MACH__)
          BaseString libdir =
              g_resources.getLibraryDirectory(g_resources.LIBMYSQLCLIENT_DYLIB)
                  .c_str();
          fprintf(fenv, "DYLD_LIBRARY_PATH=%s:$DYLD_LIBRARY_PATH\n",
                  libdir.c_str());
          keys.push_back("DYLD_LIBRARY_PATH");
#else
          BaseString libdir =
              g_resources.getLibraryDirectory(g_resources.LIBMYSQLCLIENT_SO)
                  .c_str();
          fprintf(fenv, "LD_LIBRARY_PATH=%s:$LD_LIBRARY_PATH\n",
                  libdir.c_str());
          keys.push_back("LD_LIBRARY_PATH");
#endif
        }

        for (unsigned k = 0; k < keys.size(); k++)
          fprintf(fenv, "export %s\n", keys[k].c_str());

        fflush(fenv);
        fclose(fenv);
      }
      free(env);

      {
        tmp.assfmt("%s/ssh-login.sh", proc.m_proc.m_cwd.c_str());
        FILE* fenv = fopen(tmp.c_str(), "w+");
        if (fenv == 0) {
          g_logger.error("Failed to open %s for writing", tmp.c_str());
          return false;
        }
        fprintf(fenv, "#!/bin/sh\n");
        fprintf(fenv, "cd %s\n", proc.m_proc.m_cwd.c_str());
        fprintf(fenv, "[ -f /etc/profile ] && . /etc/profile\n");
        fprintf(fenv, ". ./env.sh\n");
        fprintf(fenv, "ulimit -Sc unlimited\n");
        fprintf(fenv, "bash -i");
        fflush(fenv);
        fclose(fenv);
      }
    }
  }

  return true;
}

static bool create_directory(const char* path) {
  BaseString native(path);
  to_native(native);
  BaseString tmp(path);
  Vector<BaseString> list;

  if (tmp.split(list, "/") == 0) {
    g_logger.error("Failed to create directory: %s", tmp.c_str());
    return false;
  }

  BaseString cwd = IF_WIN("", "/");
  for (unsigned i = 0; i < list.size(); i++) {
    cwd.append(list[i].c_str());
    cwd.append("/");
    NdbDir::create(cwd.c_str(), NdbDir::u_rwx() | NdbDir::g_r() | NdbDir::g_x(),
                   true);
  }

  struct stat sbuf;
  if (lstat(native.c_str(), &sbuf) != 0 || !S_ISDIR(sbuf.st_mode)) {
    g_logger.error("Failed to create directory: %s (%s)", native.c_str(),
                   cwd.c_str());
    return false;
  }

  return true;
}

bool remove_dir(const char* path, bool inclusive) {
  if (access(path, 0)) return true;

  const int max_retries = 20;
  int attempt = 0;

  while (true) {
    if (NdbDir::remove_recursive(path, !inclusive)) return true;

    attempt++;
    if (attempt > max_retries) {
      g_logger.error("Failed to remove directory '%s'!", path);
      return false;
    }

    g_logger.warning(
        " - attempt %d to remove directory '%s' failed "
        ", retrying...",
        attempt, path);

    NdbSleep_MilliSleep(100);
  }

  abort();  // Never reached
  return false;
}

bool generate_mysql_init_file_configurations(const char* fileName) {
  FILE* fp = fopen(fileName, "w");
  if (fp == NULL) {
    g_logger.error("Failed to open mysql init file: %s", fileName);
    return false;
  }

  if (fprintf(fp, "rename user root@localhost to root@'%%';") < 0) {
    g_logger.error("Failed to write to mysql init file: %s", fileName);
    return false;
  }

  if (fclose(fp) == EOF) {
    g_logger.error("Failed to close mysql init file: %s", fileName);
    return false;
  }

  return true;
}
