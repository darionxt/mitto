#include "mitto/indexed_db_messages.h"

#include <bit>
#include <cmath>
#include <utility>

#include "mitto/message_coding.h"
#include "mitto/page_messages.h"

namespace mitto {
namespace {

constexpr uint8_t kMaxIndexedDbException = 33;

bool IsException(uint8_t value) {
  switch (static_cast<IndexedDbException>(value)) {
    case IndexedDbException::kNoError:
    case IndexedDbException::kIgnorableAbortError:
    case IndexedDbException::kAbortError:
    case IndexedDbException::kQuotaError:
    case IndexedDbException::kTimeoutError:
    case IndexedDbException::kUnknownError:
    case IndexedDbException::kConstraintError:
    case IndexedDbException::kDataError:
    case IndexedDbException::kVersionError:
      return value <= kMaxIndexedDbException;
  }
  return false;
}

// A key: its type, and for an array its members, for a binary or a string
// its bytes or code units, and for a date or a number its double. A None
// travels for a key that is not given, and as a hole in an index key.
void WriteKey(MessageWriter& writer, const IDBKey& key) {
  writer.Byte(static_cast<uint8_t>(key.type()));
  switch (key.type()) {
    case IDBKeyType::kArray:
      writer.U32(static_cast<uint32_t>(key.array().size()));
      for (const IDBKey& subkey : key.array()) {
        WriteKey(writer, subkey);
      }
      return;
    case IDBKeyType::kBinary:
      writer.Sized(key.binary());
      return;
    case IDBKeyType::kString:
      writer.Sized16(key.string());
      return;
    case IDBKeyType::kDate:
    case IDBKeyType::kNumber: {
      writer.U64(std::bit_cast<uint64_t>(key.number()));
      return;
    }
    case IDBKeyType::kInvalid:
    case IDBKeyType::kNone:
    case IDBKeyType::kMin:
      return;
  }
}

bool ReadKey(MessageReader& reader, IDBKey& key, size_t depth = 0) {
  uint8_t type = 0;
  if (!reader.Byte(type) || type > static_cast<uint8_t>(IDBKeyType::kMin) ||
      depth > IDBKey::kMaximumDepth) {
    return false;
  }
  switch (static_cast<IDBKeyType>(type)) {
    case IDBKeyType::kArray: {
      uint32_t count = 0;
      if (!reader.Count(count, 1) || count > IDBKey::kMaximumArraySize) {
        return false;
      }
      IDBKey::KeyArray array(count);
      for (IDBKey& subkey : array) {
        if (!ReadKey(reader, subkey, depth + 1)) {
          return false;
        }
      }
      key = IDBKey::Array(std::move(array));
      return true;
    }
    case IDBKeyType::kBinary: {
      std::string binary;
      if (!reader.Sized(binary)) {
        return false;
      }
      key = IDBKey::Binary(std::move(binary));
      return true;
    }
    case IDBKeyType::kString: {
      std::u16string string;
      if (!reader.Sized16(string)) {
        return false;
      }
      key = IDBKey::String(std::move(string));
      return true;
    }
    case IDBKeyType::kDate:
    case IDBKeyType::kNumber: {
      uint64_t bits = 0;
      if (!reader.U64(bits)) {
        return false;
      }
      const double number = std::bit_cast<double>(bits);
      if (std::isnan(number)) {
        return false;
      }
      key = static_cast<IDBKeyType>(type) == IDBKeyType::kDate
                ? IDBKey::Date(number)
                : IDBKey::Number(number);
      return true;
    }
    case IDBKeyType::kInvalid:
    case IDBKeyType::kNone:
    case IDBKeyType::kMin:
      key = IDBKey(static_cast<IDBKeyType>(type));
      return true;
  }
  return false;
}

// A key that is valid, or None for one not given.
bool ReadValidOrNoneKey(MessageReader& reader, IDBKey& key) {
  return ReadKey(reader, key) &&
         (key.IsValid() || key.type() == IDBKeyType::kNone);
}

void WriteKeyPath(MessageWriter& writer, const IDBKeyPath& key_path) {
  writer.Byte(static_cast<uint8_t>(key_path.type));
  switch (key_path.type) {
    case IDBKeyPathType::kNull:
      return;
    case IDBKeyPathType::kString:
      writer.Sized16(key_path.string);
      return;
    case IDBKeyPathType::kArray:
      writer.U32(static_cast<uint32_t>(key_path.array.size()));
      for (const std::u16string& path : key_path.array) {
        writer.Sized16(path);
      }
      return;
  }
}

bool ReadKeyPath(MessageReader& reader, IDBKeyPath& key_path) {
  uint8_t type = 0;
  if (!reader.Byte(type) ||
      type > static_cast<uint8_t>(IDBKeyPathType::kArray)) {
    return false;
  }
  key_path = IDBKeyPath();
  key_path.type = static_cast<IDBKeyPathType>(type);
  switch (key_path.type) {
    case IDBKeyPathType::kNull:
      return true;
    case IDBKeyPathType::kString:
      return reader.Sized16(key_path.string);
    case IDBKeyPathType::kArray: {
      uint32_t count = 0;
      if (!reader.Count(count, 4)) {
        return false;
      }
      key_path.array.resize(count);
      for (std::u16string& path : key_path.array) {
        if (!reader.Sized16(path)) {
          return false;
        }
      }
      return true;
    }
  }
  return false;
}

void WriteRange(MessageWriter& writer, const IndexedDbKeyRange& range) {
  WriteKey(writer, range.lower);
  WriteKey(writer, range.upper);
  writer.Bool(range.lower_open);
  writer.Bool(range.upper_open);
}

bool ReadRange(MessageReader& reader, IndexedDbKeyRange& range) {
  return ReadValidOrNoneKey(reader, range.lower) &&
         ReadValidOrNoneKey(reader, range.upper) &&
         reader.Bool(range.lower_open) && reader.Bool(range.upper_open);
}

void WriteIndexMetadata(MessageWriter& writer,
                        const IndexedDbIndexMetadata& index) {
  writer.I64(index.id);
  writer.Sized16(index.name);
  WriteKeyPath(writer, index.key_path);
  writer.Bool(index.unique);
  writer.Bool(index.multi_entry);
}

bool ReadIndexMetadata(MessageReader& reader, IndexedDbIndexMetadata& index) {
  return reader.I64(index.id) && reader.Sized16(index.name) &&
         ReadKeyPath(reader, index.key_path) && reader.Bool(index.unique) &&
         reader.Bool(index.multi_entry);
}

void WriteMetadata(MessageWriter& writer,
                   const IndexedDbDatabaseMetadata& metadata) {
  writer.Sized16(metadata.name);
  writer.I64(metadata.version);
  writer.I64(metadata.max_object_store_id);
  writer.U32(static_cast<uint32_t>(metadata.object_stores.size()));
  for (const auto& [id, store] : metadata.object_stores) {
    writer.I64(store.id);
    writer.Sized16(store.name);
    WriteKeyPath(writer, store.key_path);
    writer.Bool(store.auto_increment);
    writer.I64(store.max_index_id);
    writer.U32(static_cast<uint32_t>(store.indexes.size()));
    for (const auto& [index_id, index] : store.indexes) {
      WriteIndexMetadata(writer, index);
    }
  }
}

bool ReadMetadata(MessageReader& reader, IndexedDbDatabaseMetadata& metadata) {
  uint32_t stores = 0;
  if (!reader.Sized16(metadata.name) || !reader.I64(metadata.version) ||
      !reader.I64(metadata.max_object_store_id) || !reader.Count(stores, 22)) {
    return false;
  }
  for (uint32_t i = 0; i < stores; ++i) {
    IndexedDbObjectStoreMetadata store;
    uint32_t indexes = 0;
    if (!reader.I64(store.id) || !reader.Sized16(store.name) ||
        !ReadKeyPath(reader, store.key_path) ||
        !reader.Bool(store.auto_increment) || !reader.I64(store.max_index_id) ||
        !reader.Count(indexes, 15)) {
      return false;
    }
    for (uint32_t j = 0; j < indexes; ++j) {
      IndexedDbIndexMetadata index;
      if (!ReadIndexMetadata(reader, index)) {
        return false;
      }
      const int64_t index_id = index.id;
      store.indexes.emplace(index_id, std::move(index));
    }
    const int64_t store_id = store.id;
    metadata.object_stores.emplace(store_id, std::move(store));
  }
  return true;
}

void WriteReturnValue(MessageWriter& writer,
                      const IndexedDbReturnValue& value) {
  writer.Sized(value.bits);
  WriteKey(writer, value.primary_key);
  WriteKeyPath(writer, value.key_path);
}

bool ReadReturnValue(MessageReader& reader, IndexedDbReturnValue& value) {
  return reader.Sized(value.bits) &&
         ReadValidOrNoneKey(reader, value.primary_key) &&
         ReadKeyPath(reader, value.key_path);
}

void WriteIndexKeys(MessageWriter& writer,
                    const std::vector<IndexedDbIndexKeys>& index_keys) {
  writer.U32(static_cast<uint32_t>(index_keys.size()));
  for (const IndexedDbIndexKeys& keys : index_keys) {
    writer.I64(keys.index_id);
    writer.U32(static_cast<uint32_t>(keys.keys.size()));
    for (const IDBKey& key : keys.keys) {
      WriteKey(writer, key);
    }
  }
}

// An index key is valid, or an array with holes for a primary key that a
// key generator gives.
bool ReadIndexKeys(MessageReader& reader,
                   std::vector<IndexedDbIndexKeys>& index_keys) {
  uint32_t count = 0;
  if (!reader.Count(count, 12)) {
    return false;
  }
  index_keys.resize(count);
  for (IndexedDbIndexKeys& keys : index_keys) {
    uint32_t key_count = 0;
    if (!reader.I64(keys.index_id) || !reader.Count(key_count, 1)) {
      return false;
    }
    keys.keys.resize(key_count);
    for (IDBKey& key : keys.keys) {
      if (!ReadKey(reader, key)) {
        return false;
      }
      if (!key.IsValid() &&
          !(key.HasHoles() && key.FillHoles(IDBKey::Number(0)).IsValid())) {
        return false;
      }
    }
  }
  return true;
}

void WriteRecord(MessageWriter& writer, const IndexedDbRecord& record) {
  writer.Byte(static_cast<uint8_t>((record.primary_key ? 1 : 0) |
                                   (record.value ? 2 : 0) |
                                   (record.index_key ? 4 : 0)));
  if (record.primary_key) {
    WriteKey(writer, *record.primary_key);
  }
  if (record.value) {
    WriteReturnValue(writer, *record.value);
  }
  if (record.index_key) {
    WriteKey(writer, *record.index_key);
  }
}

bool ReadRecord(MessageReader& reader, IndexedDbRecord& record) {
  uint8_t flags = 0;
  if (!reader.Byte(flags) || flags > 7) {
    return false;
  }
  if (flags & 1) {
    record.primary_key.emplace();
    if (!ReadKey(reader, *record.primary_key) ||
        !record.primary_key->IsValid()) {
      return false;
    }
  }
  if (flags & 2) {
    record.value.emplace();
    if (!ReadReturnValue(reader, *record.value)) {
      return false;
    }
  }
  if (flags & 4) {
    record.index_key.emplace();
    if (!ReadKey(reader, *record.index_key) || !record.index_key->IsValid()) {
      return false;
    }
  }
  return true;
}

bool IsMode(uint8_t value) {
  return value <=
         static_cast<uint8_t>(IndexedDbTransactionMode::kVersionChange);
}

bool IsDirection(uint8_t value) {
  return value <=
         static_cast<uint8_t>(IndexedDbCursorDirection::kPrevNoDuplicate);
}

}  // namespace

IndexedDbIndexMetadata::IndexedDbIndexMetadata() = default;
IndexedDbIndexMetadata::IndexedDbIndexMetadata(const IndexedDbIndexMetadata&) =
    default;
IndexedDbIndexMetadata::IndexedDbIndexMetadata(IndexedDbIndexMetadata&&) =
    default;
IndexedDbIndexMetadata& IndexedDbIndexMetadata::operator=(
    const IndexedDbIndexMetadata&) = default;
IndexedDbIndexMetadata& IndexedDbIndexMetadata::operator=(
    IndexedDbIndexMetadata&&) = default;
IndexedDbIndexMetadata::~IndexedDbIndexMetadata() = default;

IndexedDbObjectStoreMetadata::IndexedDbObjectStoreMetadata() = default;
IndexedDbObjectStoreMetadata::IndexedDbObjectStoreMetadata(
    const IndexedDbObjectStoreMetadata&) = default;
IndexedDbObjectStoreMetadata::IndexedDbObjectStoreMetadata(
    IndexedDbObjectStoreMetadata&&) = default;
IndexedDbObjectStoreMetadata& IndexedDbObjectStoreMetadata::operator=(
    const IndexedDbObjectStoreMetadata&) = default;
IndexedDbObjectStoreMetadata& IndexedDbObjectStoreMetadata::operator=(
    IndexedDbObjectStoreMetadata&&) = default;
IndexedDbObjectStoreMetadata::~IndexedDbObjectStoreMetadata() = default;

IndexedDbDatabaseMetadata::IndexedDbDatabaseMetadata() = default;
IndexedDbDatabaseMetadata::IndexedDbDatabaseMetadata(
    const IndexedDbDatabaseMetadata&) = default;
IndexedDbDatabaseMetadata::IndexedDbDatabaseMetadata(
    IndexedDbDatabaseMetadata&&) = default;
IndexedDbDatabaseMetadata& IndexedDbDatabaseMetadata::operator=(
    const IndexedDbDatabaseMetadata&) = default;
IndexedDbDatabaseMetadata& IndexedDbDatabaseMetadata::operator=(
    IndexedDbDatabaseMetadata&&) = default;
IndexedDbDatabaseMetadata::~IndexedDbDatabaseMetadata() = default;

IndexedDbIndexKeys::IndexedDbIndexKeys() = default;
IndexedDbIndexKeys::IndexedDbIndexKeys(const IndexedDbIndexKeys&) = default;
IndexedDbIndexKeys::IndexedDbIndexKeys(IndexedDbIndexKeys&&) = default;
IndexedDbIndexKeys& IndexedDbIndexKeys::operator=(const IndexedDbIndexKeys&) =
    default;
IndexedDbIndexKeys& IndexedDbIndexKeys::operator=(IndexedDbIndexKeys&&) =
    default;
IndexedDbIndexKeys::~IndexedDbIndexKeys() = default;

IndexedDbReturnValue::IndexedDbReturnValue() = default;
IndexedDbReturnValue::IndexedDbReturnValue(const IndexedDbReturnValue&) =
    default;
IndexedDbReturnValue::IndexedDbReturnValue(IndexedDbReturnValue&&) = default;
IndexedDbReturnValue& IndexedDbReturnValue::operator=(
    const IndexedDbReturnValue&) = default;
IndexedDbReturnValue& IndexedDbReturnValue::operator=(IndexedDbReturnValue&&) =
    default;
IndexedDbReturnValue::~IndexedDbReturnValue() = default;

IndexedDbRecord::IndexedDbRecord() = default;
IndexedDbRecord::IndexedDbRecord(const IndexedDbRecord&) = default;
IndexedDbRecord::IndexedDbRecord(IndexedDbRecord&&) = default;
IndexedDbRecord& IndexedDbRecord::operator=(const IndexedDbRecord&) = default;
IndexedDbRecord& IndexedDbRecord::operator=(IndexedDbRecord&&) = default;
IndexedDbRecord::~IndexedDbRecord() = default;

PageIndexedDbCall::PageIndexedDbCall() = default;
PageIndexedDbCall::PageIndexedDbCall(PageIndexedDbCall&&) = default;
PageIndexedDbCall& PageIndexedDbCall::operator=(PageIndexedDbCall&&) = default;
PageIndexedDbCall::~PageIndexedDbCall() = default;

PageIndexedDbCallback::PageIndexedDbCallback() = default;
PageIndexedDbCallback::PageIndexedDbCallback(PageIndexedDbCallback&&) = default;
PageIndexedDbCallback& PageIndexedDbCallback::operator=(
    PageIndexedDbCallback&&) = default;
PageIndexedDbCallback::~PageIndexedDbCallback() = default;

std::string EncodeIndexedDbCall(const PageIndexedDbCall& call) {
  MessageWriter writer;
  writer.Byte(static_cast<uint8_t>(call.op));
  switch (call.op) {
    case IndexedDbCallOp::kGetDatabaseInfo:
      writer.U64(call.call);
      break;
    case IndexedDbCallOp::kOpen:
      writer.U64(call.call);
      writer.U64(call.connection);
      writer.U64(call.transaction);
      writer.Sized16(call.name);
      writer.I64(call.version);
      break;
    case IndexedDbCallOp::kDeleteDatabase:
      writer.U64(call.call);
      writer.Sized16(call.name);
      break;
    case IndexedDbCallOp::kCreateTransaction:
      writer.U64(call.connection);
      writer.U64(call.transaction);
      writer.U32(static_cast<uint32_t>(call.scope.size()));
      for (int64_t id : call.scope) {
        writer.I64(id);
      }
      writer.Byte(static_cast<uint8_t>(call.mode));
      writer.Byte(static_cast<uint8_t>(call.durability));
      break;
    case IndexedDbCallOp::kVersionChangeIgnored:
    case IndexedDbCallOp::kDidBecomeInactive:
    case IndexedDbCallOp::kCloseConnection:
      writer.U64(call.connection);
      break;
    case IndexedDbCallOp::kGet:
      writer.U64(call.call);
      writer.U64(call.connection);
      writer.U64(call.transaction);
      writer.I64(call.object_store_id);
      writer.I64(call.index_id);
      WriteRange(writer, call.key_range);
      writer.Bool(call.key_only);
      break;
    case IndexedDbCallOp::kGetAll:
      writer.U64(call.call);
      writer.U64(call.connection);
      writer.U64(call.transaction);
      writer.I64(call.object_store_id);
      writer.I64(call.index_id);
      WriteRange(writer, call.key_range);
      writer.Byte(static_cast<uint8_t>(call.result_type));
      writer.U32(call.count);
      writer.Byte(static_cast<uint8_t>(call.direction));
      break;
    case IndexedDbCallOp::kOpenCursor:
      writer.U64(call.call);
      writer.U64(call.connection);
      writer.U64(call.transaction);
      writer.U64(call.cursor);
      writer.I64(call.object_store_id);
      writer.I64(call.index_id);
      WriteRange(writer, call.key_range);
      writer.Byte(static_cast<uint8_t>(call.direction));
      writer.Bool(call.key_only);
      writer.Byte(static_cast<uint8_t>(call.task_type));
      break;
    case IndexedDbCallOp::kCount:
      writer.U64(call.call);
      writer.U64(call.connection);
      writer.U64(call.transaction);
      writer.I64(call.object_store_id);
      writer.I64(call.index_id);
      WriteRange(writer, call.key_range);
      break;
    case IndexedDbCallOp::kDeleteRange:
      writer.U64(call.call);
      writer.U64(call.connection);
      writer.U64(call.transaction);
      writer.I64(call.object_store_id);
      WriteRange(writer, call.key_range);
      break;
    case IndexedDbCallOp::kGetKeyGeneratorCurrentNumber:
    case IndexedDbCallOp::kClear:
      writer.U64(call.call);
      writer.U64(call.connection);
      writer.U64(call.transaction);
      writer.I64(call.object_store_id);
      break;
    case IndexedDbCallOp::kCreateIndex:
      writer.U64(call.connection);
      writer.U64(call.transaction);
      writer.I64(call.object_store_id);
      WriteIndexMetadata(writer, call.index);
      break;
    case IndexedDbCallOp::kDeleteIndex:
      writer.U64(call.connection);
      writer.U64(call.transaction);
      writer.I64(call.object_store_id);
      writer.I64(call.index_id);
      break;
    case IndexedDbCallOp::kRenameIndex:
      writer.U64(call.connection);
      writer.U64(call.transaction);
      writer.I64(call.object_store_id);
      writer.I64(call.index_id);
      writer.Sized16(call.name);
      break;
    case IndexedDbCallOp::kRenameObjectStore:
      writer.U64(call.connection);
      writer.U64(call.transaction);
      writer.I64(call.object_store_id);
      writer.Sized16(call.name);
      break;
    case IndexedDbCallOp::kAbort:
      writer.U64(call.connection);
      writer.U64(call.transaction);
      break;
    case IndexedDbCallOp::kCreateObjectStore:
      writer.U64(call.transaction);
      writer.I64(call.object_store_id);
      writer.Sized16(call.name);
      WriteKeyPath(writer, call.key_path);
      writer.Bool(call.auto_increment);
      break;
    case IndexedDbCallOp::kDeleteObjectStore:
      writer.U64(call.transaction);
      writer.I64(call.object_store_id);
      break;
    case IndexedDbCallOp::kPut:
      writer.U64(call.call);
      writer.U64(call.transaction);
      writer.I64(call.object_store_id);
      writer.Sized(call.value);
      WriteKey(writer, call.key);
      writer.Byte(static_cast<uint8_t>(call.put_mode));
      WriteIndexKeys(writer, call.index_keys);
      break;
    case IndexedDbCallOp::kSetIndexKeys:
      writer.U64(call.transaction);
      writer.I64(call.object_store_id);
      WriteKey(writer, call.primary_key);
      WriteIndexKeys(writer, call.index_keys);
      break;
    case IndexedDbCallOp::kSetIndexKeysDone:
      writer.U64(call.transaction);
      break;
    case IndexedDbCallOp::kCommit:
      writer.U64(call.transaction);
      writer.I64(call.num_errors_handled);
      break;
    case IndexedDbCallOp::kCursorAdvance:
    case IndexedDbCallOp::kCursorPrefetch:
      writer.U64(call.call);
      writer.U64(call.cursor);
      writer.U32(call.count);
      break;
    case IndexedDbCallOp::kCursorContinue:
      writer.U64(call.call);
      writer.U64(call.cursor);
      WriteKey(writer, call.key);
      WriteKey(writer, call.primary_key);
      break;
    case IndexedDbCallOp::kCursorPrefetchReset:
      writer.U64(call.cursor);
      writer.U32(call.count);
      break;
    case IndexedDbCallOp::kCloseCursor:
      writer.U64(call.cursor);
      break;
  }
  return writer.Take();
}

bool IndexedDbCallFits(const PageIndexedDbCall& call) {
  // Its frame starts with the ID of the document that asks.
  return EncodeIndexedDbCall(call).size() <=
         kMaxPayloadSize - sizeof(uint64_t);
}

base::expected<PageIndexedDbCall, std::string> ReadIndexedDbCall(
    std::string_view payload) {
  MessageReader reader(payload);
  PageIndexedDbCall call;
  uint8_t op = 0;
  if (!reader.Byte(op) || op == 0 ||
      op > static_cast<uint8_t>(IndexedDbCallOp::kMaxValue)) {
    return base::unexpected("an IndexedDB call of an unknown operation");
  }
  call.op = static_cast<IndexedDbCallOp>(op);
  uint8_t byte = 0;
  bool valid = false;
  switch (call.op) {
    case IndexedDbCallOp::kGetDatabaseInfo:
      valid = reader.U64(call.call);
      break;
    case IndexedDbCallOp::kOpen:
      valid = reader.U64(call.call) && reader.U64(call.connection) &&
              reader.U64(call.transaction) && reader.Sized16(call.name) &&
              reader.I64(call.version);
      break;
    case IndexedDbCallOp::kDeleteDatabase:
      valid = reader.U64(call.call) && reader.Sized16(call.name);
      break;
    case IndexedDbCallOp::kCreateTransaction: {
      uint32_t count = 0;
      valid = reader.U64(call.connection) && reader.U64(call.transaction) &&
              reader.Count(count, 8);
      for (uint32_t i = 0; valid && i < count; ++i) {
        valid = reader.I64(call.scope.emplace_back());
      }
      valid = valid && reader.Byte(byte) && IsMode(byte);
      call.mode = static_cast<IndexedDbTransactionMode>(byte);
      valid = valid && reader.Byte(byte) &&
              byte <= static_cast<uint8_t>(IndexedDbDurability::kRelaxed);
      call.durability = static_cast<IndexedDbDurability>(byte);
      break;
    }
    case IndexedDbCallOp::kVersionChangeIgnored:
    case IndexedDbCallOp::kDidBecomeInactive:
    case IndexedDbCallOp::kCloseConnection:
      valid = reader.U64(call.connection);
      break;
    case IndexedDbCallOp::kGet:
      valid = reader.U64(call.call) && reader.U64(call.connection) &&
              reader.U64(call.transaction) &&
              reader.I64(call.object_store_id) && reader.I64(call.index_id) &&
              ReadRange(reader, call.key_range) && reader.Bool(call.key_only);
      break;
    case IndexedDbCallOp::kGetAll:
      valid = reader.U64(call.call) && reader.U64(call.connection) &&
              reader.U64(call.transaction) &&
              reader.I64(call.object_store_id) && reader.I64(call.index_id) &&
              ReadRange(reader, call.key_range) && reader.Byte(byte) &&
              byte <= static_cast<uint8_t>(IndexedDbGetAllResultType::kRecords);
      call.result_type = static_cast<IndexedDbGetAllResultType>(byte);
      valid = valid && reader.U32(call.count) && call.count > 0 &&
              reader.Byte(byte) && IsDirection(byte);
      call.direction = static_cast<IndexedDbCursorDirection>(byte);
      break;
    case IndexedDbCallOp::kOpenCursor:
      valid = reader.U64(call.call) && reader.U64(call.connection) &&
              reader.U64(call.transaction) && reader.U64(call.cursor) &&
              reader.I64(call.object_store_id) && reader.I64(call.index_id) &&
              ReadRange(reader, call.key_range) && reader.Byte(byte) &&
              IsDirection(byte);
      call.direction = static_cast<IndexedDbCursorDirection>(byte);
      valid = valid && reader.Bool(call.key_only) && reader.Byte(byte) &&
              byte <= static_cast<uint8_t>(IndexedDbTaskType::kPreemptive);
      call.task_type = static_cast<IndexedDbTaskType>(byte);
      break;
    case IndexedDbCallOp::kCount:
      valid = reader.U64(call.call) && reader.U64(call.connection) &&
              reader.U64(call.transaction) &&
              reader.I64(call.object_store_id) && reader.I64(call.index_id) &&
              ReadRange(reader, call.key_range);
      break;
    case IndexedDbCallOp::kDeleteRange:
      valid = reader.U64(call.call) && reader.U64(call.connection) &&
              reader.U64(call.transaction) &&
              reader.I64(call.object_store_id) &&
              ReadRange(reader, call.key_range);
      break;
    case IndexedDbCallOp::kGetKeyGeneratorCurrentNumber:
    case IndexedDbCallOp::kClear:
      valid = reader.U64(call.call) && reader.U64(call.connection) &&
              reader.U64(call.transaction) && reader.I64(call.object_store_id);
      break;
    case IndexedDbCallOp::kCreateIndex:
      valid = reader.U64(call.connection) && reader.U64(call.transaction) &&
              reader.I64(call.object_store_id) &&
              ReadIndexMetadata(reader, call.index);
      break;
    case IndexedDbCallOp::kDeleteIndex:
      valid = reader.U64(call.connection) && reader.U64(call.transaction) &&
              reader.I64(call.object_store_id) && reader.I64(call.index_id);
      break;
    case IndexedDbCallOp::kRenameIndex:
      valid = reader.U64(call.connection) && reader.U64(call.transaction) &&
              reader.I64(call.object_store_id) && reader.I64(call.index_id) &&
              reader.Sized16(call.name);
      break;
    case IndexedDbCallOp::kRenameObjectStore:
      valid = reader.U64(call.connection) && reader.U64(call.transaction) &&
              reader.I64(call.object_store_id) && reader.Sized16(call.name);
      break;
    case IndexedDbCallOp::kAbort:
      valid = reader.U64(call.connection) && reader.U64(call.transaction);
      break;
    case IndexedDbCallOp::kCreateObjectStore:
      valid = reader.U64(call.transaction) &&
              reader.I64(call.object_store_id) && reader.Sized16(call.name) &&
              ReadKeyPath(reader, call.key_path) &&
              reader.Bool(call.auto_increment);
      break;
    case IndexedDbCallOp::kDeleteObjectStore:
      valid = reader.U64(call.transaction) && reader.I64(call.object_store_id);
      break;
    case IndexedDbCallOp::kPut:
      valid = reader.U64(call.call) && reader.U64(call.transaction) &&
              reader.I64(call.object_store_id) && reader.Sized(call.value) &&
              ReadValidOrNoneKey(reader, call.key) && reader.Byte(byte) &&
              byte <= static_cast<uint8_t>(IndexedDbPutMode::kCursorUpdate);
      call.put_mode = static_cast<IndexedDbPutMode>(byte);
      valid = valid && ReadIndexKeys(reader, call.index_keys);
      break;
    case IndexedDbCallOp::kSetIndexKeys:
      valid = reader.U64(call.transaction) &&
              reader.I64(call.object_store_id) &&
              ReadKey(reader, call.primary_key) && call.primary_key.IsValid() &&
              ReadIndexKeys(reader, call.index_keys);
      break;
    case IndexedDbCallOp::kSetIndexKeysDone:
      valid = reader.U64(call.transaction);
      break;
    case IndexedDbCallOp::kCommit:
      valid = reader.U64(call.transaction) &&
              reader.I64(call.num_errors_handled) &&
              call.num_errors_handled >= 0;
      break;
    case IndexedDbCallOp::kCursorAdvance:
    case IndexedDbCallOp::kCursorPrefetch:
      valid = reader.U64(call.call) && reader.U64(call.cursor) &&
              reader.U32(call.count) && call.count > 0;
      break;
    case IndexedDbCallOp::kCursorContinue:
      valid = reader.U64(call.call) && reader.U64(call.cursor) &&
              ReadValidOrNoneKey(reader, call.key) &&
              ReadValidOrNoneKey(reader, call.primary_key);
      break;
    case IndexedDbCallOp::kCursorPrefetchReset:
      valid = reader.U64(call.cursor) && reader.U32(call.count);
      break;
    case IndexedDbCallOp::kCloseCursor:
      valid = reader.U64(call.cursor);
      break;
  }
  if (!valid || !reader.AtEnd()) {
    return base::unexpected("an IndexedDB call that is not valid");
  }
  return call;
}

std::string EncodeIndexedDbCallback(const PageIndexedDbCallback& callback) {
  MessageWriter writer;
  writer.Byte(static_cast<uint8_t>(callback.op));
  switch (callback.op) {
    case IndexedDbCallbackOp::kError:
      writer.U64(callback.call);
      writer.Byte(static_cast<uint8_t>(callback.error));
      writer.Sized16(callback.message);
      break;
    case IndexedDbCallbackOp::kBlocked:
      writer.U64(callback.call);
      writer.I64(callback.old_version);
      break;
    case IndexedDbCallbackOp::kUpgradeNeeded:
      writer.U64(callback.call);
      writer.I64(callback.old_version);
      writer.Bool(callback.data_loss);
      WriteMetadata(writer, callback.metadata);
      break;
    case IndexedDbCallbackOp::kOpenSuccess:
      writer.U64(callback.call);
      WriteMetadata(writer, callback.metadata);
      break;
    case IndexedDbCallbackOp::kDeleteSuccess:
      writer.U64(callback.call);
      writer.I64(callback.old_version);
      break;
    case IndexedDbCallbackOp::kDatabaseInfo:
      writer.U64(callback.call);
      writer.Byte(static_cast<uint8_t>(callback.error));
      writer.Sized16(callback.message);
      writer.U32(static_cast<uint32_t>(callback.databases.size()));
      for (const auto& [name, version] : callback.databases) {
        writer.Sized16(name);
        writer.I64(version);
      }
      break;
    case IndexedDbCallbackOp::kForcedClose:
      writer.U64(callback.connection);
      break;
    case IndexedDbCallbackOp::kVersionChange:
      writer.U64(callback.connection);
      writer.I64(callback.old_version);
      writer.I64(callback.new_version);
      break;
    case IndexedDbCallbackOp::kAbort:
      writer.U64(callback.connection);
      writer.U64(callback.transaction);
      writer.Byte(static_cast<uint8_t>(callback.error));
      writer.Sized16(callback.message);
      break;
    case IndexedDbCallbackOp::kComplete:
      writer.U64(callback.connection);
      writer.U64(callback.transaction);
      break;
    case IndexedDbCallbackOp::kGetResult:
    case IndexedDbCallbackOp::kOpenCursorResult:
    case IndexedDbCallbackOp::kPutResult:
      writer.U64(callback.call);
      writer.Byte(static_cast<uint8_t>(callback.error));
      writer.Sized16(callback.message);
      writer.Bool(callback.empty);
      WriteKey(writer, callback.key);
      WriteKey(writer, callback.primary_key);
      writer.Bool(callback.value.has_value());
      if (callback.value) {
        WriteReturnValue(writer, *callback.value);
      }
      break;
    case IndexedDbCallbackOp::kGetAllResult:
      writer.U64(callback.call);
      writer.Byte(static_cast<uint8_t>(callback.error));
      writer.Sized16(callback.message);
      writer.Bool(callback.done);
      writer.U32(static_cast<uint32_t>(callback.records.size()));
      for (const IndexedDbRecord& record : callback.records) {
        WriteRecord(writer, record);
      }
      break;
    case IndexedDbCallbackOp::kCountResult:
    case IndexedDbCallbackOp::kSuccessResult:
      writer.U64(callback.call);
      writer.Bool(callback.success);
      writer.U32(callback.count);
      break;
    case IndexedDbCallbackOp::kKeyGeneratorResult:
      writer.U64(callback.call);
      writer.Byte(static_cast<uint8_t>(callback.error));
      writer.Sized16(callback.message);
      writer.I64(callback.number);
      break;
    case IndexedDbCallbackOp::kCursorResult:
      writer.U64(callback.call);
      writer.Byte(static_cast<uint8_t>(callback.error));
      writer.Sized16(callback.message);
      writer.Bool(callback.empty);
      writer.U32(static_cast<uint32_t>(callback.keys.size()));
      for (size_t i = 0; i < callback.keys.size(); ++i) {
        WriteKey(writer, callback.keys[i]);
        WriteKey(writer, callback.primary_keys[i]);
        writer.Bool(i < callback.values.size());
        if (i < callback.values.size()) {
          WriteReturnValue(writer, callback.values[i]);
        }
      }
      break;
  }
  return writer.Take();
}

bool IndexedDbCallbackFits(const PageIndexedDbCallback& callback) {
  return EncodeIndexedDbCallback(callback).size() <= kMaxPayloadSize;
}

base::expected<PageIndexedDbCallback, std::string> ReadIndexedDbCallback(
    std::string_view payload) {
  MessageReader reader(payload);
  PageIndexedDbCallback callback;
  uint8_t op = 0;
  if (!reader.Byte(op) || op == 0 ||
      op > static_cast<uint8_t>(IndexedDbCallbackOp::kMaxValue)) {
    return base::unexpected("an IndexedDB callback of an unknown operation");
  }
  callback.op = static_cast<IndexedDbCallbackOp>(op);
  uint8_t error = 0;
  bool valid = false;
  const auto read_error = [&]() {
    if (!reader.Byte(error) || !IsException(error)) {
      return false;
    }
    callback.error = static_cast<IndexedDbException>(error);
    return reader.Sized16(callback.message);
  };
  switch (callback.op) {
    case IndexedDbCallbackOp::kError:
      valid = reader.U64(callback.call) && read_error();
      break;
    case IndexedDbCallbackOp::kBlocked:
    case IndexedDbCallbackOp::kDeleteSuccess:
      valid = reader.U64(callback.call) && reader.I64(callback.old_version);
      break;
    case IndexedDbCallbackOp::kUpgradeNeeded:
      valid = reader.U64(callback.call) && reader.I64(callback.old_version) &&
              reader.Bool(callback.data_loss) &&
              ReadMetadata(reader, callback.metadata);
      break;
    case IndexedDbCallbackOp::kOpenSuccess:
      valid =
          reader.U64(callback.call) && ReadMetadata(reader, callback.metadata);
      break;
    case IndexedDbCallbackOp::kDatabaseInfo: {
      uint32_t count = 0;
      valid =
          reader.U64(callback.call) && read_error() && reader.Count(count, 12);
      for (uint32_t i = 0; valid && i < count; ++i) {
        auto& [name, version] = callback.databases.emplace_back();
        valid = reader.Sized16(name) && reader.I64(version);
      }
      break;
    }
    case IndexedDbCallbackOp::kForcedClose:
      valid = reader.U64(callback.connection);
      break;
    case IndexedDbCallbackOp::kVersionChange:
      valid = reader.U64(callback.connection) &&
              reader.I64(callback.old_version) &&
              reader.I64(callback.new_version);
      break;
    case IndexedDbCallbackOp::kAbort:
      valid = reader.U64(callback.connection) &&
              reader.U64(callback.transaction) && read_error();
      break;
    case IndexedDbCallbackOp::kComplete:
      valid =
          reader.U64(callback.connection) && reader.U64(callback.transaction);
      break;
    case IndexedDbCallbackOp::kGetResult:
    case IndexedDbCallbackOp::kOpenCursorResult:
    case IndexedDbCallbackOp::kPutResult: {
      bool has_value = false;
      valid = reader.U64(callback.call) && read_error() &&
              reader.Bool(callback.empty) &&
              ReadValidOrNoneKey(reader, callback.key) &&
              ReadValidOrNoneKey(reader, callback.primary_key) &&
              reader.Bool(has_value);
      if (valid && has_value) {
        callback.value.emplace();
        valid = ReadReturnValue(reader, *callback.value);
      }
      break;
    }
    case IndexedDbCallbackOp::kGetAllResult: {
      uint32_t count = 0;
      valid = reader.U64(callback.call) && read_error() &&
              reader.Bool(callback.done) && reader.Count(count, 1);
      for (uint32_t i = 0; valid && i < count; ++i) {
        valid = ReadRecord(reader, callback.records.emplace_back());
      }
      break;
    }
    case IndexedDbCallbackOp::kCountResult:
    case IndexedDbCallbackOp::kSuccessResult:
      valid = reader.U64(callback.call) && reader.Bool(callback.success) &&
              reader.U32(callback.count);
      break;
    case IndexedDbCallbackOp::kKeyGeneratorResult:
      valid = reader.U64(callback.call) && read_error() &&
              reader.I64(callback.number);
      break;
    case IndexedDbCallbackOp::kCursorResult: {
      uint32_t count = 0;
      valid = reader.U64(callback.call) && read_error() &&
              reader.Bool(callback.empty) && reader.Count(count, 3);
      for (uint32_t i = 0; valid && i < count; ++i) {
        bool has_value = false;
        valid = ReadKey(reader, callback.keys.emplace_back()) &&
                callback.keys.back().IsValid() &&
                ReadKey(reader, callback.primary_keys.emplace_back()) &&
                callback.primary_keys.back().IsValid() &&
                reader.Bool(has_value);
        if (valid && has_value) {
          valid = ReadReturnValue(reader, callback.values.emplace_back());
        }
      }
      break;
    }
  }
  if (!valid || !reader.AtEnd()) {
    return base::unexpected("an IndexedDB callback that is not valid");
  }
  return callback;
}

}  // namespace mitto
