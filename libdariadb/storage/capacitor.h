#pragma once

#include "../meas.h"
#include "../storage.h"
#include <memory>

namespace dariadb {
namespace storage {

const std::string CAP_FILE_EXT = ".cap"; // append-only-file
const size_t CAP_DEFAULT_MAX_LEVELS = 10;

class Capacitor : public MeasStorage {
public:
  struct Params {
    size_t B; // measurements count in one datra block
    std::string path;
    size_t max_levels;
    Params(const size_t _B, const std::string _path) {
      B = _B;
      path = _path;
      max_levels = CAP_DEFAULT_MAX_LEVELS;
    }
  };
#pragma pack(push, 1)
  struct Header {
    dariadb::Time minTime;
    dariadb::Time maxTime;
    bool is_dropped : 1;
    bool is_closed : 1;
    bool is_full : 1;
    size_t B;
    size_t size;    // sizeof file in bytes
    size_t _size_B; // how many block (sizeof(B)) addeded.
    size_t levels_count;
    size_t _writed;
    size_t _memvalues_pos;
  };
#pragma pack(pop)
  virtual ~Capacitor();
  Capacitor(const Params &param);
  Capacitor(const Capacitor::Params &params, const std::string &fname,
            bool readonly = false);
  static Header readHeader(std::string file_name);
  append_result append(const Meas &value) override;
  Reader_ptr readInterval(const QueryInterval &q) override;
  Reader_ptr readInTimePoint(const QueryTimePoint &q) override;
  Reader_ptr currentValue(const IdArray &ids, const Flag &flag) override;
  dariadb::Time minTime() override;
  dariadb::Time maxTime() override;
  bool minMaxTime(dariadb::Id id, dariadb::Time *minResult,
                  dariadb::Time *maxResult) override;
  void flush() override; // write all to storage;

  size_t files_count() const;
  size_t levels_count() const;
  size_t size() const;

  void subscribe(const IdArray &, const Flag &, const ReaderClb_ptr &) override {
    throw MAKE_EXCEPTION("not supported");
  }

  void drop_to_stor(MeasWriter *stor);

protected:
  class Private;
  std::unique_ptr<Private> _Impl;

  // Inherited via MeasStorage
};

typedef std::shared_ptr<Capacitor> Capacitor_Ptr;
}
}
