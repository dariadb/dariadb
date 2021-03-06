#ifdef MSVC
#define _CRT_SECURE_NO_WARNINGS // for fopen
#define _SCL_SECURE_NO_WARNINGS // for stx::btree in msvc build.
#endif
#include <libdariadb/storage/bloom_filter.h>
#include <libdariadb/storage/callbacks.h>
#include <libdariadb/storage/pages/helpers.h>
#include <libdariadb/storage/pages/page.h>
#include <libdariadb/timeutil.h>
#include <libdariadb/utils/async/thread_manager.h>
#include <libdariadb/utils/exception.h>
#include <algorithm>

#include <cstring>
#include <fstream>
#include <map>
#include <stx/btree_map.h>

using namespace dariadb::storage;
using namespace dariadb;

Page::~Page() {
  _index = nullptr;
}

uint64_t Page::index_file_size(uint32_t chunk_per_storage) {
  return chunk_per_storage * sizeof(IndexReccord) + sizeof(IndexHeader);
}

Page_Ptr Page::create(const std::string &file_name, uint64_t chunk_id,
                   uint32_t max_chunk_size, const MeasArray &ma) {
  auto to_compress = PageInner::splitById(ma);

  PageHeader phdr = PageInner::emptyPageHeader(chunk_id);

  std::list<PageInner::HdrAndBuffer> compressed_results =
      PageInner::compressValues(to_compress, phdr, max_chunk_size);
  auto file = std::fopen(file_name.c_str(), "ab");
  if (file == nullptr) {
    THROW_EXCEPTION("file is null");
  }

  IndexHeader ihdr;
  memset(&ihdr, 0, sizeof(IndexHeader));
  ihdr.minTime = MAX_TIME;
  ihdr.maxTime = MIN_TIME;
  auto index_file =
      std::fopen(PageIndex::index_name_from_page_name(file_name).c_str(), "ab");
  if (index_file == nullptr) {
    THROW_EXCEPTION("can`t open file ", file_name);
  }

  auto page_size =
      PageInner::writeToFile(file, index_file, phdr, ihdr, compressed_results);
  phdr.filesize = page_size;

  std::fwrite((char *)&phdr, sizeof(PageHeader), 1, file);
  std::fclose(file);

  std::fwrite(&ihdr, sizeof(IndexHeader), 1, index_file);
  std::fclose(index_file);
  return make_page(file_name, phdr);
}

Page_Ptr Page::make_page(const std::string &file_name, const PageHeader &phdr) {
  auto res = new Page;
  res->header = phdr;
  res->filename = file_name;
  res->_index = PageIndex::open(PageIndex::index_name_from_page_name(file_name));
  return Page_Ptr(res);
}

// COMPACTION
Page_Ptr Page::create(const std::string &file_name, uint64_t chunk_id,
                   uint32_t max_chunk_size,
                   const std::list<std::string> &pages_full_paths) {
  std::unordered_map<std::string, Page_Ptr> openned_pages;
  openned_pages.reserve(pages_full_paths.size());
  std::map<uint64_t, ChunkLinkList> links;
  QueryInterval qi({}, 0, MIN_TIME, MAX_TIME);
  for (auto &p_full_path : pages_full_paths) {
	Page_Ptr p = Page::open(p_full_path);
    openned_pages.emplace(std::make_pair(p_full_path, p));

    auto clinks = p->chunksByIterval(qi);
    for (auto &cl : clinks) {
      cl.page_name = p_full_path;
      links[cl.meas_id].push_back(cl);
    }
  }
  ENSURE(openned_pages.size() == pages_full_paths.size());

  PageHeader phdr = PageInner::emptyPageHeader(chunk_id);
  phdr.minTime = MAX_TIME;
  phdr.maxTime = MIN_TIME;
  auto file = std::fopen(file_name.c_str(), "ab");
  if (file == nullptr) {
    THROW_EXCEPTION("file is null");
  }

  IndexHeader ihdr;
  memset(&ihdr, 0, sizeof(IndexHeader));
  ihdr.minTime = MAX_TIME;
  ihdr.maxTime = MIN_TIME;
  auto index_file =
      std::fopen(PageIndex::index_name_from_page_name(file_name).c_str(), "ab");
  if (index_file == nullptr) {
    THROW_EXCEPTION("can`t open file ", file_name);
  }
  for (auto &kv : links) {
    auto lst = kv.second;
    std::vector<ChunkLink> link_vec(lst.begin(), lst.end());
    std::sort(
        link_vec.begin(), link_vec.end(),
        [](const ChunkLink &left, const ChunkLink &right) { return left.id < right.id; });
    stx::btree_map<dariadb::Time, dariadb::Meas> values_map;

    for (auto c : link_vec) {
      MList_ReaderClb clb;
      auto p = openned_pages[c.page_name];
      p->readLinks(qi, {c}, &clb);
      for (auto v : clb.mlist) {
        values_map[v.time] = v;
      }
    }
    MeasArray sorted_and_filtered;
    sorted_and_filtered.reserve(values_map.size());
    for (auto &time2meas : values_map) {
      sorted_and_filtered.push_back(time2meas.second);
    }

    std::map<Id, MeasArray> all_values;
    all_values[sorted_and_filtered.front().id] = sorted_and_filtered;

    auto compressed_results = PageInner::compressValues(all_values, phdr, max_chunk_size);

    auto page_size = PageInner::writeToFile(file, index_file, phdr, ihdr,
                                            compressed_results, phdr.filesize);
    phdr.filesize = page_size;
  }

  std::fwrite((char *)&phdr, sizeof(PageHeader), 1, file);
  std::fclose(file);

  std::fwrite(&ihdr, sizeof(IndexHeader), 1, index_file);
  std::fclose(index_file);

  return make_page(file_name, phdr);
}

Page_Ptr Page::create(const std::string &file_name, uint64_t chunk_id,
                   const std::vector<Chunk *> &a, size_t count) {
  using namespace dariadb::utils::async;

  PageHeader phdr = PageInner::emptyPageHeader(chunk_id);

  auto file = std::fopen(file_name.c_str(), "ab");
  if (file == nullptr) {
    throw MAKE_EXCEPTION("WALFile: append error.");
  }

  IndexHeader ihdr;
  memset(&ihdr, 0, sizeof(IndexHeader));
  ihdr.minTime = MAX_TIME;
  ihdr.maxTime = MIN_TIME;
  auto index_file =
      std::fopen(PageIndex::index_name_from_page_name(file_name).c_str(), "ab");
  if (index_file == nullptr) {
    THROW_EXCEPTION("can`t open file ", file_name);
  }

  uint64_t offset = 0;
  size_t page_size = 0;
  std::vector<IndexReccord> ireccords;
  ireccords.resize(count);
  size_t pos = 0;

  for (size_t i = 0; i < count; ++i) {
    ChunkHeader *chunk_header = a[i]->header;
    auto chunk_buffer_ptr = a[i]->_buffer_t;
#ifdef DEBUG
    {
      auto ch = Chunk::open(chunk_header, chunk_buffer_ptr);
      auto rdr = ch->getReader();
      size_t readed = 0;
      while (!rdr->is_end()) {
        rdr->readNext();
        readed++;
      }
      ENSURE(readed == (ch->header->count + 1));
    }
#endif //  DEBUG
    phdr.max_chunk_id++;
    phdr.minTime = std::min(phdr.minTime, chunk_header->minTime);
    phdr.maxTime = std::max(phdr.maxTime, chunk_header->maxTime);
    chunk_header->id = phdr.max_chunk_id;

    phdr.addeded_chunks++;
    chunk_header->offset_in_page = offset;

    auto skip_count = Chunk::compact(chunk_header);
    // update checksum;
    Chunk::updateChecksum(*chunk_header, chunk_buffer_ptr + skip_count);

#ifdef DEBUG
    {
      auto ch = Chunk::open(chunk_header, chunk_buffer_ptr + skip_count);
      ENSURE(ch->checkChecksum());
      auto rdr = ch->getReader();
      size_t readed = 0;
      while (!rdr->is_end()) {
        rdr->readNext();
        readed++;
      }
      ENSURE(readed == (ch->header->count + 1));
      ch->close();
    }
#endif //  DEBUG

    std::fwrite(chunk_header, sizeof(ChunkHeader), 1, file);
    std::fwrite(chunk_buffer_ptr + skip_count, sizeof(uint8_t), chunk_header->size, file);

    offset += sizeof(ChunkHeader) + chunk_header->size;

    auto index_reccord = PageInner::init_chunk_index_rec(*chunk_header, &ihdr);
    ireccords[pos] = index_reccord;
    pos++;
  }
  page_size = offset;
  phdr.filesize = page_size;
  std::fwrite(&(phdr), sizeof(PageHeader), 1, file);
  std::fclose(file);

  std::fwrite(ireccords.data(), sizeof(IndexReccord), ireccords.size(), index_file);
  std::fwrite(&ihdr, sizeof(IndexHeader), 1, index_file);
  std::fclose(index_file);

  return Page::make_page(file_name, phdr);
}

Page_Ptr Page::open(std::string file_name) {
  auto phdr = Page::readHeader(file_name);
  auto res = new Page;

  res->filename = file_name;
  res->_index = PageIndex::open(PageIndex::index_name_from_page_name(file_name));

  res->header = phdr;
  return Page_Ptr{ res };
}

void Page::restoreIndexFile(const std::string &file_name) {
  logger_info("engine: page - restore index file ", file_name);
  auto phdr = Page::readHeader(file_name);
  auto res = new Page;

  res->filename = file_name;

  res->header = phdr;
  res->update_index_recs(phdr);
  res->_index = PageIndex::open(PageIndex::index_name_from_page_name(file_name));
  delete res;
}

PageHeader Page::readHeader(std::string file_name) {
  std::ifstream istream;
  istream.open(file_name, std::fstream::in | std::fstream::binary);
  if (!istream.is_open()) {
    THROW_EXCEPTION("can't open file. filename=", file_name);
  }
  istream.seekg(-(int)sizeof(PageHeader), istream.end);
  PageHeader result;
  memset(&result, 0, sizeof(PageHeader));
  istream.read((char *)&result, sizeof(PageHeader));
  istream.close();
  return result;
}

IndexHeader Page::readIndexHeader(std::string ifile) {
  return PageIndex::readIndexHeader(ifile);
}

bool Page::checksum() {
  using dariadb::timeutil::to_string;
  logger_info("engine: checksum page ", this->filename);

  auto page_io = std::fopen(filename.c_str(), "rb");
  if (page_io == nullptr) {
    THROW_EXCEPTION("can`t open file ", this->filename);
  }
  bool result = true;
  auto indexReccords = _index->readReccords();
  for (auto it : indexReccords) {
    Chunk_Ptr c = readChunkByOffset(page_io, it.offset);
    if (!c->checkChecksum()) {
      result = false;
      break;
    }
  }

  std::fclose(page_io);
  return result;
}

void Page::update_index_recs(const PageHeader &phdr) {
  auto index_file =
      std::fopen(PageIndex::index_name_from_page_name(filename).c_str(), "ab");
  if (index_file == nullptr) {
    THROW_EXCEPTION("can`t open file ", this->filename);
  }
  auto page_io = std::fopen(filename.c_str(), "rb");
  if (page_io == nullptr) {
    THROW_EXCEPTION("can`t open file ", this->filename);
  }

  IndexHeader ihdr;
  memset(&ihdr, 0, sizeof(IndexHeader));
  ihdr.minTime = MAX_TIME;
  ihdr.maxTime = MIN_TIME;
  for (size_t i = 0; i < phdr.addeded_chunks; ++i) {
    ChunkHeader info;
    auto readed = std::fread(&info, sizeof(ChunkHeader), 1, page_io);
    if (readed < size_t(1)) {
      THROW_EXCEPTION("engine: page read error - ", this->filename);
    }
    auto index_reccord = PageInner::init_chunk_index_rec(info, &ihdr);
    ENSURE(index_reccord.offset == info.offset_in_page);
    std::fwrite(&index_reccord, sizeof(IndexReccord), 1, index_file);

    std::fseek(page_io, info.size, SEEK_CUR);
  }
  std::fwrite(&ihdr, sizeof(IndexHeader), 1, index_file);
  std::fclose(index_file);
  std::fclose(page_io);
}

bool Page::minMaxTime(dariadb::Id id, dariadb::Time *minTime, dariadb::Time *maxTime) {
  QueryInterval qi{dariadb::IdArray{id}, 0, this->header.minTime, this->header.maxTime};
  auto all_chunks = this->chunksByIterval(qi);

  bool result = false;
  if (!all_chunks.empty()) {
    result = true;
  }
  *minTime = dariadb::MAX_TIME;
  *maxTime = dariadb::MIN_TIME;
  auto indexReccords = _index->readReccords();
  for (auto &link : all_chunks) {
    auto _index_it = indexReccords[link.index_rec_number];
    *minTime = std::min(*minTime, _index_it.minTime);
    *maxTime = std::max(*maxTime, _index_it.maxTime);
  }
  return result;
}

ChunkLinkList Page::chunksByIterval(const QueryInterval &query) {
  return _index->get_chunks_links(query.ids, query.from, query.to, query.flag);
}

Chunk_Ptr Page::readChunkByOffset(FILE *page_io, int offset) {
  std::fseek(page_io, offset, SEEK_SET);
  ChunkHeader *cheader = new ChunkHeader;
  auto readed = std::fread(cheader, sizeof(ChunkHeader), 1, page_io);
  if (readed < size_t(1)) {
    delete cheader;
    THROW_EXCEPTION("engine: page read error - ", this->filename);
  }
  uint8_t *buffer = new uint8_t[cheader->size];
  memset(buffer, 0, cheader->size);
  readed = std::fread(buffer, cheader->size, 1, page_io);
  if (readed < size_t(1)) {
    delete cheader;
    delete[] buffer;
    THROW_EXCEPTION("engine: page read error - ", this->filename);
  }
  Chunk_Ptr ptr = nullptr;
  ptr = Chunk::open(cheader, buffer);
  ptr->is_owner = true;
  if (!ptr->checkChecksum()) {
    logger_fatal("engine: bad checksum of chunk #", ptr->header->id,
                 " for measurement id:", ptr->header->meas_id, " page: ", this->filename);
    return nullptr;
  }
  return ptr;
}

dariadb::Id2Meas Page::valuesBeforeTimePoint(const QueryTimePoint &q) {
  dariadb::Id2Meas result;
  auto raw_links =
      _index->get_chunks_links(q.ids, _index->iheader.minTime, q.time_point, q.flag);
  if (raw_links.empty()) {
    return result;
  }
  auto page_io = std::fopen(filename.c_str(), "rb");
  if (page_io == nullptr) {
    THROW_EXCEPTION("can`t open file ", this->filename);
  }

  dariadb::IdSet to_read{q.ids.begin(), q.ids.end()};
  auto indexReccords = _index->readReccords();
  for (auto it = raw_links.rbegin(); it != raw_links.rend(); ++it) {
    if (to_read.empty()) {
      break;
    }
    auto _index_it = indexReccords[it->index_rec_number];
    Chunk_Ptr c = readChunkByOffset(page_io, _index_it.offset);
    if (c == nullptr) {
      continue;
    }
    auto reader = c->getReader();
    while (!reader->is_end()) {
      auto m = reader->readNext();
      if (m.time <= q.time_point && m.inQuery(q.ids, q.flag)) {
        auto f_res = result.find(m.id);
        if (f_res == result.end()) {
          to_read.erase(m.id);
          result[m.id] = m;
        } else {
          if (m.time > f_res->first) {
            result[m.id] = m;
          }
        }
      }
    }
  }

  fclose(page_io);
  return result;
}

void Page::readLinks(const QueryInterval &query, const ChunkLinkList &links,
                     IReaderClb *clbk) {
  auto _ch_links_iterator = links.cbegin();
  if (_ch_links_iterator == links.cend()) {
    return;
  }
  auto page_io = std::fopen(filename.c_str(), "rb");
  if (page_io == nullptr) {
    THROW_EXCEPTION("can`t open file ", this->filename);
  }
  auto indexReccords = _index->readReccords();
  for (; _ch_links_iterator != links.cend(); ++_ch_links_iterator) {
    if (clbk->is_canceled()) {
      break;
    }
    auto _index_it = indexReccords[_ch_links_iterator->index_rec_number];
    Chunk_Ptr search_res = readChunkByOffset(page_io, _index_it.offset);
    if (search_res == nullptr) {
      continue;
    }
    auto rdr = search_res->getReader();
    while (!rdr->is_end()) {
      auto subres = rdr->readNext();
      if (subres.time > query.to) {
        break;
      }
      if (subres.inQuery(query.ids, query.flag, query.from, query.to)) {
        clbk->call(subres);
      }
    }
  }
  fclose(page_io);
}

void Page::appendChunks(const std::vector<Chunk *> &, size_t) {
  NOT_IMPLEMENTED;
}

Id2MinMax Page::loadMinMax() {
  Id2MinMax result;
  auto page_io = std::fopen(filename.c_str(), "rb");
  if (page_io == nullptr) {
    THROW_EXCEPTION("can`t open file ", this->filename);
  }
  auto indexReccords = _index->readReccords();
  for (uint32_t i = 0; i < header.addeded_chunks; ++i) {
    auto _index_it = indexReccords[i];
    Chunk_Ptr search_res = readChunkByOffset(page_io, _index_it.offset);
    if (search_res == nullptr) {
      continue;
    }
    auto info = search_res->header;
    auto fres = result.find(info->meas_id);
    if (fres == result.end()) {
      result[info->meas_id].min = info->first();
      result[info->meas_id].max = info->last();
    } else {
      result[info->meas_id].updateMin(info->first());
      result[info->meas_id].updateMax(info->last());
    }
  }
  std::fclose(page_io);
  return result;
}
