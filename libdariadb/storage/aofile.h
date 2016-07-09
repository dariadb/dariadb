#pragma once

#include "../meas.h"
#include "../storage.h"
#include <memory>

namespace dariadb {
namespace storage {

const std::string AOF_FILE_EXT = ".aof"; // append-only-file

class AOFile : public MeasStorage {
public:
  struct Params {
    size_t size; // measurements count
    std::string path;
    Params(const size_t _size, const std::string _path) {
      size = _size;
      path = _path;
    }
  };
  virtual ~AOFile();
  AOFile(const Params &param);
  AOFile(const AOFile::Params &params, const std::string &fname, bool readonly = false);
  // static Header readHeader(std::string file_name);
  append_result append(const Meas &value) override;
  append_result append(const Meas::MeasArray &ma) override;
  append_result append(const Meas::MeasList &ml) override;
  Reader_ptr readInterval(const QueryInterval &q) override;
  Reader_ptr readInTimePoint(const QueryTimePoint &q) override;
  Reader_ptr currentValue(const IdArray &ids, const Flag &flag) override;
  dariadb::Time minTime() override;
  dariadb::Time maxTime() override;
  bool minMaxTime(dariadb::Id id, dariadb::Time *minResult,
                  dariadb::Time *maxResult) override;
  void flush() override; // write all to storage;

  void drop_to_stor(MeasWriter *stor);

  std::string filename() const;

  Meas::MeasArray readAll();
  static size_t writed(std::string fname);

protected:
  class Private;
  std::unique_ptr<Private> _Impl;
};

typedef std::shared_ptr<AOFile> AOFile_Ptr;
}
}
