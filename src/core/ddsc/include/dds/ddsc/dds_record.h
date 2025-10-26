// Copyright(c) 2024 ZettaScale Technology and others
//
// This program and the accompanying materials are made available under the
// terms of the Eclipse Public License v. 2.0 which is available at
// http://www.eclipse.org/legal/epl-2.0, or the Eclipse Distribution License
// v. 1.0 which is available at
// http://www.eclipse.org/org/documents/edl-v10.php.
//
// SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause

#ifndef DDS_RECORD_H
#define DDS_RECORD_H

#include <stdint.h>

#include "dds/export.h"
#include "dds/dds.h"
#include "dds/ddsc/dds_public_impl.h"

#if defined (__cplusplus)
extern "C" {
#endif

struct dds_record_writer;
typedef struct dds_record_writer dds_record_writer_t;

struct dds_record_reader;
typedef struct dds_record_reader dds_record_reader_t;

/** Chunk kind returned by the record reader. */
typedef enum dds_record_kind
{
  DDS_RECORD_KIND_NONE = 0,
  DDS_RECORD_KIND_TYPE = 1,
  DDS_RECORD_KIND_TOPIC = 2,
  DDS_RECORD_KIND_SAMPLE = 3
}
dds_record_kind_t;

typedef struct dds_record_key_descriptor
{
  char * name;
  uint32_t offset;
  uint32_t index;
}
dds_record_key_descriptor_t;

typedef struct dds_record_type
{
  uint32_t id;
  uint32_t size;
  uint32_t align;
  uint32_t flagset;
  uint32_t nkeys;
  dds_record_key_descriptor_t * keys;
  uint32_t nops;
  uint32_t * ops;
  char * type_name;
  char * meta;
  uint32_t restrict_data_representation;
  unsigned char * type_information;
  uint32_t type_information_len;
  unsigned char * type_mapping;
  uint32_t type_mapping_len;
}
dds_record_type_t;

typedef struct dds_record_topic
{
  uint32_t id;
  uint32_t type_id;
  char * name;
}
dds_record_topic_t;

typedef struct dds_record_sample
{
  uint32_t topic_id;
  dds_data_representation_id_t data_representation;
  dds_time_t timestamp;
  uint32_t payload_size;
  unsigned char * payload;
}
dds_record_sample_t;

typedef struct dds_record_entry
{
  dds_record_kind_t kind;
  union
  {
    dds_record_type_t type;
    dds_record_topic_t topic;
    dds_record_sample_t sample;
  }
  data;
}
dds_record_entry_t;

DDS_EXPORT dds_record_writer_t *
dds_record_writer_open(const char *path);

DDS_EXPORT void
dds_record_writer_close(dds_record_writer_t *writer);

DDS_EXPORT dds_return_t
dds_record_writer_register_descriptor(dds_record_writer_t *writer,
  const char *topic_name,
  const dds_topic_descriptor_t *descriptor,
  uint32_t *topic_id_out);

DDS_EXPORT dds_return_t
dds_record_writer_register_entity(dds_record_writer_t *writer,
  dds_entity_t entity,
  uint32_t *topic_id_out);

DDS_EXPORT dds_return_t
dds_record_writer_write_serialized(dds_record_writer_t *writer,
  uint32_t topic_id,
  dds_time_t timestamp,
  dds_data_representation_id_t data_representation,
  const void *payload,
  uint32_t payload_size);

DDS_EXPORT dds_return_t
dds_record_writer_flush(dds_record_writer_t *writer);

DDS_EXPORT dds_record_reader_t *
dds_record_reader_open(const char *path);

DDS_EXPORT void
dds_record_reader_close(dds_record_reader_t *reader);

DDS_EXPORT dds_return_t
dds_record_reader_next(dds_record_reader_t *reader, dds_record_entry_t *entry);

DDS_EXPORT void
dds_record_entry_fini(dds_record_entry_t *entry);

#if defined (__cplusplus)
}
#endif

#endif /* DDS_RECORD_H */
