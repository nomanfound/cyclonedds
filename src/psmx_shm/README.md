# PSMX Shared Memory Plugin

## Overview

This is a PSMX (Publish Subscribe Message Exchange) plugin for CycloneDDS that implements shared memory transport using POSIX APIs. It provides zero-copy data transfer between processes on the same machine using shared memory segments.

## Features

- **Version 1 PSMX Interface**: Implements the latest PSMX interface with type information support
- **Per-Topic Segments**: Each topic gets its own shared memory segment for better isolation
- **Zero-Copy**: Supports loan mechanism for efficient data transfer without copying
- **KEEP_LAST QoS**: Optimized for KEEP_LAST history policy
- **Single Writer, Multiple Reader**: Designed for the common SWMR pattern
- **POSIX Compliance**: Uses standard POSIX APIs (shm_open, mmap, pthread) for embedded systems

## Architecture

The plugin uses the following components:

1. **Shared Memory Segments**: Created per topic using `shm_open()` and `mmap()`
2. **Process-Shared Synchronization**: Uses `pthread_mutex_t` and `pthread_cond_t` with `PTHREAD_PROCESS_SHARED` attribute
3. **Reference Counting**: Tracks sample usage across readers to manage memory
4. **Circular Buffer**: Implements KEEP_LAST behavior with a ring buffer structure

## Configuration

The plugin can be configured through the CycloneDDS configuration XML:

```xml
<Domain>
  <PubSubMessageExchanges>
    <PubSubMessageExchange name="shm" library="libpsmx_shm" priority="0">
      <ServiceName>my_shm_instance</ServiceName>
    </PubSubMessageExchange>
  </PubSubMessageExchanges>
</Domain>
```

Configuration options:
- `ServiceName`: Name prefix for shared memory segments (default: "default_shm")

## Building

The plugin is built as part of the CycloneDDS build process. It requires:

- POSIX-compliant operating system (Linux, macOS, etc.)
- pthread library
- rt library (for POSIX shared memory)

To build:

```bash
mkdir build && cd build
cmake ..
make psmx_shm
```

## Usage

### Enabling the Plugin

To use the plugin, configure it in your CycloneDDS XML configuration file and create topics/readers/writers with the `psmx_instances` QoS setting:

```c
#include "dds/dds.h"

// Create participant with PSMX configuration
dds_entity_t participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);

// Create topic
dds_entity_t topic = dds_create_topic(participant, &desc, "MyTopic", NULL, NULL);

// Create reader/writer with PSMX enabled (configured in XML)
dds_entity_t writer = dds_create_writer(participant, topic, NULL, NULL);
dds_entity_t reader = dds_create_reader(participant, topic, NULL, NULL);
```

### Loan-Based Writing (Zero-Copy)

```c
// Request a loan
MyDataType *sample;
dds_return_t rc = dds_request_loan(writer, (void**)&sample);
if (rc == DDS_RETCODE_OK) {
    // Fill in data
    sample->field1 = 42;
    sample->field2 = 3.14;
    
    // Write the sample (transfers ownership)
    dds_write(writer, sample);
}
```

## Limitations

- **Single Process Shared Memory**: Works only between processes on the same machine
- **KEEP_LAST Only**: Optimized for KEEP_LAST history; KEEP_ALL is not fully supported
- **No Keyed Topics**: Keyed topic support is limited
- **Fixed Sample Count**: Uses a fixed circular buffer size (default: 32 samples)

## Implementation Notes

### Memory Layout

Each shared memory segment contains:
```
+-------------------+
| shm_segment_t     | <- Control structure with mutex, condition variable
+-------------------+
| Sample 0 Header   | <- Metadata and refcount
| Sample 0 Data     |
+-------------------+
| Sample 1 Header   |
| Sample 1 Data     |
+-------------------+
| ...               |
+-------------------+
```

### Thread Safety

- All shared memory operations are protected by a process-shared mutex
- Reference counting uses atomic operations
- Readers wait on a process-shared condition variable for new data

### Sample Lifecycle

1. **Writer Requests Loan**: Allocates next slot in ring buffer
2. **Writer Fills Data**: Application writes to loaned memory
3. **Writer Publishes**: Increments write index, signals readers
4. **Reader Takes Sample**: Increments refcount, returns loan
5. **Reader Returns Loan**: Decrements refcount, frees when zero

## License

This code is licensed under EPL-2.0 OR BSD-3-Clause, same as CycloneDDS.
