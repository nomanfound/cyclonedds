// Copyright(c) 2025 ZettaScale Technology and others
//
// This program and the accompanying materials are made available under the
// terms of the Eclipse Public License v. 2.0 which is available at
// http://www.eclipse.org/legal/epl-2.0, or the Eclipse Distribution License
// v. 1.0 which is available at
// http://www.eclipse.org/org/documents/edl-v10.php.
//
// SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause

/**
 * Example demonstrating PSMX shared memory plugin usage
 * 
 * This example shows how to:
 * 1. Use the loan mechanism for zero-copy writes
 * 2. Configure PSMX shared memory transport
 * 3. Communicate between processes using shared memory
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

// Note: In a real application, you would use IDL-generated types
// For this example, we'll use a simple struct

typedef struct SimpleData {
    int32_t id;
    int32_t value;
    char message[256];
} SimpleData;

// Simplified example - normally these would be auto-generated from IDL
static const char* TOPIC_NAME = "ShmExample";

static volatile sig_atomic_t running = 1;

static void sigint_handler(int sig)
{
    (void)sig;
    running = 0;
}

void run_publisher(void)
{
    printf("PSMX Shared Memory Publisher Example\n");
    printf("=====================================\n\n");
    
    printf("This example demonstrates:\n");
    printf("1. Using loan mechanism for zero-copy writes\n");
    printf("2. Shared memory transport between processes\n");
    printf("3. Single-writer, multi-reader pattern\n\n");
    
    printf("To use this example:\n");
    printf("1. Configure CycloneDDS with PSMX shared memory plugin in cyclonedds.xml:\n\n");
    printf("   <Domain>\n");
    printf("     <PubSubMessageExchanges>\n");
    printf("       <PubSubMessageExchange name=\"shm\" library=\"libpsmx_shm\" priority=\"0\">\n");
    printf("         <ServiceName>example_shm</ServiceName>\n");
    printf("       </PubSubMessageExchange>\n");
    printf("     </PubSubMessageExchanges>\n");
    printf("   </Domain>\n\n");
    printf("2. Set environment variable: export CYCLONEDDS_URI=file://cyclonedds.xml\n");
    printf("3. Run publisher in one terminal\n");
    printf("4. Run subscriber in another terminal\n\n");
    
    printf("Note: This is a demonstration skeleton. To run a complete example:\n");
    printf("- Define your data type in IDL\n");
    printf("- Use idlc to generate type support\n");
    printf("- Create participant, topic, writer/reader entities\n");
    printf("- Use dds_request_loan() for zero-copy writes\n");
    printf("- Use dds_write() to publish loaned samples\n\n");

    int count = 0;
    while (running && count < 10) {
        printf("Would publish sample %d (id=%d, value=%d)\n", 
               count, count, count * 100);
        sleep(1);
        count++;
    }
    
    printf("\nPublisher finished.\n");
}

void run_subscriber(void)
{
    printf("PSMX Shared Memory Subscriber Example\n");
    printf("======================================\n\n");
    
    printf("Waiting for samples from shared memory...\n");
    printf("(This is a demonstration skeleton)\n\n");
    
    printf("In a complete implementation:\n");
    printf("- Use dds_take() or dds_read() to receive samples\n");
    printf("- Samples are received via zero-copy from shared memory\n");
    printf("- Use dds_return_loan() to release samples\n\n");
    
    int count = 0;
    while (running && count < 10) {
        printf("Would receive sample %d from shared memory\n", count);
        sleep(1);
        count++;
    }
    
    printf("\nSubscriber finished.\n");
}

int main(int argc, char *argv[])
{
    signal(SIGINT, sigint_handler);
    
    if (argc < 2) {
        printf("Usage: %s [pub|sub]\n", argv[0]);
        printf("  pub - Run as publisher\n");
        printf("  sub - Run as subscriber\n");
        return 1;
    }
    
    if (strcmp(argv[1], "pub") == 0) {
        run_publisher();
    } else if (strcmp(argv[1], "sub") == 0) {
        run_subscriber();
    } else {
        printf("Invalid argument. Use 'pub' or 'sub'\n");
        return 1;
    }
    
    return 0;
}
