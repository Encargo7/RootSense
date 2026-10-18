#pragma once
// =============================================================================
//  EventLog + MoistureHistory — small ring buffers kept in RAM.
//  EventLog: the last few things that happened (shown on the dashboard).
//  MoistureHistory: one moisture value every 10 min for the 24 h chart.
// =============================================================================
#include <stdint.h>
#include <string.h>

class EventLog {
 public:
  static const uint8_t CAPACITY = 30;
  static const uint16_t TEXT_LEN = 220;

  struct Entry {
    uint32_t uptimeMs;
    int64_t epoch;  // 0 if the clock was not set yet
    char text[TEXT_LEN];
  };

  EventLog() : head_(0), count_(0), total_(0) {}

  void add(uint32_t uptimeMs, int64_t epoch, const char* text) {
    Entry& e = entries_[head_];
    e.uptimeMs = uptimeMs;
    e.epoch = epoch;
    size_t n = text ? strlen(text) : 0;
    if (n > TEXT_LEN - 1) n = TEXT_LEN - 1;
    if (n > 0) memcpy(e.text, text, n);
    e.text[n] = '\0';
    head_ = (uint8_t)((head_ + 1) % CAPACITY);
    if (count_ < CAPACITY) count_++;
    total_++;
  }

  uint8_t size() const { return count_; }
  uint32_t totalAdded() const { return total_; }

  // i = 0 is the oldest entry still kept, size()-1 the newest.
  const Entry& get(uint8_t i) const {
    uint8_t oldest = (uint8_t)((head_ + CAPACITY - count_) % CAPACITY);
    return entries_[(oldest + i) % CAPACITY];
  }

 private:
  Entry entries_[CAPACITY];
  uint8_t head_;
  uint8_t count_;
  uint32_t total_;
};

class MoistureHistory {
 public:
  static const uint16_t SIZE = 144;  // 24 h at one point per 10 min
  static const uint8_t NO_DATA = 255;

  MoistureHistory() : head_(0), count_(0) { memset(values_, NO_DATA, sizeof(values_)); }

  void push(uint8_t value) {
    values_[head_] = value;
    head_ = (uint16_t)((head_ + 1) % SIZE);
    if (count_ < SIZE) count_++;
  }

  uint16_t size() const { return count_; }

  // i = 0 is the oldest value.
  uint8_t get(uint16_t i) const {
    uint16_t oldest = (uint16_t)((head_ + SIZE - count_) % SIZE);
    return values_[(oldest + i) % SIZE];
  }

 private:
  uint8_t values_[SIZE];
  uint16_t head_;
  uint16_t count_;
};
