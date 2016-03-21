#include "memstorage.h"
#include "utils.h"
#include "compression.h"
#include "flags.h"
#include "subscribe.h"
#include "bloom_filter.h"

#include <limits>
#include <algorithm>
#include <map>
#include <tuple>
#include <assert.h>
#include <mutex>

using namespace dariadb;
using namespace dariadb::compression;
using namespace dariadb::storage;
using dariadb::utils::Range;


struct Chunk
{
    std::vector<uint8_t> _buffer_t;
    Range range;
    CopmressedWriter c_writer;
    size_t count;
    Meas first,last;

    Time minTime,maxTime;
	std::mutex _mutex;
	dariadb::Flag flag_bloom;

    BinaryBuffer_Ptr bw;
    Chunk(size_t size, Meas first_m):
        _buffer_t(size),
        count(0),
        first(first_m),
        _mutex()
    {
        minTime=std::numeric_limits<Time>::max();
        maxTime=std::numeric_limits<Time>::min();

        std::fill(_buffer_t.begin(), _buffer_t.end(),0);


        using compression::BinaryBuffer;
        range = Range{ _buffer_t.data(),_buffer_t.data() + size - 1};
        bw=std::make_shared<BinaryBuffer>(range);

        c_writer = compression::CopmressedWriter(bw);
        c_writer.append(first);
        minTime=std::min(minTime,first_m.time);
        maxTime=std::max(maxTime,first_m.time);
		flag_bloom = dariadb::bloom_empty<dariadb::Flag>();
    }

    ~Chunk(){
       
    }
    bool append(const Meas&m)
    {
		std::lock_guard<std::mutex> lg(_mutex);
        auto t_f = this->c_writer.append(m);

        if (!t_f) {
            return false;
        }else{
            count++;

            minTime=std::min(minTime,m.time);
            maxTime=std::max(maxTime,m.time);
			flag_bloom = dariadb::bloom_add(flag_bloom, m.flag);
			last = m;
            return true;
        }
    }


    bool is_full()const { return c_writer.is_full(); }
};

typedef std::shared_ptr<Chunk>    Chunk_Ptr;
typedef std::list<Chunk_Ptr>      ChuncksList;
typedef std::map<Id, ChuncksList> ChunkMap;
typedef std::map<Id, Chunk_Ptr>   FreeChunksMap;

class InnerReader: public Reader{
public:
    struct ReadChunk
    {
        size_t    count;
        Chunk_Ptr chunk;
        ReadChunk()=default;
        ReadChunk(const ReadChunk&other){
            count=other.count;
            chunk=other.chunk;
        }
        ReadChunk&operator=(const ReadChunk&other){
            if(this!=&other){
                count=other.count;
                chunk=other.chunk;
            }
            return *this;
        }
    };
    InnerReader(dariadb::Flag flag, dariadb::Time from, dariadb::Time to):
        _chunks{},
        _flag(flag),
        _from(from),
        _to(to),
        _tp_readed(false)
    {
        is_time_point_reader = false;
        end=false;
    }

    void add(Chunk_Ptr c, size_t count){
		std::lock_guard<std::mutex> lg(_mutex);
        ReadChunk rc;
        rc.chunk = c;
        rc.count = count;
        this->_chunks[c->first.id].push_back(rc);
    }

    void add_tp(Chunk_Ptr c, size_t count){
		std::lock_guard<std::mutex> lg(_mutex);
        ReadChunk rc;
        rc.chunk = c;
        rc.count = count;
        this->_tp_chunks[c->first.id].push_back(rc);
    }

    bool isEnd() const override{
        return this->end && this->_tp_readed;
    }

	dariadb::IdArray getIds()const override {
		dariadb::IdArray result;
		result.resize(_chunks.size());
		size_t pos = 0;
		for (auto &kv : _chunks) {
			result[pos] = kv.first;
			pos++;
		}
		return result;
	}

	void readCurVals(storage::ReaderClb*clb) {
		for (auto v : _cur_values) {
			clb->call(v);
		}
		this->end = true;
		this->_tp_readed = true;
	}

    void readNext(storage::ReaderClb*clb) override {
		std::lock_guard<std::mutex> lg(_mutex);
		
		if (_cur_values.size() != 0) {
			readCurVals(clb);
			return;
		}

        if (!_tp_readed) {
            this->readTimePoint(clb);
        }

        for (auto ch : _chunks) {
            for (size_t i = 0; i < ch.second.size(); i++) {
                auto cur_ch=ch.second[i].chunk;
                auto bw=std::make_shared<BinaryBuffer>(cur_ch->bw->get_range());
                bw->reset_pos();
                CopmressedReader crr(bw, cur_ch->first);

                if (check_meas(ch.second[i].chunk->first)) {
                    auto sub=ch.second[i].chunk->first;
                    clb->call(sub);
                }

                for (size_t j = 0; j < ch.second[i].count; j++) {
                    auto sub = crr.read();
                    sub.id = ch.second[i].chunk->first.id;
                    if (check_meas(sub)) {
                        clb->call(sub);
                    }else{
                        if(sub.time>_to){
                            break;
                        }
                    }
                }

            }
        }
        end=true;
    }

    void readTimePoint(storage::ReaderClb*clb){
        std::lock_guard<std::mutex> lg(_mutex_tp);
        std::list<InnerReader::ReadChunk> to_read_chunks{};
        for (auto ch : _tp_chunks) {
            auto candidate = ch.second.front();

            for (size_t i = 0; i < ch.second.size(); i++) {
                auto cur_chunk=ch.second[i].chunk;
                if (candidate.chunk->first.time < cur_chunk->first.time){
                    candidate = ch.second[i];
                }
            }
            to_read_chunks.push_back(candidate);
        }

        for (auto ch : to_read_chunks) {
            auto bw=std::make_shared<BinaryBuffer>(ch.chunk->bw->get_range());
            bw->reset_pos();
            CopmressedReader crr(bw, ch.chunk->first);

            Meas candidate;
            candidate = ch.chunk->first;
            for (size_t i = 0; i < ch.count; i++) {
                auto sub = crr.read();
                sub.id = ch.chunk->first.id;
                if ((sub.time <= _from) && (sub.time >= candidate.time)) {
                    candidate = sub;
                }if(sub.time>_from){
                    break;
                }
            }
            if (candidate.time <= _from) {
				//TODO make as options
				candidate.time = _from;

                clb->call(candidate);
				_tp_readed_times.insert(std::make_tuple(candidate.id, candidate.time));
            }
        }
        auto m=dariadb::Meas::empty();
        m.time=_from;
        m.flag=dariadb::Flags::NO_DATA;
        for(auto id:_not_exist){
            m.id=id;
            clb->call(m);
        }
        _tp_readed=true;
    }


    bool is_time_point_reader;

    bool check_meas(const Meas&m)const{
		auto tmp = std::make_tuple(m.id, m.time);
		if (this->_tp_readed_times.find(tmp) != _tp_readed_times.end()) {
			return false;
		}
        using utils::inInterval;

        if ((in_filter(_flag, m.flag))&&(inInterval(_from, _to, m.time))) {
            return true;
        }
        return false;
    }

	Reader_ptr clone()const override{
		auto res= std::make_shared<InnerReader>(_flag, _from,_to);
		res->_chunks = _chunks;
		res->_tp_chunks = _tp_chunks;
		res->_flag = _flag;
		res->_from = _from;
		res->_to = _to;
		res->_tp_readed = _tp_readed;
		res->end = end;
		res->_not_exist = _not_exist;
		res->_tp_readed_times = _tp_readed_times;
		return res;
	}
	void reset()override {
		end = false;
		_tp_readed = false;
		_tp_readed_times.clear();
	}
    typedef std::vector<ReadChunk> ReadChuncksVector;
    typedef std::map<Id, ReadChuncksVector> ReadChunkMap;

    ReadChunkMap _chunks;
    ReadChunkMap _tp_chunks;
    dariadb::Flag _flag;
    dariadb::Time _from;
    dariadb::Time _to;
    bool _tp_readed;
    bool end;
    IdArray _not_exist;

	typedef std::tuple<dariadb::Id, dariadb::Time> IdTime;
	std::set<IdTime> _tp_readed_times;

    std::mutex _mutex,_mutex_tp;

	dariadb::Meas::MeasList _cur_values;
};



class MemoryStorage::Private
{
public:
    Private(size_t size):
        _size(size),
        _min_time(std::numeric_limits<dariadb::Time>::max()),
        _max_time(std::numeric_limits<dariadb::Time>::min()),
		_subscribe_notify(new SubscribeNotificator)
    {
		_subscribe_notify->start();
	}

    ~Private(){
		_subscribe_notify->stop();
        _chuncks.clear();
    }

    Time minTime(){return _min_time;}
    Time maxTime(){return _max_time;}

    Chunk_Ptr getFreeChunk(dariadb::Id id){
        Chunk_Ptr resulted_chunk=nullptr;
		auto ch_iter = _free_chunks.find(id);
		if (ch_iter != _free_chunks.end()) {
			if (!ch_iter->second->is_full()){
				return ch_iter->second;
			}
        }
        return resulted_chunk;
    }

    append_result append(const Meas& value){
		std::lock_guard<std::mutex> lg(_mutex);

        Chunk_Ptr chunk=this->getFreeChunk(value.id);

       
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


    append_result append(const Meas::PMeas begin, const size_t size){
        dariadb::append_result result{};
        for (size_t i = 0; i < size; i++) {
            result = result + append(begin[i]);
        }
        return result;

    }

    std::shared_ptr<InnerReader> readInterval(const IdArray &ids, Flag flag, Time from, Time to){
		std::lock_guard<std::mutex> lg(_mutex);
		std::shared_ptr<InnerReader> res;
		if (from > this->minTime())  {
			res = this->readInTimePoint(ids, flag, from);
			res->_from = from;
			res->_to = to;
			res->_flag = flag;
		}
		else {
			res= std::make_shared<InnerReader>(flag, from, to);
		}
        
        for (auto ch : _chuncks) {
            if ((ids.size() != 0) && (std::find(ids.begin(), ids.end(), ch.first) == ids.end())) {
                continue;
            }
			for (auto &cur_chunk : ch.second) {
				if (flag != 0) {
					if (!bloom_check(cur_chunk->flag_bloom, flag)) {
						continue;
					}
				}
				if ((utils::inInterval(from, to, cur_chunk->minTime)) || (utils::inInterval(from, to, cur_chunk->maxTime))) {
					res->add(cur_chunk, cur_chunk->count);
				}
			}

        }
        res->is_time_point_reader=false;
        return res;
    }

    std::shared_ptr<InnerReader> readInTimePoint(const IdArray &ids, Flag flag, Time time_point){
        std::lock_guard<std::mutex> lg(_mutex_tp);
		auto res = std::make_shared<InnerReader>(flag, time_point, 0);
		res->is_time_point_reader = true;
		
		if (ids.size() == 0) {
			for (auto ch : _chuncks) {
				load_tp_from_chunks(res.get(), ch.second, time_point, ch.first, flag);
			}
		}
		else {
			for (auto id : ids) {
				auto search_res = _chuncks.find(id);
				if (search_res == _chuncks.end()) {
					res->_not_exist.push_back(id);
				}
				else {
					auto ch = search_res->second;
					load_tp_from_chunks(res.get(), ch, time_point, id, flag);
				}
			}
		}
        return res;
    }

	void load_tp_from_chunks(InnerReader *_ptr, ChuncksList chunks, Time time_point, Id id,Flag flag) {
		bool is_exists = false;
		for (auto&cur_chunk : chunks) {
			if (flag != 0) {
				if (!dariadb::bloom_check(cur_chunk->flag_bloom, flag)) {
					continue;
				}
			}
			if (cur_chunk->minTime <= time_point) {
				_ptr->add_tp(cur_chunk, cur_chunk->count);
				is_exists = true;
			}
		}
		if (!is_exists) {
			_ptr->_not_exist.push_back(id);
		}
	}

    size_t size()const { return _size; }
    size_t chunks_size()const { return _chuncks.size(); }

    void subscribe(const IdArray&ids,const Flag& flag, const ReaderClb_ptr &clbk) {
        auto new_s=std::make_shared<SubscribeInfo>(ids,flag,clbk);
		_subscribe_notify->add(new_s);
	}

	Reader_ptr currentValue(const IdArray&ids, const Flag& flag) {
		std::lock_guard<std::mutex> lg(_mutex_tp);
		auto res = std::make_shared<InnerReader>(flag, 0, 0);
		for (auto &kv: _free_chunks) {
			auto l = kv.second->last;
			if ((ids.size() != 0) && (std::find(ids.begin(), ids.end(), l.id) == ids.end())) {
				continue;
			}
			if ((flag==0) || (l.flag == flag)) {
				res->_cur_values.push_back(l);
			}
		}
		return res;
	}

protected:
    size_t _size;

    ChunkMap _chuncks;
	FreeChunksMap _free_chunks;
    Time _min_time,_max_time;
	std::unique_ptr<SubscribeNotificator> _subscribe_notify;
	std::mutex _mutex;
    std::mutex _mutex_tp;
};


MemoryStorage::MemoryStorage(size_t size)
    :_Impl(new MemoryStorage::Private(size)){
}


MemoryStorage::~MemoryStorage(){
}

Time MemoryStorage::minTime(){
    return _Impl->minTime();
}

Time MemoryStorage::maxTime(){
    return _Impl->maxTime();
}

append_result MemoryStorage::append(const dariadb::Meas &value){
    return _Impl->append(value);
}

append_result MemoryStorage::append(const dariadb::Meas::PMeas begin, const size_t size){
    return _Impl->append(begin,size);
}

Reader_ptr MemoryStorage::readInterval(const dariadb::IdArray &ids, dariadb::Flag flag, dariadb::Time from, dariadb::Time to){
    return _Impl->readInterval(ids,flag,from,to);
}

Reader_ptr  MemoryStorage::readInTimePoint(const dariadb::IdArray &ids, dariadb::Flag flag, dariadb::Time time_point){
    return _Impl->readInTimePoint(ids,flag,time_point);
}

size_t  MemoryStorage::size()const {
    return _Impl->size();
}

size_t  MemoryStorage::chunks_size()const {
    return _Impl->chunks_size();
}


void MemoryStorage::subscribe(const IdArray&ids,const Flag& flag, const ReaderClb_ptr &clbk) {
	return _Impl->subscribe(ids, flag, clbk);
}

Reader_ptr MemoryStorage::currentValue(const IdArray&ids, const Flag& flag) {
	return  _Impl->currentValue(ids, flag);
}