/*
 * Copyright (c) 2023, 2024, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "cds/archiveHeapLoader.hpp"
#include "cds/cdsConfig.hpp"
#include "cds/cds_globals.hpp"
#include "cds/classListWriter.hpp"
#include "cds/heapShared.hpp"
#include "cds/metaspaceShared.hpp"
#include "classfile/classLoaderDataShared.hpp"
#include "classfile/moduleEntry.hpp"
#include "classfile/systemDictionaryShared.hpp"
#include "include/jvm_io.h"
#include "logging/log.hpp"
#include "prims/jvmtiExport.hpp"
#include "memory/universe.hpp"
#include "runtime/arguments.hpp"
#include "runtime/globals_extension.hpp"
#include "runtime/java.hpp"
#include "utilities/defaultStream.hpp"
#include "utilities/formatBuffer.hpp"

bool CDSConfig::_is_dumping_static_archive = false;
bool CDSConfig::_is_dumping_dynamic_archive = false;
bool CDSConfig::_is_using_optimized_module_handling = true;
bool CDSConfig::_is_dumping_full_module_graph = true;
bool CDSConfig::_is_using_full_module_graph = true;
bool CDSConfig::_has_aot_linked_classes = false;
bool CDSConfig::_has_archived_invokedynamic = false;
bool CDSConfig::_is_loading_packages = false;
bool CDSConfig::_is_loading_protection_domains = false;
bool CDSConfig::_is_security_manager_allowed = false;
bool CDSConfig::_old_cds_flags_used = false;

char* CDSConfig::_default_archive_path = nullptr;
char* CDSConfig::_static_archive_path = nullptr;
char* CDSConfig::_dynamic_archive_path = nullptr;

int CDSConfig::get_status() {
  assert(Universe::is_fully_initialized(), "status is finalized only after Universe is initialized");
  return (is_dumping_archive()              ? IS_DUMPING_ARCHIVE : 0) |
         (is_dumping_static_archive()       ? IS_DUMPING_STATIC_ARCHIVE : 0) |
         (is_logging_lambda_form_invokers() ? IS_LOGGING_LAMBDA_FORM_INVOKERS : 0) |
         (is_using_archive()                ? IS_USING_ARCHIVE : 0) |
         (is_dumping_heap()                 ? IS_DUMPING_HEAP : 0) |
         (is_logging_dynamic_proxies()      ? IS_LOGGING_DYNAMIC_PROXIES : 0) |
         (is_dumping_packages()             ? IS_DUMPING_PACKAGES : 0) |
         (is_dumping_protection_domains()   ? IS_DUMPING_PROTECTION_DOMAINS : 0);
}

void CDSConfig::initialize() {
  if (is_dumping_static_archive() && !is_dumping_final_static_archive()) {
    UseSharedSpaces = false;
  }

  // Initialize shared archive paths which could include both base and dynamic archive paths
  // This must be after set_ergonomics_flags() called so flag UseCompressedOops is set properly.
  //
  // UseSharedSpaces may be disabled if -XX:SharedArchiveFile is invalid.
  if (is_dumping_static_archive() || is_using_archive()) {
    init_shared_archive_paths();
  }

  if (!is_dumping_heap()) {
    _is_dumping_full_module_graph = false;
  }
}

char* CDSConfig::default_archive_path() {
  if (_default_archive_path == nullptr) {
    char jvm_path[JVM_MAXPATHLEN];
    os::jvm_path(jvm_path, sizeof(jvm_path));
    char *end = strrchr(jvm_path, *os::file_separator());
    if (end != nullptr) *end = '\0';
    size_t jvm_path_len = strlen(jvm_path);
    size_t file_sep_len = strlen(os::file_separator());
    const size_t len = jvm_path_len + file_sep_len + 20;
    _default_archive_path = NEW_C_HEAP_ARRAY(char, len, mtArguments);
    jio_snprintf(_default_archive_path, len,
                LP64_ONLY(!UseCompressedOops ? "%s%sclasses_nocoops.jsa":) "%s%sclasses.jsa",
                jvm_path, os::file_separator());
  }
  return _default_archive_path;
}

int CDSConfig::num_archives(const char* archive_path) {
  if (archive_path == nullptr) {
    return 0;
  }
  int npaths = 1;
  char* p = (char*)archive_path;
  while (*p != '\0') {
    if (*p == os::path_separator()[0]) {
      npaths++;
    }
    p++;
  }
  return npaths;
}

void CDSConfig::extract_shared_archive_paths(const char* archive_path,
                                             char** base_archive_path,
                                             char** top_archive_path) {
  char* begin_ptr = (char*)archive_path;
  char* end_ptr = strchr((char*)archive_path, os::path_separator()[0]);
  if (end_ptr == nullptr || end_ptr == begin_ptr) {
    vm_exit_during_initialization("Base archive was not specified", archive_path);
  }
  size_t len = end_ptr - begin_ptr;
  char* cur_path = NEW_C_HEAP_ARRAY(char, len + 1, mtInternal);
  strncpy(cur_path, begin_ptr, len);
  cur_path[len] = '\0';
  *base_archive_path = cur_path;

  begin_ptr = ++end_ptr;
  if (*begin_ptr == '\0') {
    vm_exit_during_initialization("Top archive was not specified", archive_path);
  }
  end_ptr = strchr(begin_ptr, '\0');
  assert(end_ptr != nullptr, "sanity");
  len = end_ptr - begin_ptr;
  cur_path = NEW_C_HEAP_ARRAY(char, len + 1, mtInternal);
  strncpy(cur_path, begin_ptr, len + 1);
  *top_archive_path = cur_path;
}

static void set_new_workflow_default_CachedCodeFile() {
  size_t len = strlen(CacheDataStore) + 6;
  char* file = AllocateHeap(len, mtArguments);
  jio_snprintf(file, len, "%s.code", CacheDataStore);
  FLAG_SET_ERGO(CachedCodeFile, file);
}

void CDSConfig::init_shared_archive_paths() {
  if (ArchiveClassesAtExit != nullptr) {
    assert(!RecordDynamicDumpInfo, "already checked");
    if (is_dumping_static_archive()) {
      vm_exit_during_initialization("-XX:ArchiveClassesAtExit cannot be used with -Xshare:dump");
    }
    check_unsupported_dumping_module_options();

    if (os::same_files(default_archive_path(), ArchiveClassesAtExit)) {
      vm_exit_during_initialization(
        "Cannot specify the default CDS archive for -XX:ArchiveClassesAtExit", default_archive_path());
    }
  }

  if (SharedArchiveFile == nullptr) {
    _static_archive_path = default_archive_path();
  } else {
    int archives = num_archives(SharedArchiveFile);
    assert(archives > 0, "must be");

    if (is_dumping_archive() && archives > 1) {
      vm_exit_during_initialization(
        "Cannot have more than 1 archive file specified in -XX:SharedArchiveFile during CDS dumping");
    }

    if (CDSPreimage != nullptr && archives > 1) {
      vm_exit_during_initialization("CDSPreimage must point to a single file", CDSPreimage);
    }

    if (is_dumping_static_archive()) {
      assert(archives == 1, "must be");
      // Static dump is simple: only one archive is allowed in SharedArchiveFile. This file
      // will be overwritten no matter regardless of its contents
      _static_archive_path = os::strdup_check_oom(SharedArchiveFile, mtArguments);
    } else {
      // SharedArchiveFile may specify one or two files. In case (c), the path for base.jsa
      // is read from top.jsa
      //    (a) 1 file:  -XX:SharedArchiveFile=base.jsa
      //    (b) 2 files: -XX:SharedArchiveFile=base.jsa:top.jsa
      //    (c) 2 files: -XX:SharedArchiveFile=top.jsa
      //
      // However, if either RecordDynamicDumpInfo or ArchiveClassesAtExit is used, we do not
      // allow cases (b) and (c). Case (b) is already checked above.

      if (archives > 2) {
        vm_exit_during_initialization(
          "Cannot have more than 2 archive files specified in the -XX:SharedArchiveFile option");
      }
      if (archives == 1) {
        char* base_archive_path = nullptr;
        bool success =
          FileMapInfo::get_base_archive_name_from_header(SharedArchiveFile, &base_archive_path);
        if (!success) {
          if (CDSPreimage != nullptr) {
            vm_exit_during_initialization("Unable to map shared spaces from CDSPreimage", CDSPreimage);
          }

          // If +AutoCreateSharedArchive and the specified shared archive does not exist,
          // regenerate the dynamic archive base on default archive.
          if (AutoCreateSharedArchive && !os::file_exists(SharedArchiveFile)) {
            enable_dumping_dynamic_archive();
            ArchiveClassesAtExit = const_cast<char *>(SharedArchiveFile);
            _static_archive_path = default_archive_path();
            SharedArchiveFile = nullptr;
          } else {
            if (AutoCreateSharedArchive) {
              warning("-XX:+AutoCreateSharedArchive is unsupported when base CDS archive is not loaded. Run with -Xlog:cds for more info.");
              AutoCreateSharedArchive = false;
            }
            Arguments::no_shared_spaces("invalid archive");
          }
        } else if (base_archive_path == nullptr) {
          // User has specified a single archive, which is a static archive.
          _static_archive_path = const_cast<char *>(SharedArchiveFile);
        } else {
          // User has specified a single archive, which is a dynamic archive.
          _dynamic_archive_path = const_cast<char *>(SharedArchiveFile);
          _static_archive_path = base_archive_path; // has been c-heap allocated.
        }
      } else {
        extract_shared_archive_paths((const char*)SharedArchiveFile,
                                      &_static_archive_path, &_dynamic_archive_path);
        if (_static_archive_path == nullptr) {
          assert(_dynamic_archive_path == nullptr, "must be");
          Arguments::no_shared_spaces("invalid archive");
        }
      }

      if (_dynamic_archive_path != nullptr) {
        // Check for case (c)
        if (RecordDynamicDumpInfo) {
          vm_exit_during_initialization("-XX:+RecordDynamicDumpInfo is unsupported when a dynamic CDS archive is specified in -XX:SharedArchiveFile",
                                        SharedArchiveFile);
        }
        if (ArchiveClassesAtExit != nullptr) {
          vm_exit_during_initialization("-XX:ArchiveClassesAtExit is unsupported when a dynamic CDS archive is specified in -XX:SharedArchiveFile",
                                        SharedArchiveFile);
        }
      }

      if (ArchiveClassesAtExit != nullptr && os::same_files(SharedArchiveFile, ArchiveClassesAtExit)) {
          vm_exit_during_initialization(
            "Cannot have the same archive file specified for -XX:SharedArchiveFile and -XX:ArchiveClassesAtExit",
            SharedArchiveFile);
      }
    }
  }
}

static char* bad_module_prop_key   = nullptr;
static char* bad_module_prop_value = nullptr;

void CDSConfig::check_internal_module_property(const char* key, const char* value) {
  if (Arguments::is_internal_module_property(key)) {
    stop_using_optimized_module_handling();
    if (bad_module_prop_key == nullptr) {
      // We don't want to print an unconditional warning here, as we are still processing the command line.
      // A later argument may specify something like -Xshare:off, which makes such a warning irrelevant.
      //
      // Instead, we save the info so we can warn when necessary: we are doing it only during CacheDataStore
      // creation for now, but could add it to other places.
      bad_module_prop_key   = os::strdup(key);
      bad_module_prop_value = os::strdup(value);
    }
    log_info(cds)("optimized module handling/full module graph: disabled due to incompatible property: %s=%s", key, value);
  }
}

void CDSConfig::check_incompatible_property(const char* key, const char* value) {
  static const char* incompatible_properties[] = {
    "java.system.class.loader",
    "jdk.module.showModuleResolution",
    "jdk.module.validation"
  };

  for (const char* property : incompatible_properties) {
    if (strcmp(key, property) == 0) {
      stop_dumping_full_module_graph();
      stop_using_full_module_graph();
      log_info(cds)("full module graph: disabled due to incompatible property: %s=%s", key, value);
      break;
    }
  }

  // Match the logic in java/lang/System.java, but we need to know this before the System class is initialized.
  if (strcmp(key, "java.security.manager") == 0) {
    if (strcmp(value, "disallowed") != 0) {
      _is_security_manager_allowed = true;
    }
  }
}

// Returns any JVM command-line option, such as "--patch-module", that's not supported by CDS.
static const char* find_any_unsupported_module_option() {
  // Note that arguments.cpp has translated the command-line options into properties. If we find an
  // unsupported property, translate it back to its command-line option for better error reporting.

  // The following properties are checked by Arguments::is_internal_module_property() and cannot be
  // directly specified in the command-line.
  static const char* unsupported_module_properties[] = {
    "jdk.module.limitmods",
    "jdk.module.upgrade.path",
    "jdk.module.patch.0"
  };
  static const char* unsupported_module_options[] = {
    "--limit-modules",
    "--upgrade-module-path",
    "--patch-module"
  };

  assert(ARRAY_SIZE(unsupported_module_properties) == ARRAY_SIZE(unsupported_module_options), "must be");
  SystemProperty* sp = Arguments::system_properties();
  while (sp != nullptr) {
    for (uint i = 0; i < ARRAY_SIZE(unsupported_module_properties); i++) {
      if (strcmp(sp->key(), unsupported_module_properties[i]) == 0) {
        return unsupported_module_options[i];
      }
    }
    sp = sp->next();
  }

  return nullptr; // not found
}

void CDSConfig::check_unsupported_dumping_module_options() {
  assert(is_dumping_archive(), "this function is only used with CDS dump time");
  const char* option = find_any_unsupported_module_option();
  if (option != nullptr) {
    vm_exit_during_initialization("Cannot use the following option when dumping the shared archive", option);
  }
  // Check for an exploded module build in use with -Xshare:dump.
  if (!Arguments::has_jimage()) {
    vm_exit_during_initialization("Dumping the shared archive is not supported with an exploded module build");
  }
}

bool CDSConfig::has_unsupported_runtime_module_options() {
  assert(is_using_archive(), "this function is only used with -Xshare:{on,auto}");
  if (ArchiveClassesAtExit != nullptr) {
    // dynamic dumping, just return false for now.
    // check_unsupported_dumping_properties() will be called later to check the same set of
    // properties, and will exit the VM with the correct error message if the unsupported properties
    // are used.
    return false;
  }
  const char* option = find_any_unsupported_module_option();
  if (option != nullptr) {
    if (RequireSharedSpaces) {
      warning("CDS is disabled when the %s option is specified.", option);
    } else {
      log_info(cds)("CDS is disabled when the %s option is specified.", option);
    }
    return true;
  }
  return false;
}

#define CHECK_ALIAS(f) check_flag_alias(FLAG_IS_DEFAULT(f), #f)

void CDSConfig::check_flag_alias(bool alias_is_default, const char* alias_name) {
  if (_old_cds_flags_used && !alias_is_default) {
    vm_exit_during_initialization(err_msg("Option %s cannot be used at the same time with "
                                          "-Xshare:on, -Xshare:auto, -Xshare:off, -Xshare:dump, "
                                          "DumpLoadedClassList, SharedClassListFile, or SharedArchiveFile",
                                          alias_name));
  }
}

void CDSConfig::check_flag_aliases() {
  if (!FLAG_IS_DEFAULT(DumpLoadedClassList) ||
      !FLAG_IS_DEFAULT(SharedClassListFile) ||
      !FLAG_IS_DEFAULT(SharedArchiveFile)) {
    _old_cds_flags_used = true;
  }

  CHECK_ALIAS(AOTCache);
  CHECK_ALIAS(AOTConfiguration);
  CHECK_ALIAS(AOTMode);

  if (FLAG_IS_DEFAULT(AOTCache) && FLAG_IS_DEFAULT(AOTConfiguration) && FLAG_IS_DEFAULT(AOTMode)) {
    // Aliases not used.
    return;
  }

  if (FLAG_IS_DEFAULT(AOTMode) || strcmp(AOTMode, "auto") == 0 || strcmp(AOTMode, "on") == 0) {
    if (!FLAG_IS_DEFAULT(AOTConfiguration)) {
      vm_exit_during_initialization("AOTConfiguration can only be used with -XX:AOTMode=record or -XX:AOTMode=create");
    }

    if (!FLAG_IS_DEFAULT(AOTCache)) {
      assert(FLAG_IS_DEFAULT(SharedArchiveFile), "already checked");
      FLAG_SET_ERGO(SharedArchiveFile, AOTCache);
    }

    UseSharedSpaces = true;
    if (FLAG_IS_DEFAULT(AOTMode) || (strcmp(AOTMode, "auto") == 0)) {
      RequireSharedSpaces = false;
    } else {
      assert(strcmp(AOTMode, "on") == 0, "already checked");
      RequireSharedSpaces = true;
    }
  } else if (strcmp(AOTMode, "off") == 0) {
    UseSharedSpaces = false;
    RequireSharedSpaces = false;
  } else {
    // AOTMode is record or create
    if (FLAG_IS_DEFAULT(AOTConfiguration)) {
      vm_exit_during_initialization(err_msg("-XX:AOTMode=%s cannot be used without setting AOTConfiguration", AOTMode));
    }

    if (strcmp(AOTMode, "record") == 0) {
      if (!FLAG_IS_DEFAULT(AOTCache)) {
        vm_exit_during_initialization("AOTCache must not be specified when using -XX:AOTMode=record");
      }

      assert(FLAG_IS_DEFAULT(DumpLoadedClassList), "already checked");
      FLAG_SET_ERGO(DumpLoadedClassList, AOTConfiguration);
      UseSharedSpaces = false;
      RequireSharedSpaces = false;
    } else {
      assert(strcmp(AOTMode, "create") == 0, "checked by AOTModeConstraintFunc");
      if (FLAG_IS_DEFAULT(AOTCache)) {
        vm_exit_during_initialization("AOTCache must be specified when using -XX:AOTMode=create");
      }

      assert(FLAG_IS_DEFAULT(SharedClassListFile), "already checked");
      FLAG_SET_ERGO(SharedClassListFile, AOTConfiguration);
      assert(FLAG_IS_DEFAULT(SharedArchiveFile), "already checked");
      FLAG_SET_ERGO(SharedArchiveFile, AOTCache);

      CDSConfig::enable_dumping_static_archive();
    }
  }
}

bool CDSConfig::check_vm_args_consistency(bool patch_mod_javabase, bool mode_flag_cmd_line, bool xshare_auto_cmd_line) {
  check_flag_aliases();

  if (CacheDataStore != nullptr) {
    // Leyden temp work-around:
    //
    // By default, when using CacheDataStore, use the HeapBasedNarrowOop mode so that
    // AOT code can be always work regardless of runtime heap range.
    //
    // If you are *absolutely sure* that the CompressedOops::mode() will be the same
    // between training and production runs (e.g., if you specify -Xmx128m
    // for both training and production runs, and you know the OS will always reserve
    // the heap under 4GB), you can explicitly disable this with:
    //     java -XX:-UseCompatibleCompressedOops -XX:CacheDataStore=...
    // However, this is risky and there's a chance that the production run will be slower
    // because it is unable to load the AOT code cache.
#ifdef _LP64
    FLAG_SET_ERGO_IF_DEFAULT(UseCompatibleCompressedOops, true);
#endif

    // Leyden temp: make sure the user knows if CDS archive somehow fails to load.
    if (UseSharedSpaces && !xshare_auto_cmd_line) {
      log_info(cds)("Enabled -Xshare:on by default for troubleshooting Leyden prototype");
      RequireSharedSpaces = true;
    }

    if (FLAG_IS_DEFAULT(AOTClassLinking)) {
      // New workflow - enable AOTClassLinking by default.
      // TODO: make new workflow work, even when AOTClassLinking is false.
      //
      // NOTE: in old workflow, we cannot enable AOTClassLinking by default. That
      // should be an opt-in option, per JEP nnn.
      FLAG_SET_ERGO(AOTClassLinking, true);
    }

    if (SharedArchiveFile != nullptr) {
      vm_exit_during_initialization("CacheDataStore and SharedArchiveFile cannot be both specified");
    }
    if (!AOTClassLinking) {
      // TODO: in the forked JVM, we should ensure all classes are loaded from the hotspot.cds.preimage.
      // AOTClassLinking only loads the classes for built-in loaders. We need to load the classes
      // for custom loaders as well.
      vm_exit_during_initialization("CacheDataStore requires AOTClassLinking");
    }

    if (CDSPreimage == nullptr) {
      if (os::file_exists(CacheDataStore) /* && TODO: CDS file is valid*/) {
        // The CacheDataStore is already up to date. Use it. Also turn on cached code by default.
        SharedArchiveFile = CacheDataStore;
        FLAG_SET_ERGO_IF_DEFAULT(ReplayTraining, true);
        FLAG_SET_ERGO_IF_DEFAULT(LoadCachedCode, true);
        if (LoadCachedCode && FLAG_IS_DEFAULT(CachedCodeFile)) {
          set_new_workflow_default_CachedCodeFile();
        }
      } else {
        // The preimage dumping phase -- run the app and write the preimage file
        size_t len = strlen(CacheDataStore) + 10;
        char* preimage = AllocateHeap(len, mtArguments);
        jio_snprintf(preimage, len, "%s.preimage", CacheDataStore);

        UseSharedSpaces = false;
        enable_dumping_static_archive();
        SharedArchiveFile = preimage;
        log_info(cds)("CacheDataStore needs to be updated. Writing %s file", SharedArchiveFile);

        // At VM exit, the module graph may be contaminated with program states. We should rebuild the
        // module graph when dumping the CDS final image.
        log_info(cds)("full module graph: disabled when writing CDS preimage");
        HeapShared::disable_writing();
        stop_dumping_full_module_graph();
        FLAG_SET_ERGO(ArchivePackages, false);
        FLAG_SET_ERGO(ArchiveProtectionDomains, false);

        FLAG_SET_ERGO_IF_DEFAULT(RecordTraining, true);
      }
    } else {
      // The final image dumping phase -- load the preimage and write the final image file
      SharedArchiveFile = CDSPreimage;
      UseSharedSpaces = true;
      log_info(cds)("Generate CacheDataStore %s from CDSPreimage %s", CacheDataStore, CDSPreimage);
      // Force -Xbatch for AOT compilation.
      if (FLAG_SET_CMDLINE(BackgroundCompilation, false) != JVMFlag::SUCCESS) {
        return false;
      }
      RecordTraining = false; // This will be updated inside MetaspaceShared::preload_and_dump()

      FLAG_SET_ERGO_IF_DEFAULT(ReplayTraining, true);
      // Settings for AOT
      FLAG_SET_ERGO_IF_DEFAULT(StoreCachedCode, true);
      if (StoreCachedCode && FLAG_IS_DEFAULT(CachedCodeFile)) {
        set_new_workflow_default_CachedCodeFile();
        // Cannot dump cached code until metadata and heap are dumped.
        disable_dumping_cached_code();
      }
    }
  } else {
    // Old workflow
    if (CDSPreimage != nullptr) {
      vm_exit_during_initialization("CDSPreimage must be specified only when CacheDataStore is specified");
    }
  }

  if (FLAG_IS_DEFAULT(UsePermanentHeapObjects)) {
    if (StoreCachedCode || AOTClassLinking) {
      FLAG_SET_ERGO(UsePermanentHeapObjects, true);
    }
  }

  if (LoadCachedCode) {
    // This must be true. Cached code is hard-wired to use permanent objects.
    UsePermanentHeapObjects = true;
  }

  if (AOTClassLinking) {
    // If AOTClassLinking is specified, enable all these optimizations by default.
    FLAG_SET_ERGO_IF_DEFAULT(AOTInvokeDynamicLinking, true);
    FLAG_SET_ERGO_IF_DEFAULT(ArchiveDynamicProxies, true);
    FLAG_SET_ERGO_IF_DEFAULT(ArchiveLoaderLookupCache, true);
    FLAG_SET_ERGO_IF_DEFAULT(ArchivePackages, true);
    FLAG_SET_ERGO_IF_DEFAULT(ArchiveProtectionDomains, true);
    FLAG_SET_ERGO_IF_DEFAULT(ArchiveReflectionData, true);
  } else {
    // All of these *might* depend on AOTClassLinking. Better be safe than sorry.
    // TODO: more fine-grained handling.
    FLAG_SET_ERGO(AOTInvokeDynamicLinking, false);
    FLAG_SET_ERGO(ArchiveDynamicProxies, false);
    FLAG_SET_ERGO(ArchiveLoaderLookupCache, false);
    FLAG_SET_ERGO(ArchivePackages, false);
    FLAG_SET_ERGO(ArchiveProtectionDomains, false);
    FLAG_SET_ERGO(ArchiveReflectionData, false);
  }

#ifdef _WINDOWS
  // This optimization is not working on Windows for some reason. See JDK-8338604.
  FLAG_SET_ERGO(ArchiveReflectionData, false);
#endif

  if (is_dumping_static_archive()) {
    if (is_dumping_preimage_static_archive() || is_dumping_final_static_archive()) {
      // Don't tweak execution mode
    } else if (!mode_flag_cmd_line) {
      // By default, -Xshare:dump runs in interpreter-only mode, which is required for deterministic archive.
      //
      // If your classlist is large and you don't care about deterministic dumping, you can use
      // -Xshare:dump -Xmixed to improve dumping speed.
      Arguments::set_mode_flags(Arguments::_int);
    } else if (Arguments::mode() == Arguments::_comp) {
      // -Xcomp may use excessive CPU for the test tiers. Also, -Xshare:dump runs a small and fixed set of
      // Java code, so there's not much benefit in running -Xcomp.
      log_info(cds)("reduced -Xcomp to -Xmixed for static dumping");
      Arguments::set_mode_flags(Arguments::_mixed);
    }

    // String deduplication may cause CDS to iterate the strings in different order from one
    // run to another which resulting in non-determinstic CDS archives.
    // Disable UseStringDeduplication while dumping CDS archive.
    UseStringDeduplication = false;

    // Don't use SoftReferences so that some java.lang.invoke tables can be archived.
    Arguments::PropertyList_add(new SystemProperty("java.lang.invoke.MethodHandle.NO_SOFT_CACHE", "true", false));
  }

  // RecordDynamicDumpInfo is not compatible with ArchiveClassesAtExit
  if (ArchiveClassesAtExit != nullptr && RecordDynamicDumpInfo) {
    jio_fprintf(defaultStream::output_stream(),
                "-XX:+RecordDynamicDumpInfo cannot be used with -XX:ArchiveClassesAtExit.\n");
    return false;
  }

  if (ArchiveClassesAtExit == nullptr && !RecordDynamicDumpInfo) {
    disable_dumping_dynamic_archive();
  } else {
    enable_dumping_dynamic_archive();
  }

  if (AutoCreateSharedArchive) {
    if (SharedArchiveFile == nullptr) {
      log_warning(cds)("-XX:+AutoCreateSharedArchive requires -XX:SharedArchiveFile");
      return false;
    }
    if (ArchiveClassesAtExit != nullptr) {
      log_warning(cds)("-XX:+AutoCreateSharedArchive does not work with ArchiveClassesAtExit");
      return false;
    }
  }

  if (is_using_archive() && patch_mod_javabase) {
    Arguments::no_shared_spaces("CDS is disabled when " JAVA_BASE_NAME " module is patched.");
  }
  if (is_using_archive() && has_unsupported_runtime_module_options()) {
    UseSharedSpaces = false;
  }

  if (is_dumping_archive()) {
    // Always verify non-system classes during CDS dump
    if (!BytecodeVerificationRemote) {
      BytecodeVerificationRemote = true;
      log_info(cds)("All non-system classes will be verified (-Xverify:remote) during CDS dump time.");
    }
  }

  if (AOTClassLinking) {
    if ((is_dumping_preimage_static_archive() && !is_using_optimized_module_handling()) ||
        (is_dumping_final_static_archive()    && !is_dumping_full_module_graph())) {
      if (bad_module_prop_key != nullptr) {
        log_warning(cds)("optimized module handling/full module graph: disabled due to incompatible property: %s=%s",
                         bad_module_prop_key, bad_module_prop_value);
      }
      vm_exit_during_initialization("CacheDataStore cannot be created because AOTClassLinking is enabled but full module graph is disabled");
    }
  }

  return true;
}

bool CDSConfig::is_dumping_classic_static_archive() {
  return _is_dumping_static_archive && CacheDataStore == nullptr && CDSPreimage == nullptr;
}

bool CDSConfig::is_dumping_preimage_static_archive() {
  return _is_dumping_static_archive && CacheDataStore != nullptr && CDSPreimage == nullptr;
}

bool CDSConfig::is_dumping_preimage_static_archive_with_triggers() {
  return _is_dumping_static_archive && CacheDataStore != nullptr && CDSPreimage == nullptr && !FLAG_IS_DEFAULT(AOTEndTrainingOnMethodEntry);
}

bool CDSConfig::is_dumping_final_static_archive() {
  if (CDSPreimage != nullptr) {
    assert(CacheDataStore != nullptr, "must be"); // should have been properly initialized by arguments.cpp
  }

  // Note: _is_dumping_static_archive is false! // FIXME -- refactor this so it makes more sense!
  return CacheDataStore != nullptr && CDSPreimage != nullptr;
}

bool CDSConfig::allow_only_single_java_thread() {
  // See comments in JVM_StartThread()
  return is_dumping_classic_static_archive() || is_dumping_final_static_archive();
}

bool CDSConfig::is_dumping_regenerated_lambdaform_invokers() {
  if (is_dumping_final_static_archive()) {
    // Not yet supported in new workflow -- the training data may point
    // to a method in a lambdaform holder class that was not regenerated
    // due to JDK-8318064.
    return false;
  } else {
    return is_dumping_archive();
  }
}

bool CDSConfig::is_logging_dynamic_proxies() {
  return ClassListWriter::is_enabled() || is_dumping_preimage_static_archive();
}

// Preserve all states that were examined used during dumptime verification, such
// that the verification result (pass or fail) cannot be changed at runtime.
//
// For example, if the verification of ik requires that class A must be a subtype of B,
// then this relationship between A and B cannot be changed at runtime. I.e., the app
// cannot load alternative versions of A and B such that A is not a subtype of B.
bool CDSConfig::preserve_all_dumptime_verification_states(const InstanceKlass* ik) {
  return is_dumping_aot_linked_classes() && SystemDictionaryShared::is_builtin(ik);
}

bool CDSConfig::is_using_archive() {
  return UseSharedSpaces;
}

bool CDSConfig::is_logging_lambda_form_invokers() {
  return ClassListWriter::is_enabled() || is_dumping_dynamic_archive() || is_dumping_preimage_static_archive();
}

void CDSConfig::stop_using_optimized_module_handling() {
  _is_using_optimized_module_handling = false;
  _is_dumping_full_module_graph = false; // This requires is_using_optimized_module_handling()
  _is_using_full_module_graph = false; // This requires is_using_optimized_module_handling()
}

#if INCLUDE_CDS_JAVA_HEAP
bool CDSConfig::is_dumping_heap() {
  return is_dumping_static_archive() && !is_dumping_preimage_static_archive()
    && HeapShared::can_write();
}

bool CDSConfig::is_loading_heap() {
  return ArchiveHeapLoader::is_in_use();
}

bool CDSConfig::is_using_full_module_graph() {
  if (ClassLoaderDataShared::is_full_module_graph_loaded()) {
    return true;
  }

  if (!_is_using_full_module_graph) {
    return false;
  }

  if (is_using_archive() && ArchiveHeapLoader::can_use()) {
    // Classes used by the archived full module graph are loaded in JVMTI early phase.
    assert(!(JvmtiExport::should_post_class_file_load_hook() && JvmtiExport::has_early_class_hook_env()),
           "CDS should be disabled if early class hooks are enabled");
    return true;
  } else {
    _is_using_full_module_graph = false;
    return false;
  }
}

void CDSConfig::stop_dumping_full_module_graph(const char* reason) {
  if (_is_dumping_full_module_graph) {
    _is_dumping_full_module_graph = false;
    if (reason != nullptr) {
      log_info(cds)("full module graph cannot be dumped: %s", reason);
    }
  }
}

void CDSConfig::stop_using_full_module_graph(const char* reason) {
  assert(!ClassLoaderDataShared::is_full_module_graph_loaded(), "you call this function too late!");
  if (_is_using_full_module_graph) {
    _is_using_full_module_graph = false;
    if (reason != nullptr) {
      log_info(cds)("full module graph cannot be loaded: %s", reason);
    }
  }
}

bool CDSConfig::is_dumping_aot_linked_classes() {
  if (is_dumping_preimage_static_archive()) {
    return AOTClassLinking;
  } else if (is_dumping_dynamic_archive()) {
    return is_using_full_module_graph() && AOTClassLinking;
  } else if (is_dumping_static_archive()) {
    return is_dumping_full_module_graph() && AOTClassLinking;
  } else {
    return false;
  }
}

bool CDSConfig::is_using_aot_linked_classes() {
  if (is_dumping_final_static_archive()) {
    // We assume that the final image is being dumped with the exact same module graph as the training run,
    // so all aot-linked classes can be loaded.
    return _has_aot_linked_classes;
  }
  // Make sure we have the exact same module graph as in the assembly phase, or else
  // some aot-linked classes may not be visible so cannot be loaded.
  return is_using_full_module_graph() && _has_aot_linked_classes;
}

void CDSConfig::set_has_aot_linked_classes(bool is_static_archive, bool has_aot_linked_classes) {
  _has_aot_linked_classes |= has_aot_linked_classes;
}

bool CDSConfig::is_loading_invokedynamic() {
  return UseSharedSpaces && is_using_full_module_graph() && _has_archived_invokedynamic;
}

bool CDSConfig::is_dumping_dynamic_proxies() {
  return is_dumping_full_module_graph() && is_dumping_invokedynamic() && ArchiveDynamicProxies;
}

// NOTE: do not upstream this to mainline yet.
bool CDSConfig::is_initing_classes_at_dump_time() {
  return is_dumping_heap() && is_dumping_aot_linked_classes();
}

bool CDSConfig::is_dumping_invokedynamic() {
  // Requires is_dumping_aot_linked_classes(). Otherwise the classes of some archived heap
  // objects used by the archive indy callsites may be replaced at runtime.
  return AOTInvokeDynamicLinking && is_dumping_aot_linked_classes() && is_dumping_heap();
}

bool CDSConfig::is_dumping_packages() {
  return ArchivePackages && is_dumping_heap();
}

bool CDSConfig::is_loading_packages() {
  return UseSharedSpaces && is_using_full_module_graph() && _is_loading_packages;
}

bool CDSConfig::is_dumping_protection_domains() {
  if (_is_security_manager_allowed) {
    // For sanity, don't archive PDs. TODO: can this be relaxed?
    return false;
  }
  // Archived PDs for the modules will reference their java.lang.Module, which must
  // also be archived.
  return ArchiveProtectionDomains && is_dumping_full_module_graph();
}

bool CDSConfig::is_loading_protection_domains() {
  if (_is_security_manager_allowed) {
    // For sanity, don't used any archived PDs. TODO: can this be relaxed?
    return false;
  }
  return UseSharedSpaces && is_using_full_module_graph() && _is_loading_protection_domains;
}

bool CDSConfig::is_dumping_reflection_data() {
  // reflection data use LambdaForm classes
  return ArchiveReflectionData && is_dumping_invokedynamic();
}

#endif // INCLUDE_CDS_JAVA_HEAP

// This is allowed by default. We disable it only in the final image dump before the
// metadata and heap are dumped.
static bool _is_dumping_cached_code = true;

bool CDSConfig::is_dumping_cached_code() {
  return _is_dumping_cached_code;
}

void CDSConfig::disable_dumping_cached_code() {
  _is_dumping_cached_code = false;
}

void CDSConfig::enable_dumping_cached_code() {
  _is_dumping_cached_code = true;
}
