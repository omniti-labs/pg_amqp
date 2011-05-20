amqp 0.3.0
==========

Synopsis
--------

    % CREATE EXTENSION amqp;
    CREATE EXTENSION

    % SELECT amqp.publish(broker_id, 'amqp.direct', 'foo', 'message');

Description
-----------

The pg_amqp package provides the ability for postgres statements to directly
publish messages to an [AMQP](http://www.amqp.org/) broker.

Usage
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

Support
-------

This library is stored in an open [GitHub
repository](http://github.com/omniti-labs/pg_amqp). Feel free to fork and
contribute! Please file bug reports via [GitHub
Issues](http://github.com/omniti-labs/pg_amqp/issues/).

Author
------

[Theo Schlossnagle](http://lethargy.org/~jesus/)
