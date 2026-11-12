#!/bin/sh
# Throwaway MySQL server for the catalog tests (T16; MySQL 8.0.19+, Q14).
#
#   tests/catalog_testdb.sh start [DIR]   # prints the export line
#   tests/catalog_testdb.sh stop  [DIR]
#
# Runs a private mysqld as the current user in DIR (default
# ${TMPDIR:-/tmp}/atmo-catalog-test): socket only, no TCP, root without a
# password.  Creates atmo_catalog_test and applies the web app's Doctrine
# migrations (../web, which owns the schema) to it, then writes
# DIR/catalog-test.json (mode 600).  Never touches any other server.
set -eu
cmd=${1:-start}
dir=${2:-${TMPDIR:-/tmp}/atmo-catalog-test}
here=$(cd "$(dirname "$0")/.." && pwd)
web=${ATMO_WEB_DIR:-$here/../web}
mysqld=${MYSQLD:-$(command -v mysqld || echo /usr/sbin/mysqld)}
sock=$dir/sock

case $cmd in
  stop)
    [ -f "$dir/pid" ] && kill "$(cat "$dir/pid")" 2>/dev/null || true
    exit 0 ;;
  start) ;;
  *) echo "usage: $0 start|stop [DIR]" >&2; exit 2 ;;
esac

mkdir -p "$dir"
if [ ! -d "$dir/data" ]; then
  "$mysqld" --no-defaults --initialize-insecure --datadir="$dir/data" \
    --user="$(id -un)" >"$dir/init.log" 2>&1
fi
if [ ! -S "$sock" ]; then
  "$mysqld" --no-defaults --datadir="$dir/data" --socket="$sock" \
    --skip-networking --mysqlx=OFF --pid-file="$dir/pid" \
    --log-error="$dir/err.log" --user="$(id -un)" &
  i=0
  while [ ! -S "$sock" ] && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done
fi
mysql -S "$sock" -uroot -e \
  "DROP DATABASE IF EXISTS atmo_catalog_test; CREATE DATABASE atmo_catalog_test"
(cd "$web" && DATABASE_URL="mysql://root@localhost/atmo_catalog_test?unix_socket=$sock&serverVersion=8.0.32&charset=utf8mb4" \
  php bin/console doctrine:migrations:migrate -n >"$dir/migrate.log" 2>&1) || {
  echo "migrations failed, see $dir/migrate.log" >&2; exit 1; }
umask 077
printf '{"socket": "%s", "user": "root", "database": "atmo_catalog_test"}\n' \
  "$sock" >"$dir/catalog-test.json"
echo "export ATMO_TEST_CATALOG=$dir/catalog-test.json"
