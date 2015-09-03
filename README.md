pg_amqp 0.3.0
=============

The pg_amqp package provides the ability for postgres statements to directly
publish messages to an [AMQP](http://www.amqp.org/) broker.

All bug reports, feature requests and general questions can be directed to the Issues section on Github. - http://github.com/omniti-labs/pg_amqp


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

Some prepackaged Mac installs of postgres might need a little coaxing with
modern XCodes.  If you encounter an error such as:

    make: /Applications/Xcode.app/Contents/Developer/Toolchains/OSX10.8.xctoolchain/usr/bin/cc: No such file or directory

Then you'll need to link the toolchain

    sudo ln -s /Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain /Applications/Xcode.app/Contents/Developer/Toolchains/OSX10.8.xctoolchain

And if you encounter an error about a missing `/usr/bin/postgres`:

    ld: file not found: /usr/bin/postgres

You might need to link in your real postgres:

    sudo ln -s /usr/bin/postgres_real /usr/bin/postgres

Loading
-------

Once amqp is installed, you can add it to a database. Add this line to your
postgresql config

    shared_preload_libraries = 'pg_amqp.so'

This extension requires PostgreSQL 9.1.0 or greater, so loading amqp is as simple
as connecting to a database as a super user and running 
(currently requires being in amqp schema):

    CREATE EXTENSION amqp SCHEMA amqp;

If you've upgraded your cluster to PostgreSQL 9.1 and already had amqp
installed, you can upgrade it to a properly packaged extension with:

    CREATE EXTENSION amqp FROM unpackaged;

This is required to update to any versions >= 0.4.0.

To update to the latest version, run the following command after running "make install" again:

    ALTER EXTENSION amqp UPDATE;

Basic Usage
-----------

Insert AMQP broker information (host/port/user/pass) into the
`amqp.broker` table.

A process starts and connects to PostgreSQL and runs:

    SELECT amqp.publish(broker_id, 'amqp.direct', 'foo', 'message');

Upon process termination, all broker connections will be torn down.
If there is a need to disconnect from a specific broker, one can call:

    select amqp.disconnect(broker_id);

which will disconnect from the broker if it is connected and do nothing
if it is already disconnected.

