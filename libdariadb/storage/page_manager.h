#pragma once

#include "../storage.h"
#include "../utils/utils.h"
#include "chunk.h"
#include "chunk_container.h"
#include "cursor.h"

#include <vector>

namespace dariadb {
namespace storage {
	const uint16_t OPENNED_PAGE_CACHE_SIZE = 10;
class PageManager : public utils::NonCopy,
                    public ChunkContainer,
                    public MeasWriter {
public:
  struct Params {
    std::string path;
    uint32_t chunk_per_storage;
    uint32_t chunk_size;
	uint16_t openned_page_chache_size; ///max oppend pages in cache(readonly pages stored).
    Params(const std::string storage_path, size_t chunks_per_storage,
           size_t one_chunk_size) {
      path = storage_path;
      chunk_per_storage = uint32_t(chunks_per_storage);
      chunk_size = uint32_t(one_chunk_size);
	  openned_page_chache_size = OPENNED_PAGE_CACHE_SIZE;
    }
  };

protected:
  virtual ~PageManager();

  PageManager(const Params &param);

public:
  typedef uint32_t handle;
  static void start(const Params &param);
  static void stop();
  void flush()override;
  static PageManager *instance();

  // bool append(const Chunk_Ptr &c) override;
  // bool append(const ChunksList &lst) override;

  // ChunkContainer
  bool minMaxTime(dariadb::Id id, dariadb::Time *minResult,
                  dariadb::Time *maxResult) override;
  ChunkLinkList chunksByIterval(const QueryInterval &query) override;
  ChunkLinkList chunksBeforeTimePoint(const QueryTimePoint &q) override;
  Cursor_ptr  readLinks(const ChunkLinkList&links) override;
  IdArray getIds() override;

  // dariadb::storage::ChunksList get_open_chunks();
  size_t in_queue_size() const; // TODO rename to queue_size

  dariadb::Time minTime();
  dariadb::Time maxTime();

  append_result append(const Meas &value) override;

private:
  static PageManager *_instance;
  class Private;
  std::unique_ptr<Private> impl;
};
}
}
