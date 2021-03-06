/* -*- Mode: C; c-basic-offset:2 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2016 Los Alamos National Security, LLC.  All rights
 *                         reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#include "builtin-posix_component.h"

#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdarg.h>
#include <ftw.h>
#include <assert.h>

#include <string.h>

#if defined(HAVE_STRINGS_H)
#include <strings.h>
#endif

#include <errno.h>

#include <dirent.h>
#include <unistd.h>

#if defined(HAVE_SYS_STAT_H)
#include <sys/stat.h>
#endif


static hio_var_enum_value_t hioi_dataset_file_mode_values[] = {
  {.string_value = "basic", .value = HIO_FILE_MODE_BASIC},
  {.string_value = "file_per_node", .value = HIO_FILE_MODE_OPTIMIZED},
  {.string_value = "strided", .value = HIO_FILE_MODE_STRIDED},
};

static hio_var_enum_t hioi_dataset_file_modes = {
  .count  = 3,
  .values = hioi_dataset_file_mode_values,
};

/** static functions */
static int builtin_posix_module_dataset_unlink (struct hio_module_t *module, const char *name, int64_t set_id);
static int builtin_posix_module_dataset_close (hio_dataset_t dataset);
static int builtin_posix_module_element_open (hio_dataset_t dataset, hio_element_t element);
static int builtin_posix_module_element_close (hio_element_t element);
static int builtin_posix_module_element_flush (hio_element_t element, hio_flush_mode_t mode);
static int builtin_posix_module_element_complete (hio_element_t element);
static int builtin_posix_module_process_reqs (hio_dataset_t dataset, hio_internal_request_t **reqs, int req_count);


static void builtin_posix_trace (builtin_posix_module_dataset_t *posix_dataset, const char *event,
                                 int64_t value, uint64_t value2, uint64_t start, uint64_t stop) {
  if (NULL == posix_dataset->ds_trace_fh) {
    return;
  }

  fprintf (posix_dataset->ds_trace_fh, "%s::%" PRId64 ":%s:%" PRIu64 ":%" PRIu64 ":%" PRIu64 ":%" PRIu64 ":%" PRIu64 "\n",
           hioi_object_identifier (&posix_dataset->base), posix_dataset->base.ds_id, event, value, value2, start, stop,
           stop - start);
}

#define POSIX_TRACE_CALL(ds, c, e, v, v2)                       \
  do {                                                          \
    uint64_t _start, _stop;                                     \
    _start = hioi_gettime ();                                   \
    c;                                                          \
    _stop = hioi_gettime ();                                    \
    builtin_posix_trace ((ds), e, v, v2, _start, _stop);        \
  } while (0)

static int builtin_posix_dataset_path (struct hio_module_t *module, char **path, const char *name, uint64_t set_id) {
  hio_context_t context = module->context;
  int rc;

  rc = asprintf (path, "%s/%s.hio/%s/%lu", module->data_root, hioi_object_identifier(context), name,
                 (unsigned long) set_id);
  return (0 > rc) ? hioi_err_errno (errno) : HIO_SUCCESS;
}

static int builtin_posix_create_dataset_dirs (builtin_posix_module_t *posix_module, builtin_posix_module_dataset_t *posix_dataset) {
  mode_t access_mode = posix_module->access_mode;
  hio_context_t context = posix_module->base.context;
  char *path;
  int rc;

  if (context->c_rank > 0) {
    return HIO_SUCCESS;
  }

  /* create the data directory*/
  hioi_log (context, HIO_VERBOSE_DEBUG_MED, "posix: creating dataset directory @ %s", posix_dataset->base_path);

  rc = asprintf (&path, "%s/data", posix_dataset->base_path);
  if (0 > rc) {
    return hioi_err_errno (errno);
  }

  rc = hioi_mkpath (context, path, access_mode);
  if (0 > rc || EEXIST == errno) {
    if (EEXIST != errno) {
      hioi_err_push (hioi_err_errno (errno), &context->c_object, "posix: error creating context directory: %s",
                    path);
    }
    free (path);

    return hioi_err_errno (errno);
  }

  /* set striping parameters on the directory */
  if (posix_dataset->base.ds_fsattr.fs_flags & HIO_FS_SUPPORTS_STRIPING) {
    rc = hioi_fs_set_stripe (path, &posix_dataset->base.ds_fsattr);
    if (HIO_SUCCESS != rc) {
      hioi_log (context, HIO_VERBOSE_DEBUG_LOW, "posix: could not set file system striping on %s", path);
    }
  }

  free (path);

  /* create trace directory if requested */
  if (context->c_enable_tracing) {
    rc = asprintf (&path, "%s/trace", posix_dataset->base_path);
    if (0 > rc) {
      return hioi_err_errno (errno);
    }

    rc = hioi_mkpath (context, path, access_mode);
    if (0 > rc || EEXIST == errno) {
      if (EEXIST != errno) {
        hioi_err_push (hioi_err_errno (errno), &context->c_object, "posix: error creating context directory: %s",
                       path);
      }
      free (path);

      return hioi_err_errno (errno);
    }
  }

  hioi_log (context, HIO_VERBOSE_DEBUG_LOW, "posix: successfully created dataset directories %s", posix_dataset->base_path);

  return HIO_SUCCESS;
}

static int builtin_posix_module_dataset_list (struct hio_module_t *module, const char *name,
                                              hio_dataset_header_t **headers, int *count) {
  hio_context_t context = module->context;
  int num_set_ids = 0, set_id_index = 0;
  int rc = HIO_SUCCESS;
  struct dirent *dp;
  char *path = NULL;
  DIR *dir;

  *headers = NULL;
  *count = 0;

  do {
    if (0 != context->c_rank) {
      break;
    }

    rc = asprintf (&path, "%s/%s.hio/%s", module->data_root, hioi_object_identifier(context), name);
    assert (0 <= rc);

    dir = opendir (path);
    if (NULL == dir) {
      num_set_ids = 0;
      break;
    }

    while (NULL != (dp = readdir (dir))) {
      if (dp->d_name[0] != '.') {
        num_set_ids++;
      }
    }

    if (0 == num_set_ids) {
      break;
    }

    *headers = (hio_dataset_header_t *) calloc (num_set_ids, sizeof (**headers));
    assert (NULL != *headers);

    rewinddir (dir);

    while (NULL != (dp = readdir (dir))) {
      if ('.' == dp->d_name[0]) {
        continue;
      }

      char *manifest_path;

      rc = asprintf (&manifest_path, "%s/%s/manifest.json.bz2", path, dp->d_name);
      assert (0 <= rc);

      rc = hioi_manifest_read_header (context, headers[0] + set_id_index, manifest_path);
      if (HIO_SUCCESS == rc) {
        ++set_id_index;
      } else {
        free (manifest_path);
        rc = asprintf (&manifest_path, "%s/%s/manifest.json", path, dp->d_name);
        assert (0 <= rc);

        rc = hioi_manifest_read_header (context, headers[0] + set_id_index, manifest_path);
        if (HIO_SUCCESS == rc) {
          ++set_id_index;
        } else {
          /* skip dataset */
          hioi_log (context, HIO_VERBOSE_WARN, "posix:dataset_list: could not read manifest at path: %s. rc: %d",
                    manifest_path, rc);
        }
      }

      free (manifest_path);
    }

    num_set_ids = set_id_index;
  } while (0);

#if HIO_MPI_HAVE(1)
  if (hioi_context_using_mpi (context)) {
    MPI_Bcast (&num_set_ids, 1, MPI_INT, 0, context->c_comm);
  }
#endif

  if (0 == context->c_rank) {
    closedir (dir);
    free (path);
  }

  if (0 == num_set_ids) {
    free (*headers);
    *headers = NULL;

    return HIO_SUCCESS;
  }

  if (0 != context->c_rank) {
    *headers = (hio_dataset_header_t *) calloc (num_set_ids, sizeof (**headers));
    assert (NULL != *headers);
  }

#if HIO_MPI_HAVE(1)
  if (hioi_context_using_mpi (context)) {
    MPI_Bcast (*headers, sizeof (**headers) * num_set_ids, MPI_BYTE, 0, context->c_comm);
  }
#endif

  *count = num_set_ids;

  return HIO_SUCCESS;
}

static int manifest_index_compare (const void *a, const void *b) {
  int ia = ((int *) a)[0], ib = ((int *) b)[0];
  return ia - ib;
}

#if HIO_MPI_HAVE(3)
static int builtin_posix_module_dataset_manifest_list (builtin_posix_module_dataset_t *posix_dataset, int **manifest_ids, size_t *count) {
  hio_context_t context = hioi_object_context (&posix_dataset->base.ds_object);
  int num_manifest_ids = 0, manifest_id_index = 0;
  unsigned int manifest_id;
  int rc = HIO_SUCCESS;
  int *tmp = NULL;
  struct dirent *dp;
  DIR *dir;

  *manifest_ids = NULL;
  *count = 0;

  rc = hioi_context_generate_leader_list (context);
  if (HIO_SUCCESS != rc) {
    return rc;
  }

  if (0 != context->c_shared_rank) {
    return HIO_SUCCESS;
  }

  do {
    if (0 != context->c_rank) {
      break;
    }

    dir = opendir (posix_dataset->base_path);
    if (NULL == dir) {
      num_manifest_ids = hioi_err_errno (errno);
      break;
    }

    while (NULL != (dp = readdir (dir))) {
      if (dp->d_name[0] != '.' && 0 != sscanf (dp->d_name, "manifest.%x.json", &manifest_id)) {
        ++num_manifest_ids;
      }
    }

    if (0 == num_manifest_ids) {
      break;
    }

    /* round up to a multiple of the number nodes */
    num_manifest_ids = context->c_node_count * (num_manifest_ids + context->c_node_count - 1) / context->c_node_count;

    tmp = (int *) malloc (num_manifest_ids * sizeof (int));
    assert (NULL != tmp);
    memset (tmp, 0xff, sizeof (int) * num_manifest_ids);

    rewinddir (dir);

    while (NULL != (dp = readdir (dir))) {
      if ('.' == dp->d_name[0] || 0 == sscanf (dp->d_name, "manifest.%x.json", &manifest_id)) {
        continue;
      }

      tmp[manifest_id_index++] = (int) manifest_id;
    }

    /* put manifest files in numerical order */
    qsort (tmp, manifest_id_index, sizeof (int), manifest_index_compare);
  } while (0);

  if (0 == context->c_rank) {
    closedir (dir);
  }

  if (num_manifest_ids > 0) {
    num_manifest_ids /= context->c_node_count;
  }

  MPI_Bcast (&num_manifest_ids, 1, MPI_INT, 0, context->c_node_leader_comm);

  if (0 < num_manifest_ids) {
    *manifest_ids = (int *) malloc (num_manifest_ids * sizeof (int));
    assert (NULL != *manifest_ids);

    MPI_Scatter (tmp, num_manifest_ids, MPI_INT, *manifest_ids, num_manifest_ids, MPI_INT,
                 0, context->c_node_leader_comm);
  }

  free (tmp);

  *count = num_manifest_ids;

  return num_manifest_ids >= 0 ? HIO_SUCCESS : num_manifest_ids;
}
#endif /* HIO_MPI_HAVE(3) */

static int builtin_posix_module_dataset_init (struct hio_module_t *module,
                                              builtin_posix_module_dataset_t *posix_dataset) {
  hio_context_t context = hioi_object_context ((hio_object_t) posix_dataset);
  int rc;

  rc = asprintf (&posix_dataset->base_path, "%s/%s.hio/%s/%lu", module->data_root,
                 hioi_object_identifier(context), hioi_object_identifier (posix_dataset),
                 (unsigned long) posix_dataset->base.ds_id);
  assert (0 < rc);

  /* initialize posix dataset specific data */
  for (int i = 0 ; i < HIO_POSIX_MAX_OPEN_FILES ; ++i) {
    posix_dataset->files[i].f_bid = -1;
    posix_dataset->files[i].f_hndl = NULL;
    posix_dataset->files[i].f_fd = -1;
  }

  /* default to strided output mode */
  posix_dataset->ds_fmode = HIO_FILE_MODE_STRIDED;
  hioi_config_add (context, &posix_dataset->base.ds_object, &posix_dataset->ds_fmode,
                   "dataset_file_mode", HIO_CONFIG_TYPE_INT32, &hioi_dataset_file_modes,
                   "Modes for writing dataset files. Valid values: (0: basic, 1: file_per_node, 2: strided)", 0);

  if (HIO_FILE_MODE_STRIDED == posix_dataset->ds_fmode && HIO_SET_ELEMENT_UNIQUE == posix_dataset->base.ds_mode) {
    /* strided mode only applies to shared datasets */
    posix_dataset->ds_fmode = HIO_FILE_MODE_BASIC;
  }

  if (HIO_FILE_MODE_BASIC != posix_dataset->ds_fmode) {
    posix_dataset->ds_bs = 1ul << 23;
    hioi_config_add (context, &posix_dataset->base.ds_object, &posix_dataset->ds_bs,
                     "dataset_block_size", HIO_CONFIG_TYPE_INT64, NULL,
                     "Block size to use when writing in optimized mode (default: 8M)", 0);
  }

  return HIO_SUCCESS;
}

static int builtin_posix_module_setup_striping (hio_context_t context, struct hio_module_t *module, hio_dataset_t dataset) {
  builtin_posix_module_dataset_t *posix_dataset = (builtin_posix_module_dataset_t *) dataset;
  hio_fs_attr_t *fs_attr = &dataset->ds_fsattr;
  int rc;

  /* query the filesystem for current striping parameters */
  rc = hioi_fs_query (context, module->data_root, fs_attr);
  if (HIO_SUCCESS != rc) {
    hioi_err_push (rc, &context->c_object, "posix: error querying the filesystem");
    return rc;
  }

  /* for now do not use stripe exclusivity in any path */
  posix_dataset->my_stripe = 0;

  /* set default stripe count */
  fs_attr->fs_scount = 1;

  posix_dataset->ds_fcount = 1;

  if (fs_attr->fs_flags & HIO_FS_SUPPORTS_STRIPING) {
    if (HIO_FILE_MODE_OPTIMIZED == posix_dataset->ds_fmode) {
      /* pick a reasonable default stripe size */
      fs_attr->fs_ssize = 1 << 24;

      /* use group locking if available as we guarantee stripe exclusivity in optimized mode */
      fs_attr->fs_use_group_locking = true;

#if HIO_MPI_HAVE(3)
      /* if group locking is not available then each rank should attempt to write to
       * a different stripe to maximize the available IO bandwidth */
      fs_attr->fs_scount = min(context->c_shared_size, fs_attr->fs_smax_count);
#endif
    } else if (HIO_FILE_MODE_STRIDED == posix_dataset->ds_fmode) {
      /* pick a reasonable default stripe size */
      fs_attr->fs_ssize = posix_dataset->ds_bs;

      posix_dataset->ds_fcount = fs_attr->fs_smax_count * 32;
      fs_attr->fs_scount = 16;
      if (context->c_size < posix_dataset->ds_fcount) {
        posix_dataset->ds_fcount = context->c_size;
      }
    } else if (HIO_SET_ELEMENT_UNIQUE != dataset->ds_mode) {
      /* set defaults striping count */
      fs_attr->fs_ssize = 1 << 20;
      fs_attr->fs_scount = max (1, (unsigned) ((float) fs_attr->fs_smax_count * 0.9));
    } else {
      fs_attr->fs_ssize = 1 << 20;
    }

    hioi_config_add (context, &dataset->ds_object, &fs_attr->fs_scount,
                     "stripe_count", HIO_CONFIG_TYPE_UINT32, NULL, "Stripe count for all dataset "
                     "data files", 0);

    hioi_config_add (context, &dataset->ds_object, &fs_attr->fs_ssize,
                     "stripe_size", HIO_CONFIG_TYPE_UINT64, NULL, "Stripe size for all dataset "
                     "data files", 0);

    if (fs_attr->fs_flags & HIO_FS_SUPPORTS_RAID) {
      hioi_config_add (context, &dataset->ds_object, &fs_attr->fs_raid_level,
                       "raid_level", HIO_CONFIG_TYPE_UINT64, NULL, "RAID level for dataset "
                       "data files. Keep in mind that some filesystems only support 1/2 RAID "
                       "levels", 0);
    }

    /* ensure stripe count is sane */
    if (fs_attr->fs_scount > fs_attr->fs_smax_count) {
      hioi_log (context, HIO_VERBOSE_WARN, "posix:dataset_open: requested stripe count %u exceeds the available resources. "
                "adjusting to maximum %u", fs_attr->fs_scount, fs_attr->fs_smax_count);
      fs_attr->fs_scount = fs_attr->fs_smax_count;
    }

    /* ensure the stripe size is a multiple of the stripe unit */
    fs_attr->fs_ssize = fs_attr->fs_sunit * ((fs_attr->fs_ssize + fs_attr->fs_sunit - 1) / fs_attr->fs_sunit);
    if (fs_attr->fs_ssize > fs_attr->fs_smax_size) {
      hioi_log (context, HIO_VERBOSE_WARN, "posix:dataset_open: requested stripe size %" PRIu64 " exceeds the maximum %"
                PRIu64 ". ", fs_attr->fs_ssize, fs_attr->fs_smax_size);
      fs_attr->fs_ssize = fs_attr->fs_smax_size;
    }

    if (HIO_FILE_MODE_OPTIMIZED == posix_dataset->ds_fmode && posix_dataset->ds_bs < fs_attr->fs_ssize) {
      posix_dataset->ds_bs = fs_attr->fs_ssize;
    }
  }

  return HIO_SUCCESS;
}

#if HIO_MPI_HAVE(3)
static int bultin_posix_scatter_data (builtin_posix_module_dataset_t *posix_dataset) {
  hio_context_t context = hioi_object_context ((hio_object_t) posix_dataset);
  size_t manifest_size = 0, manifest_id_count = 0;
  unsigned char *manifest = NULL;
  int rc = HIO_SUCCESS;
  int *manifest_ids;
  char *path;

  if (HIO_SET_ELEMENT_UNIQUE == posix_dataset->base.ds_mode) {
    /* only read the manifest this rank wrote */
    manifest_id_count = 1;
    manifest_ids = malloc (sizeof (*manifest_ids));
    manifest_ids[0] = context->c_rank;
  } else {
    rc = builtin_posix_module_dataset_manifest_list (posix_dataset, &manifest_ids, &manifest_id_count);
    if (HIO_SUCCESS != rc) {
      return rc;
    }
  }

  for (size_t i = 0 ; i < manifest_id_count ; ++i) {
    if (-1 == manifest_ids[i]) {
      /* nothing more to do */
      break;
    }

    hioi_log (context, HIO_VERBOSE_DEBUG_LOW, "posix:dataset_open: reading manifest data from id %x\n",
              manifest_ids[i]);

    /* when writing the manifest in optimized mode each IO manager writes its own manifest. try
     * to open the manifest. if a manifest does not exist then it is likely this rank did not
     * write a manifest. IO managers will distribute the manifest data to the appropriate ranks
     * in hioi_dataset_scatter(). */
    rc = asprintf (&path, "%s/manifest.%x.json.bz2", posix_dataset->base_path, manifest_ids[i]);
    assert (0 < rc);

    if (access (path, F_OK)) {
      free (path);
      /* Check for a non-bzip'd manifest file. */
      rc = asprintf (&path, "%s/manifest.%x.json", posix_dataset->base_path, manifest_ids[i]);
      assert (0 < rc);
      if (access (path, F_OK)) {
        /* no manifest found. this might be a non-optimized file format or this rank may not be an
         * IO master rank. */
        free (path);
        path = NULL;
      }
    }

    if (path) {
      unsigned char *tmp = NULL;
      size_t tmp_size = 0;
      /* read the manifest if it exists */
      rc = hioi_manifest_read (path, &tmp, &tmp_size);
      if (HIO_SUCCESS == rc) {
        rc = hioi_manifest_merge_data2 (&manifest, &manifest_size, tmp, tmp_size);
        free (tmp);
      }

      free (path);

      if (HIO_SUCCESS != rc) {
        break;
      }
    }
  }

  /* share dataset information with all processes on this node */
  if (HIO_SET_ELEMENT_UNIQUE == posix_dataset->base.ds_mode) {
    rc = hioi_dataset_scatter_unique (&posix_dataset->base, manifest, manifest_size, rc);
  } else {
    rc = hioi_dataset_scatter_comm (&posix_dataset->base, context->c_shared_comm, manifest, manifest_size, rc);
  }

  free (manifest_ids);
  free (manifest);

  return rc;
}
#endif

static int builtin_posix_module_dataset_open (struct hio_module_t *module, hio_dataset_t dataset) {
  builtin_posix_module_dataset_t *posix_dataset = (builtin_posix_module_dataset_t *) dataset;
  builtin_posix_module_t *posix_module = (builtin_posix_module_t *) module;
  hio_context_t context = hioi_object_context ((hio_object_t) dataset);
  unsigned char *manifest = NULL;
  size_t manifest_size = 0;
  uint64_t start, stop;
  int rc = HIO_SUCCESS;
  char *path = NULL;

  start = hioi_gettime ();

  hioi_log (context, HIO_VERBOSE_DEBUG_MED, "posix:dataset_open: opening dataset %s:%lu mpi: %d flags: 0x%x mode: 0x%x",
	    hioi_object_identifier (dataset), (unsigned long) dataset->ds_id, hioi_context_using_mpi (context),
            dataset->ds_flags, dataset->ds_mode);

  rc = builtin_posix_module_dataset_init (module, posix_dataset);
  if (HIO_SUCCESS != rc) {
    return rc;
  }

  rc = builtin_posix_module_setup_striping (context, module, dataset);
  if (HIO_SUCCESS != rc) {
    return rc;
  }

  if (HIO_FILE_MODE_STRIDED == posix_dataset->ds_fmode) {
    hioi_config_add (context, &dataset->ds_object, &posix_dataset->ds_fcount,
                     "dataset_file_count", HIO_CONFIG_TYPE_UINT64, NULL, "Number of files to use "
                     "in strided file mode", 0);
  } else if (HIO_FILE_MODE_OPTIMIZED == posix_dataset->ds_fmode) {
    posix_dataset->ds_use_bzip = true;
    hioi_config_add (context, &dataset->ds_object, &posix_dataset->ds_use_bzip,
                     "dataset_use_bzip", HIO_CONFIG_TYPE_BOOL, NULL,
                     "Use bzip2 compression for dataset manifests", 0);
  }

  if (dataset->ds_flags & HIO_FLAG_TRUNC) {
    /* blow away the existing dataset */
    if (0 == context->c_rank) {
      (void) builtin_posix_module_dataset_unlink (module, hioi_object_identifier(dataset),
                                                  dataset->ds_id);
    }
  }

  if (!(dataset->ds_flags & HIO_FLAG_CREAT)) {
    if (0 == context->c_rank) {
      /* load manifest. the manifest data will be shared with other processes in hioi_dataset_scatter */
      rc = asprintf (&path, "%s/manifest.json", posix_dataset->base_path);
      assert (0 < rc);
      if (access (path, F_OK)) {
        /* this should never happen on a valid dataset */
        free (path);
        hioi_log (context, HIO_VERBOSE_DEBUG_LOW, "posix:dataset_open: could not find top-level manifest %s", path);
        rc = HIO_ERR_NOT_FOUND;
      } else {
        rc = HIO_SUCCESS;
      }
    }

    /* read the manifest if it exists */
    if (HIO_SUCCESS == rc) {
      hioi_log (context, HIO_VERBOSE_DEBUG_LOW, "posix:dataset_open: loading manifest header from %s...", path);
      rc = hioi_manifest_read (path, &manifest, &manifest_size);
      free (path);
      path = NULL;
    }
  } else if (0 == context->c_rank) {
    rc = builtin_posix_create_dataset_dirs (posix_module, posix_dataset);
    if (HIO_SUCCESS == rc) {
      /* serialize the manifest to send to remote ranks */
      rc = hioi_manifest_serialize (dataset, &manifest, &manifest_size, false, false);
    }
  }

#if HIO_MPI_HAVE(1)
  /* share dataset header will all processes in the communication domain */
  rc = hioi_dataset_scatter_comm (dataset, context->c_comm, manifest, manifest_size, rc);
#endif
  free (manifest);
  if (HIO_SUCCESS != rc) {
    free (posix_dataset->base_path);
    return rc;
  }

  if (context->c_enable_tracing) {
    char *path;

    rc = asprintf (&path, "%s/trace/trace.%d", posix_dataset->base_path, context->c_rank);
    if (rc > 0) {
      posix_dataset->ds_trace_fh = fopen (path, "a");
      free (path);
    }

    builtin_posix_trace (posix_dataset, "trace_begin", 0, 0, 0, 0);
  }

#if HIO_MPI_HAVE(3)
  if (!(dataset->ds_flags & HIO_FLAG_CREAT) && HIO_FILE_MODE_OPTIMIZED == posix_dataset->ds_fmode) {
    rc = bultin_posix_scatter_data (posix_dataset);
    if (HIO_SUCCESS != rc) {
      free (posix_dataset->base_path);
      return rc;
    }
  }

  /* if possible set up a shared memory window for this dataset */
  POSIX_TRACE_CALL(posix_dataset, hioi_dataset_shared_init (dataset, 1), "shared_init", 0, 0);

  if (HIO_FILE_MODE_OPTIMIZED == posix_dataset->ds_fmode) {
    if (2 > context->c_size || NULL == dataset->ds_shared_control) {
      /* no point in using optimized mode in this case */
      posix_dataset->ds_fmode = HIO_FILE_MODE_BASIC;
      hioi_log (context, HIO_VERBOSE_WARN, "posix:dataset_open: optimized file mode requested but not supported in this "
                "dataset mode. falling back to basic file mode, path: %s", posix_dataset->base_path);
    } else if (HIO_SET_ELEMENT_SHARED == dataset->ds_mode) {
      POSIX_TRACE_CALL(posix_dataset, rc = hioi_dataset_generate_map (dataset), "generate_map", 0, 0);
      if (HIO_SUCCESS != rc) {
        free (posix_dataset->base_path);
        return rc;
      }
    }
  }

  /* NTH: if requested more code is needed to load an optimized dataset with an older MPI */
#endif /* HIO_MPI_HAVE(3) */

  dataset->ds_module = module;
  dataset->ds_close = builtin_posix_module_dataset_close;
  dataset->ds_element_open = builtin_posix_module_element_open;
  dataset->ds_process_reqs = builtin_posix_module_process_reqs;

  /* record the open time */
  gettimeofday (&dataset->ds_otime, NULL);

  stop = hioi_gettime ();

  hioi_log (context, HIO_VERBOSE_DEBUG_LOW, "posix:dataset_open: successfully %s posix dataset "
            "%s:%" PRIu64 " on data root %s. open time %" PRIu64 " usec",
            (dataset->ds_flags & HIO_FLAG_CREAT) ? "created" : "opened", hioi_object_identifier(dataset),
            dataset->ds_id, module->data_root, stop - start);

  builtin_posix_trace (posix_dataset, "open", 0, 0, start, stop);

  return HIO_SUCCESS;
}

static int builtin_posix_module_dataset_close (hio_dataset_t dataset) {
  builtin_posix_module_dataset_t *posix_dataset = (builtin_posix_module_dataset_t *) dataset;
  hio_context_t context = hioi_object_context ((hio_object_t) dataset);
  hio_module_t *module = dataset->ds_module;
  unsigned char *manifest = NULL;
  uint64_t start, stop;
  int rc = HIO_SUCCESS;
  size_t manifest_size;

  start = hioi_gettime ();

  for (int i = 0 ; i < HIO_POSIX_MAX_OPEN_FILES ; ++i) {
    if (posix_dataset->files[i].f_bid >= 0) {
      POSIX_TRACE_CALL(posix_dataset, hioi_file_close (posix_dataset->files + i), "file_close",
                       posix_dataset->files[i].f_bid, 0);
    }
  }

#if HIO_MPI_HAVE(3)
  /* release the shared state if it was allocated */
  (void) hioi_dataset_shared_fini (dataset);

  /* release the dataset map if one was allocated */
  (void) hioi_dataset_map_release (dataset);
#endif


  if (dataset->ds_flags & HIO_FLAG_WRITE) {
    char *path;

    /* write manifest header */
    POSIX_TRACE_CALL(posix_dataset, rc = hioi_dataset_gather_manifest (dataset, &manifest, &manifest_size, false, true),
                     "gather_manifest", 0, 0);
    if (HIO_SUCCESS != rc) {
      dataset->ds_status = rc;
    }

    if (0 == context->c_rank) {
      rc = asprintf (&path, "%s/manifest.json", posix_dataset->base_path);
      if (0 > rc) {
        /* out of memory. not much we can do now */
        return hioi_err_errno (errno);
      }

      rc = hioi_manifest_save (dataset, manifest, manifest_size, path);
      free (manifest);
      free (path);
      if (HIO_SUCCESS != rc) {
        hioi_err_push (rc, &dataset->ds_object, "posix: error writing dataset manifest");
      }
    }

#if HIO_MPI_HAVE(3)
    if (HIO_FILE_MODE_OPTIMIZED == posix_dataset->ds_fmode) {
      /* optimized mode requires a data manifest to describe how the data landed on the filesystem */
      POSIX_TRACE_CALL(posix_dataset, rc = hioi_dataset_gather_manifest_comm (dataset, context->c_shared_comm, &manifest, &manifest_size,
                                                                              posix_dataset->ds_use_bzip, false),
                       "gather_manifest", 0, 0);
      if (HIO_SUCCESS != rc) {
        dataset->ds_status = rc;
      }

      if (NULL != manifest) {
        rc = asprintf (&path, "%s/manifest.%x.json%s", posix_dataset->base_path, context->c_rank,
                       posix_dataset->ds_use_bzip ? ".bz2" : "");
        if (0 > rc) {
          return hioi_err_errno (errno);
        }

        rc = hioi_manifest_save (dataset, manifest, manifest_size, path);
        free (manifest);
        free (path);
        if (HIO_SUCCESS != rc) {
          hioi_err_push (rc, &dataset->ds_object, "posix: error writing dataset manifest");
        }
      }
    }
#endif
  }

#if HIO_MPI_HAVE(1)
  /* ensure all ranks have closed the dataset before continuing */
  if (hioi_context_using_mpi (context)) {
    MPI_Allreduce (MPI_IN_PLACE, &rc, 1, MPI_INT, MPI_MIN, context->c_comm);
  }
#endif

  free (posix_dataset->base_path);

  stop = hioi_gettime ();

  builtin_posix_trace (posix_dataset, "close", 0, 0, start, stop);

  hioi_log (context, HIO_VERBOSE_DEBUG_LOW, "posix:dataset_open: successfully closed posix dataset "
            "%s:%" PRIu64 " on data root %s. close time %" PRIu64 " usec", hioi_object_identifier(dataset),
            dataset->ds_id, module->data_root, stop - start);

  builtin_posix_trace (posix_dataset, "trace_end", 0, 0, 0, 0);
  if (posix_dataset->ds_trace_fh) {
    fclose (posix_dataset->ds_trace_fh);
  }

  return rc;
}

static int builtin_posix_unlink_cb (const char *path, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
  return remove (path);
}

static int builtin_posix_module_dataset_unlink (struct hio_module_t *module, const char *name, int64_t set_id) {
  struct stat statinfo;
  char *path = NULL;
  int rc;

  if (module->context->c_rank) {
    return HIO_ERR_NOT_AVAILABLE;
  }

  rc = builtin_posix_dataset_path (module, &path, name, set_id);
  if (HIO_SUCCESS != rc) {
    return rc;
  }

  if (stat (path, &statinfo)) {
    free (path);
    return hioi_err_errno (errno);
  }

  hioi_log (module->context, HIO_VERBOSE_DEBUG_LOW, "posix: unlinking existing dataset %s::%" PRId64,
            name, set_id);

  /* use tree walk depth-first to remove all of the files for this dataset */
  rc = nftw (path, builtin_posix_unlink_cb, 32, FTW_DEPTH | FTW_PHYS);
  free (path);
  if (0 > rc) {
    hioi_err_push (hioi_err_errno (errno), &module->context->c_object, "posix: could not unlink dataset. errno: %d",
                  errno);
    return hioi_err_errno (errno);
  }

  return HIO_SUCCESS;
}

static int builtin_posix_open_file (builtin_posix_module_t *posix_module, builtin_posix_module_dataset_t *posix_dataset,
                                    char *path, hio_file_t *file) {
  hio_object_t hio_object = &posix_dataset->base.ds_object;
  int open_flags, fd;
#if BUILTIN_POSIX_USE_STDIO
  char *file_mode;
#endif

  /* determine the fopen file mode to use */
  if (HIO_FLAG_WRITE & posix_dataset->base.ds_flags) {
    open_flags = O_CREAT | O_WRONLY;
  } else {
    open_flags = O_RDONLY;
  }

  /* it is not possible to get open with create without truncation using fopen so use a
   * combination of open and fdopen to get the desired effect */
  //hioi_log (context, HIO_VERBOSE_DEBUG_HIGH, "posix: calling open; path: %s open_flags: %i", path, open_flags);
  fd = open (path, open_flags, posix_module->access_mode);
  if (fd < 0) {
    hioi_err_push (fd, hio_object, "posix: error opening element path %s. "
                  "errno: %d", path, errno);
    return fd;
  }

#if BUILTIN_POSIX_USE_STDIO
  if (HIO_FLAG_WRITE & posix_dataset->base.ds_flags) {
    file_mode = "w";
  } else {
    file_mode = "r";
  }

  file->f_hndl = fdopen (fd, file_mode);
  if (NULL == file->f_hndl) {
    int hrc = hioi_err_errno (errno);
    hioi_err_push (hrc, hio_object, "posix: error opening element file %s. "
                   "errno: %d", path, errno);
    return hrc;
  }
#else
  file->f_fd = fd;
#endif

  file->f_offset = 0;

  return HIO_SUCCESS;
}

static int builtin_posix_module_element_open_basic (builtin_posix_module_t *posix_module, builtin_posix_module_dataset_t *posix_dataset,
                                                    hio_element_t element) {
  const char *element_name = hioi_object_identifier(element);
  char *path;
  int rc;

  if (HIO_SET_ELEMENT_UNIQUE == posix_dataset->base.ds_mode) {
    rc = asprintf (&path, "%s/data/element_data.%s.%08d", posix_dataset->base_path, element_name,
                   element->e_rank);
  } else {
    rc = asprintf (&path, "%s/data/element_data.%s", posix_dataset->base_path, element_name);
  }

  if (0 > rc) {
    return HIO_ERR_OUT_OF_RESOURCE;
  }

  if (access (path, R_OK)) {
    /* fall back on old naming scheme */
    if (HIO_SET_ELEMENT_UNIQUE == posix_dataset->base.ds_mode) {
      rc = asprintf (&path, "%s/element_data.%s.%08d", posix_dataset->base_path, element_name,
                     element->e_rank);
    } else {
      rc = asprintf (&path, "%s/element_data.%s", posix_dataset->base_path, element_name);
    }

    if (0 > rc) {
      return hioi_err_errno (errno);
    }
  }

  POSIX_TRACE_CALL(posix_dataset, rc = builtin_posix_open_file (posix_module, posix_dataset, path, &element->e_file),
                   "file_open", 0, 0);
  free (path);
  if (HIO_SUCCESS != rc) {
    return rc;
  }

#if BUILTIN_POSIX_USE_STDIO
  fseek (element->e_file.f_hndl, 0, SEEK_END);
  element->e_size = ftell (element->e_file.f_hndl);
  fseek (element->e_file.f_hndl, 0, SEEK_SET);
#else
  element->e_size = lseek (element->e_file.f_fd, 0, SEEK_END);
  lseek (element->e_file.f_fd, 0, SEEK_SET);
#endif

  return HIO_SUCCESS;
}

static int builtin_posix_module_element_open (hio_dataset_t dataset, hio_element_t element) {
  builtin_posix_module_dataset_t *posix_dataset = (builtin_posix_module_dataset_t *) dataset;
  builtin_posix_module_t *posix_module = (builtin_posix_module_t *) dataset->ds_module;
  hio_context_t context = hioi_object_context (&dataset->ds_object);
  int rc;

  if (HIO_FILE_MODE_BASIC == posix_dataset->ds_fmode) {
    rc = builtin_posix_module_element_open_basic (posix_module, posix_dataset, element);
    if (HIO_SUCCESS != rc) {
      hioi_object_release (&element->e_object);
      return rc;
    }
  }

  hioi_log (context, HIO_VERBOSE_DEBUG_LOW, "posix: %s element %p (identifier %s) for dataset %s",
	    (HIO_FLAG_WRITE & dataset->ds_flags) ? "created" : "opened", (void *) element,
            hioi_object_identifier(element), hioi_object_identifier(dataset));

  element->e_flush = builtin_posix_module_element_flush;
  element->e_complete = builtin_posix_module_element_complete;
  element->e_close = builtin_posix_module_element_close;

  return HIO_SUCCESS;
}

static int builtin_posix_module_element_close (hio_element_t element) {
  return HIO_SUCCESS;
}

/* reserve space in the local shared file for this rank's data */
static unsigned long builtin_posix_reserve (builtin_posix_module_dataset_t *posix_dataset, size_t *requested) {
  /* NTH: this value should be changed to be non-0 if stripe exclusivity is re-enabled */
  uint32_t stripe_count = 1;
  uint64_t block_size = posix_dataset->ds_bs;
  const int stripe = posix_dataset->my_stripe;
  unsigned long new_offset, to_use, space;
  int nstripes;

  if (posix_dataset->reserved_remaining) {
    to_use = (*requested > posix_dataset->reserved_remaining) ? posix_dataset->reserved_remaining : *requested;
    new_offset = posix_dataset->reserved_offset;

    /* update cached values */
    posix_dataset->reserved_offset += to_use;
    posix_dataset->reserved_remaining -= to_use;

    *requested = to_use;
    return new_offset;
  }

  space = *requested;

  if (space % posix_dataset->ds_bs) {
    space += posix_dataset->ds_bs - (space % posix_dataset->ds_bs);
  }

  if (1 < stripe_count && space > posix_dataset->ds_bs) {
    *requested = space = posix_dataset->ds_bs;
    nstripes = 1;
  } else {
    nstripes = space / posix_dataset->ds_bs;
  }

  unsigned long s_index = atomic_fetch_add (&posix_dataset->base.ds_shared_control->s_stripes[stripe].s_index, nstripes);
  new_offset = (s_index * stripe_count * block_size) + stripe * block_size;

  posix_dataset->reserved_offset = new_offset + *requested;
  posix_dataset->reserved_remaining = space - *requested;

  return new_offset;
}

static int builtin_posix_element_translate_strided (builtin_posix_module_t *posix_module, hio_element_t element,
                                                    uint64_t offset, size_t *size, hio_file_t **file_out) {
  builtin_posix_module_dataset_t *posix_dataset = (builtin_posix_module_dataset_t *) hioi_element_dataset (element);
  size_t block_id, block_base, block_bound, block_offset, file_id, file_block;
  hio_context_t context = hioi_object_context (&element->e_object);
  hio_file_t *file;
  int32_t file_index;
  char *path;
  int rc;

  block_id = offset / posix_dataset->ds_bs;

  file_id = block_id % posix_dataset->ds_fcount;
  file_block = block_id / posix_dataset->ds_fcount;

  block_base = block_id * posix_dataset->ds_bs;
  block_bound = block_base + posix_dataset->ds_bs;
  block_offset = file_block * posix_dataset->ds_bs + offset - block_base;

  hioi_log (context, HIO_VERBOSE_DEBUG_LOW, "builtin_posix_element_translate_strided: element: %s, offset: %"
            PRIu64 ", file_id: %lu, file_block: %lu, block_offset: %lu, block_size: %" PRIu64,
            hioi_object_identifier(element), offset, file_id, file_id, block_offset, posix_dataset->ds_bs);

  if (offset + *size > block_bound) {
    *size = block_bound - offset;
  }

  rc = asprintf (&path, "%s/data/%s_block.%08lu", posix_dataset->base_path, hioi_object_identifier(element),
                 (unsigned long) file_id);
  if (0 > rc) {
    return HIO_ERR_OUT_OF_RESOURCE;
  }

  /* use crc as a hash to pick a file index to use */
  file_index = file_id % HIO_POSIX_MAX_OPEN_FILES;
  file = posix_dataset->files + file_index;

  if (file_id != file->f_bid || file->f_element != element) {
    if (file->f_bid >= 0) {
      POSIX_TRACE_CALL(posix_dataset, hioi_file_close (file), "file_close", file->f_bid, 0);
    }
    file->f_bid = -1;

    file->f_element = element;

    POSIX_TRACE_CALL(posix_dataset, rc = builtin_posix_open_file (posix_module, posix_dataset, path, file),
                     "file_open", file_id, 0);
    if (HIO_SUCCESS != rc) {
      return rc;
    }

    file->f_bid = file_id;
  }

  POSIX_TRACE_CALL(posix_dataset, hioi_file_seek (file, block_offset, SEEK_SET), "file_seek", file->f_bid, block_offset);

  *file_out = file;

  return HIO_SUCCESS;
}

static int builtin_posix_element_translate_opt (builtin_posix_module_t *posix_module, hio_element_t element,
                                                uint64_t offset, size_t *size, hio_file_t **file_out,
                                                bool reading) {
  builtin_posix_module_dataset_t *posix_dataset = (builtin_posix_module_dataset_t *) hioi_element_dataset (element);
  hio_context_t context = hioi_object_context (&element->e_object);
  hio_file_t *file;
  uint64_t file_offset;
  int file_index = 0;
  char *path;
  int rc;

  hioi_log (context, HIO_VERBOSE_DEBUG_MED, "translating element %s offset %" PRIu64 " size %lu",
            hioi_object_identifier (&element->e_object), offset, *size);
  POSIX_TRACE_CALL(posix_dataset, rc = hioi_element_translate_offset (element, offset, &file_index, &file_offset, size),
                   "translate_offset", offset, *size);
#if HIO_MPI_HAVE(3)
  if (HIO_SUCCESS != rc && reading) {
    POSIX_TRACE_CALL(posix_dataset, rc = hioi_dataset_map_translate_offset (element, offset, &file_index, &file_offset, size),
                     "map_translate_offset", offset, *size);
  }
#endif

  if (HIO_SUCCESS != rc) {
    if (reading) {
      hioi_log (context, HIO_VERBOSE_DEBUG_MED, "offset %" PRIu64 " not found", offset);
      /* not found */
      return rc;
    }

    file_offset = builtin_posix_reserve (posix_dataset, size);

    if (hioi_context_using_mpi (context)) {
      file_index = posix_dataset->base.ds_shared_control->s_master;
    } else {
      file_index = 0;
    }

    rc = asprintf (&path, "%s/data/data.%x", posix_dataset->base_path, file_index);
    if (0 > rc) {
      return HIO_ERR_OUT_OF_RESOURCE;
    }

    hioi_element_add_segment (element, file_index, file_offset, offset, *size);
  } else {
    hioi_log (context, HIO_VERBOSE_DEBUG_MED, "offset found in file @ rank %d, offset %" PRIu64
              ", size %lu", file_index, file_offset, *size);
    rc = asprintf (&path, "%s/data/data.%x", posix_dataset->base_path, file_index);
    if (0 > rc) {
      return HIO_ERR_OUT_OF_RESOURCE;
    }

    if (access (path, R_OK)) {
      free (path);
      rc = asprintf (&path, "%s/data.%x", posix_dataset->base_path, file_index);
      if (0 > rc) {
        return HIO_ERR_OUT_OF_RESOURCE;
      }
    }
  }

  /* use crc as a hash to pick a file index to use */
  int internal_index = file_index % HIO_POSIX_MAX_OPEN_FILES;
  file = posix_dataset->files + internal_index;

  if (file_index != file->f_bid) {
    if (file->f_bid >= 0) {
      POSIX_TRACE_CALL(posix_dataset, hioi_file_close (file), "file_close", file->f_bid, 0);
    }

    file->f_bid = -1;

    POSIX_TRACE_CALL(posix_dataset, rc = builtin_posix_open_file (posix_module, posix_dataset, path, file),
                     "file_open", file_index, 0);
    if (HIO_SUCCESS != rc) {
      free (path);
      return rc;
    }

    file->f_bid = file_index;
  }

  free (path);

  POSIX_TRACE_CALL(posix_dataset, hioi_file_seek (file, file_offset, SEEK_SET), "file_seek", file->f_bid, file_offset);

  *file_out = file;

  return HIO_SUCCESS;
}

static int builtin_posix_element_translate (builtin_posix_module_t *posix_module, hio_element_t element,
                                            uint64_t offset, size_t *size, hio_file_t **file_out, bool reading) {
  builtin_posix_module_dataset_t *posix_dataset = (builtin_posix_module_dataset_t *) hioi_element_dataset (element);
  int rc = HIO_SUCCESS;

  switch (posix_dataset->ds_fmode) {
  case HIO_FILE_MODE_BASIC:
    *file_out = &element->e_file;
    hioi_file_seek (&element->e_file, offset, SEEK_SET);
    break;
  case HIO_FILE_MODE_STRIDED:
    rc = builtin_posix_element_translate_strided (posix_module, element, offset, size, file_out);
    break;
  case HIO_FILE_MODE_OPTIMIZED:
    rc = builtin_posix_element_translate_opt (posix_module, element, offset, size, file_out, reading);
    break;
  }

  return rc;
}

static ssize_t builtin_posix_module_element_write_strided_internal (builtin_posix_module_t *posix_module, hio_element_t element,
                                                                    uint64_t offset, const void *ptr, size_t count, size_t size,
                                                                    size_t stride) {
  builtin_posix_module_dataset_t *posix_dataset = (builtin_posix_module_dataset_t *) hioi_element_dataset (element);
  hio_dataset_t dataset = hioi_element_dataset (element);
  size_t bytes_written = 0, ret;
  hio_file_t *file;
  uint64_t stop, start;
  int rc;

  assert (dataset->ds_flags & HIO_FLAG_WRITE);

  if (!(count * size)) {
    return 0;
  }

  if (0 == stride) {
    size *= count;
    count = 1;
  }

  start = hioi_gettime ();

  errno = 0;

  for (size_t i = 0 ; i < count ; ++i) {
    size_t req = size, actual;

    do {
      actual = req;

      POSIX_TRACE_CALL(posix_dataset, rc = builtin_posix_element_translate (posix_module, element, offset, &actual,
                                                                            &file, false),
                       "element_translate", offset, req);
      if (HIO_SUCCESS != rc) {
        break;
      }

      hioi_log (hioi_object_context (&element->e_object), HIO_VERBOSE_DEBUG_HIGH,
                "posix: writing %lu bytes to file offset %" PRIu64, actual, file->f_offset);

      POSIX_TRACE_CALL(posix_dataset, ret = hioi_file_write (file, ptr, actual), "file_write", offset, actual);
      if (ret > 0) {
        bytes_written += ret;
      }

      if (ret < actual) {
        /* short write */
        break;
      }

      req -= actual;
      offset += actual;
      ptr = (void *) ((intptr_t) ptr + actual);
    } while (req);

    if (HIO_SUCCESS != rc || req) {
      break;
    }

    ptr = (void *) ((intptr_t) ptr + stride);
  }

  if (0 == bytes_written || HIO_SUCCESS != rc) {
    if (0 == bytes_written) {
      rc = hioi_err_errno (errno);
    }

    dataset->ds_status = rc;
    return rc;
  }

  if (offset + bytes_written > element->e_size) {
    element->e_size = offset + bytes_written;
  }

  stop = hioi_gettime ();

  dataset->ds_stat.s_wtime += stop - start;

  if (0 < bytes_written) {
    dataset->ds_stat.s_bwritten += bytes_written;
  }

  hioi_log (hioi_object_context (&element->e_object), HIO_VERBOSE_DEBUG_LOW,
            "posix: finished write. bytes written: %lu, time: %" PRIu64 " usec",
            bytes_written, stop - start);

  return bytes_written;
}

static ssize_t builtin_posix_module_element_read_strided_internal (builtin_posix_module_t *posix_module, hio_element_t element,
                                                                   uint64_t offset, void *ptr, size_t count, size_t size,
                                                                   size_t stride) {
  builtin_posix_module_dataset_t *posix_dataset = (builtin_posix_module_dataset_t *) hioi_element_dataset (element);
  size_t bytes_read = 0, ret;
  hio_file_t *file;
  uint64_t start, stop;
  int rc;

  if (0 == count || 0 == size) {
    return 0;
  }

  errno = 0;

  start = hioi_gettime ();

  for (size_t i = 0 ; i < count ; ++i) {
    size_t req = size, actual;

    do {
      actual = req;

      /* find out where the data lives */
      POSIX_TRACE_CALL(posix_dataset, rc = builtin_posix_element_translate (posix_module, element, offset, &actual,
                                                                            &file, true),
                       "element_translate", offset, req);
      if (HIO_SUCCESS != rc) {
        break;
      }

      POSIX_TRACE_CALL(posix_dataset, ret = hioi_file_read (file, ptr, actual), "file_read", offset, actual);
      if (ret > 0) {
        bytes_read += ret;
      }

      if (ret < actual) {
        /* short read */
        break;
      }

      req -= actual;
      offset += actual;
      ptr = (void *) ((intptr_t) ptr + actual);
    } while (req);

    if (req || HIO_SUCCESS != rc) {
      break;
    }

    ptr = (void *) ((intptr_t) ptr + stride);
  }

  if (0 == bytes_read || HIO_SUCCESS != rc) {
    if (0 == bytes_read && HIO_SUCCESS == rc) {
      rc = hioi_err_errno (errno);
    }

    return rc;
  }

  stop = hioi_gettime ();
  posix_dataset->base.ds_stat.s_rtime += stop - start;
  posix_dataset->base.ds_stat.s_bread += bytes_read;

  return bytes_read;
}

static int builtin_posix_module_process_reqs (hio_dataset_t dataset, hio_internal_request_t **reqs, int req_count) {
  builtin_posix_module_dataset_t *posix_dataset = (builtin_posix_module_dataset_t *) dataset;
  builtin_posix_module_t *posix_module = (builtin_posix_module_t *) dataset->ds_module;
  hio_context_t context = hioi_object_context (&dataset->ds_object);
  uint64_t start, stop;
  int rc = HIO_SUCCESS;

  start = hioi_gettime ();

  hioi_object_lock (&dataset->ds_object);
  for (int i = 0 ; i < req_count ; ++i) {
    hio_internal_request_t *req = reqs[i];

    if (HIO_REQUEST_TYPE_READ == req->ir_type) {
      POSIX_TRACE_CALL(posix_dataset,
                       req->ir_status = builtin_posix_module_element_read_strided_internal (posix_module, req->ir_element, req->ir_offset,
                                                                                            req->ir_data.r, req->ir_count, req->ir_size,
                                                                                            req->ir_stride),
                       "element_read", req->ir_offset, req->ir_count * req->ir_size);

    } else {
      POSIX_TRACE_CALL(posix_dataset,
                       req->ir_status = builtin_posix_module_element_write_strided_internal (posix_module, req->ir_element, req->ir_offset,
                                                                                             req->ir_data.w, req->ir_count, req->ir_size,
                                                                                             req->ir_stride),
                       "element_write", req->ir_offset, req->ir_count * req->ir_size);
    }

    if (req->ir_urequest && req->ir_status > 0) {
      hio_request_t new_request = hioi_request_alloc (context);
      if (NULL == new_request) {
        rc = HIO_ERR_OUT_OF_RESOURCE;
        break;
      }

      req->ir_urequest[0] = new_request;
      new_request->req_transferred = req->ir_status;
      new_request->req_complete = true;
      new_request->req_status = HIO_SUCCESS;
    }

    if (req->ir_status < 0) {
      rc = (int) req->ir_status;
      break;
    }
  }

  hioi_object_unlock (&dataset->ds_object);

  stop = hioi_gettime ();

  builtin_posix_trace (posix_dataset, "process_requests", req_count, 0, start, stop);

  return rc;
}

static int builtin_posix_module_element_flush (hio_element_t element, hio_flush_mode_t mode) {
  builtin_posix_module_dataset_t *posix_dataset =
    (builtin_posix_module_dataset_t *) hioi_element_dataset (element);

  if (!(posix_dataset->base.ds_flags & HIO_FLAG_WRITE)) {
    return HIO_ERR_PERM;
  }

  if (HIO_FLUSH_MODE_COMPLETE != mode) {
    /* nothing to do at this time */
    return HIO_SUCCESS;
  }

  if (HIO_FILE_MODE_BASIC != posix_dataset->ds_fmode) {
    for (int i = 0 ; i < HIO_POSIX_MAX_OPEN_FILES ; ++i) {
      hioi_file_flush (posix_dataset->files + i);
    }
  } else {
    hioi_file_flush (&element->e_file);
  }

  return HIO_SUCCESS;
}

static int builtin_posix_module_element_complete (hio_element_t element) {
  hio_dataset_t dataset = hioi_element_dataset (element);

  /* reads in this module are always blocking. need to update this code if
   * that ever changes */
  if (!(dataset->ds_flags & HIO_FLAG_READ)) {
    return HIO_ERR_PERM;
  }

  return HIO_SUCCESS;
}

static int builtin_posix_module_fini (struct hio_module_t *module) {
  hioi_log (module->context, HIO_VERBOSE_DEBUG_LOW, "posix: finalizing module for data root %s",
	    module->data_root);

  free (module->data_root);
  free (module);

  return HIO_SUCCESS;
}

hio_module_t builtin_posix_module_template = {
  .dataset_open   = builtin_posix_module_dataset_open,
  .dataset_unlink = builtin_posix_module_dataset_unlink,

  .ds_object_size = sizeof (builtin_posix_module_dataset_t),

  .dataset_list   = builtin_posix_module_dataset_list,

  .fini           = builtin_posix_module_fini,
};

static int builtin_posix_component_init (hio_context_t context) {
  /* nothing to do */
  return HIO_SUCCESS;
}

static int builtin_posix_component_fini (void) {
  /* nothing to do */
  return HIO_SUCCESS;
}

static int builtin_posix_component_query (hio_context_t context, const char *data_root,
					  const char *next_data_root, hio_module_t **module) {
  builtin_posix_module_t *new_module;

  if (0 == strncasecmp("datawarp", data_root, 8) || 0 == strncasecmp("dw", data_root, 2)) {
    return HIO_ERR_NOT_AVAILABLE;
  }

  if (0 == strncasecmp("posix:", data_root, 6)) {
    /* skip posix: */
    data_root += 6;
  }

  if (access (data_root, F_OK)) {
    hioi_err_push (hioi_err_errno (errno), &context->c_object, "posix: data root %s does not exist or can not be accessed",
                  data_root);
    return hioi_err_errno (errno);
  }

  new_module = calloc (1, sizeof (builtin_posix_module_t));
  if (NULL == new_module) {
    return HIO_ERR_OUT_OF_RESOURCE;
  }

  memcpy (new_module, &builtin_posix_module_template, sizeof (builtin_posix_module_template));

  new_module->base.data_root = strdup (data_root);
  new_module->base.context = context;

  /* get the current umask */
  new_module->access_mode = umask (0);
  umask (new_module->access_mode);

  hioi_log (context, HIO_VERBOSE_DEBUG_LOW, "posix: created module for data root %s. using umask %o",
	    data_root, new_module->access_mode);

  new_module->access_mode ^= 0777;
  *module = &new_module->base;

  return HIO_SUCCESS;
}

hio_component_t builtin_posix_component = {
  .init = builtin_posix_component_init,
  .fini = builtin_posix_component_fini,

  .query = builtin_posix_component_query,
  .flags = 0,
  .priority = 10,
};
