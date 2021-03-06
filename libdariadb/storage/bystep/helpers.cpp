#include <libdariadb/storage/bystep/step_kind.h>
#include <libdariadb/timeutil.h>

namespace dariadb {
namespace storage {
namespace bystep {
enum INTERVAL_SIZE { MS = 1000 * 60, SEC = 60 * 60, MIN = 60, HR = 24 };

size_t step_to_size(STEP_KIND kind) {
  switch (kind) {
  case STEP_KIND::MILLISECOND:
    return (size_t)INTERVAL_SIZE::MS;
  case STEP_KIND::SECOND:
    return (size_t)INTERVAL_SIZE::SEC;
  case STEP_KIND::MINUTE:
    return (size_t)INTERVAL_SIZE::MIN;
  case STEP_KIND::HOUR:
    return (size_t)INTERVAL_SIZE::HR;
  default:
    return 0;
  }
}

/// result - rounded time and step in miliseconds
std::tuple<Time, Time> roundTime(const STEP_KIND stepkind, const Time t) {
  Time rounded = 0;
  Time step = 0;
  switch (stepkind) {
  case STEP_KIND::MILLISECOND:
    rounded = t;
    step = 1;
    break;
  case STEP_KIND::SECOND:
    rounded = timeutil::round_to_seconds(t);
    step = 1000;
    break;
  case STEP_KIND::MINUTE:
    rounded = timeutil::round_to_minutes(t);
    step = 60 * 1000;
    break;
  case STEP_KIND::HOUR:
    rounded = timeutil::round_to_hours(t);
    step = 3600 * 1000;
    break;
  }
  return std::tie(rounded, step);
}

uint64_t intervalForTime(const STEP_KIND stepkind, const size_t valsInInterval,
                         const Time t) {
  Time rounded = 0;
  Time step = 0;
  auto rs = roundTime(stepkind, t);
  rounded = std::get<0>(rs);
  step = std::get<1>(rs);

  auto stepped = (rounded / step);
  if (stepped == valsInInterval) {
    return 1;
  }
  if (stepped < valsInInterval) {
    return 0;
  }
  return stepped / valsInInterval;
}
}
}
}