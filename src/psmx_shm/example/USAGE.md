# PSMX Shared Memory Plugin - Example Usage

## Overview

This directory contains example configuration and usage guidelines for the PSMX shared memory plugin.

## Prerequisites

1. CycloneDDS built with psmx_shm plugin
2. POSIX-compliant operating system (Linux, macOS, etc.)
3. Applications running on the same machine

## Quick Start

### 1. Build the Plugin

```bash
cd /path/to/cyclonedds
mkdir build && cd build
cmake ..
make psmx_shm
sudo make install  # Optional: install system-wide
```

### 2. Configure CycloneDDS

Copy the example configuration file:

```bash
cp src/psmx_shm/example/cyclonedds.xml .
```

Edit `cyclonedds.xml` to configure the plugin (see example configuration).

### 3. Set Environment Variable

```bash
export CYCLONEDDS_URI=file://$(pwd)/cyclonedds.xml
```

Or for system-wide configuration:

```bash
export CYCLONEDDS_URI=file:///etc/cyclonedds/config.xml
```

### 4. Run Your Application

The plugin will automatically be loaded and used when:
- The configuration enables the PSMX plugin
- Topics/Readers/Writers are created
- QoS policies are compatible (KEEP_LAST history)

## Configuration Options

### ServiceName

The `ServiceName` parameter sets the prefix for shared memory segment names:

```xml
<ServiceName>my_application</ServiceName>
```

This results in shared memory segments named:
```
/cyclonedds_shm_my_application_TopicName1
/cyclonedds_shm_my_application_TopicName2
...
```

## Using the Loan Mechanism

The PSMX shared memory plugin supports zero-copy data transfer through the loan API:

### Publisher with Loan

```c
#include "dds/dds.h"
#include "MyDataType.h"  // Generated from IDL

int main(void)
{
    // Create participant, topic, writer
    dds_entity_t participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
    dds_entity_t topic = dds_create_topic(participant, &MyDataType_desc, "MyTopic", NULL, NULL);
    
    // Create QoS with KEEP_LAST
    dds_qos_t *qos = dds_create_qos();
    dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, 10);
    dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_INFINITY);
    
    dds_entity_t writer = dds_create_writer(participant, topic, qos, NULL);
    dds_delete_qos(qos);
    
    // Request a loan for zero-copy write
    MyDataType *sample = NULL;
    dds_return_t rc = dds_request_loan(writer, (void**)&sample);
    if (rc == DDS_RETCODE_OK) {
        // Fill in the data directly in shared memory
        sample->id = 42;
        sample->value = 3.14;
        strcpy(sample->message, "Hello from shared memory!");
        
        // Write the sample (transfers ownership to DDS)
        rc = dds_write(writer, sample);
    }
    
    // Cleanup
    dds_delete(participant);
    return 0;
}
```

### Subscriber with Take

```c
#include "dds/dds.h"
#include "MyDataType.h"  // Generated from IDL

int main(void)
{
    // Create participant, topic, reader
    dds_entity_t participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
    dds_entity_t topic = dds_create_topic(participant, &MyDataType_desc, "MyTopic", NULL, NULL);
    
    dds_qos_t *qos = dds_create_qos();
    dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, 10);
    dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_INFINITY);
    
    dds_entity_t reader = dds_create_reader(participant, topic, qos, NULL);
    dds_delete_qos(qos);
    
    // Wait for data
    void *samples[1] = { NULL };
    dds_sample_info_t info[1];
    
    while (true) {
        dds_return_t rc = dds_take(reader, samples, info, 1, 1);
        if (rc > 0) {
            MyDataType *sample = (MyDataType *)samples[0];
            printf("Received: id=%d, value=%f, message=%s\n",
                   sample->id, sample->value, sample->message);
            
            // Return the loan
            dds_return_loan(reader, samples, rc);
        }
    }
    
    dds_delete(participant);
    return 0;
}
```

## Performance Considerations

### Benefits of Shared Memory

1. **Zero-Copy**: Data is not copied between processes
2. **Low Latency**: Direct memory access, no network stack
3. **High Throughput**: No serialization/deserialization overhead

### Best Practices

1. **Use Loans**: Always use `dds_request_loan()` for writers to enable zero-copy
2. **Return Loans Promptly**: Call `dds_return_loan()` as soon as you're done with samples
3. **Keep Messages Small**: While efficient, shared memory segments have limited size
4. **KEEP_LAST QoS**: The plugin is optimized for KEEP_LAST history

### Limitations

1. **Same Machine Only**: Works only between processes on the same machine
2. **KEEP_LAST Optimized**: KEEP_ALL history is not fully supported
3. **Fixed Buffer Size**: Default 32 samples per topic (compile-time constant)

## Debugging

### Enable Tracing

Add to your `cyclonedds.xml`:

```xml
<Tracing>
  <Verbosity>finest</Verbosity>
  <Category>trace</Category>
  <OutputFile>cyclonedds.log</OutputFile>
</Tracing>
```

### Check Shared Memory Segments

List active shared memory segments:

```bash
# Linux
ls -l /dev/shm/cyclonedds_shm_*

# macOS
ls -l /private/tmp/cyclonedds_shm_*
```

### Clean Up Shared Memory

If segments aren't cleaned up properly:

```bash
# Linux
rm /dev/shm/cyclonedds_shm_*

# macOS
rm /private/tmp/cyclonedds_shm_*
```

## Troubleshooting

### Plugin Not Loading

**Problem**: Plugin library not found

**Solution**: 
- Ensure `libpsmx_shm.so` is in library path
- Set `LD_LIBRARY_PATH` to include the build directory:
  ```bash
  export LD_LIBRARY_PATH=/path/to/cyclonedds/build/lib:$LD_LIBRARY_PATH
  ```

### Permission Denied

**Problem**: Cannot create shared memory segments

**Solution**:
- Check `/dev/shm` permissions (Linux)
- Ensure your user has sufficient privileges
- Try a different `ServiceName` in configuration

### No Data Received

**Problem**: Publisher sends but subscriber doesn't receive

**Solution**:
- Verify both processes use the same configuration
- Check that QoS policies are compatible
- Ensure CYCLONEDDS_URI is set for both processes
- Check that topics have the same name and type

## Example IDL

Create a simple data type for testing:

```idl
// MyDataType.idl
module Example {
    struct MyDataType {
        long id;
        double value;
        string<256> message;
    };
};
```

Generate type support:

```bash
idlc MyDataType.idl
```

## Advanced Configuration

### Multiple PSMX Instances

You can configure multiple PSMX instances for different use cases:

```xml
<PubSubMessageExchanges>
  <!-- High priority shared memory for critical data -->
  <PubSubMessageExchange name="shm_critical" library="libpsmx_shm" priority="100">
    <ServiceName>critical</ServiceName>
  </PubSubMessageExchange>
  
  <!-- Normal priority shared memory for regular data -->
  <PubSubMessageExchange name="shm_normal" library="libpsmx_shm" priority="50">
    <ServiceName>normal</ServiceName>
  </PubSubMessageExchange>
</PubSubMessageExchanges>
```

### QoS Configuration

Compatible QoS settings:

```c
dds_qos_t *qos = dds_create_qos();

// Required: KEEP_LAST history
dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, depth);

// Recommended: RELIABLE reliability
dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_INFINITY);

// Optional: Other QoS policies work as expected
dds_qset_deadline(qos, DDS_SECS(1));
dds_qset_latency_budget(qos, DDS_MSECS(100));
```

## Further Reading

- [CycloneDDS Documentation](https://github.com/eclipse-cyclonedds/cyclonedds)
- [PSMX Interface Specification](../../../docs/psmx.md)
- [DDS Specification](https://www.omg.org/spec/DDS/)
