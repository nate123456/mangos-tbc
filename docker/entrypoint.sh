#!/bin/bash

# Initialize defaults
MYSQL_PORT=${MYSQL_PORT:-"3306"}
MYSQL_HOST=${MYSQL_HOST:-"127.0.0.1"}
MYSQL_USER=${MYSQL_USER:-"mangos"}
MYSQL_PASSWORD=${MYSQL_PASSWORD:-"mangos"}
MYSQL_ROOT_USER=${MYSQL_ROOT_USER:-"root"}
MYSQL_ROOT_PASSWORD=${MYSQL_ROOT_PASSWORD:-"mangos"}
MYSQL_MANGOS_DB=${MYSQL_MANGOS_DB:-"tbcmangos"}
MYSQL_CHARS_TABLE=${MYSQL_CHARS_TABLE:-"tbccharacters"}
MYSQL_CHARS_TABLE=${MYSQL_CHARS_TABLE:-"tbccharacters"}
MYSQL_REALM_DB=${MYSQL_REALM_DB:-"tbcrealmd"}

REALM_ENTRY_NAME=${REALM_ENTRY_NAME:-"Mangos"}
REALM_ENTRY_HOST=${REALM_ENTRY_HOST:-"127.0.0.1"}
REALM_ENTRY_PORT=${REALM_ENTRY_PORT:-"8085"}
REALM_ENTRY_ICON=${REALM_ENTRY_ICON:-"1"}
REALM_ENTRY_FLAGS=${REALM_ENTRY_FLAGS:-"0"}
REALM_ENTRY_TIMEZONE=${REALM_ENTRY_TIMEZONE:-"1"}
REALM_ENTRY_ALLOWED_SEC_LEVEL=${REALM_ENTRY_ALLOWED_SEC_LEVEL:-"0"}

RUN_MANGOSD=${RUN_MANGOSD:-true}
INITIALIZE_DATA=${INITIALIZE_DATA:-true}
BACKUP_ACCOUNTS=${BACKUP_ACCOUNTS:-true}
BACKUP_ACCOUNTS_CRON=${BACKUP_ACCOUNTS_CRON:"@daily"}
CONFIG_FILE="InstallFullDB.config"

run_sql_file() {
    FILE=$1
    DB="${2:-}"
    # -p -s -r -N
    sed -i "s/mangos'@'localhost/$MYSQL_USER'@'%/g" $FILE
    sed -i "s/'mangos'@'localhost' IDENTIFIED BY 'mangos';/'mangos'@'localhost' IDENTIFIED BY '$MYSQL_PASSWORD';/g" $FILE
    sed -i "s/tbcmangos;/$MYSQL_MANGOS_DB;/g" $FILE
    sed -i "s/tbccharacters;/$MYSQL_CHARS_TABLE;/g" $FILE
    sed -i "s/tbcrealmd;/$MYSQL_REALM_DB;/g" $FILE
    
    echo "Executing '$FILE'"
    mysql --defaults-file=my.cnf --database="$DB" < $FILE
}


echo "Initializing configuration."

# replace localhost with db as per docker-compose service name
cp -u /mangos/tmp/*.conf /mangos/etc/

# copy all conf files to mounted host config directory (does not overwrite)
cd /mangos/etc
sed -i "s|127.0.0.1;3306;mangos;mangos;|$MYSQL_HOST;$MYSQL_PORT;$MYSQL_USER;$MYSQL_PASSWORD;|g" *.conf
sed -i 's|DataDir = "."|DataDir = "/mangos/data"|g' *.conf
sed -i 's|LogsDir = ""|LogsDir = "/mangos/logs"|g' *.conf
sed -i 's|Ra.Enable = 0|Ra.Enable = 1|g' mangosd.conf
sed -i 's|Ra.Enable = 0|Ra.Enable = 1|g' mangosd.conf
sed -i 's|Ra.Restricted = 1|Ra.Restricted = 0|g' mangosd.conf

# save sql parameters to config file
cd /mangos/sql

echo "[client]" > my.cnf
echo "host=$MYSQL_HOST" >> my.cnf
echo "port=$MYSQL_PORT" >> my.cnf
echo "user=$MYSQL_ROOT_USER" >> my.cnf
echo "password=$MYSQL_ROOT_PASSWORD" >> my.cnf

# Wait for DB to start
while mysql --defaults-file=my.cnf -e 'SHOW DATABASES;' 2>&1 | grep -q -io 'ERROR'
do
    echo "Unable to connect to database. Waiting for connection."
    sleep 5
done

echo "Connected to database. Proceeding."

if [ "$MANUAL_BACKUP" = true ] ; then
    echo "Creating complete mysql server backup.";
    /mangos/realmd -c /mangos/etc/realmd.conf
else
    echo "Starting realmd";
    /mangos/bin/mangosd -c /mangos/etc/mangosd.conf -a /mangos/etc/playerbot.conf
fi

if ! mysql --defaults-file=my.cnf  -e 'SHOW DATABASES;' | grep -q -io $MYSQL_MANGOS_DB; then
    echo "Datases are not present. Initializing data structure."
    
    # fix error in newer MYSQL versions.
    mysql --defaults-file=my.cnf -e "SET SQL_REQUIRE_PRIMARY_KEY = OFF;"
    
    run_sql_file "/mangos/sql/create/db_create_mysql.sql"
    run_sql_file "/mangos/sql/base/mangos.sql" $MYSQL_MANGOS_DB
    
    for f in /mangos/sql/base/dbc/original_data/*.sql; do
        run_sql_file $f $MYSQL_MANGOS_DB
    done
    
    for f in /mangos/sql/base/dbc/cmangos_fixes/*.sql; do
        run_sql_file $f $MYSQL_MANGOS_DB
    done
    
    run_sql_file "/mangos/sql/base/characters.sql" $MYSQL_CHARS_TABLE
    run_sql_file "/mangos/sql/base/logs.sql" $MYSQL_REALM_DB
    run_sql_file "/mangos/sql/base/realmd.sql" $MYSQL_REALM_DB
fi

if [ "$INITIALIZE_DATA" = true ] ; then
    echo 'Initialization is enabled. Repopulating.'
    cd /mangos/db
    
    cat > $CONFIG_FILE << EOF
DB_HOST=$MYSQL_HOST
DB_PORT=$MYSQL_PORT
DATABASE=$MYSQL_MANGOS_DB
USERNAME=$MYSQL_USER
PASSWORD=$MYSQL_PASSWORD
CORE_PATH=../
MYSQL="mysql"
FORCE_WAIT="NO"
DEV_UPDATES="NO"
AHBOT="YES"
EOF
    echo "Running installer script."
    ./InstallFullDB.sh
fi

echo "Setup finished. Running server."

for i in {1..10}
do
    echo -ne .
    sleep 1
done
echo .

echo "Database is ready. Updating realmlist.";

mysql --defaults-file=my.cnf -e "DELETE FROM $MYSQL_CLASSIC_REALM_DB.realmlist WHERE id=1;"
mysql --defaults-file=my.cnf -e "INSERT INTO $MYSQL_CLASSIC_REALM_DB.realmlist (id, name, address, port, icon, realmflags, timezone, allowedSecurityLevel) VALUES ('1', '$REALM_ENTRY_NAME', '$REALM_ENTRY_HOST', '$REALM_ENTRY_PORT', '$REALM_ENTRY_ICON', '$REALM_ENTRY_FLAGS', '$REALM_ENTRY_TIMEZONE', '$REALM_ENTRY_ALLOWED_SEC_LEVEL');"

echo "Realmlist updated.";

# Run realmd
cd /mangos/bin
if [ "$RUN_MANGOSD" = true ] ; then
    echo "Starting mangosd";
    /mangos/bin/mangosd -c /mangos/etc/mangosd.conf -a /mangos/etc/playerbot.conf
else
    echo "Starting realmd";
    /mangos/realmd -c /mangos/etc/realmd.conf
fi