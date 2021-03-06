#pragma once

#include <libdariadb/utils/in_interval.h>

#include <libdariadb/st_exports.h>
#include <algorithm>
#include <deque>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <stdint.h>
#include <unordered_map>
#include <vector>

namespace dariadb {
typedef uint64_t Time;
typedef uint32_t Id;
typedef uint32_t Flag;
typedef double Value;
typedef std::vector<Id> IdArray;
typedef std::set<Id> IdSet;

const Time MIN_TIME = std::numeric_limits<dariadb::Time>::min();
const Time MAX_TIME = std::numeric_limits<dariadb::Time>::max();

const Id MIN_ID = std::numeric_limits<dariadb::Id>::min();
const Id MAX_ID = std::numeric_limits<dariadb::Id>::max();

EXPORT bool areSame(Value a, Value b, const Value EPSILON = 1E-5);
#pragma pack(push, 1)
struct Meas {
  Id id;
  Time time;
  Value value;
  Flag flag;

  EXPORT static Meas empty();
  EXPORT static Meas empty(Id id);

  EXPORT Meas();

  bool operator==(const Meas &other) const {
    return id == other.id && time == other.time && flag == other.flag &&
           areSame(value, other.value);
  }

  bool operator!=(const Meas &other) const { return !(*this == other); }

  bool inFlag(Flag f) const { return (f == 0) || (f == flag); }

  bool inIds(const IdArray &ids) const {
    return (ids.size() == 0) || (std::count(ids.begin(), ids.end(), id));
  }

  bool inQuery(const IdArray &ids, const Flag f) const { return inFlag(f) && inIds(ids); }

  bool inInterval(Time from, Time to) const { return utils::inInterval(from, to, time); }

  bool inQuery(const IdArray &ids, const Flag f, Time from, Time to) const {
    return inQuery(ids, f) && inInterval(from, to);
  }
};
#pragma pack(pop)

struct meas_id_compare_less {
  bool operator()(const dariadb::Meas &lhs, const dariadb::Meas &rhs) const {
    return lhs.time < rhs.time;
  }
};

struct meas_time_compare_less {
  bool operator()(const dariadb::Meas &lhs, const dariadb::Meas &rhs) const {
    return lhs.time < rhs.time;
  }
};

struct meas_time_compare_greater {
  bool operator()(const dariadb::Meas &lhs, const dariadb::Meas &rhs) const {
    return lhs.time > rhs.time;
  }
};

struct MeasMinMax {
  Meas min;
  Meas max;

  EXPORT void updateMax(const Meas &m);
  EXPORT void updateMin(const Meas &m);
};

using MeasArray = std::vector<Meas>;
using MeasList = std::deque<Meas>;
/// used in readTimePoint queries
using Id2Meas = std::unordered_map<Id, Meas>;
/// id to meas, sorted by time. needed in readInterval, to sort and filter raw values.
using Id2MSet = std::map<Id, std::set<Meas, meas_time_compare_less>>;
/// in loadMinMax();
using Id2MinMax = std::unordered_map<Id, MeasMinMax>;
/// in memory_storage.
using Id2Time = std::unordered_map<Id, Time>;
/// to map raw.id=>bystep.id
using Id2Id = std::unordered_map<Id, Id>;

EXPORT void minmax_append(Id2MinMax &out, const Id2MinMax &source);
EXPORT void mlist2mset(MeasList &mlist, Id2MSet &sub_result);
}
