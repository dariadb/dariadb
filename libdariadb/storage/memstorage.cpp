#include "memstorage.h"
#include "../utils/utils.h"
#include "../compression.h"
#include "../flags.h"
#include "subscribe.h"
#include "chunk.h"
#include "../timeutil.h"
#include "inner_readers.h"

#include <limits>
#include <algorithm>
#include <map>
#include <tuple>
#include <assert.h>
#include <mutex>

using namespace dariadb;
using namespace dariadb::compression;
using namespace dariadb::storage;

typedef std::map<Id, ChuncksList> ChunkMap;

class MemoryStorage::Private
{
public:
	Private(size_t size) :
		_size(size),
		_min_time(std::numeric_limits<dariadb::Time>::max()),
		_max_time(std::numeric_limits<dariadb::Time>::min()),
		_subscribe_notify(new SubscribeNotificator)
	{
		_subscribe_notify->start();
	}

	~Private() {
		_subscribe_notify->stop();
		_chuncks.clear();
	}

	Time minTime() { return _min_time; }
	Time maxTime() { return _max_time; }

	Chunk_Ptr getFreeChunk(dariadb::Id id) {
		Chunk_Ptr resulted_chunk = nullptr;
		auto ch_iter = _free_chunks.find(id);
		if (ch_iter != _free_chunks.end()) {
			if (!ch_iter->second->is_full()) {
				return ch_iter->second;
			}
		}
		return resulted_chunk;
	}

	append_result append(const Meas& value) {
		std::lock_guard<std::mutex> lg(_mutex);

		Chunk_Ptr chunk = this->getFreeChunk(value.id);


		if (chunk == nullptr) {
			chunk = std::make_shared<Chunk>(_size, value);
			this->_chuncks[value.id].push_back(chunk);
			this->_free_chunks[value.id] = chunk;

		}
		else {
			if (!chunk->append(value)) {
				chunk = std::make_shared<Chunk>(_size, value);
				this->_chuncks[value.id].push_back(chunk);

			}
		}

		_min_time = std::min(_min_time, value.time);
		_max_time = std::max(_max_time, value.time);

		_subscribe_notify->on_append(value);

		return dariadb::append_result(1, 0);
	}


	append_result append(const Meas::PMeas begin, const size_t size) {
		dariadb::append_result result{};
		for (size_t i = 0; i < size; i++) {
			result = result + append(begin[i]);
		}
		return result;

	}

	std::shared_ptr<InnerReader> readInterval(const IdArray &ids, Flag flag, Time from, Time to) {
		std::shared_ptr<InnerReader> res;
		if (from > this->minTime()) {
			res = this->readInTimePoint(ids, flag, from);
			res->_from = from;
			res->_to = to;
			res->_flag = flag;
		}
		else {
			res = std::make_shared<InnerReader>(flag, from, to);
		}

		auto neededChunks = chunksByIterval(ids, flag, from, to);
		for (auto cur_chunk : neededChunks) {
			res->add(cur_chunk, cur_chunk->count);
		}
		res->is_time_point_reader = false;
		return res;
	}

	std::shared_ptr<InnerReader> readInTimePoint(const IdArray &ids, Flag flag, Time time_point) {
		auto res = std::make_shared<InnerReader>(flag, time_point, 0);
		res->is_time_point_reader = true;

		auto chunks_before = chunksBeforeTimePoint(ids, flag, time_point);
		IdArray target_ids{ ids };
		if (target_ids.size() == 0) {
			target_ids = getIds();
		}

		for (auto id : target_ids) {
			auto search_res = chunks_before.find(id);
			if (search_res == chunks_before.end()) {
				res->_not_exist.push_back(id);
			}
			else {
				auto ch = search_res->second;
				res->add_tp(ch, ch->count);
			}
		}

		return res;
	}

	size_t size()const { return _size; }
	size_t chunks_size()const { return _chuncks.size(); }

	size_t chunks_total_size()const {
		size_t result = 0;
		for (auto kv : _chuncks) {
			result += kv.second.size();
		}
		return result;
	}

	void subscribe(const IdArray&ids, const Flag& flag, const ReaderClb_ptr &clbk) {
		auto new_s = std::make_shared<SubscribeInfo>(ids, flag, clbk);
		_subscribe_notify->add(new_s);
	}

	Reader_ptr currentValue(const IdArray&ids, const Flag& flag) {
		std::lock_guard<std::mutex> lg(_mutex_tp);
		auto res_raw = new InnerCurrentValuesReader();
		Reader_ptr res{ res_raw };
		for (auto &kv : _free_chunks) {
			auto l = kv.second->last;
			if ((ids.size() != 0) && (std::find(ids.begin(), ids.end(), l.id) == ids.end())) {
				continue;
			}
			if ((flag == 0) || (l.flag == flag)) {
				res_raw->_cur_values.push_back(l);
			}
		}
		return res;
	}

	dariadb::storage::ChuncksList drop_old_chunks(const dariadb::Time min_time) {
		std::lock_guard<std::mutex> lg(_mutex_tp);
		ChuncksList result;
		auto now = dariadb::timeutil::current_time();

		for (auto& kv : _chuncks) {
			while ((kv.second.size() > 0)) {
				auto past = (now - min_time);
				if (kv.second.front()->maxTime < past) {
					result.push_back(kv.second.front());
					kv.second.pop_front();
				}
				else {
					break;
				}
			}
		}
		return result;
	}

	ChuncksList chunksByIterval(const IdArray &ids, Flag flag, Time from, Time to) {
		ChuncksList result{};

		for (auto ch : _chuncks) {
			if ((ids.size() != 0) && (std::find(ids.begin(), ids.end(), ch.first) == ids.end())) {
				continue;
			}
			for (auto &cur_chunk : ch.second) {
				if (flag != 0) {
					if (!cur_chunk->check_flag(flag)) {
						continue;
					}
				}
				if ((utils::inInterval(from, to, cur_chunk->minTime)) || (utils::inInterval(from, to, cur_chunk->maxTime))) {
					result.push_back(cur_chunk);
				}
			}

		}
		return result;
	}

	IdToChunkMap chunksBeforeTimePoint(const IdArray &ids, Flag flag, Time timePoint) {
		IdToChunkMap result;
		for (auto ch : _chuncks) {
			if ((ids.size() != 0) && (std::find(ids.begin(), ids.end(), ch.first) == ids.end())) {
				continue;
			}
			auto chunks = &ch.second;
			for (auto it = chunks->rbegin(); it != chunks->crend(); ++it) {
				auto cur_chunk = *it;
				if (!cur_chunk->check_flag(flag)) {
					continue;
				}
				if (cur_chunk->minTime <= timePoint) {
					result[ch.first] = cur_chunk;
					break;
				}
			}
		}
		return result;
	}

	dariadb::IdArray getIds()const {
		dariadb::IdArray result;
		result.resize(_chuncks.size());
		size_t pos = 0;
		for (auto&kv : _chuncks) {
			result[pos] = kv.first;
			pos++;
		}
		return result;
	}
protected:
	size_t _size;

	ChunkMap _chuncks;
	IdToChunkMap _free_chunks;
	Time _min_time, _max_time;
	std::unique_ptr<SubscribeNotificator> _subscribe_notify;
	std::mutex _mutex;
	std::mutex _mutex_tp;
};


MemoryStorage::MemoryStorage(size_t size)
	:_Impl(new MemoryStorage::Private(size)) {
}


MemoryStorage::~MemoryStorage() {
}

Time MemoryStorage::minTime() {
	return _Impl->minTime();
}

Time MemoryStorage::maxTime() {
	return _Impl->maxTime();
}

append_result MemoryStorage::append(const dariadb::Meas &value) {
	return _Impl->append(value);
}

append_result MemoryStorage::append(const dariadb::Meas::PMeas begin, const size_t size) {
	return _Impl->append(begin, size);
}

Reader_ptr MemoryStorage::readInterval(const dariadb::IdArray &ids, dariadb::Flag flag, dariadb::Time from, dariadb::Time to) {
	return _Impl->readInterval(ids, flag, from, to);
}

Reader_ptr  MemoryStorage::readInTimePoint(const dariadb::IdArray &ids, dariadb::Flag flag, dariadb::Time time_point) {
	return _Impl->readInTimePoint(ids, flag, time_point);
}

size_t  MemoryStorage::size()const {
	return _Impl->size();
}

size_t  MemoryStorage::chunks_size()const {
	return _Impl->chunks_size();
}

size_t MemoryStorage::chunks_total_size()const {
	return _Impl->chunks_total_size();
}

void MemoryStorage::subscribe(const IdArray&ids, const Flag& flag, const ReaderClb_ptr &clbk) {
	return _Impl->subscribe(ids, flag, clbk);
}

Reader_ptr MemoryStorage::currentValue(const IdArray&ids, const Flag& flag) {
	return  _Impl->currentValue(ids, flag);
}

void MemoryStorage::flush()
{
}

dariadb::storage::ChuncksList MemoryStorage::drop_old_chunks(const dariadb::Time min_time)
{
	return _Impl->drop_old_chunks(min_time);
}

ChuncksList MemoryStorage::chunksByIterval(const IdArray &ids, Flag flag, Time from, Time to) {
	return _Impl->chunksByIterval(ids, flag, from, to);
}

IdToChunkMap MemoryStorage::chunksBeforeTimePoint(const IdArray &ids, Flag flag, Time timePoint) {
	return _Impl->chunksBeforeTimePoint(ids, flag, timePoint);
}

dariadb::IdArray MemoryStorage::getIds()const {
	return _Impl->getIds();
}