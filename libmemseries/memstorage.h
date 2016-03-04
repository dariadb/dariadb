#pragma once

#include "compression.h"
#include "storage.h"
#include <vector>
#include <map>
#include <memory>
#include <set>

namespace memseries{
    namespace storage {

        class MemoryStorage:public AbstractStorage
        {
            struct Block
            {
                uint8_t *begin;
                uint8_t *end;
                Block(size_t size);
                ~Block();
            };

            struct MeasChunk
            {
                Block times;
                Block flags;
                Block values;
                compression::CopmressedWriter c_writer;
                size_t count;
                Meas first,last;
                MeasChunk(size_t size, Meas first_m);
                ~MeasChunk();
                bool append(const Meas&m);
                bool is_full()const { return c_writer.is_full(); }
            };
            typedef std::shared_ptr<MeasChunk> Chunk_Ptr;
            typedef std::vector<Chunk_Ptr> ChuncksVector;
            typedef std::map<Id, ChuncksVector> ChunkMap;
        public:
            class InnerReader:public Reader{
            public:
                struct ReadChunk
                {
                    size_t    count;
                    Chunk_Ptr chunk;
                };
                InnerReader(memseries::Flag flag, memseries::Time from, memseries::Time to);
                void add(Chunk_Ptr c, size_t count);
                bool isEnd() const override;
                void readNext(Meas::MeasList*output)  override;
                void readTimePoint(Meas::MeasList*output);
                bool is_time_point_reader;
            protected:
                bool check_meas(Meas&m);
                typedef std::vector<ReadChunk> ReadChuncksVector;
                typedef std::map<Id, ReadChuncksVector> ReadChunkMap;
            protected:
                ReadChunkMap _chunks;
                memseries::Flag _flag;
                memseries::Time _from;
                memseries::Time _to;
                ReadChunk _next;
                ReadChuncksVector _cur_vector;
                size_t _cur_vector_pos;
                bool _tp_readed;
            };
        public:
            MemoryStorage(size_t size);
            virtual ~MemoryStorage();

            using AbstractStorage::append;
            using AbstractStorage::readInterval;
            using AbstractStorage::readInTimePoint;

            Time minTime();
            Time maxTime();
            append_result append(const Meas& value)override;
            append_result append(const Meas::PMeas begin, const size_t size)override;
            Reader_ptr readInterval(const IdArray &ids, Flag flag, Time from, Time to)override;
            Reader_ptr readInTimePoint(const IdArray &ids, Flag flag, Time time_point)override;

            size_t size()const { return _size; }
            size_t chinks_size()const { return _chuncks.size(); }
        protected:
            size_t _size;

            ChunkMap _chuncks;
            Time _min_time,_max_time;
        };
    }
}
