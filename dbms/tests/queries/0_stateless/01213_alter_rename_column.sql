DROP TABLE IF EXISTS table_for_rename;

CREATE TABLE table_for_rename
(
  date Date,
  key UInt64,
  value1 String,
  value2 String,
  value3 String
)
ENGINE = MergeTree()
PARTITION BY date
ORDER BY key;

INSERT INTO table_for_rename SELECT toDate('2019-10-01') + number % 3, number, toString(number), toString(number), toString(number) from numbers(9);

SELECT value1 FROM table_for_rename WHERE key = 1;

ALTER TABLE table_for_rename RENAME COLUMN value1 to renamed_value1;

SELECT renamed_value1 FROM table_for_rename WHERE key = 1;

DROP TABLE IF EXISTS table_for_rename;