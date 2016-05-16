#include "page_manager.h"
#include "../utils/asyncworker.h"
#include "../utils/fs.h"
#include "../utils/locker.h"
#include "../utils/utils.h"
#include "manifest.h"
#include "page.h"

#include <condition_variable>
#include <cstring>
#include <queue>
#include <thread>
#include <functional>
#include <memory>
#include <boost/thread/locks.hpp>
#include <boost/thread/shared_mutex.hpp>

const std::string MANIFEST_FILE_NAME = "Manifest";

using namespace dariadb::storage;
dariadb::storage::PageManager *PageManager::_instance = nullptr;
// PM
class PageManager::Private /*:public dariadb::utils::AsyncWorker<Chunk_Ptr>*/ {
public:
  Private(const PageManager::Params &param)
      : _cur_page(nullptr), _param(param),
        _manifest(utils::fs::append_path(param.path, MANIFEST_FILE_NAME)) {
    /*this->start_async();*/
  }

  ~Private() {
    /* this->stop_async();*/

    if (_cur_page != nullptr) {
      delete _cur_page;
      _cur_page = nullptr;
    }
  }

  uint64_t calc_page_size() const {
    auto sz_info = _param.chunk_per_storage * sizeof(ChunkIndexInfo);
    auto sz_buffers = _param.chunk_per_storage * _param.chunk_size;
    return sizeof(PageHeader) + sz_buffers + sz_info;
  }

  Page *create_page() {
    if (!dariadb::utils::fs::path_exists(_param.path)) {
      dariadb::utils::fs::mkdir(_param.path);
    }

    Page *res = nullptr;

    auto names = _manifest.page_list();
    for (auto n : names) {
      auto file_name = utils::fs::append_path(_param.path, n);
      auto hdr = Page::readHeader(file_name);
      if (!hdr.is_full) {
        res = Page::open(file_name);
      }
    }
    if (res == nullptr) {
      std::string page_name = utils::fs::random_file_name(".page");
      std::string file_name =
          dariadb::utils::fs::append_path(_param.path, page_name);
      auto sz = calc_page_size();
      res = Page::create(file_name, sz, _param.chunk_per_storage,
                         _param.chunk_size);
      _manifest.page_append(page_name);
    }

    return res;
  }
  // PM
  void flush() { /*this->flush_async();*/
  }
  // void call_async(const Chunk_Ptr &ch) override { /*write_to_page(ch);*/ }

  Page *get_cur_page() {
    if (_cur_page == nullptr) {
      _cur_page = create_page();
    }
    return _cur_page;
  }

  bool minMaxTime(dariadb::Id id, dariadb::Time *minResult,
                  dariadb::Time *maxResult) {
	boost::shared_lock<boost::shared_mutex> lg(_locker);

	auto pages = pages_by_filter([id](const IndexHeader&ih) {
		return (storage::bloom_check(ih.id_bloom, id));
	});
	*minResult = std::numeric_limits<dariadb::Time>::max();
	*maxResult = std::numeric_limits<dariadb::Time>::min();
	auto res = false;
	for (auto pname : pages) {
		auto p = Page::open(pname);
		dariadb::Time local_min, local_max;
		if (p->minMaxTime(id, &local_min, &local_max)) {
			*minResult = std::min(local_min, *minResult);
			*maxResult = std::max(local_max, *maxResult);
			res = true;
		}
		delete p;
	}
	return res;
  }

  class PageManagerCursor:public Cursor{
  public:
      bool is_end() const override{
          return _chunk_iterator==chunks.end();
      }
      void readNext(Callback *cbk)  override{
          if(!is_end()){
              cbk->call(*_chunk_iterator);
              ++_chunk_iterator;
          }
      }
      void reset_pos()  override{
          _chunk_iterator=chunks.begin();
      }

      ChunksList chunks;
      ChunksList::iterator _chunk_iterator;
  };

  class AddCursorClbk:public Cursor::Callback{
    public:
      void call(dariadb::storage::Chunk_Ptr &ptr){
		  if (ptr != nullptr) {
			  (*out)[ptr->info->first.id].push_back(ptr);
		  }
      }

      ChunkMap*out;
  };

  Cursor_ptr chunksByIterval(const QueryInterval &query) {
	boost::shared_lock<boost::shared_mutex> lg(_locker);
    PageManagerCursor*raw_cursor=new PageManagerCursor;
    ChunkMap chunks;

    auto pred=[query](const IndexHeader &hdr){
        auto interval_check((hdr.minTime >= query.from && hdr.maxTime <= query.to) ||
                   (utils::inInterval(query.from, query.to, hdr.minTime)) ||
                    (utils::inInterval(query.from, query.to, hdr.maxTime)));
        if(interval_check){
            for(auto id:query.ids){
                if(storage::bloom_check(hdr.id_bloom,id)){
                    return true;
                }
            }
        }
        return false;
    };
    auto page_list=pages_by_filter(std::function<bool(const IndexHeader&)>(pred));

    std::unique_ptr<AddCursorClbk> clbk{new AddCursorClbk};
    clbk->out=&chunks;
    for(auto pname:page_list){
        Page *cand=Page::open(pname,true);

        cand->chunksByIterval(query)->readAll(clbk.get());
        delete cand;
    }

    for(auto&kv:chunks){
        for(auto&ch:kv.second){
            raw_cursor->chunks.push_back(ch);
        }
    }
    raw_cursor->reset_pos();
    return Cursor_ptr{raw_cursor};
  }

  std::list<std::string> pages_by_filter(std::function<bool(const IndexHeader&)>pred){
      std::list<std::string> result;
      auto names = _manifest.page_list();
      for (auto n : names) {
        auto index_file_name = utils::fs::append_path(_param.path, n + "i");
        auto hdr = Page::readIndexHeader(index_file_name);
        if(pred(hdr)){
            auto page_file_name = utils::fs::append_path(_param.path, n);
            result.push_back(page_file_name);
        }
      }
      return result;
  }

  IdToChunkMap chunksBeforeTimePoint(const QueryTimePoint &query) {
	  boost::shared_lock<boost::shared_mutex> lg(_locker);
	  
	  IdToChunkMap chunks;

	  auto pred = [query](const IndexHeader&hdr) {
		  auto in_check=utils::inInterval(hdr.minTime, hdr.maxTime, query.time_point);
		  if (in_check) {
			  for (auto id : query.ids) {
				  if (storage::bloom_check(hdr.id_bloom, id)) {
					  return true;
				  }
			  }
		  }
		  return false;
	  };
	  auto page_list = pages_by_filter(std::function<bool(IndexHeader)>(pred));

	  for (auto pname : page_list) {
		  Page *pg = Page::open(pname, true);

		  auto chMap=pg->chunksBeforeTimePoint(query);
		  for (auto kv : chMap) {
			  if (kv.second != nullptr) {
				  chunks[kv.first] = kv.second;
			  }
		  }
		  delete pg;
	  }

	  return chunks;
  }

  dariadb::IdArray getIds() {
	  boost::shared_lock<boost::shared_mutex> lg(_locker);

	  auto pred = [](const IndexHeader&) {
		  return true;
	  };

	  auto page_list = pages_by_filter(std::function<bool(IndexHeader)>(pred));
	  dariadb::IdSet s;
	  for (auto pname : page_list) {
		  Page *pg = Page::open(pname, true);
		  auto subres=pg->getIds();
		  s.insert(subres.begin(), subres.end());
		  delete pg;
	  }

	  return dariadb::IdArray(s.begin(), s.end());
  }

  size_t chunks_in_cur_page() const {
    if (_cur_page == nullptr) {
      return 0;
    }
    return _cur_page->header->addeded_chunks;
  }
  // PM
  size_t in_queue_size() const {
    return 0; /*return this->async_queue_size();*/
  }

  dariadb::Time minTime() {
	boost::shared_lock<boost::shared_mutex> lg(_locker);
    if (_cur_page == nullptr) {
      return dariadb::Time(0);
    } else {
      return _cur_page->iheader->minTime;
    }
  }

  dariadb::Time maxTime() {
	boost::shared_lock<boost::shared_mutex> lg(_locker);
    if (_cur_page == nullptr) {
      return dariadb::Time(0);
    } else {
      return _cur_page->iheader->maxTime;
    }
  }

  append_result append(const Meas &value) {
	boost::upgrade_lock<boost::shared_mutex> lg(_locker);
	uint64_t last_id = 0;
	bool update_id = false;
    while (true) {
      auto cur_page = this->get_cur_page();
	  if (update_id) {
		  _cur_page->header->max_chunk_id= last_id;
		  update_id = false;
	  }
      auto res = cur_page->append(value);
      if (res.writed != 1) {
		  last_id = _cur_page->header->max_chunk_id;
		  update_id = true;
		  delete _cur_page;
        _cur_page = nullptr;
      } else {
        return res;
      }
    }
  }

protected:
  Page *_cur_page;
  PageManager::Params _param;
  Manifest _manifest;
  boost::shared_mutex _locker;
};

PageManager::PageManager(const PageManager::Params &param)
    : impl(new PageManager::Private{param}) {}

PageManager::~PageManager() {}

void PageManager::start(const PageManager::Params &param) {
  if (PageManager::_instance == nullptr) {
    PageManager::_instance = new PageManager(param);
  }
}

void PageManager::stop() {
  if (_instance != nullptr) {
    delete PageManager::_instance;
    _instance = nullptr;
  }
}

void PageManager::flush() {
  this->impl->flush();
}

PageManager *PageManager::instance() {
  return _instance;
}
// PM
// bool PageManager::append(const Chunk_Ptr &c) {
//  return impl->append(c);
//}
//
// bool PageManager::append(const ChunksList &c) {
//  return impl->append(c);
//}

// Cursor_ptr PageManager::get_chunks(const dariadb::IdArray&ids, dariadb::Time
// from, dariadb::Time to, dariadb::Flag flag) {
//    return impl->get_chunks(ids, from, to, flag);
//}
// ChunkContainer
bool PageManager::minMaxTime(dariadb::Id id, dariadb::Time *minResult,
                             dariadb::Time *maxResult) {
  return impl->minMaxTime(id, minResult, maxResult);
}

dariadb::storage::Cursor_ptr
PageManager::chunksByIterval(const QueryInterval &query) {
  return impl->chunksByIterval(query);
}

dariadb::storage::IdToChunkMap
PageManager::chunksBeforeTimePoint(const QueryTimePoint &q) {
  return impl->chunksBeforeTimePoint(q);
}

dariadb::IdArray PageManager::getIds() {
  return impl->getIds();
}

// dariadb::storage::ChunksList PageManager::get_open_chunks() {
//  return impl->get_open_chunks();
//}

size_t PageManager::chunks_in_cur_page() const {
  return impl->chunks_in_cur_page();
}

size_t PageManager::in_queue_size() const {
  return impl->in_queue_size();
}

dariadb::Time PageManager::minTime() {
  return impl->minTime();
}

dariadb::Time PageManager::maxTime() {
  return impl->maxTime();
}

dariadb::append_result
dariadb::storage::PageManager::append(const Meas &value) {
  return impl->append(value);
}
