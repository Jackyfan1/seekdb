-- file_sql: read local CSV, JSONL and Parquet files as database tables
select @@session.enable_file_sql AS default_enabled;
select count(*) from read_csv('/root/suite_run/data_for_mysqltest_file_sql/basic.csv');
set session enable_file_sql = ON;

-- syntax and format validation
select count(*) from read_csv('/root/suite_run/data_for_mysqltest_file_sql/basic.csv');
select * from read_csv('/root/suite_run/data_for_mysqltest_file_sql/basic.csv', 'xml');
select * from read_csv('/root/suite_run/data_for_mysqltest_file_sql/basic.csv', sample_size = 10);

-- CSV scan, type conversion, duplicate headers and multiline quotes
select id, name, score, active, day, created,
       replace(note, char(10), '<NL>') AS note, id__2
  from read_csv('/root/suite_run/data_for_mysqltest_file_sql/basic.csv') f
 order by id;
select count(*) AS row_count, sum(id) AS id_sum,
       round(avg(score), 2) AS score_avg, sum(active) AS active_count
  from read_csv('/root/suite_run/data_for_mysqltest_file_sql/basic.csv', header = true, sample_size = -1) f;

-- JSONL union schema and missing fields
select id, name, score, active, day, note
  from read_json('/root/suite_run/data_for_mysqltest_file_sql/basic.jsonl', format = 'newline_delimited') f
 order by id;

-- inferred CSV schema using DuckDB DESCRIBE syntax
describe select * from read_csv('/root/suite_run/data_for_mysqltest_file_sql/basic.csv');

-- Parquet scan and aggregate push through projected columns
select count(*) AS row_count, min(c1) AS min_c1,
       max(c1) AS max_c1, sum(c1) AS sum_c1
  from read_parquet('/root/suite_run/data_for_mysqltest_file_sql/simple.parquet') p;

-- DuckDB glob returns one file column
select substring_index(file, '/', -1) AS file
  from glob('/root/suite_run/data_for_mysqltest_file_sql/*')
 order by file;
