#pragma once
#include "../interfaces/ichunkcontainer.h"
#include "../interfaces/imeaswriter.h"
#include "../utils/fs.h"
#include "chunk.h"
#include "index.h"

namespace dariadb {
namespace storage {
#pragma pack(push, 1)
struct PageHeader {
  uint64_t write_offset;      // next write pos (bytes)
  uint32_t count_readers;     // readers count. on close must be zero.
  uint32_t addeded_chunks;    // total count of chunks in page.
  uint32_t removed_chunks;    // total chunks marked as not init in rollbacks or fsck.
  uint32_t chunk_per_storage; // max chunks count
  uint32_t chunk_size;        // each chunks size in bytes
  bool is_full : 1;           // is full :)
  bool is_closed : 1;         // is correctly closed.
  bool is_open_to_write : 1;  // true if oppened to write.
  dariadb::Time minTime; // minimal stored time
  dariadb::Time maxTime; // maximum stored time
  uint64_t max_chunk_id; // max(chunk->id)
};
#pragma pack(pop)

class Page : public IChunkContainer, public IMeasWriter {
  Page() = default;

public:
  static Page *create(std::string file_name, uint64_t sz, uint32_t chunk_per_storage,
                      uint32_t chunk_size);
  static Page *create(std::string file_name, uint32_t max_chunk_size, const Meas::MeasArray &ma);
  static Page *open(std::string file_name, bool read_only = false);
  static PageHeader readHeader(std::string file_name);
  static IndexHeader readIndexHeader(std::string page_file_name);
  ~Page();
  void fsck();
  // PM
  bool add_to_target_chunk(const dariadb::Meas &m);
  /*bool append(const ChunksList &ch) override;*/
  bool is_full() const;

  // ChunksList get_open_chunks();
  void dec_reader();
  // ChunkContainer
  bool minMaxTime(dariadb::Id id, dariadb::Time *minResult,
                  dariadb::Time *maxResult) override;
  ChunkLinkList chunksByIterval(const QueryInterval &query) override;
  Meas::Id2Meas valuesBeforeTimePoint(const QueryTimePoint &q) override;
  void readLinks(const QueryInterval &query, const ChunkLinkList &links,
                 IReaderClb *clb) override;

  // Inherited via MeasWriter
  virtual append_result append(const Meas &value) override;
  virtual void flush() override;

  std::list<Chunk_Ptr> get_not_full_chunks(); // list of not full chunks
  std::list<Chunk_Ptr> chunks_by_pos(std::vector<uint32_t> poses);
  void mark_as_non_init(Chunk_Ptr &ch);
  void mark_as_init(Chunk_Ptr &ch);

private:
  void init_chunk_index_rec(Chunk_Ptr ch);
  void close_corrent_chunk();
  struct ChunkWithIndex {
    Chunk_Ptr ch;        /// ptr to chunk in page
    IndexReccord *index; /// ptr to index reccord
    uint32_t pos;        /// position number of 'index' field in index file.
  };
  /// cache of openned chunks. before search chunk in page, we search in cache.
  ChunkWithIndex _openned_chunk;

  void check_page_struct();

  void init_header();
public:
  uint8_t *region; // page  file mapp region
  PageHeader *header;
  uint8_t *chunks;

  std::string filename;
  bool readonly;

protected:
  PageIndex_ptr _index;
  mutable std::mutex _locker;
  mutable utils::fs::MappedFile::MapperFile_ptr page_mmap;
};

typedef std::shared_ptr<Page> Page_Ptr;
}
}
