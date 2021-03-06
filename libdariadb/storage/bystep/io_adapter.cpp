#include <libdariadb/flags.h>
#include <libdariadb/storage/bystep/io_adapter.h>
#include <libdariadb/utils/async/thread_manager.h>
#include <libdariadb/utils/logger.h>

#include <condition_variable>
#include <cstring>
#include <ctime>
#include <extern/libsqlite3/sqlite3.h>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
using namespace dariadb;
using namespace dariadb::storage;
using namespace dariadb::utils;
using namespace dariadb::utils::async;

const char *BYSTEP_CREATE_SQL = "PRAGMA page_size = 4096;"
                                "PRAGMA journal_mode =WAL;"
                                "CREATE TABLE IF NOT EXISTS Chunks("
                                "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                                "number INTEGER,"  // chunk::id and period number.
                                "meas_id INTEGER," // measurement id
                                "min_time INTEGER,"
                                "max_time INTEGER,"
                                "chunk_header blob,"
                                "chunk_buffer blob);";

class IOAdapter::Private {
  // TODO move to settings.
  const size_t MAX_QUEUE_SIZE = 1024;

public:
  Private(const std::string &fname) {
    _chunks_pos = 0;
    _chunks_pos_drop = 0;
    _chunks_list = new ChunkMinMax[MAX_QUEUE_SIZE];
    _chunks_list_drop = new ChunkMinMax[MAX_QUEUE_SIZE];
    _db = nullptr;
    logger("engine: io_adapter - open ", fname);
    int rc = sqlite3_open(fname.c_str(), &_db);
    if (rc) {
      auto err_msg = sqlite3_errmsg(_db);
      THROW_EXCEPTION("Can't open database: ", err_msg);
    }
    init_tables();
    _stop_flag = false;
    _is_stoped = false;
    _write_thread = std::thread(std::bind(&IOAdapter::Private::write_thread_func, this));
  }
  ~Private() { stop(); }

  void stop() {
    if (_db != nullptr) {
      flush();
      _stop_flag = true;
      while (!_is_stoped) {
        _cond_var.notify_all();
      }
      _write_thread.join();
      sqlite3_close(_db);
      _db = nullptr;
      delete[] _chunks_list;
      delete[] _chunks_list_drop;
      logger("engine: io_adapter - stoped.");
    }
  }
  struct ChunkMinMax {
    Chunk_Ptr ch;
    Time min;
    Time max;
  };

  void append(const Chunk_Ptr &ch, Time min, Time max) {
    ChunkMinMax cmm;
    cmm.ch = ch;
    cmm.min = min;
    cmm.max = max;
    while (true) {
      _chunks_list_locker.lock();
      if (_chunks_pos < MAX_QUEUE_SIZE) {
        _chunks_list[_chunks_pos] = cmm;
        _chunks_pos++;
        _chunks_list_locker.unlock();
        _cond_var.notify_all();
        break;
      } else {
        _chunks_list_locker.unlock();
        _cond_var.notify_all();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
  }

  void _append(const Chunk_Ptr &ch, Time min, Time max) {
    const std::string sql_query =
        "INSERT INTO Chunks(number, meas_id,min_time,max_time,chunk_header, "
        "chunk_buffer) values (?,?,?,?,?,?);";
    /*
    logger("engine: io_adapter - append chunk #", ch->header->id, " id:",
            ch->header->meas_id);*/
    sqlite3_stmt *pStmt;
    int rc;
    do {
      rc = sqlite3_prepare(_db, sql_query.c_str(), -1, &pStmt, 0);
      if (rc != SQLITE_OK) {
        auto err_msg = std::string(sqlite3_errmsg(_db));
        this->stop();
        THROW_EXCEPTION("engine: IOAdapter - ", err_msg);
      }

      auto skip_count = Chunk::compact(ch->header);
      // update checksum;
      Chunk::updateChecksum(*ch->header, ch->_buffer_t + skip_count);

      sqlite3_bind_int64(pStmt, 1, ch->header->id);
      sqlite3_bind_int64(pStmt, 2, ch->header->meas_id);
      sqlite3_bind_int64(pStmt, 3, min);
      sqlite3_bind_int64(pStmt, 4, max);
      sqlite3_bind_blob(pStmt, 5, ch->header, sizeof(ChunkHeader), SQLITE_STATIC);
      sqlite3_bind_blob(pStmt, 6, ch->_buffer_t + skip_count, ch->header->size,
                        SQLITE_STATIC);
      rc = sqlite3_step(pStmt);
      ENSURE(rc != SQLITE_ROW);
      rc = sqlite3_finalize(pStmt);
    } while (rc == SQLITE_SCHEMA);
  }

  ChunksList readInterval(uint64_t period_from, uint64_t period_to, Id meas_id) {
    lock();
    ChunksList result;
    const std::string sql_query = "SELECT chunk_header, chunk_buffer FROM Chunks WHERE "
                                  "meas_id=? AND number>=? AND number<=? ORDER BY number";
    sqlite3_stmt *pStmt;
    int rc;

    do {
      rc = sqlite3_prepare(_db, sql_query.c_str(), -1, &pStmt, 0);
      if (rc != SQLITE_OK) {
        auto err_msg = std::string(sqlite3_errmsg(_db));
        this->stop();
        THROW_EXCEPTION("engine: IOAdapter - ", err_msg);
      }
      sqlite3_bind_int64(pStmt, 1, meas_id);
      sqlite3_bind_int64(pStmt, 2, period_from);
      sqlite3_bind_int64(pStmt, 3, period_to);

      while (1) {
        rc = sqlite3_step(pStmt);
        if (rc == SQLITE_ROW) {
          using utils::inInterval;
          auto headerSize = sqlite3_column_bytes(pStmt, 0);
          ENSURE(headerSize == sizeof(ChunkHeader));
          auto header_blob = (ChunkHeader *)sqlite3_column_blob(pStmt, 0);
          ChunkHeader *chdr = new ChunkHeader;
          memcpy(chdr, header_blob, headerSize);

          auto buffSize = sqlite3_column_bytes(pStmt, 1);
          ENSURE(size_t(buffSize) == chdr->size);
          uint8_t *buffer = new uint8_t[buffSize];
          memcpy(buffer, sqlite3_column_blob(pStmt, 1), buffSize);

          Chunk_Ptr cptr= Chunk::open(chdr, buffer);
          cptr->is_owner = true;
          if (!cptr->checkChecksum()) {
            logger_fatal("engine: io_adapter -  bad checksum of chunk #",
                         cptr->header->id, " for measurement id:", cptr->header->meas_id);
            continue;
          }
          result.push_back(cptr);
        } else {
          break;
        }
      }
      rc = sqlite3_finalize(pStmt);
    } while (rc == SQLITE_SCHEMA);

    // TODO move to method
    for (size_t i = 0; i < _chunks_pos; ++i) {
      if (_chunks_list[i].ch->header->meas_id == meas_id) {
        if (utils::inInterval(period_from, period_to, _chunks_list[i].ch->header->id)) {
          result.push_back(_chunks_list[i].ch);
        }
      }
    }
    unlock();
    std::vector<Chunk_Ptr> ch_vec{result.begin(), result.end()};
    std::sort(ch_vec.begin(), ch_vec.end(), [](Chunk_Ptr ch1, Chunk_Ptr ch2) {
      return ch1->header->id < ch2->header->id;
    });

    return ChunksList(ch_vec.begin(), ch_vec.end());
  }

  Chunk_Ptr readTimePoint(uint64_t period, Id meas_id) {
    lock();
    Chunk_Ptr result;

    const std::string sql_query = "SELECT chunk_header, chunk_buffer FROM Chunks WHERE "
                                  "meas_id=? AND number==?";
    sqlite3_stmt *pStmt;
    int rc;

    do {
      rc = sqlite3_prepare(_db, sql_query.c_str(), -1, &pStmt, 0);
      if (rc != SQLITE_OK) {
        auto err_msg = std::string(sqlite3_errmsg(_db));
        this->stop();
        THROW_EXCEPTION("engine: IOAdapter - ", err_msg);
      }

      sqlite3_bind_int64(pStmt, 1, meas_id);
      sqlite3_bind_int64(pStmt, 2, period);

      while (1) {
        rc = sqlite3_step(pStmt);
        if (rc == SQLITE_ROW) {
          using utils::inInterval;
          auto headerSize = sqlite3_column_bytes(pStmt, 0);
          ENSURE(headerSize == sizeof(ChunkHeader));
          auto header_blob = (ChunkHeader *)sqlite3_column_blob(pStmt, 0);

          ChunkHeader *chdr = new ChunkHeader;
          memcpy(chdr, header_blob, headerSize);

          auto buffSize = sqlite3_column_bytes(pStmt, 1);
          ENSURE(size_t(buffSize) == chdr->size);
          uint8_t *buffer = new uint8_t[buffSize];
          memcpy(buffer, sqlite3_column_blob(pStmt, 1), buffSize);

          Chunk_Ptr cptr= Chunk::open(chdr, buffer);
          cptr->is_owner = true;
          result = cptr;
        } else {
          break;
        }
      }
      rc = sqlite3_finalize(pStmt);
    } while (rc == SQLITE_SCHEMA);

    if (result == nullptr) {
      for (size_t i = 0; i < _chunks_pos; ++i) {
        if (_chunks_list[i].ch->header->id == period &&
            _chunks_list[i].ch->header->meas_id == meas_id) {
          result = _chunks_list[i].ch;
          break;
        }
      }
    }
    unlock();
    return result;
  }

  Id2Meas currentValue() {
    lock();
    Id2Meas result;
    const std::string sql_query = "SELECT a.chunk_header, a.chunk_buffer FROM "
                                  "Chunks a INNER JOIN(select id, number, MAX(number) "
                                  "from Chunks GROUP BY id) b ON a.id = b.id AND "
                                  "a.number = b.number";
    sqlite3_stmt *pStmt;
    int rc;

    do {
      rc = sqlite3_prepare(_db, sql_query.c_str(), -1, &pStmt, 0);
      if (rc != SQLITE_OK) {
        auto err_msg = std::string(sqlite3_errmsg(_db));
        this->stop();
        THROW_EXCEPTION("engine: IOAdapter - ", err_msg);
      }

      while (1) {
        rc = sqlite3_step(pStmt);
        if (rc == SQLITE_ROW) {
          using utils::inInterval;
          ChunkHeader *chdr = (ChunkHeader *)sqlite3_column_blob(pStmt, 0);
#ifdef DEBUG
          auto headerSize = sqlite3_column_bytes(pStmt, 0);
          ENSURE(headerSize == sizeof(ChunkHeader));
#endif // DEBUG

          uint8_t *buffer = (uint8_t *)sqlite3_column_blob(pStmt, 1);
          Chunk_Ptr cptr= Chunk::open(chdr, buffer);
          currentValueFromChunk(result, cptr);
        } else {
          break;
        }
      }
      rc = sqlite3_finalize(pStmt);
    } while (rc == SQLITE_SCHEMA);

    for (size_t i = 0; i < _chunks_pos; ++i) {
      currentValueFromChunk(result, _chunks_list[i].ch);
    }
    unlock();
    return result;
  }

  void currentValueFromChunk(Id2Meas &result, Chunk_Ptr &c) {
    auto reader = c->getReader();
    while (!reader->is_end()) {
      auto v = reader->readNext();
      if (v.flag != Flags::_NO_DATA) {
        if (result[v.id].time < v.time) {
          result[v.id] = v;
        }
      }
    }
  }

  void replace(const Chunk_Ptr &ch, Time min, Time max) {
    this->flush();
    lock();
    logger("engine: io_adapter - replace chunk #", ch->header->id, " id:",
           ch->header->meas_id);
    const std::string sql_query = "UPDATE Chunks SET min_time=?, max_time=?, "
                                  "chunk_header=?, chunk_buffer=? where number=? AND "
                                  "meas_id=?";
    sqlite3_stmt *pStmt;
    int rc;
    do {
      rc = sqlite3_prepare(_db, sql_query.c_str(), -1, &pStmt, 0);
      if (rc != SQLITE_OK) {
        auto err_msg = std::string(sqlite3_errmsg(_db));
        this->stop();
        THROW_EXCEPTION("engine: IOAdapter - ", err_msg);
      }

      auto skip_count = Chunk::compact(ch->header);
      // update checksum;
      Chunk::updateChecksum(*ch->header, ch->_buffer_t + skip_count);

      sqlite3_bind_int64(pStmt, 1, min);
      sqlite3_bind_int64(pStmt, 2, max);
      sqlite3_bind_blob(pStmt, 3, ch->header, sizeof(ChunkHeader), SQLITE_STATIC);
      sqlite3_bind_blob(pStmt, 4, ch->_buffer_t + skip_count, ch->header->size,
                        SQLITE_STATIC);
      sqlite3_bind_int64(pStmt, 5, ch->header->id);
      sqlite3_bind_int64(pStmt, 6, ch->header->meas_id);
      rc = sqlite3_step(pStmt);
      ENSURE(rc != SQLITE_ROW);
      rc = sqlite3_finalize(pStmt);
    } while (rc == SQLITE_SCHEMA);
    unlock();
  }

  // min and max
  std::tuple<Time, Time> minMax() {
    lock();

    logger("engine: io_adapter - minMax");
    const std::string sql_query =
        "SELECT min(min_time), max(max_time), count(max_time) FROM Chunks";
    sqlite3_stmt *pStmt;
    int rc;
    Time min = MAX_TIME;
    Time max = MIN_TIME;
    do {
      rc = sqlite3_prepare(_db, sql_query.c_str(), -1, &pStmt, 0);
      if (rc != SQLITE_OK) {
        auto err_msg = std::string(sqlite3_errmsg(_db));
        this->stop();
        THROW_EXCEPTION("engine: IOAdapter - ", err_msg);
      }

      rc = sqlite3_step(pStmt);
      if (rc == SQLITE_ROW) {
        auto count = sqlite3_column_int64(pStmt, 2);
        if (count > 0) {
          min = sqlite3_column_int64(pStmt, 0);
          max = sqlite3_column_int64(pStmt, 1);
        }
      }

      rc = sqlite3_finalize(pStmt);
    } while (rc == SQLITE_SCHEMA);

    for (size_t i = 0; i < _chunks_pos; ++i) {
      min = std::min(min, _chunks_list[i].min);
      max = std::min(max, _chunks_list[i].max);
    }
    unlock();
    return std::tie(min, max);
  }

  Time minTime() { return std::get<0>(minMax()); }

  Time maxTime() { return std::get<1>(minMax()); }

  bool minMaxTime(Id id, Time *minResult, Time *maxResult) {

    lock();
    logger("engine: io_adapter - minMaxTime #", id);
    const std::string sql_query = "SELECT min(min_time), max(max_time), count(max_time) "
                                  "FROM Chunks WHERE meas_id=?";
    sqlite3_stmt *pStmt;
    int rc;
    *minResult = MAX_TIME;
    *maxResult = MIN_TIME;
    bool result = false;
    do {
      rc = sqlite3_prepare(_db, sql_query.c_str(), -1, &pStmt, 0);
      if (rc != SQLITE_OK) {
        auto err_msg = std::string(sqlite3_errmsg(_db));
        this->stop();
        THROW_EXCEPTION("engine: IOAdapter - ", err_msg);
      }
      sqlite3_bind_int64(pStmt, 1, id);
      rc = sqlite3_step(pStmt);
      if (rc == SQLITE_ROW) {
        auto count = sqlite3_column_int64(pStmt, 2);
        if (count > 0) {
          *minResult = sqlite3_column_int64(pStmt, 0);
          *maxResult = sqlite3_column_int64(pStmt, 1);
          result = true;
        }
      }

      rc = sqlite3_finalize(pStmt);
    } while (rc == SQLITE_SCHEMA);

    for (size_t i = 0; i < _chunks_pos; ++i) {
      if (_chunks_list[i].ch->header->meas_id == id) {
        result = true;
        *minResult = std::min(*minResult, _chunks_list[i].min);
        *maxResult = std::min(*maxResult, _chunks_list[i].max);
      }
    }
    unlock();
    return result;
  }

  void init_tables() {
    logger_info("engine: io_adapter - init_tables");
    char *err = 0;
    if (sqlite3_exec(_db, BYSTEP_CREATE_SQL, 0, 0, &err)) {
      auto err_msg = sqlite3_errmsg(_db);
      sqlite3_free(err);
      this->stop();
      THROW_EXCEPTION("Can't init database: ", err_msg);
    }
  }

  void lock() { std::lock(_dropper_locker, _chunks_list_locker); }

  void unlock() {
    _dropper_locker.unlock();
    _chunks_list_locker.unlock();
  }

  void write_thread_func() {
    while (!_stop_flag) {
      std::unique_lock<std::mutex> lock(_chunks_list_locker);
      _cond_var.wait(lock);

      if (_stop_flag && _chunks_pos == 0) {
        break;
      }

      bool not_full = false;
      while (!_dropper_locker.try_lock()) {
        if (_chunks_pos < MAX_QUEUE_SIZE) {
          not_full = true;
          break;
        }
        std::this_thread::yield();
      }
      if (not_full) {
        // need to not lock queue if dropper locked and queue not filled.
        logger("engine: io_adapter - can't lock dropper and queue not full.");
        continue;
      }
      if (_chunks_pos != 0) {
        std::swap(_chunks_list, _chunks_list_drop);
        _chunks_pos_drop = _chunks_pos;
        _chunks_pos = 0;
        lock.unlock();

        auto start_time = clock();
        while (true) { // try drop disk, while not success.
          logger("engine: io_adapter - dropping start. ", _chunks_pos_drop, " chunks.");
          try {
            sqlite3_exec(_db, "BEGIN TRANSACTION;", NULL, NULL, NULL);

            for (size_t i = 0; i < _chunks_pos_drop; ++i) {
              auto c = _chunks_list_drop[i];
              this->_append(c.ch, c.min, c.max);
            }
            sqlite3_exec(_db, "END TRANSACTION;", NULL, NULL, NULL);
            break;
          } catch (const std::exception &e) {
            logger_fatal("engine: io_adapter - exception on write to disk - ", e.what());
            sqlite3_exec(_db, "ROLLBACK TRANSACTION;", NULL, NULL, NULL);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
          }
        }
        for (size_t i = 0; i < _chunks_pos_drop; ++i) {
          _chunks_list_drop[i].ch = nullptr;
        }
        _chunks_pos_drop = 0;
        auto end = clock();
        auto elapsed = double(end - start_time) / CLOCKS_PER_SEC;

        logger("engine: io_adapter - dropping end. elapsed ", elapsed, "s");
      } else {
        lock.unlock();
      }
      _dropper_locker.unlock();
    }
    logger_info("engine: io_adapter - write thread is stoped.");
    _is_stoped = true;
  }

  void flush() {
    logger_info("engine: io_adapter - flush start.");
    while (true) { // TODO make more smarter.
      _chunks_list_locker.lock();
      auto is_empty = (_chunks_pos + _chunks_pos_drop) == 0;
      _chunks_list_locker.unlock();
      if (is_empty) {
        break;
      }
      _cond_var.notify_all();
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    logger_info("engine: io_adapter - flush end.");
  }

  bystep::Description description() {
    bystep::Description result;
    _chunks_list_locker.lock();
    result.in_queue = _chunks_pos + _chunks_pos_drop;
    _chunks_list_locker.unlock();
    return result;
  }

  void eraseOld(uint64_t period_from, uint64_t period_to, Id id) {
    lock();
    logger("engine: io_adapter - erase id:", id);
    const std::string sql_query =
        " DELETE FROM Chunks WHERE meas_id=? AND number>=? AND number<?;";
    sqlite3_stmt *pStmt;
    int rc;
    do {
      rc = sqlite3_prepare(_db, sql_query.c_str(), -1, &pStmt, 0);
      if (rc != SQLITE_OK) {
        auto err_msg = std::string(sqlite3_errmsg(_db));
        this->stop();
        THROW_EXCEPTION("engine: IOAdapter - ", err_msg);
      }

      sqlite3_bind_int64(pStmt, 1, id);
      sqlite3_bind_int64(pStmt, 2, period_from);
      sqlite3_bind_int64(pStmt, 3, period_to);

      rc = sqlite3_step(pStmt);
      ENSURE(rc != SQLITE_ROW);
      rc = sqlite3_finalize(pStmt);
    } while (rc == SQLITE_SCHEMA);
    unlock();
  }

  sqlite3 *_db;
  bool _is_stoped;
  bool _stop_flag;
  // in queue
  ChunkMinMax *_chunks_list;
  size_t _chunks_pos;
  // queue for dropping to disk.
  ChunkMinMax *_chunks_list_drop;
  size_t _chunks_pos_drop;
  std::mutex _chunks_list_locker;
  std::condition_variable _cond_var;
  std::mutex _dropper_locker;
  std::thread _write_thread;
};

IOAdapter::IOAdapter(const std::string &fname) : _impl(new IOAdapter::Private(fname)) {}

IOAdapter::~IOAdapter() {
  _impl = nullptr;
}

void IOAdapter::stop() {
  _impl->stop();
}

void IOAdapter::append(const Chunk_Ptr &ch, Time min, Time max) {
  _impl->append(ch, min, max);
}

ChunksList IOAdapter::readInterval(uint64_t period_from, uint64_t period_to, Id meas_id) {
  return _impl->readInterval(period_from, period_to, meas_id);
}

Chunk_Ptr IOAdapter::readTimePoint(uint64_t period, Id meas_id) {
  return _impl->readTimePoint(period, meas_id);
}

Id2Meas IOAdapter::currentValue() {
  return _impl->currentValue();
}

void IOAdapter::replace(const Chunk_Ptr &ch, Time min, Time max) {
  return _impl->replace(ch, min, max);
}

Time IOAdapter::minTime() {
  return _impl->minTime();
}

Time IOAdapter::maxTime() {
  return _impl->maxTime();
}

bool IOAdapter::minMaxTime(Id id, Time *minResult, Time *maxResult) {
  return _impl->minMaxTime(id, minResult, maxResult);
}

void IOAdapter::flush() {
  _impl->flush();
}

bystep::Description IOAdapter::description() {
  return _impl->description();
}

void IOAdapter::eraseOld(uint64_t period_from, uint64_t period_to, Id id) {
  _impl->eraseOld(period_from, period_to, id);
}
