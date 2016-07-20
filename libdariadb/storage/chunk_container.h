#pragma once

#include "../meas.h"
#include "callbacks.h"
#include "chunk.h"
#include "query_param.h"

namespace dariadb {
namespace storage {

struct ChunkLink {
  uint64_t id;
  uint64_t id_bloom;
  dariadb::Time maxTime;
  std::string page_name;
  uint32_t offset;
};

using ChunkLinkList = std::list<ChunkLink>;

class ChunkContainer {
public:
  virtual bool minMaxTime(dariadb::Id id, dariadb::Time *minResult,
                          dariadb::Time *maxResult) = 0;
  virtual ChunkLinkList chunksByIterval(const QueryInterval &query) = 0;
  virtual Meas::Id2Meas valuesBeforeTimePoint(const QueryTimePoint &q) = 0;
  virtual void readLinks(const QueryInterval &query, const ChunkLinkList &links,
                         ReaderClb *clb) = 0;
  virtual ~ChunkContainer();
};
}
}
