#include "engine.h"
#include "storage/capacitor.h"
#include "storage/memstorage.h"
#include "storage/page_manager.h"
#include "storage/subscribe.h"
#include "utils/exception.h"
#include "utils/locker.h"
#include "storage/inner_readers.h"
#include <algorithm>
#include <cassert>

using namespace dariadb;
using namespace dariadb::storage;

class Engine::Private {
public:
  Private(const PageManager::Params &page_storage_params,
          dariadb::storage::Capacitor::Params cap_params,
          dariadb::storage::Engine::Limits limits)
      : mem_storage{new MemoryStorage()},
        _page_manager_params(page_storage_params), _cap_params(cap_params),
        _limits(limits) {
    _subscribe_notify.start();
	PageManager::start(_page_manager_params);
    mem_cap = new Capacitor(PageManager::instance(), _cap_params);
    mem_storage_raw = dynamic_cast<MemoryStorage *>(mem_storage.get());
  }
  ~Private() {
    _subscribe_notify.stop();
    this->flush();
    delete mem_cap;
    PageManager::stop();
  }

  Time minTime() {
    std::lock_guard<std::recursive_mutex> lg(_locker);
    return PageManager::instance()->minTime();
  }

  Time maxTime() { return PageManager::instance()->minTime(); }

  append_result append(const Meas &value) {
    append_result result{};
	//result = PageManager::instance()->append(value);
	result = mem_cap->append(value);
    if (result.writed == 1) {
      _subscribe_notify.on_append(value);
    }

    return result;
  }

  void subscribe(const IdArray &ids, const Flag &flag,
                 const ReaderClb_ptr &clbk) {
    auto new_s = std::make_shared<SubscribeInfo>(ids, flag, clbk);
    _subscribe_notify.add(new_s);
  }

  Reader_ptr currentValue(const IdArray &ids, const Flag &flag) {
    return mem_storage->currentValue(ids, flag);
  }

  void flush() {
    std::lock_guard<std::recursive_mutex> lg(_locker);
    this->mem_cap->flush();
    PageManager::instance()->flush();
  }

  Engine::QueueSizes queue_size() const {
    QueueSizes result;
    result.page = PageManager::instance()->in_queue_size();
    result.cap = this->mem_cap->in_queue_size();
    return result;
  }

  class UnionReader :public Reader {
  public:
	  UnionReader() {
		  cap_reader = page_reader = nullptr;
	  }
	  // Inherited via Reader
    bool isEnd() const override {
      return (page_reader == nullptr || page_reader->isEnd()) &&
             (cap_reader == nullptr || cap_reader->isEnd());
    }

    IdArray getIds() const override {
      dariadb::IdSet idset;
      if (page_reader != nullptr && !page_reader->isEnd()) {
        auto sub_res = page_reader->getIds();
        for (auto id : sub_res) {
          idset.insert(id);
        }
      } else {
        if (cap_reader != nullptr && !cap_reader->isEnd()) {
          auto sub_res = cap_reader->getIds();
          for (auto id : sub_res) {
            idset.insert(id);
          }
        }
      }
      return dariadb::IdArray{idset.begin(), idset.end()};
	  }
	  
	  void readNext(ReaderClb * clb) override{
		  if (page_reader != nullptr && !page_reader->isEnd()) {
			  page_reader->readNext(clb);
		  }
		  else {
			  if (cap_reader != nullptr && !cap_reader->isEnd()) {
				  cap_reader->readNext(clb);
			  }
		  }
	  }
	  Reader_ptr clone() const override{
		  UnionReader*raw_res = new UnionReader();
		  raw_res->page_reader = this->page_reader;
		  raw_res->cap_reader = this->cap_reader;
		  return Reader_ptr(raw_res);
	  }

	  void reset() override{
		  if (page_reader != nullptr) {
			  page_reader->reset();
		  }
		  if (cap_reader != nullptr) {
			  cap_reader->reset();
		  }
	  }

	  Reader_ptr page_reader;
	  Reader_ptr cap_reader;
  };

  // Inherited via MeasStorage
  Reader_ptr readInterval(const QueryInterval &q) { 
	  auto chunkLinks = PageManager::instance()->chunksByIterval(q);
	  auto cursor = PageManager::instance()->readLinks(chunkLinks);
	  InnerReader *raw_rdr = new InnerReader(q.flag, q.from, q.to);
	  raw_rdr->add(cursor);

	  UnionReader* raw_res = new UnionReader();
	  raw_res->page_reader = Reader_ptr{ raw_rdr };
	  if (mem_cap->minTime() <= q.from) {
		  auto mc_reader=mem_cap->readInterval(q);
		  raw_res->cap_reader = Reader_ptr{ mc_reader };
	  }
	  return Reader_ptr(raw_res);
  }

  Reader_ptr readInTimePoint(const QueryTimePoint &q) { 
	  if (q.time_point < mem_cap->minTime()) {
		  auto chunkLinks = PageManager::instance()->chunksBeforeTimePoint(q);
		  auto cursor = PageManager::instance()->readLinks(chunkLinks);
		  ChunksList clist;
		  cursor->readAll(&clist);
		  IdToChunkMap  chunks_before;
		  for (auto ch : clist) {
			  chunks_before[ch->info->first.id] = ch;
		  }
		  auto res = std::make_shared<InnerReader>(q.flag, q.time_point, 0);
		  res->is_time_point_reader = true;

		  for (auto id : q.ids) {
			  auto search_res = chunks_before.find(id);
			  if (search_res == chunks_before.end()) {
				  res->_not_exist.push_back(id);
			  }
			  else {
				  auto ch = search_res->second;
				  res->add_tp(ch);
			  }
		  }
		  return res;
	  }
	  else {
		  return mem_cap->readInTimePoint(q);
	  }
  }

protected:
  std::shared_ptr<MemoryStorage> mem_storage;
  storage::MemoryStorage *mem_storage_raw;
  storage::Capacitor *mem_cap;

  storage::PageManager::Params _page_manager_params;
  dariadb::storage::Capacitor::Params _cap_params;
  dariadb::storage::Engine::Limits _limits;

  mutable std::recursive_mutex _locker;
  SubscribeNotificator _subscribe_notify;
};

Engine::Engine(storage::PageManager::Params page_manager_params,
               dariadb::storage::Capacitor::Params cap_params,
               const dariadb::storage::Engine::Limits &limits)
    : _impl{new Engine::Private(page_manager_params, cap_params, limits)} {}

Engine::~Engine() {
  _impl = nullptr;
}

Time Engine::minTime() {
  return _impl->minTime();
}

Time Engine::maxTime() {
  return _impl->maxTime();
}

append_result Engine::append(const Meas &value) {
  return _impl->append(value);
}

void Engine::subscribe(const IdArray &ids, const Flag &flag,
                       const ReaderClb_ptr &clbk) {
  _impl->subscribe(ids, flag, clbk);
}

Reader_ptr Engine::currentValue(const IdArray &ids, const Flag &flag) {
  return _impl->currentValue(ids, flag);
}

void Engine::flush() {
  _impl->flush();
}

Engine::QueueSizes Engine::queue_size() const {
  return _impl->queue_size();
}

// Reader_ptr dariadb::storage::Engine::readInterval(Time from, Time to){
//	return _impl->readInterval(from, to);
//}

// Reader_ptr dariadb::storage::Engine::readInTimePoint(Time time_point) {
//	return _impl->readInTimePoint(time_point);
//}

Reader_ptr dariadb::storage::Engine::readInterval(const QueryInterval &q) {
  return _impl->readInterval(q);
}

Reader_ptr dariadb::storage::Engine::readInTimePoint(const QueryTimePoint &q) {
  return _impl->readInTimePoint(q);
}