# Remove only the fixture directory created by copy_file_sql_data.inc.
connect (file_sql_cleanup,$OBMYSQL_MS0,admin,$OBMYSQL_PWD,test,$OBMYSQL_PORT);
connection file_sql_cleanup;

--disable_query_log
let $FILE_SQL_HOST = query_get_value('select host_ip()',host_ip(), 1);
let $FILE_SQL_DATA_DIR = $OBSERVER_DIR/data_for_mysqltest_file_sql;
let $FILE_SQL_CLEANUP_CMD = python3 mysql_test/test_suite/file_sql/include/file_sql_fixture.py cleanup "$FILE_SQL_HOST" "$FILE_SQL_DATA_DIR";
exec $FILE_SQL_CLEANUP_CMD;
--enable_query_log

disconnect file_sql_cleanup;
connection default;
