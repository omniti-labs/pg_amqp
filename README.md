pg_amqp 0.4.0
=============

The pg_amqp package provides the ability for postgres statements to directly
publish messages to an [AMQP](http://www.amqp.org/) broker. Version 0.4.0 also provides an implementation 
for secured connections using open ssl. To build v0.4.0 openssl dev package is required.

Building
--------

To build pg_amqp, just do this:

    make
    make install

If you encounter an error such as:

    "Makefile", line 8: Need an operator

You need to use GNU make, which may well be installed on your system as
`gmake`:

    gmake
    gmake install

If you encounter an error such as:

    make: pg_config: Command not found

Be sure that you have `pg_config` installed and in your path. If you used a
package management system such as RPM to install PostgreSQL, be sure that the
`-devel` package is also installed. If necessary tell the build process where
to find it:

    env PG_CONFIG=/path/to/pg_config make && make install

Loading
-------

Once amqp is installed, you can add it to a database. Add this line to your
postgresql config

    shared_preload_libraries = 'pg_amqp.so'

Then, If you're running PostgreSQL 9.1.0 or greater, loading amqp is as simple
as connecting to a database as a super user and running:

    CREATE EXTENSION amqp;

If you've upgraded your cluster to PostgreSQL 9.1 and already had amqp
installed, you can upgrade it to a properly packaged extension with:

    CREATE EXTENSION amqp FROM unpackaged;

For versions of PostgreSQL less than 9.1.0, you'll need to run the
installation script:

    psql -d mydb -f /path/to/pgsql/share/contrib/amqp.sql

Using
-----

Insert AMQP broker information (host/port/user/pass) into the
`amqp.broker` table.

A process starts and connects to PostgreSQL and runs:

    SELECT amqp.publish(broker_id, 'amqp.direct', 'foo', 'message');

Upon process termination, all broker connections will be torn down.
If there is a need to disconnect from a specific broker, one can call:

    select amqp.disconnect(broker_id);

which will disconnect from the broker if it is connected and do nothing
if it is already disconnected.

