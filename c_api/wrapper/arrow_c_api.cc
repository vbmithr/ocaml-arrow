// The Arrow C++ library uses the arrow::Result and arrow::Status
// struct to propagate errors.
// However we convert these to exceptions so that this triggers
// leaving the scope where the C++ objects are allocated. The exceptions
// are then converted to a caml_failwith.
// Calling caml_failwith directly could lead to some memory leaks.
//
// This is done using some C macros:
//
// OCAML_BEGIN_PROTECT_EXN
// ... use the arrow C++ library ...
// return result;
// OCAML_END_PROTECT_EXN
// return nullptr;

#include "arrow_c_api.h"

#include<iostream>

#include <caml/mlvalues.h>
#include <caml/threads.h>
#include<caml/fail.h>
// invalid_argument is defined in the ocaml runtime and would
// shadow the C++ std::invalid_argument
#undef invalid_argument

#define OCAML_BEGIN_PROTECT_EXN                     \
  char *__ocaml_protect_err = nullptr;              \
  try {
#define OCAML_END_PROTECT_EXN                       \
  } catch (const std::exception& e) {               \
    __ocaml_protect_err = strdup(e.what());         \
  }                                                 \
  if (__ocaml_protect_err) {                        \
    char err[256];                                  \
    snprintf(err, 255, "%s", __ocaml_protect_err);  \
    caml_failwith(err);                             \
  }

template<class T>
T ok_exn(arrow::Result<T> &a) {
  if (!a.ok()) {
    throw std::invalid_argument(a.status().ToString());
  }
  return std::move(a.ValueOrDie());
}

void status_exn(arrow::Status &st) {
  if (!st.ok()) {
    throw std::invalid_argument(st.ToString());
  }
}

struct ArrowSchema *arrow_schema(char *filename) {
  OCAML_BEGIN_PROTECT_EXN

  auto file = arrow::io::ReadableFile::Open(filename, arrow::default_memory_pool());
  std::shared_ptr<arrow::io::RandomAccessFile> infile = ok_exn(file);
  auto reader = arrow::ipc::RecordBatchFileReader::Open(infile);
  std::shared_ptr<arrow::Schema> schema = ok_exn(reader)->schema();
  struct ArrowSchema *out = (struct ArrowSchema*)malloc(sizeof *out);
  arrow::ExportSchema(*schema, out);
  return out;

  OCAML_END_PROTECT_EXN
  return nullptr;
}

struct ArrowSchema *feather_schema(char *filename) {
  OCAML_BEGIN_PROTECT_EXN

  auto file = arrow::io::ReadableFile::Open(filename, arrow::default_memory_pool());
  std::shared_ptr<arrow::io::RandomAccessFile> infile = ok_exn(file);
  auto reader = arrow::ipc::feather::Reader::Open(infile);
  std::shared_ptr<arrow::Schema> schema = ok_exn(reader)->schema();
  struct ArrowSchema *out = (struct ArrowSchema*)malloc(sizeof *out);
  arrow::ExportSchema(*schema, out);
  return out;

  OCAML_END_PROTECT_EXN
  return nullptr;
}

struct ArrowSchema *parquet_schema(char *filename, int64_t *num_rows) {
  OCAML_BEGIN_PROTECT_EXN

  arrow::Status st;
  auto file = arrow::io::ReadableFile::Open(filename, arrow::default_memory_pool());
  std::shared_ptr<arrow::io::RandomAccessFile> infile = ok_exn(file);
  std::unique_ptr<parquet::arrow::FileReader> reader;
  st = parquet::arrow::OpenFile(infile, arrow::default_memory_pool(), &reader);
  status_exn(st);
  std::shared_ptr<arrow::Schema> schema;
  st = reader->GetSchema(&schema);
  status_exn(st);
  *num_rows = reader->parquet_reader()->metadata()->num_rows();
  struct ArrowSchema *out = (struct ArrowSchema*)malloc(sizeof *out);
  arrow::ExportSchema(*schema, out);
  return out;

  OCAML_END_PROTECT_EXN
  return nullptr;
}

void free_schema(struct ArrowSchema *schema) {
  if (schema->release != NULL)
    schema->release(schema);
  schema->release = NULL;
  free(schema);
}

void check_column_idx(int column_idx, int n_cols) {
  if (column_idx < 0 || column_idx >= n_cols) {
    char err[128];
    snprintf(err, 127, "invalid column index %d (ncols: %d)", column_idx, n_cols);
    caml_failwith(err);
  }
}

struct ArrowArray *table_chunked_column_(TablePtr *table, char *column_name, int column_idx, int *nchunks, int dt) {
  OCAML_BEGIN_PROTECT_EXN

  arrow::Type::type expected_type;
  const char *expected_type_str = "";
  if (dt == 0) {
    expected_type = arrow::Type::INT64;
    expected_type_str = "int64";
  }
  else if (dt == 1) {
    expected_type = arrow::Type::DOUBLE;
    expected_type_str = "float64";
  }
  else if (dt == 2) {
    // TODO: also handle large_utf8 here.
    expected_type = arrow::Type::STRING;
    expected_type_str = "utf8";
  }
  else if (dt == 3) {
    expected_type = arrow::Type::DATE32;
    expected_type_str = "date32";
  }
  else if (dt == 4) {
    expected_type = arrow::Type::TIMESTAMP;
    expected_type_str = "timestamp";
  }
  else if (dt == 5) {
    expected_type = arrow::Type::BOOL;
    expected_type_str = "bool";
  }
  else {
    throw std::invalid_argument(std::string("unknown datatype ") + std::to_string(dt));
  }
  std::shared_ptr<arrow::ChunkedArray> array;
  if (column_name) {
    array = (*table)->GetColumnByName(std::string(column_name));
    if (!array) {
      throw std::invalid_argument(std::string("cannot find column ") + column_name);
    }
  }
  else {
    array = (*table)->column(column_idx);
  }
  if (!array) {
    throw std::invalid_argument("error finding column");
  }
  *nchunks = array->num_chunks();
  struct ArrowArray *out = (struct ArrowArray*)malloc(array->num_chunks() * sizeof *out);
  for (int i = 0; i < array->num_chunks(); ++i) {
    auto chunk = array->chunk(i);
    if (chunk->type()->id() != expected_type) {
      throw std::invalid_argument(
        std::string("expected type with ") + expected_type_str + " (id "
        + std::to_string(expected_type) + ") got " + chunk->type()->ToString());
    }
    arrow::ExportArray(*chunk, out + i);
  }
  return out;

  OCAML_END_PROTECT_EXN
  return nullptr;
}

struct ArrowArray *table_chunked_column(TablePtr *table, int column_idx, int *nchunks, int dt) {
  int n_cols = (*table)->num_columns();
  check_column_idx(column_idx, n_cols);
  return table_chunked_column_(table, NULL, column_idx, nchunks, dt);
}

struct ArrowArray *table_chunked_column_by_name(TablePtr *table, char *col_name, int *nchunks, int dt) {
  return table_chunked_column_(table, col_name, 0, nchunks, dt);
}

void free_chunked_column(struct ArrowArray *arrays, int nchunks) {
  for (int i = 0; i < nchunks; ++i) {
    if (arrays[i].release != NULL) arrays[i].release(arrays + i);
  }
  free(arrays);
}

arrow::Compression::type compression_of_int(int compression) {
  arrow::Compression::type compression_ = arrow::Compression::UNCOMPRESSED;
  if (compression == 1) compression_ = arrow::Compression::SNAPPY;
  else if (compression == 2) compression_ = arrow::Compression::GZIP;
  else if (compression == 3) compression_ = arrow::Compression::BROTLI;
  else if (compression == 4) compression_ = arrow::Compression::ZSTD;
  else if (compression == 5) compression_ = arrow::Compression::LZ4;
  else if (compression == 6) compression_ = arrow::Compression::LZ4_FRAME;
  else if (compression == 7) compression_ = arrow::Compression::LZO;
  else if (compression == 8) compression_ = arrow::Compression::BZ2;
  return compression_;
}

TablePtr *create_table(struct ArrowArray *array, struct ArrowSchema *schema) {
  OCAML_BEGIN_PROTECT_EXN

  auto record_batch = arrow::ImportRecordBatch(array, schema);
  auto table = arrow::Table::FromRecordBatches({ok_exn(record_batch)});
  return new std::shared_ptr<arrow::Table>(std::move(ok_exn(table)));

  OCAML_END_PROTECT_EXN
  return nullptr;
}

void parquet_write_file(char *filename, struct ArrowArray *array, struct ArrowSchema *schema, int chunk_size, int compression) {
  OCAML_BEGIN_PROTECT_EXN

  auto file = arrow::io::FileOutputStream::Open(filename);
  auto outfile = ok_exn(file);
  auto record_batch = arrow::ImportRecordBatch(array, schema);
  auto table = arrow::Table::FromRecordBatches({ok_exn(record_batch)});
  arrow::Compression::type compression_ = compression_of_int(compression);
  arrow::Status st = parquet::arrow::WriteTable(*(ok_exn(table)),
                                                arrow::default_memory_pool(),
                                                outfile,
                                                chunk_size,
                                                parquet::WriterProperties::Builder().compression(compression_)->build(),
                                                parquet::ArrowWriterProperties::Builder().enable_deprecated_int96_timestamps()->build());
  status_exn(st);

  OCAML_END_PROTECT_EXN
}

void arrow_write_file(char *filename, struct ArrowArray *array, struct ArrowSchema *schema, int chunk_size) {
  OCAML_BEGIN_PROTECT_EXN

  auto file = arrow::io::FileOutputStream::Open(filename);
  auto outfile = ok_exn(file);
  auto record_batch = arrow::ImportRecordBatch(array, schema);
  auto table = arrow::Table::FromRecordBatches({ok_exn(record_batch)});
  auto table_ = ok_exn(table);
  auto batch_writer = arrow::ipc::NewFileWriter(&(*outfile), table_->schema());
  arrow::Status st = ok_exn(batch_writer)->WriteTable(*table_);
  status_exn(st);

  OCAML_END_PROTECT_EXN
}

void feather_write_file(char *filename, struct ArrowArray *array, struct ArrowSchema *schema, int chunk_size, int compression) {
  OCAML_BEGIN_PROTECT_EXN

  auto file = arrow::io::FileOutputStream::Open(filename);
  auto outfile = ok_exn(file);
  auto record_batch = arrow::ImportRecordBatch(array, schema);
  auto table = arrow::Table::FromRecordBatches({ok_exn(record_batch)});
  struct arrow::ipc::feather::WriteProperties wp;
  wp.compression = compression_of_int(compression);
  wp.chunksize = chunk_size;
  arrow::Status st = arrow::ipc::feather::WriteTable(*(ok_exn(table)),
                                                &(*outfile),
                                                wp);
  status_exn(st);

  OCAML_END_PROTECT_EXN
}

void parquet_write_table(char *filename, TablePtr *table, int chunk_size, int compression) {
  OCAML_BEGIN_PROTECT_EXN

  auto file = arrow::io::FileOutputStream::Open(filename);
  auto outfile = ok_exn(file);
  arrow::Compression::type compression_ = compression_of_int(compression);
  arrow::Status st = parquet::arrow::WriteTable(**table,
                                                arrow::default_memory_pool(),
                                                outfile,
                                                chunk_size,
                                                parquet::WriterProperties::Builder().compression(compression_)->build(),
                                                parquet::ArrowWriterProperties::Builder().enable_deprecated_int96_timestamps()->build());
  status_exn(st);

  OCAML_END_PROTECT_EXN
}

void feather_write_table(char *filename, TablePtr *table, int chunk_size, int compression) {
  OCAML_BEGIN_PROTECT_EXN

  auto file = arrow::io::FileOutputStream::Open(filename);
  auto outfile = ok_exn(file);
  struct arrow::ipc::feather::WriteProperties wp;
  wp.compression = compression_of_int(compression);
  wp.chunksize = chunk_size;
  arrow::Status st = arrow::ipc::feather::WriteTable(**table, &(*outfile), wp);
  status_exn(st);

  OCAML_END_PROTECT_EXN
}

ParquetReader *parquet_reader_open(char *filename, int *col_idxs, int ncols, int use_threads, int mmap) {
  OCAML_BEGIN_PROTECT_EXN

  arrow::Status st;
  std::unique_ptr<parquet::ParquetFileReader> preader = parquet::ParquetFileReader::OpenFile(filename, mmap);
  std::unique_ptr<parquet::arrow::FileReader> reader;
  st = parquet::arrow::FileReader::Make(arrow::default_memory_pool(), std::move(preader), &reader);
  status_exn(st);
  if (use_threads >= 0) reader->set_use_threads(use_threads);
  std::unique_ptr<arrow::RecordBatchReader> batch_reader;
  std::vector<int> all_groups(reader->num_row_groups());
  std::iota(all_groups.begin(), all_groups.end(), 0);
  if (ncols)
    st = reader->GetRecordBatchReader(
      all_groups,
      std::vector<int>(col_idxs, col_idxs+ncols),
      &batch_reader);
  else
    st = reader->GetRecordBatchReader(all_groups, &batch_reader);
  status_exn(st);
  return new ParquetReader{ std::move(reader), std::move(batch_reader)};

  OCAML_END_PROTECT_EXN
  return nullptr;
}

TablePtr *parquet_reader_next(ParquetReader *pr) {
  if (!pr->batch_reader) caml_failwith("reader has already been closed");

  OCAML_BEGIN_PROTECT_EXN

  arrow::Status st;
  std::shared_ptr<arrow::RecordBatch> batch;
  st = pr->batch_reader->ReadNext(&batch);
  status_exn(st);
  if (batch == nullptr) {
    return nullptr;
  }
  auto table_ = arrow::Table::FromRecordBatches({batch});
  std::shared_ptr<arrow::Table> table = std::move(ok_exn(table_));
  return new std::shared_ptr<arrow::Table>(std::move(table));

  OCAML_END_PROTECT_EXN
  return nullptr;
}

void parquet_reader_close(ParquetReader *pr) {
  pr->batch_reader.reset();
  pr->reader.reset();
}

void parquet_reader_free(ParquetReader *pr) {
  delete pr;
}

TablePtr *parquet_read_table(char *filename, int *col_idxs, int ncols, int use_threads, int64_t only_first) {
  OCAML_BEGIN_PROTECT_EXN

  arrow::Status st;
  auto file = arrow::io::ReadableFile::Open(filename, arrow::default_memory_pool());
  std::shared_ptr<arrow::io::RandomAccessFile> infile = ok_exn(file);
  std::unique_ptr<parquet::arrow::FileReader> reader;
  st = parquet::arrow::OpenFile(infile, arrow::default_memory_pool(), &reader);
  status_exn(st);
  if (use_threads >= 0) reader->set_use_threads(use_threads);
  std::shared_ptr<arrow::Table> table;
  if (only_first < 0) {
    if (ncols)
      st = reader->ReadTable(std::vector<int>(col_idxs, col_idxs+ncols), &table);
    else
      st = reader->ReadTable(&table);
    status_exn(st);
  } else {
    std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
    for (int row_group_idx = 0; row_group_idx < reader->num_row_groups(); ++row_group_idx) {
      std::unique_ptr<arrow::RecordBatchReader> batch_reader;
      if (ncols)
        st = reader->GetRecordBatchReader({row_group_idx}, std::vector<int>(col_idxs, col_idxs+ncols), &batch_reader);
      else
        st = reader->GetRecordBatchReader({row_group_idx}, &batch_reader);
      status_exn(st);
      std::shared_ptr<arrow::RecordBatch> batch;
      while (only_first > 0) {
        st = batch_reader->ReadNext(&batch);
        status_exn(st);
        if (batch == nullptr) break;
        if (only_first <= batch->num_rows()) {
          batches.push_back(std::move(batch->Slice(0, only_first)));
          only_first = 0;
          break;
        }
        else {
          only_first -= batch->num_rows();
          batches.push_back(std::move(batch));
        }
      }
      if (only_first <= 0)
        break;
    }
    auto table_ = arrow::Table::FromRecordBatches(batches);
    table = std::move(ok_exn(table_));
  }
  return new std::shared_ptr<arrow::Table>(std::move(table));

  OCAML_END_PROTECT_EXN
  return nullptr;
}

TablePtr *feather_read_table(char *filename, int *col_idxs, int ncols) {
  OCAML_BEGIN_PROTECT_EXN

  arrow::Status st;
  auto file = arrow::io::ReadableFile::Open(filename, arrow::default_memory_pool());
  std::shared_ptr<arrow::io::RandomAccessFile> infile = ok_exn(file);
  auto reader = arrow::ipc::feather::Reader::Open(infile);
  std::shared_ptr<arrow::Table> table;
  if (ncols)
    st = ok_exn(reader)->Read(std::vector<int>(col_idxs, col_idxs+ncols), &table);
  else
    st = ok_exn(reader)->Read(&table);
  status_exn(st);
  return new std::shared_ptr<arrow::Table>(std::move(table));
  OCAML_END_PROTECT_EXN
  return nullptr;
}

TablePtr *csv_read_table(char *filename) {
  OCAML_BEGIN_PROTECT_EXN

  auto file = arrow::io::ReadableFile::Open(filename, arrow::default_memory_pool());
  std::shared_ptr<arrow::io::RandomAccessFile> infile = ok_exn(file);

  auto reader =
    arrow::csv::TableReader::Make(arrow::default_memory_pool(),
                                  infile,
                                  arrow::csv::ReadOptions::Defaults(),
                                  arrow::csv::ParseOptions::Defaults(),
                                  arrow::csv::ConvertOptions::Defaults());

  auto table = ok_exn(reader)->Read();
  return new std::shared_ptr<arrow::Table>(std::move(table.ValueOrDie()));

  OCAML_END_PROTECT_EXN
  return nullptr;
}

TablePtr *json_read_table(char *filename) {
  OCAML_BEGIN_PROTECT_EXN

  arrow::Status st;
  auto file = arrow::io::ReadableFile::Open(filename, arrow::default_memory_pool());
  std::shared_ptr<arrow::io::RandomAccessFile> infile = ok_exn(file);

  std::shared_ptr<arrow::json::TableReader> reader;
  st = arrow::json::TableReader::Make(arrow::default_memory_pool(),
                                      infile,
                                      arrow::json::ReadOptions::Defaults(),
                                      arrow::json::ParseOptions::Defaults(),
                                      &reader);
  status_exn(st);
  std::shared_ptr<arrow::Table> table;
  st = reader->Read(&table);
  status_exn(st);
  return new std::shared_ptr<arrow::Table>(std::move(table));

  OCAML_END_PROTECT_EXN
  return nullptr;
}

TablePtr *table_concatenate(TablePtr **tables, int ntables) {
  OCAML_BEGIN_PROTECT_EXN

  std::vector<std::shared_ptr<arrow::Table>> vec;
  for (int i = 0; i < ntables; ++i) vec.push_back(**(tables+i));
  auto table = arrow::ConcatenateTables(vec);
  return new std::shared_ptr<arrow::Table>(std::move(ok_exn(table)));

  OCAML_END_PROTECT_EXN
  return nullptr;
}

TablePtr *table_slice(TablePtr *table, int64_t offset, int64_t length) {
  if (offset < 0) caml_invalid_argument("negative offset");
  if (length < 0) caml_invalid_argument("negative length");
  auto slice = (*table)->Slice(offset, length);
  return new std::shared_ptr<arrow::Table>(std::move(slice));
}

int64_t table_num_rows(TablePtr *table) {
  if (table != NULL) return (*table)->num_rows();
  return 0;
}

struct ArrowSchema *table_schema(TablePtr *table) {
  std::shared_ptr<arrow::Schema> schema = (*table)->schema();
  struct ArrowSchema *out = (struct ArrowSchema*)malloc(sizeof *out);
  arrow::ExportSchema(*schema, out);
  return out;
}

void free_table(TablePtr *table) {
  if (table != NULL)
    delete table;
}
