// Copyright(c) 2024 ZettaScale Technology and others
//
// This program and the accompanying materials are made available under the
// terms of the Eclipse Public License v. 2.0 which is available at
// http://www.eclipse.org/legal/epl-2.0, or the Eclipse Distribution License
// v. 1.0 which is available at
// http://www.eclipse.org/org/documents/edl-v10.php.
//
// SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause

#include "dds/ddsc/dds_record.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "dds/ddsrt/heap.h"
#include "dds/ddsrt/string.h"
#include "dds/ddsrt/bswap.h"
#include "dds/ddsrt/endian.h"
#include "dds/ddsc/dds_opcodes.h"

#define DDS_RECORD_MAGIC "DDSR"
#define DDS_RECORD_VERSION 1u

#define DDS_RECORD_CHUNK_TYPE   1u
#define DDS_RECORD_CHUNK_TOPIC  2u
#define DDS_RECORD_CHUNK_SAMPLE 3u

struct dds_record_writer_type
{
  uint32_t id;
  unsigned char *type_information;
  uint32_t type_information_len;
};

struct dds_record_writer_topic
{
  uint32_t id;
  uint32_t type_id;
};

struct dds_record_writer
{
  FILE *fp;
  uint32_t next_type_id;
  uint32_t next_topic_id;
  struct dds_record_writer_type *types;
  size_t ntypes;
  size_t mtypes;
  struct dds_record_writer_topic *topics;
  size_t ntopics;
  size_t mtopics;
};

struct dds_record_reader
{
  FILE *fp;
};

struct dds_record_chunk_header
{
  uint16_t kind;
  uint16_t flags;
  uint32_t length;
};

struct dds_record_file_header
{
  char magic[4];
  uint32_t version;
  uint32_t flags;
};

#if DDSRT_ENDIAN == DDSRT_LITTLE_ENDIAN
#define RR_FROM_LE16(x) (x)
#define RR_FROM_LE32(x) (x)
#define RR_FROM_LE64(x) (x)
#else
#define RR_FROM_LE16(x) ddsrt_bswap2u (x)
#define RR_FROM_LE32(x) ddsrt_bswap4u (x)
#define RR_FROM_LE64(x) ddsrt_bswap8u (x)
#endif

static void
writer_free (dds_record_writer_t *writer)
{
  if (!writer)
    return;
  for (size_t i = 0; i < writer->ntypes; i++)
  {
    ddsrt_free (writer->types[i].type_information);
  }
  ddsrt_free (writer->types);
  ddsrt_free (writer->topics);
  if (writer->fp)
    fclose (writer->fp);
  ddsrt_free (writer);
}

static dds_return_t
writer_write_all (FILE *fp, const void *ptr, size_t size)
{
  if (size == 0)
    return DDS_RETCODE_OK;
  if (fwrite (ptr, size, 1, fp) != 1)
    return DDS_RETCODE_ERROR;
  return DDS_RETCODE_OK;
}

static dds_return_t
writer_write_chunk (dds_record_writer_t *writer, uint16_t kind, const void *payload, uint32_t payload_len)
{
  struct dds_record_chunk_header hdr;
  hdr.kind = ddsrt_toLE2u (kind);
  hdr.flags = ddsrt_toLE2u (0u);
  hdr.length = ddsrt_toLE4u (payload_len);
  dds_return_t rc;
  if ((rc = writer_write_all (writer->fp, &hdr, sizeof (hdr))) != DDS_RETCODE_OK)
    return rc;
  return writer_write_all (writer->fp, payload, payload_len);
}

static dds_return_t
writer_store_type_entry (dds_record_writer_t *writer, uint32_t type_id, const struct dds_type_meta_ser *type_information)
{
  if (writer->ntypes == writer->mtypes)
  {
    size_t newcap = writer->mtypes == 0 ? 4 : writer->mtypes * 2;
    struct dds_record_writer_type *tmp = ddsrt_realloc (writer->types, newcap * sizeof (*tmp));
    if (!tmp)
      return DDS_RETCODE_OUT_OF_RESOURCES;
    writer->types = tmp;
    writer->mtypes = newcap;
  }
  writer->types[writer->ntypes].id = type_id;
  if (type_information && type_information->data && type_information->sz > 0)
  {
    writer->types[writer->ntypes].type_information = ddsrt_memdup (type_information->data, type_information->sz);
    if (!writer->types[writer->ntypes].type_information)
      return DDS_RETCODE_OUT_OF_RESOURCES;
    writer->types[writer->ntypes].type_information_len = type_information->sz;
  }
  else
  {
    writer->types[writer->ntypes].type_information = NULL;
    writer->types[writer->ntypes].type_information_len = 0;
  }
  writer->ntypes++;
  return DDS_RETCODE_OK;
}

static dds_return_t
writer_store_topic_entry (dds_record_writer_t *writer, uint32_t topic_id, uint32_t type_id)
{
  if (writer->ntopics == writer->mtopics)
  {
    size_t newcap = writer->mtopics == 0 ? 4 : writer->mtopics * 2;
    struct dds_record_writer_topic *tmp = ddsrt_realloc (writer->topics, newcap * sizeof (*tmp));
    if (!tmp)
      return DDS_RETCODE_OUT_OF_RESOURCES;
    writer->topics = tmp;
    writer->mtopics = newcap;
  }
  writer->topics[writer->ntopics].id = topic_id;
  writer->topics[writer->ntopics].type_id = type_id;
  writer->ntopics++;
  return DDS_RETCODE_OK;
}

static dds_return_t
writer_emit_type (dds_record_writer_t *writer, uint32_t type_id, const dds_topic_descriptor_t *descriptor)
{
  if (descriptor->m_nkeys > 0 && descriptor->m_keys == NULL)
    return DDS_RETCODE_PRECONDITION_NOT_MET;
  if (descriptor->m_nops > 0 && descriptor->m_ops == NULL)
    return DDS_RETCODE_PRECONDITION_NOT_MET;
  const uint32_t type_name_len = descriptor->m_typename ? (uint32_t) strlen (descriptor->m_typename) : 0u;
  const uint32_t meta_len = descriptor->m_meta ? (uint32_t) strlen (descriptor->m_meta) : 0u;
  const uint32_t ops_len = descriptor->m_nops * (uint32_t) sizeof (uint32_t);
  const uint32_t type_info_len = descriptor->type_information.data ? descriptor->type_information.sz : 0u;
  const uint32_t type_mapping_len = descriptor->type_mapping.data ? descriptor->type_mapping.sz : 0u;
  uint64_t keys_len = 0;
  for (uint32_t i = 0; i < descriptor->m_nkeys; i++)
  {
    const dds_key_descriptor_t *key = &descriptor->m_keys[i];
    const uint32_t key_name_len = key->m_name ? (uint32_t) strlen (key->m_name) : 0u;
    keys_len += 3u * sizeof (uint32_t) + key_name_len;
  }
  const uint64_t base = 12u * sizeof (uint32_t);
  const uint64_t total = base + type_name_len + meta_len + keys_len + ops_len + type_info_len + type_mapping_len;
  if (total > UINT32_MAX)
    return DDS_RETCODE_OUT_OF_RESOURCES;
  const uint32_t payload_len = (uint32_t) total;
  unsigned char *payload = ddsrt_malloc (payload_len);
  if (!payload && payload_len > 0)
    return DDS_RETCODE_OUT_OF_RESOURCES;
  unsigned char *dst = payload;
  const unsigned char * const end = payload ? payload + payload_len : NULL;
  (void) end;

  const uint32_t header_fields[12] = {
    type_id,
    descriptor->m_size,
    descriptor->m_align,
    descriptor->m_flagset,
    descriptor->m_nkeys,
    descriptor->m_nops,
    type_name_len,
    meta_len,
    ops_len,
    type_info_len,
    type_mapping_len,
    descriptor->m_flagset & DDS_TOPIC_RESTRICT_DATA_REPRESENTATION ? descriptor->restrict_data_representation : 0u
  };
  for (size_t i = 0; i < 12; i++)
  {
    const uint32_t v = ddsrt_toLE4u (header_fields[i]);
    memcpy (dst, &v, sizeof (v));
    dst += sizeof (v);
  }
  if (type_name_len > 0)
  {
    memcpy (dst, descriptor->m_typename, type_name_len);
    dst += type_name_len;
  }
  if (meta_len > 0)
  {
    memcpy (dst, descriptor->m_meta, meta_len);
    dst += meta_len;
  }
  for (uint32_t i = 0; i < descriptor->m_nkeys; i++)
  {
    const dds_key_descriptor_t *key = &descriptor->m_keys[i];
    const uint32_t key_name_len = key->m_name ? (uint32_t) strlen (key->m_name) : 0u;
    const uint32_t key_fields[3] = {
      key_name_len,
      key->m_offset,
      key->m_idx
    };
    for (size_t j = 0; j < 3; j++)
    {
      const uint32_t v = ddsrt_toLE4u (key_fields[j]);
      memcpy (dst, &v, sizeof (v));
      dst += sizeof (v);
    }
    if (key_name_len > 0)
    {
      memcpy (dst, key->m_name, key_name_len);
      dst += key_name_len;
    }
  }
  for (uint32_t i = 0; i < descriptor->m_nops; i++)
  {
    const uint32_t v = ddsrt_toLE4u (descriptor->m_ops[i]);
    memcpy (dst, &v, sizeof (v));
    dst += sizeof (v);
  }
  if (type_info_len > 0)
  {
    memcpy (dst, descriptor->type_information.data, type_info_len);
    dst += type_info_len;
  }
  if (type_mapping_len > 0)
  {
    memcpy (dst, descriptor->type_mapping.data, type_mapping_len);
    dst += type_mapping_len;
  }

  dds_return_t rc = writer_write_chunk (writer, DDS_RECORD_CHUNK_TYPE, payload, payload_len);
  ddsrt_free (payload);
  return rc;
}

static dds_return_t
writer_emit_topic (dds_record_writer_t *writer, uint32_t topic_id, uint32_t type_id, const char *topic_name)
{
  const uint32_t name_len = topic_name ? (uint32_t) strlen (topic_name) : 0u;
  const uint32_t payload_len = 3u * sizeof (uint32_t) + name_len;
  unsigned char *payload = ddsrt_malloc (payload_len);
  if (!payload && payload_len > 0)
    return DDS_RETCODE_OUT_OF_RESOURCES;
  unsigned char *dst = payload;
  const uint32_t fields[3] = { topic_id, type_id, name_len };
  for (size_t i = 0; i < 3; i++)
  {
    const uint32_t v = ddsrt_toLE4u (fields[i]);
    memcpy (dst, &v, sizeof (v));
    dst += sizeof (v);
  }
  if (name_len > 0)
  {
    memcpy (dst, topic_name, name_len);
    dst += name_len;
  }
  dds_return_t rc = writer_write_chunk (writer, DDS_RECORD_CHUNK_TOPIC, payload, payload_len);
  ddsrt_free (payload);
  return rc;
}

static dds_return_t
writer_emit_sample (dds_record_writer_t *writer, uint32_t topic_id, dds_time_t timestamp,
  dds_data_representation_id_t data_representation, const void *payload, uint32_t payload_size)
{
  const uint32_t header_len = 3u * sizeof (uint32_t) + sizeof (uint64_t);
  const uint64_t total = header_len + payload_size;
  if (total > UINT32_MAX)
    return DDS_RETCODE_OUT_OF_RESOURCES;
  const uint32_t chunk_len = (uint32_t) total;
  unsigned char *buf = ddsrt_malloc (chunk_len);
  if (!buf && chunk_len > 0)
    return DDS_RETCODE_OUT_OF_RESOURCES;
  unsigned char *dst = buf;
  const uint32_t topic_field = ddsrt_toLE4u (topic_id);
  const uint32_t repr_field = ddsrt_toLE4u ((uint32_t) data_representation);
  const uint64_t ts_field = ddsrt_toLE8u ((uint64_t) timestamp);
  const uint32_t payload_field = ddsrt_toLE4u (payload_size);
  memcpy (dst, &topic_field, sizeof (topic_field)); dst += sizeof (topic_field);
  memcpy (dst, &repr_field, sizeof (repr_field)); dst += sizeof (repr_field);
  memcpy (dst, &ts_field, sizeof (ts_field)); dst += sizeof (ts_field);
  memcpy (dst, &payload_field, sizeof (payload_field)); dst += sizeof (payload_field);
  if (payload_size > 0)
  {
    memcpy (dst, payload, payload_size);
    dst += payload_size;
  }
  dds_return_t rc = writer_write_chunk (writer, DDS_RECORD_CHUNK_SAMPLE, buf, chunk_len);
  ddsrt_free (buf);
  return rc;
}

dds_record_writer_t *
dds_record_writer_open (const char *path)
{
  dds_record_writer_t *writer = ddsrt_calloc (1, sizeof (*writer));
  if (!writer)
    return NULL;
  writer->fp = fopen (path, "wb");
  if (!writer->fp)
  {
    ddsrt_free (writer);
    return NULL;
  }
  struct dds_record_file_header header;
  memcpy (header.magic, DDS_RECORD_MAGIC, sizeof (header.magic));
  header.version = ddsrt_toLE4u (DDS_RECORD_VERSION);
  header.flags = ddsrt_toLE4u (0u);
  if (writer_write_all (writer->fp, &header, sizeof (header)) != DDS_RETCODE_OK)
  {
    writer_free (writer);
    return NULL;
  }
  writer->next_type_id = 1u;
  writer->next_topic_id = 1u;
  return writer;
}

void
dds_record_writer_close (dds_record_writer_t *writer)
{
  if (!writer)
    return;
  dds_record_writer_flush (writer);
  writer_free (writer);
}

static dds_return_t
writer_lookup_type (dds_record_writer_t *writer, const dds_topic_descriptor_t *descriptor,
  uint32_t *type_id)
{
  const struct dds_type_meta_ser *ser = &descriptor->type_information;
  if (ser->data && ser->sz > 0)
  {
    for (size_t i = 0; i < writer->ntypes; i++)
    {
      if (writer->types[i].type_information_len == ser->sz &&
          memcmp (writer->types[i].type_information, ser->data, ser->sz) == 0)
      {
        *type_id = writer->types[i].id;
        return DDS_RETCODE_OK;
      }
    }
  }
  const uint32_t new_id = writer->next_type_id;
  dds_return_t rc = writer_emit_type (writer, new_id, descriptor);
  if (rc != DDS_RETCODE_OK)
    return rc;
  writer->next_type_id = new_id + 1u;
  if ((rc = writer_store_type_entry (writer, new_id, ser)) != DDS_RETCODE_OK)
    return rc;
  *type_id = new_id;
  return DDS_RETCODE_OK;
}

dds_return_t
dds_record_writer_register_descriptor (dds_record_writer_t *writer, const char *topic_name,
  const dds_topic_descriptor_t *descriptor, uint32_t *topic_id_out)
{
  if (!writer || !topic_name || !descriptor)
    return DDS_RETCODE_BAD_PARAMETER;
  uint32_t type_id;
  dds_return_t rc = writer_lookup_type (writer, descriptor, &type_id);
  if (rc != DDS_RETCODE_OK)
    return rc;
  const uint32_t topic_id = writer->next_topic_id;
  if ((rc = writer_emit_topic (writer, topic_id, type_id, topic_name)) != DDS_RETCODE_OK)
    return rc;
  if ((rc = writer_store_topic_entry (writer, topic_id, type_id)) != DDS_RETCODE_OK)
  {
    writer->next_topic_id = topic_id + 1u;
    return rc;
  }
  writer->next_topic_id = topic_id + 1u;
  if (topic_id_out)
    *topic_id_out = topic_id;
  return DDS_RETCODE_OK;
}

dds_return_t
dds_record_writer_register_entity (dds_record_writer_t *writer, dds_entity_t entity, uint32_t *topic_id_out)
{
  if (!writer)
    return DDS_RETCODE_BAD_PARAMETER;
  dds_entity_t topic = dds_get_topic (entity);
  if (topic < 0)
    return (dds_return_t) topic;
  if (topic == 0)
    return DDS_RETCODE_ILLEGAL_OPERATION;

  char tmp = '\0';
  dds_return_t name_len = dds_get_name (topic, &tmp, 1u);
  if (name_len < 0)
    return name_len;
  const size_t buf_len = (size_t) name_len + 1u;
  char *name = ddsrt_malloc (buf_len);
  if (!name)
    return DDS_RETCODE_OUT_OF_RESOURCES;
  dds_return_t rc = dds_get_name (topic, name, buf_len);
  if (rc < 0)
  {
    ddsrt_free (name);
    return rc;
  }

  dds_typeinfo_t *type_info = NULL;
  rc = dds_get_typeinfo (entity, &type_info);
  if (rc != DDS_RETCODE_OK)
  {
    ddsrt_free (name);
    return rc;
  }

  dds_entity_t participant = dds_get_participant (entity);
  if (participant <= 0)
  {
    dds_free_typeinfo (type_info);
    ddsrt_free (name);
    return participant == 0 ? DDS_RETCODE_ILLEGAL_OPERATION : (dds_return_t) participant;
  }

  dds_topic_descriptor_t *descriptor = NULL;
  rc = dds_create_topic_descriptor (DDS_FIND_SCOPE_LOCAL_DOMAIN, participant, type_info, DDS_SECS (5), &descriptor);
  dds_free_typeinfo (type_info);
  if (rc != DDS_RETCODE_OK)
  {
    ddsrt_free (name);
    return rc;
  }

  rc = dds_record_writer_register_descriptor (writer, name, descriptor, topic_id_out);
  dds_delete_topic_descriptor (descriptor);
  ddsrt_free (name);
  return rc;
}

dds_return_t
dds_record_writer_write_serialized (dds_record_writer_t *writer, uint32_t topic_id, dds_time_t timestamp,
  dds_data_representation_id_t data_representation, const void *payload, uint32_t payload_size)
{
  if (!writer)
    return DDS_RETCODE_BAD_PARAMETER;
  bool known_topic = false;
  for (size_t i = 0; i < writer->ntopics; i++)
  {
    if (writer->topics[i].id == topic_id)
    {
      known_topic = true;
      break;
    }
  }
  if (!known_topic)
    return DDS_RETCODE_PRECONDITION_NOT_MET;
  if (payload_size > 0 && payload == NULL)
    return DDS_RETCODE_BAD_PARAMETER;
  return writer_emit_sample (writer, topic_id, timestamp, data_representation, payload, payload_size);
}

dds_return_t
dds_record_writer_flush (dds_record_writer_t *writer)
{
  if (!writer)
    return DDS_RETCODE_BAD_PARAMETER;
  if (fflush (writer->fp) != 0)
    return DDS_RETCODE_ERROR;
  return DDS_RETCODE_OK;
}

static dds_return_t
reader_read_all (FILE *fp, void *ptr, size_t size)
{
  if (size == 0)
    return DDS_RETCODE_OK;
  if (fread (ptr, size, 1, fp) != 1)
    return DDS_RETCODE_ERROR;
  return DDS_RETCODE_OK;
}

static void
type_cleanup (dds_record_type_t *type)
{
  if (!type)
    return;
  ddsrt_free (type->type_name);
  ddsrt_free (type->meta);
  for (uint32_t i = 0; i < type->nkeys; i++)
    ddsrt_free (type->keys[i].name);
  ddsrt_free (type->keys);
  ddsrt_free (type->ops);
  ddsrt_free (type->type_information);
  ddsrt_free (type->type_mapping);
  memset (type, 0, sizeof (*type));
}

void
dds_record_entry_fini (dds_record_entry_t *entry)
{
  if (!entry)
    return;
  switch (entry->kind)
  {
    case DDS_RECORD_KIND_TYPE:
      type_cleanup (&entry->data.type);
      break;
    case DDS_RECORD_KIND_TOPIC:
      ddsrt_free (entry->data.topic.name);
      memset (&entry->data.topic, 0, sizeof (entry->data.topic));
      break;
    case DDS_RECORD_KIND_SAMPLE:
      ddsrt_free (entry->data.sample.payload);
      memset (&entry->data.sample, 0, sizeof (entry->data.sample));
      break;
    default:
      break;
  }
  entry->kind = DDS_RECORD_KIND_NONE;
}

static bool
reader_read_u32 (const unsigned char **ptr, size_t *remaining, uint32_t *value)
{
  if (*remaining < sizeof (uint32_t))
    return false;
  uint32_t v;
  memcpy (&v, *ptr, sizeof (v));
  *ptr += sizeof (v);
  *remaining -= sizeof (v);
  *value = RR_FROM_LE32 (v);
  return true;
}

static bool
reader_read_u64 (const unsigned char **ptr, size_t *remaining, uint64_t *value)
{
  if (*remaining < sizeof (uint64_t))
    return false;
  uint64_t v;
  memcpy (&v, *ptr, sizeof (v));
  *ptr += sizeof (v);
  *remaining -= sizeof (v);
  *value = RR_FROM_LE64 (v);
  return true;
}

static bool
reader_copy_bytes (const unsigned char **ptr, size_t *remaining, unsigned char *dst, uint32_t len)
{
  if (*remaining < len)
    return false;
  memcpy (dst, *ptr, len);
  *ptr += len;
  *remaining -= len;
  return true;
}

static dds_return_t
reader_parse_type (const unsigned char *payload, size_t payload_len, dds_record_entry_t *entry)
{
  dds_record_type_t type = { 0 };
  const unsigned char *ptr = payload;
  size_t remaining = payload_len;
  uint32_t fields[12];
  for (size_t i = 0; i < 12; i++)
  {
    if (!reader_read_u32 (&ptr, &remaining, &fields[i]))
    {
      type_cleanup (&type);
      return DDS_RETCODE_PRECONDITION_NOT_MET;
    }
  }
  type.id = fields[0];
  type.size = fields[1];
  type.align = fields[2];
  type.flagset = fields[3];
  type.nkeys = fields[4];
  type.nops = fields[5];
  const uint32_t type_name_len = fields[6];
  const uint32_t meta_len = fields[7];
  const uint32_t ops_len = fields[8];
  const uint32_t type_info_len = fields[9];
  const uint32_t type_mapping_len = fields[10];
  type.restrict_data_representation = fields[11];

  if (type_name_len > 0)
  {
    type.type_name = ddsrt_malloc (type_name_len + 1u);
    if (!type.type_name)
    {
      type_cleanup (&type);
      return DDS_RETCODE_OUT_OF_RESOURCES;
    }
    if (!reader_copy_bytes (&ptr, &remaining, (unsigned char *) type.type_name, type_name_len))
    {
      type_cleanup (&type);
      return DDS_RETCODE_PRECONDITION_NOT_MET;
    }
    type.type_name[type_name_len] = '\0';
  }

  if (meta_len > 0)
  {
    type.meta = ddsrt_malloc (meta_len + 1u);
    if (!type.meta)
    {
      type_cleanup (&type);
      return DDS_RETCODE_OUT_OF_RESOURCES;
    }
    if (!reader_copy_bytes (&ptr, &remaining, (unsigned char *) type.meta, meta_len))
    {
      type_cleanup (&type);
      return DDS_RETCODE_PRECONDITION_NOT_MET;
    }
    type.meta[meta_len] = '\0';
  }

  if (type.nkeys > 0)
  {
    type.keys = ddsrt_calloc (type.nkeys, sizeof (*type.keys));
    if (!type.keys)
    {
      type_cleanup (&type);
      return DDS_RETCODE_OUT_OF_RESOURCES;
    }
    for (uint32_t i = 0; i < type.nkeys; i++)
    {
      uint32_t key_name_len;
      if (!reader_read_u32 (&ptr, &remaining, &key_name_len) ||
          !reader_read_u32 (&ptr, &remaining, &type.keys[i].offset) ||
          !reader_read_u32 (&ptr, &remaining, &type.keys[i].index))
      {
        type_cleanup (&type);
        return DDS_RETCODE_PRECONDITION_NOT_MET;
      }
      if (key_name_len > 0)
      {
        type.keys[i].name = ddsrt_malloc (key_name_len + 1u);
        if (!type.keys[i].name)
        {
          type_cleanup (&type);
          return DDS_RETCODE_OUT_OF_RESOURCES;
        }
        if (!reader_copy_bytes (&ptr, &remaining, (unsigned char *) type.keys[i].name, key_name_len))
        {
          type_cleanup (&type);
          return DDS_RETCODE_PRECONDITION_NOT_MET;
        }
        type.keys[i].name[key_name_len] = '\0';
      }
    }
  }

  if (type.nops > 0)
  {
    const uint32_t expected_ops_len = type.nops * (uint32_t) sizeof (uint32_t);
    if (ops_len != expected_ops_len)
    {
      type_cleanup (&type);
      return DDS_RETCODE_PRECONDITION_NOT_MET;
    }
    type.ops = ddsrt_malloc (type.nops * sizeof (*type.ops));
    if (!type.ops)
    {
      type_cleanup (&type);
      return DDS_RETCODE_OUT_OF_RESOURCES;
    }
    for (uint32_t i = 0; i < type.nops; i++)
    {
      uint32_t op;
      if (!reader_read_u32 (&ptr, &remaining, &op))
      {
        type_cleanup (&type);
        return DDS_RETCODE_PRECONDITION_NOT_MET;
      }
      type.ops[i] = op;
    }
  }
  else if (ops_len != 0)
  {
    type_cleanup (&type);
    return DDS_RETCODE_PRECONDITION_NOT_MET;
  }

  if (type_info_len > 0)
  {
    if (remaining < type_info_len)
    {
      type_cleanup (&type);
      return DDS_RETCODE_PRECONDITION_NOT_MET;
    }
    type.type_information = ddsrt_memdup (ptr, type_info_len);
    if (!type.type_information)
    {
      type_cleanup (&type);
      return DDS_RETCODE_OUT_OF_RESOURCES;
    }
    type.type_information_len = type_info_len;
    ptr += type_info_len;
    remaining -= type_info_len;
  }

  if (type_mapping_len > 0)
  {
    if (remaining < type_mapping_len)
    {
      type_cleanup (&type);
      return DDS_RETCODE_PRECONDITION_NOT_MET;
    }
    type.type_mapping = ddsrt_memdup (ptr, type_mapping_len);
    if (!type.type_mapping)
    {
      type_cleanup (&type);
      return DDS_RETCODE_OUT_OF_RESOURCES;
    }
    type.type_mapping_len = type_mapping_len;
    ptr += type_mapping_len;
    remaining -= type_mapping_len;
  }

  entry->kind = DDS_RECORD_KIND_TYPE;
  entry->data.type = type;
  return DDS_RETCODE_OK;
}

static dds_return_t
reader_parse_topic (const unsigned char *payload, size_t payload_len, dds_record_entry_t *entry)
{
  dds_record_topic_t topic = { 0 };
  const unsigned char *ptr = payload;
  size_t remaining = payload_len;
  uint32_t fields[3];
  for (size_t i = 0; i < 3; i++)
  {
    if (!reader_read_u32 (&ptr, &remaining, &fields[i]))
      return DDS_RETCODE_PRECONDITION_NOT_MET;
  }
  topic.id = fields[0];
  topic.type_id = fields[1];
  const uint32_t name_len = fields[2];
  if (name_len > 0)
  {
    topic.name = ddsrt_malloc (name_len + 1u);
    if (!topic.name)
      return DDS_RETCODE_OUT_OF_RESOURCES;
    if (!reader_copy_bytes (&ptr, &remaining, (unsigned char *) topic.name, name_len))
    {
      ddsrt_free (topic.name);
      return DDS_RETCODE_PRECONDITION_NOT_MET;
    }
    topic.name[name_len] = '\0';
  }
  entry->kind = DDS_RECORD_KIND_TOPIC;
  entry->data.topic = topic;
  return DDS_RETCODE_OK;
}

static dds_return_t
reader_parse_sample (const unsigned char *payload, size_t payload_len, dds_record_entry_t *entry)
{
  dds_record_sample_t sample = { 0 };
  const unsigned char *ptr = payload;
  size_t remaining = payload_len;
  uint32_t topic_id;
  uint32_t data_repr;
  uint64_t timestamp;
  uint32_t payload_size;
  if (!reader_read_u32 (&ptr, &remaining, &topic_id) ||
      !reader_read_u32 (&ptr, &remaining, &data_repr) ||
      !reader_read_u64 (&ptr, &remaining, &timestamp) ||
      !reader_read_u32 (&ptr, &remaining, &payload_size))
    return DDS_RETCODE_PRECONDITION_NOT_MET;
  if (payload_size > remaining)
    return DDS_RETCODE_PRECONDITION_NOT_MET;
  sample.topic_id = topic_id;
  sample.data_representation = (dds_data_representation_id_t) data_repr;
  sample.timestamp = (dds_time_t) timestamp;
  sample.payload_size = payload_size;
  if (payload_size > 0)
  {
    sample.payload = ddsrt_memdup (ptr, payload_size);
    if (!sample.payload)
      return DDS_RETCODE_OUT_OF_RESOURCES;
  }
  entry->kind = DDS_RECORD_KIND_SAMPLE;
  entry->data.sample = sample;
  return DDS_RETCODE_OK;
}

dds_record_reader_t *
dds_record_reader_open (const char *path)
{
  dds_record_reader_t *reader = ddsrt_calloc (1, sizeof (*reader));
  if (!reader)
    return NULL;
  reader->fp = fopen (path, "rb");
  if (!reader->fp)
  {
    ddsrt_free (reader);
    return NULL;
  }
  struct dds_record_file_header header;
  if (reader_read_all (reader->fp, &header, sizeof (header)) != DDS_RETCODE_OK)
  {
    dds_record_reader_close (reader);
    return NULL;
  }
  if (memcmp (header.magic, DDS_RECORD_MAGIC, sizeof (header.magic)) != 0)
  {
    dds_record_reader_close (reader);
    return NULL;
  }
  const uint32_t version = RR_FROM_LE32 (header.version);
  if (version != DDS_RECORD_VERSION)
  {
    dds_record_reader_close (reader);
    return NULL;
  }
  return reader;
}

void
dds_record_reader_close (dds_record_reader_t *reader)
{
  if (!reader)
    return;
  if (reader->fp)
    fclose (reader->fp);
  ddsrt_free (reader);
}

dds_return_t
dds_record_reader_next (dds_record_reader_t *reader, dds_record_entry_t *entry)
{
  if (!reader || !entry)
    return DDS_RETCODE_BAD_PARAMETER;
  dds_record_entry_fini (entry);
  struct dds_record_chunk_header hdr;
  size_t nread = fread (&hdr, sizeof (hdr), 1, reader->fp);
  if (nread == 0)
  {
    if (feof (reader->fp))
      return DDS_RETCODE_NO_DATA;
    return DDS_RETCODE_ERROR;
  }
  if (nread != 1)
    return DDS_RETCODE_ERROR;
  const uint16_t kind = RR_FROM_LE16 (hdr.kind);
  const uint32_t length = RR_FROM_LE32 (hdr.length);
  unsigned char *payload = NULL;
  if (length > 0)
  {
    payload = ddsrt_malloc (length);
    if (!payload)
      return DDS_RETCODE_OUT_OF_RESOURCES;
    if (reader_read_all (reader->fp, payload, length) != DDS_RETCODE_OK)
    {
      ddsrt_free (payload);
      return DDS_RETCODE_ERROR;
    }
  }
  dds_return_t rc = DDS_RETCODE_OK;
  switch (kind)
  {
    case DDS_RECORD_CHUNK_TYPE:
      rc = reader_parse_type (payload, length, entry);
      break;
    case DDS_RECORD_CHUNK_TOPIC:
      rc = reader_parse_topic (payload, length, entry);
      break;
    case DDS_RECORD_CHUNK_SAMPLE:
      rc = reader_parse_sample (payload, length, entry);
      break;
    default:
      rc = DDS_RETCODE_UNSUPPORTED;
      break;
  }
  ddsrt_free (payload);
  if (rc != DDS_RETCODE_OK)
  {
    dds_record_entry_fini (entry);
  }
  return rc;
}
