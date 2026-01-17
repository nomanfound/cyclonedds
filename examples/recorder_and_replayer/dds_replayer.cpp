/*
 * Copyright(c) 2025 ZettaScale Technology and others
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License v. 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0, or the Eclipse Distribution License
 * v. 1.0 which is available at
 * http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <signal.h>
#include <string>
#include <chrono>
#include <iostream>
#include <map>
#include <thread>
#include <algorithm>

#include "dds/dds.h"
#include "mcap/reader.hpp"

#include "dds/ddsrt/threads.h"
#include "dds/ddsi/ddsi_serdata.h"
#include "dds_topic_descriptor_serde.h"

// Global participant
static dds_entity_t participant;

// Flag to stop replay
static volatile bool stop_replay = false;

// Structure to hold topic/writer information per channel
struct ChannelInfo
{
  dds_entity_t topic;
  dds_entity_t writer;
  dds_topic_descriptor_t *descriptor;
  std::string topic_name;
};

#if !DDSRT_WITH_FREERTOS && !__ZEPHYR__
static void signal_handler (int sig)
{
  (void)sig;
  stop_replay = true;
  fprintf (stderr, "\nStopping replay...\n");
}
#endif

static bool create_topic_and_writer (const mcap::Channel &channel, ChannelInfo &info)
{
  // Extract topic descriptor from channel metadata
  auto it = channel.metadata.find ("topic_descriptor");
  if (it == channel.metadata.end ())
  {
    std::cerr << "Error: Channel " << channel.id << " (" << channel.topic
              << ") does not have topic_descriptor metadata\n";
    return false;
  }

  const std::string &serialized_desc = it->second;
  info.descriptor = dds_topic_descriptor_deserialize (serialized_desc.data (), serialized_desc.size ());
  if (!info.descriptor)
  {
    std::cerr << "Error: Failed to deserialize topic descriptor for channel " << channel.id << "\n";
    return false;
  }

  info.topic_name = channel.topic;

  // Create topic
  info.topic = dds_create_topic (participant, info.descriptor, channel.topic.c_str (), NULL, NULL);
  if (info.topic < 0)
  {
    std::cerr << "Error: Failed to create topic '" << channel.topic << "': " << dds_strretcode (info.topic) << "\n";
    dds_topic_descriptor_free (info.descriptor);
    return false;
  }

  // Create writer
  info.writer = dds_create_writer (participant, info.topic, NULL, NULL);
  if (info.writer < 0)
  {
    std::cerr << "Error: Failed to create writer for topic '" << channel.topic << "': "
              << dds_strretcode (info.writer) << "\n";
    dds_delete (info.topic);
    dds_topic_descriptor_free (info.descriptor);
    return false;
  }

  std::cout << "Created topic and writer for: " << channel.topic << " (channel " << channel.id << ")\n";
  return true;
}

static void cleanup_channel_info (std::map<mcap::ChannelId, ChannelInfo> &channels)
{
  for (auto &pair : channels)
  {
    if (pair.second.writer > 0)
      dds_delete (pair.second.writer);
    if (pair.second.topic > 0)
      dds_delete (pair.second.topic);
    if (pair.second.descriptor)
      dds_topic_descriptor_free (pair.second.descriptor);
  }
  channels.clear ();
}

static bool replay_message (const mcap::Message &msg, ChannelInfo &info)
{
  // Create serdata from the raw CDR data
  struct ddsi_serdata *sd = ddsi_serdata_from_ser (
      info.descriptor->m_ops,
      DDS_KIND_OF_KEY (info.descriptor->m_flagset),
      msg.data,
      msg.dataSize);

  if (!sd)
  {
    std::cerr << "Error: Failed to create serdata from message\n";
    return false;
  }

  // Write using the serdata
  dds_return_t ret = dds_writecdr (info.writer, sd);
  ddsi_serdata_unref (sd);

  if (ret < 0)
  {
    std::cerr << "Error: dds_writecdr failed: " << dds_strretcode (ret) << "\n";
    return false;
  }

  return true;
}

static void print_usage (const char *progname)
{
  fprintf (stderr,
           "Usage:\n"
           "  %s <mcap_file> [options]\n"
           "\n"
           "Options:\n"
           "  -r <rate>    Replay rate multiplier (default: 1.0)\n"
           "               1.0 = real-time, 2.0 = 2x speed, 0 = as fast as possible\n"
           "  -l <count>   Loop replay <count> times (0 = infinite, default: 1)\n"
           "\n"
           "Examples:\n"
           "  %s recording.mcap                 # Replay at original speed, once\n"
           "  %s recording.mcap -r 0            # Replay as fast as possible\n"
           "  %s recording.mcap -r 2.0 -l 0     # Replay at 2x speed, loop forever\n"
           "\n"
           "Note: Press Ctrl+C to stop replay.\n",
           progname, progname, progname, progname);
}

int main (int argc, char **argv)
{
  const char *mcap_file = NULL;
  double rate = 1.0;
  int loop_count = 1;

  // Parse arguments
  if (argc < 2)
  {
    print_usage (argv[0]);
    return 1;
  }

  mcap_file = argv[1];

  for (int i = 2; i < argc; i++)
  {
    if (strcmp (argv[i], "-r") == 0 && i + 1 < argc)
    {
      rate = atof (argv[++i]);
      if (rate < 0)
      {
        fprintf (stderr, "Error: Rate must be >= 0\n");
        return 1;
      }
    }
    else if (strcmp (argv[i], "-l") == 0 && i + 1 < argc)
    {
      loop_count = atoi (argv[++i]);
      if (loop_count < 0)
      {
        fprintf (stderr, "Error: Loop count must be >= 0\n");
        return 1;
      }
    }
    else
    {
      fprintf (stderr, "Error: Unknown option '%s'\n", argv[i]);
      print_usage (argv[0]);
      return 1;
    }
  }

  // Setup signal handler
#ifdef _WIN32
  signal (SIGINT, signal_handler);
#elif !DDSRT_WITH_FREERTOS && !__ZEPHYR__
  signal (SIGINT, signal_handler);
  signal (SIGTERM, signal_handler);
#endif

  // Create DDS participant
  participant = dds_create_participant (DDS_DOMAIN_DEFAULT, NULL, NULL);
  if (participant < 0)
  {
    fprintf (stderr, "Error: dds_create_participant: %s\n", dds_strretcode (participant));
    return 1;
  }

  // Open MCAP file
  mcap::McapReader reader;
  auto status = reader.open (mcap_file);
  if (!status.ok ())
  {
    std::cerr << "Error: Failed to open MCAP file '" << mcap_file << "': " << status.message << "\n";
    dds_delete (participant);
    return 1;
  }

  std::cout << "Opened MCAP file: " << mcap_file << "\n";

  // Read all messages into memory for efficient replay
  std::vector<mcap::MessageView> all_messages;
  auto messages = reader.readMessages ();

  for (const auto &msg_view : messages)
  {
    all_messages.push_back (msg_view);
  }

  if (all_messages.empty ())
  {
    std::cerr << "Warning: No messages found in MCAP file\n";
    reader.close ();
    dds_delete (participant);
    return 0;
  }

  std::cout << "Loaded " << all_messages.size () << " messages\n";

  // Sort messages by publish time
  std::sort (all_messages.begin (), all_messages.end (),
             [](const mcap::MessageView &a, const mcap::MessageView &b) {
               return a.message.publishTime < b.message.publishTime;
             });

  // Create topics and writers for all channels
  std::map<mcap::ChannelId, ChannelInfo> channels;
  auto channels_map = reader.channels ();

  for (const auto &ch_pair : channels_map)
  {
    ChannelInfo info = {};
    if (!create_topic_and_writer (*ch_pair.second, info))
    {
      cleanup_channel_info (channels);
      reader.close ();
      dds_delete (participant);
      return 1;
    }
    channels[ch_pair.first] = info;
  }

  std::cout << "\nStarting replay";
  if (rate == 0)
    std::cout << " (as fast as possible)";
  else if (rate == 1.0)
    std::cout << " (real-time)";
  else
    std::cout << " (rate: " << rate << "x)";

  if (loop_count == 0)
    std::cout << ", looping forever...\n";
  else if (loop_count > 1)
    std::cout << ", " << loop_count << " loops...\n";
  else
    std::cout << "...\n";

  // Replay loop
  int current_loop = 0;
  bool infinite_loop = (loop_count == 0);

  while ((infinite_loop || current_loop < loop_count) && !stop_replay)
  {
    if (loop_count != 1)
    {
      std::cout << "\n=== Loop " << (current_loop + 1);
      if (!infinite_loop)
        std::cout << "/" << loop_count;
      std::cout << " ===\n";
    }

    uint64_t first_timestamp = all_messages[0].message.publishTime;
    auto replay_start = std::chrono::steady_clock::now ();
    size_t msg_count = 0;

    for (const auto &msg_view : all_messages)
    {
      if (stop_replay)
        break;

      const mcap::Message &msg = msg_view.message;

      // Find channel info
      auto it = channels.find (msg.channelId);
      if (it == channels.end ())
      {
        std::cerr << "Warning: Message for unknown channel " << msg.channelId << ", skipping\n";
        continue;
      }

      // Handle timing
      if (rate > 0 && all_messages.size () > 1)
      {
        uint64_t time_offset_ns = msg.publishTime - first_timestamp;
        auto target_time = replay_start + std::chrono::nanoseconds (
            static_cast<uint64_t> (time_offset_ns / rate));
        std::this_thread::sleep_until (target_time);
      }

      // Replay the message
      if (!replay_message (msg, it->second))
      {
        std::cerr << "Failed to replay message " << msg_count << "\n";
        // Continue with next message
      }

      msg_count++;
      if (msg_count % 100 == 0)
      {
        std::cout << "Replayed " << msg_count << " messages...\r" << std::flush;
      }
    }

    std::cout << "Replayed " << msg_count << " messages total\n";
    current_loop++;
  }

  if (stop_replay)
  {
    std::cout << "\nReplay stopped by user\n";
  }
  else
  {
    std::cout << "\nReplay complete\n";
  }

  // Cleanup
  cleanup_channel_info (channels);
  reader.close ();
  dds_delete (participant);

  return 0;
}
