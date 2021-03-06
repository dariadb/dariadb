#pragma once

#include <libdariadb/interfaces/imeasstorage.h>
#include <libdariadb/st_exports.h>
#include <libdariadb/storage/bystep/description.h>
#include <libdariadb/storage/bystep/step_kind.h>
#include <libdariadb/storage/engine_environment.h>
#include <istream>
#include <memory>
#include <ostream>
#include <unordered_map>

namespace dariadb {
namespace storage {

class ByStepStorage;
using ByStepStorage_ptr = std::shared_ptr<ByStepStorage>;
class ByStepStorage : public IMeasStorage {
protected:
  /// id_count - for prealloc
  EXPORT ByStepStorage(const EngineEnvironment_ptr &env);

public:
  /// id_count - for prealloc
  EXPORT static ByStepStorage_ptr create(const EngineEnvironment_ptr &env);
  EXPORT ~ByStepStorage();
  EXPORT bystep::Description description();
  // return count chunk readed from disk.
  EXPORT size_t setSteps(const Id2Step &steps);

  // Inherited via IMeasStorage
  EXPORT virtual Time minTime() override;
  EXPORT virtual Time maxTime() override;
  EXPORT virtual bool minMaxTime(dariadb::Id id, dariadb::Time *minResult,
                                 dariadb::Time *maxResult) override;
  EXPORT virtual void foreach (const QueryInterval &q, IReaderClb * clbk) override;
  EXPORT virtual Id2Meas readTimePoint(const QueryTimePoint &q) override;
  EXPORT virtual Id2Meas currentValue(const IdArray &ids, const Flag &flag) override;
  using IMeasStorage::append;
  EXPORT Status append(const Meas &value) override;
  EXPORT void flush() override;
  EXPORT void stop();
  EXPORT Id2MinMax loadMinMax() override;

  EXPORT static uint64_t intervalForTime(const STEP_KIND step,
                                         const size_t valsInInterval, const Time t);

  EXPORT void eraseOld(Id id, Time from, Time to);

private:
  struct Private;
  std::unique_ptr<Private> _impl;
};
}
}
