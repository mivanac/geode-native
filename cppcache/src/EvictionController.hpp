/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#ifndef GEODE_EVICTIONCONTROLLER_H_
#define GEODE_EVICTIONCONTROLLER_H_

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <boost/thread/shared_mutex.hpp>

#include "EvictionThread.hpp"

namespace apache {
namespace geode {
namespace client {

class CacheImpl;

/**
 * This class ensures that the cache consumes only as much memory as
 * specified by the heap-lru-limit. Every region that is created in the
 * system registers with the EvictionController. Everytime there is any
 * activity that changes the memory usage in the region, it puts a message
 * into a queue that the EvictionController waits on. The message contains
 * the full region name, the total size of the region (inclusive of keys and
 * values) and the total number of entries.The EvictionController thread picks
 * up the message, updates the total memory size as well as the size of the
 * region in question. It determines whether memory usage is within limits.
 * If so, it goes back to waiting on the queue. If memory usage is out of bounds
 * it does the following.
 *  1> Figures out the delta between specified and actual
 *  2> Determines the size percentage that needs to be evicted
 *  3> Determines the nmber of entries per region (based on size per entry) that
       needs to be evicted. This is a slice of the total eviction that is
 needed.
    4> Invokes a method on each region to trigger eviction of entries.The evict
       method on the region will return the total size evicted for the entries.
    5> Goes back and checks queue size and recalculates the heap size usage
 *
 *
 * When a region is destroyed, it deregisters itself with the EvictionController
 * Format of object that is put into the region map (int size, int numEntries)
 */
class EvictionController {
 public:
  EvictionController(size_t maxHeapSize, int32_t heapSizeDelta,
                     CacheImpl* cache);

  inline ~EvictionController() noexcept = default;

  void start();

  void stop();

  void svc(void);

  void updateRegionHeapInfo(int64_t info);
  void registerRegion(const std::string& name);
  void deregisterRegion(const std::string& name);
  void evict(int32_t percentage);

 private:
  void orderEvictions(int32_t percentage);
  void processHeapInfo(int64_t& readInfo, int64_t& pendingEvictions);

 private:
  std::thread m_thread;
  std::atomic<bool> m_run;
  int64_t m_maxHeapSize;
  int64_t m_heapSizeDelta;
  CacheImpl* m_cacheImpl;
  int64_t m_currentHeapSize;
  std::deque<int64_t> m_queue;
  std::mutex m_queueMutex;
  std::condition_variable m_queueCondition;
  std::vector<std::string> m_regions;
  boost::shared_mutex m_regionLock;
  EvictionThread m_evictionThread;
  static const char* NC_EC_Thread;
};

}  // namespace client
}  // namespace geode
}  // namespace apache

#endif  // GEODE_EVICTIONCONTROLLER_H_
