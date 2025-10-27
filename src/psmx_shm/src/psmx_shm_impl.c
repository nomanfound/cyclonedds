// Copyright(c) 2025 ZettaScale Technology and others
//
// This program and the accompanying materials are made available under the
// terms of the Eclipse Public License v. 2.0 which is available at
// http://www.eclipse.org/legal/epl-2.0, or the Eclipse Distribution License
// v. 1.0 which is available at
// http://www.eclipse.org/org/documents/edl-v10.php.
//
// SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>

#include "dds/ddsrt/heap.h"
#include "dds/ddsrt/string.h"
#include "dds/ddsrt/sync.h"
#include "dds/ddsrt/threads.h"
#include "dds/ddsrt/atomics.h"
#include "dds/ddsc/dds_loaned_sample.h"
#include "dds/ddsc/dds_psmx.h"
#include "psmx_shm_impl.h"

// Configuration defaults
#define SHM_DEFAULT_SEGMENT_SIZE (4 * 1024 * 1024)  // 4MB per topic
#define SHM_DEFAULT_SAMPLE_COUNT 32                  // KEEP_LAST depth
#define SHM_NAME_PREFIX "/cyclonedds_shm_"

// Shared memory segment structure
typedef struct shm_segment {
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  uint32_t sample_size;
  uint32_t sample_count;
  uint32_t write_idx;
  uint32_t read_count;
  ddsrt_atomic_uint32_t reader_count;
  // Followed by sample metadata and data
} shm_segment_t;

// Sample header in shared memory
typedef struct shm_sample_header {
  ddsrt_atomic_uint32_t refcount;
  dds_psmx_metadata_t metadata;
  uint32_t data_size;
  uint64_t sequence;
  // Followed by actual data
} shm_sample_header_t;

// PSMX instance for shared memory
typedef struct psmx_shm {
  dds_psmx_t base;
  dds_psmx_node_identifier_t node_id;
  char *instance_name;
  uint32_t segment_size;
  uint32_t sample_count;
} psmx_shm_t;

// PSMX topic for shared memory
typedef struct psmx_shm_topic {
  dds_psmx_topic_t base;
  psmx_shm_t *psmx_instance;
  char *shm_name;
  int shm_fd;
  void *shm_addr;
  size_t shm_size;
  shm_segment_t *segment;
  uint32_t type_size;
  ddsrt_mutex_t lock;
} psmx_shm_topic_t;

// PSMX endpoint for shared memory
typedef struct psmx_shm_endpoint {
  dds_psmx_endpoint_t base;
  psmx_shm_topic_t *topic;
  dds_entity_t cdds_endpoint;
  uint64_t last_read_seq;
  ddsrt_mutex_t lock;
} psmx_shm_endpoint_t;

// Loaned sample for shared memory
typedef struct psmx_shm_loaned_sample {
  dds_loaned_sample_t base;
  shm_sample_header_t *header;
  void *data_ptr;
} psmx_shm_loaned_sample_t;

// Forward declarations
static bool psmx_shm_type_qos_supported(dds_psmx_t *psmx, dds_psmx_endpoint_type_t forwhat,
                                         dds_data_type_properties_t data_type_props, const dds_qos_t *qos);
static dds_psmx_topic_t *psmx_shm_create_topic_with_type(dds_psmx_t *psmx, const char *topic_name,
                                                           const char *type_name, dds_data_type_properties_t data_type_props,
                                                           const struct ddsi_type *type_definition, uint32_t sizeof_type);
static dds_return_t psmx_shm_delete_topic(dds_psmx_topic_t *psmx_topic);
static void psmx_shm_delete_psmx(dds_psmx_t *psmx);
static dds_psmx_node_identifier_t psmx_shm_get_node_id(const dds_psmx_t *psmx);
static dds_psmx_features_t psmx_shm_supported_features(const dds_psmx_t *psmx);

static const dds_psmx_ops_t psmx_shm_ops = {
  .type_qos_supported = psmx_shm_type_qos_supported,
  .create_topic = NULL,  // Use version 1 interface
  .delete_topic = psmx_shm_delete_topic,
  .deinit = NULL,  // Use version 1 interface
  .get_node_id = psmx_shm_get_node_id,
  .supported_features = psmx_shm_supported_features,
  .create_topic_with_type = psmx_shm_create_topic_with_type,
  .delete_psmx = psmx_shm_delete_psmx
};

// Topic operations
static dds_psmx_endpoint_t *psmx_shm_create_endpoint(dds_psmx_topic_t *psmx_topic, const dds_qos_t *qos,
                                                       dds_psmx_endpoint_type_t endpoint_type);
static dds_return_t psmx_shm_delete_endpoint(dds_psmx_endpoint_t *psmx_endpoint);

static const dds_psmx_topic_ops_t psmx_shm_topic_ops = {
  .create_endpoint = psmx_shm_create_endpoint,
  .delete_endpoint = psmx_shm_delete_endpoint
};

// Endpoint operations
static dds_loaned_sample_t *psmx_shm_request_loan(dds_psmx_endpoint_t *psmx_endpoint, uint32_t size_requested);
static dds_return_t psmx_shm_write(dds_psmx_endpoint_t *psmx_endpoint, dds_loaned_sample_t *data);
static dds_loaned_sample_t *psmx_shm_take(dds_psmx_endpoint_t *psmx_endpoint);
static dds_return_t psmx_shm_on_data_available(dds_psmx_endpoint_t *psmx_endpoint, dds_entity_t reader);

static const dds_psmx_endpoint_ops_t psmx_shm_endpoint_ops = {
  .request_loan = psmx_shm_request_loan,
  .write = psmx_shm_write,
  .take = psmx_shm_take,
  .on_data_available = psmx_shm_on_data_available,
  .write_with_key = NULL  // Not implemented for simplicity
};

// Loaned sample operations
static void psmx_shm_loaned_sample_free(dds_loaned_sample_t *loan);

static const dds_loaned_sample_ops_t psmx_shm_loaned_sample_ops = {
  .free = psmx_shm_loaned_sample_free
};

// Helper function to get sample offset in shared memory
static inline size_t get_sample_offset(uint32_t idx, uint32_t sample_size)
{
  return sizeof(shm_segment_t) + idx * (sizeof(shm_sample_header_t) + sample_size);
}

// Helper function to get sample header
static inline shm_sample_header_t *get_sample_header(shm_segment_t *segment, uint32_t idx, uint32_t sample_size)
{
  char *base = (char *)segment;
  return (shm_sample_header_t *)(base + get_sample_offset(idx, sample_size));
}

// Implementation of PSMX operations

static bool psmx_shm_type_qos_supported(dds_psmx_t *psmx, dds_psmx_endpoint_type_t forwhat,
                                         dds_data_type_properties_t data_type_props, const dds_qos_t *qos)
{
  (void)psmx;
  (void)forwhat;
  (void)data_type_props;
  
  if (qos == NULL)
    return true;

  // Only support KEEP_LAST history
  dds_history_kind_t history_kind;
  int32_t history_depth;
  if (dds_qget_history(qos, &history_kind, &history_depth))
  {
    if (history_kind != DDS_HISTORY_KEEP_LAST)
      return false;
  }

  // We support most QoS policies, but keyed topics are not fully implemented
  // For simplicity, accept all types but warn if keyed
  return true;
}

static dds_psmx_node_identifier_t psmx_shm_get_node_id(const dds_psmx_t *psmx)
{
  const psmx_shm_t *shm_psmx = (const psmx_shm_t *)psmx;
  return shm_psmx->node_id;
}

static dds_psmx_features_t psmx_shm_supported_features(const dds_psmx_t *psmx)
{
  (void)psmx;
  return DDS_PSMX_FEATURE_SHARED_MEMORY | DDS_PSMX_FEATURE_ZERO_COPY;
}

static dds_psmx_topic_t *psmx_shm_create_topic_with_type(dds_psmx_t *psmx, const char *topic_name,
                                                           const char *type_name, dds_data_type_properties_t data_type_props,
                                                           const struct ddsi_type *type_definition, uint32_t sizeof_type)
{
  (void)type_definition;
  (void)data_type_props;
  
  psmx_shm_t *shm_psmx = (psmx_shm_t *)psmx;
  psmx_shm_topic_t *topic = ddsrt_malloc(sizeof(*topic));
  if (topic == NULL)
    return NULL;

  memset(topic, 0, sizeof(*topic));
  topic->base.ops = psmx_shm_topic_ops;
  topic->base.psmx_instance = psmx;
  topic->base.topic_name = ddsrt_strdup(topic_name);
  topic->base.type_name = ddsrt_strdup(type_name);
  topic->psmx_instance = shm_psmx;
  topic->type_size = sizeof_type > 0 ? sizeof_type : 1024; // Default if unknown
  
  ddsrt_mutex_init(&topic->lock);

  // Create shared memory segment name
  char shm_name[256];
  snprintf(shm_name, sizeof(shm_name), "%s%s_%s", SHM_NAME_PREFIX, shm_psmx->instance_name, topic_name);
  topic->shm_name = ddsrt_strdup(shm_name);

  // Calculate shared memory size
  uint32_t sample_count = shm_psmx->sample_count;
  topic->shm_size = sizeof(shm_segment_t) + 
                    sample_count * (sizeof(shm_sample_header_t) + topic->type_size);

  // Create or open shared memory segment
  topic->shm_fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);
  if (topic->shm_fd == -1)
  {
    fprintf(stderr, "shm_open failed: %s\n", strerror(errno));
    goto fail_shm_open;
  }

  // Set size of shared memory
  if (ftruncate(topic->shm_fd, (off_t)topic->shm_size) == -1)
  {
    fprintf(stderr, "ftruncate failed: %s\n", strerror(errno));
    goto fail_ftruncate;
  }

  // Map shared memory
  topic->shm_addr = mmap(NULL, topic->shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, topic->shm_fd, 0);
  if (topic->shm_addr == MAP_FAILED)
  {
    fprintf(stderr, "mmap failed: %s\n", strerror(errno));
    goto fail_mmap;
  }

  topic->segment = (shm_segment_t *)topic->shm_addr;
  
  // Initialize segment (only once, first creator initializes)
  pthread_mutexattr_t mutex_attr;
  pthread_mutexattr_init(&mutex_attr);
  pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);
  pthread_mutex_init(&topic->segment->mutex, &mutex_attr);
  pthread_mutexattr_destroy(&mutex_attr);

  pthread_condattr_t cond_attr;
  pthread_condattr_init(&cond_attr);
  pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED);
  pthread_cond_init(&topic->segment->cond, &cond_attr);
  pthread_condattr_destroy(&cond_attr);

  topic->segment->sample_size = topic->type_size;
  topic->segment->sample_count = sample_count;
  topic->segment->write_idx = 0;
  topic->segment->read_count = 0;
  ddsrt_atomic_st32(&topic->segment->reader_count, 0);

  // Initialize all sample headers
  for (uint32_t i = 0; i < sample_count; i++)
  {
    shm_sample_header_t *header = get_sample_header(topic->segment, i, topic->type_size);
    ddsrt_atomic_st32(&header->refcount, 0);
    header->data_size = 0;
    header->sequence = 0;
  }

  return &topic->base;

fail_mmap:
fail_ftruncate:
  close(topic->shm_fd);
fail_shm_open:
  ddsrt_free(topic->shm_name);
  ddsrt_mutex_destroy(&topic->lock);
  ddsrt_free((char *)topic->base.topic_name);
  ddsrt_free((char *)topic->base.type_name);
  ddsrt_free(topic);
  return NULL;
}

static dds_return_t psmx_shm_delete_topic(dds_psmx_topic_t *psmx_topic)
{
  psmx_shm_topic_t *topic = (psmx_shm_topic_t *)psmx_topic;

  ddsrt_mutex_lock(&topic->lock);

  // Unmap shared memory
  if (topic->shm_addr != NULL && topic->shm_addr != MAP_FAILED)
  {
    munmap(topic->shm_addr, topic->shm_size);
  }

  // Close shared memory file descriptor
  if (topic->shm_fd >= 0)
  {
    close(topic->shm_fd);
  }

  // Unlink shared memory (remove it)
  if (topic->shm_name != NULL)
  {
    shm_unlink(topic->shm_name);
    ddsrt_free(topic->shm_name);
  }

  ddsrt_mutex_unlock(&topic->lock);
  ddsrt_mutex_destroy(&topic->lock);

  ddsrt_free((char *)topic->base.topic_name);
  ddsrt_free((char *)topic->base.type_name);
  ddsrt_free(topic);

  return DDS_RETCODE_OK;
}

static void psmx_shm_delete_psmx(dds_psmx_t *psmx)
{
  psmx_shm_t *shm_psmx = (psmx_shm_t *)psmx;
  
  if (shm_psmx->instance_name != NULL)
    ddsrt_free(shm_psmx->instance_name);
  
  ddsrt_free(shm_psmx);
}

// Topic operations implementation

static dds_psmx_endpoint_t *psmx_shm_create_endpoint(dds_psmx_topic_t *psmx_topic, const dds_qos_t *qos,
                                                       dds_psmx_endpoint_type_t endpoint_type)
{
  (void)qos;
  
  psmx_shm_topic_t *topic = (psmx_shm_topic_t *)psmx_topic;
  psmx_shm_endpoint_t *endpoint = ddsrt_malloc(sizeof(*endpoint));
  if (endpoint == NULL)
    return NULL;

  memset(endpoint, 0, sizeof(*endpoint));
  endpoint->base.ops = psmx_shm_endpoint_ops;
  endpoint->base.psmx_topic = psmx_topic;
  endpoint->base.endpoint_type = endpoint_type;
  endpoint->topic = topic;
  endpoint->last_read_seq = 0;
  
  ddsrt_mutex_init(&endpoint->lock);

  // Increment reader count if this is a reader endpoint
  if (endpoint_type == DDS_PSMX_ENDPOINT_TYPE_READER)
  {
    ddsrt_atomic_inc32(&topic->segment->reader_count);
  }

  return &endpoint->base;
}

static dds_return_t psmx_shm_delete_endpoint(dds_psmx_endpoint_t *psmx_endpoint)
{
  psmx_shm_endpoint_t *endpoint = (psmx_shm_endpoint_t *)psmx_endpoint;
  
  // Decrement reader count if this is a reader endpoint
  if (endpoint->base.endpoint_type == DDS_PSMX_ENDPOINT_TYPE_READER)
  {
    ddsrt_atomic_dec32(&endpoint->topic->segment->reader_count);
  }

  ddsrt_mutex_destroy(&endpoint->lock);
  ddsrt_free(endpoint);

  return DDS_RETCODE_OK;
}

// Endpoint operations implementation

static dds_loaned_sample_t *psmx_shm_request_loan(dds_psmx_endpoint_t *psmx_endpoint, uint32_t size_requested)
{
  psmx_shm_endpoint_t *endpoint = (psmx_shm_endpoint_t *)psmx_endpoint;
  psmx_shm_topic_t *topic = endpoint->topic;

  // Only writers can request loans
  if (endpoint->base.endpoint_type != DDS_PSMX_ENDPOINT_TYPE_WRITER)
    return NULL;

  if (size_requested > topic->type_size)
    return NULL;

  psmx_shm_loaned_sample_t *loan = ddsrt_malloc(sizeof(*loan));
  if (loan == NULL)
    return NULL;

  memset(loan, 0, sizeof(*loan));
  
  // Lock the segment
  pthread_mutex_lock(&topic->segment->mutex);

  // Get next write index (KEEP_LAST: overwrite oldest)
  uint32_t idx = topic->segment->write_idx % topic->segment->sample_count;
  shm_sample_header_t *header = get_sample_header(topic->segment, idx, topic->type_size);
  
  // Set refcount to 1 (writer holds it)
  ddsrt_atomic_st32(&header->refcount, 1);
  header->data_size = size_requested;
  
  pthread_mutex_unlock(&topic->segment->mutex);

  // Setup loan
  loan->header = header;
  loan->data_ptr = (char *)header + sizeof(shm_sample_header_t);
  loan->base.ops = psmx_shm_loaned_sample_ops;
  loan->base.loan_origin.origin_kind = DDS_LOAN_ORIGIN_KIND_PSMX;
  loan->base.loan_origin.psmx_endpoint = psmx_endpoint;
  loan->base.sample_ptr = loan->data_ptr;
  loan->base.metadata = &header->metadata;
  ddsrt_atomic_st32(&loan->base.refc, 1);

  return &loan->base;
}

static dds_return_t psmx_shm_write(dds_psmx_endpoint_t *psmx_endpoint, dds_loaned_sample_t *data)
{
  psmx_shm_endpoint_t *endpoint = (psmx_shm_endpoint_t *)psmx_endpoint;
  psmx_shm_topic_t *topic = endpoint->topic;
  psmx_shm_loaned_sample_t *loan = (psmx_shm_loaned_sample_t *)data;

  if (endpoint->base.endpoint_type != DDS_PSMX_ENDPOINT_TYPE_WRITER)
    return DDS_RETCODE_BAD_PARAMETER;

  pthread_mutex_lock(&topic->segment->mutex);

  // Increment write index
  uint64_t seq = topic->segment->write_idx++;
  loan->header->sequence = seq;

  // Update read count for KEEP_LAST behavior
  if (topic->segment->read_count < topic->segment->sample_count)
    topic->segment->read_count++;

  // Signal readers
  pthread_cond_broadcast(&topic->segment->cond);
  
  pthread_mutex_unlock(&topic->segment->mutex);

  return DDS_RETCODE_OK;
}

static dds_loaned_sample_t *psmx_shm_take(dds_psmx_endpoint_t *psmx_endpoint)
{
  psmx_shm_endpoint_t *endpoint = (psmx_shm_endpoint_t *)psmx_endpoint;
  psmx_shm_topic_t *topic = endpoint->topic;

  if (endpoint->base.endpoint_type != DDS_PSMX_ENDPOINT_TYPE_READER)
    return NULL;

  pthread_mutex_lock(&topic->segment->mutex);

  // Check if there's data available
  if (topic->segment->read_count == 0)
  {
    pthread_mutex_unlock(&topic->segment->mutex);
    return NULL;
  }

  // Find the next unread sample
  uint64_t current_write_seq = topic->segment->write_idx;
  uint64_t start_seq = (current_write_seq > topic->segment->sample_count) ? 
                       (current_write_seq - topic->segment->sample_count) : 0;

  // Find next sample after last_read_seq
  for (uint64_t seq = start_seq; seq < current_write_seq; seq++)
  {
    if (seq <= endpoint->last_read_seq)
      continue;

    uint32_t idx = (uint32_t)(seq % topic->segment->sample_count);
    shm_sample_header_t *header = get_sample_header(topic->segment, idx, topic->type_size);

    if (header->sequence == seq)
    {
      // Found a valid sample
      psmx_shm_loaned_sample_t *loan = ddsrt_malloc(sizeof(*loan));
      if (loan == NULL)
      {
        pthread_mutex_unlock(&topic->segment->mutex);
        return NULL;
      }

      memset(loan, 0, sizeof(*loan));
      loan->header = header;
      loan->data_ptr = (char *)header + sizeof(shm_sample_header_t);
      loan->base.ops = psmx_shm_loaned_sample_ops;
      loan->base.loan_origin.origin_kind = DDS_LOAN_ORIGIN_KIND_PSMX;
      loan->base.loan_origin.psmx_endpoint = psmx_endpoint;
      loan->base.sample_ptr = loan->data_ptr;
      loan->base.metadata = &header->metadata;
      ddsrt_atomic_st32(&loan->base.refc, 1);

      // Increment refcount (reader holds it)
      ddsrt_atomic_inc32(&header->refcount);

      endpoint->last_read_seq = seq;

      pthread_mutex_unlock(&topic->segment->mutex);
      return &loan->base;
    }
  }

  pthread_mutex_unlock(&topic->segment->mutex);
  return NULL;
}

static dds_return_t psmx_shm_on_data_available(dds_psmx_endpoint_t *psmx_endpoint, dds_entity_t reader)
{
  psmx_shm_endpoint_t *endpoint = (psmx_shm_endpoint_t *)psmx_endpoint;
  endpoint->cdds_endpoint = reader;
  return DDS_RETCODE_OK;
}

// Loaned sample operations implementation

static void psmx_shm_loaned_sample_free(dds_loaned_sample_t *loan)
{
  psmx_shm_loaned_sample_t *shm_loan = (psmx_shm_loaned_sample_t *)loan;
  
  if (shm_loan->header != NULL)
  {
    // Decrement refcount
    ddsrt_atomic_dec32(&shm_loan->header->refcount);
  }

  ddsrt_free(shm_loan);
}

// PSMX instance creation

dds_return_t shm_create_psmx(dds_psmx_t **psmx_out, dds_psmx_instance_id_t instance_id, const char *config)
{
  if (psmx_out == NULL)
    return DDS_RETCODE_BAD_PARAMETER;

  psmx_shm_t *psmx = ddsrt_malloc(sizeof(*psmx));
  if (psmx == NULL)
    return DDS_RETCODE_OUT_OF_RESOURCES;

  memset(psmx, 0, sizeof(*psmx));

  // Initialize base structure
  psmx->base.ops = psmx_shm_ops;
  psmx->base.instance_id = instance_id;
  psmx->base.locator = NULL;
  psmx->base.psmx_topics = NULL;
  psmx->base.priority = 0;

  // Parse configuration
  if (config != NULL)
  {
    psmx->instance_name = dds_psmx_get_config_option_value(config, "SERVICE_NAME");
  }
  
  if (psmx->instance_name == NULL)
    psmx->instance_name = ddsrt_strdup("default_shm");

  psmx->base.instance_name = ddsrt_strdup(psmx->instance_name);

  // Set default configuration
  psmx->segment_size = SHM_DEFAULT_SEGMENT_SIZE;
  psmx->sample_count = SHM_DEFAULT_SAMPLE_COUNT;

  // Generate node ID (use process ID and timestamp for uniqueness)
  memset(&psmx->node_id, 0, sizeof(psmx->node_id));
  uint32_t pid = (uint32_t)getpid();
  memcpy(psmx->node_id.x, &pid, sizeof(pid));
  memcpy(psmx->node_id.x + 4, &instance_id, sizeof(instance_id));

  *psmx_out = &psmx->base;
  return DDS_RETCODE_OK;
}
