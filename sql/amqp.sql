BEGIN;
create schema amqp;

create function amqp.exchange_declare(integer, varchar, varchar, boolean, boolean, boolean)
returns boolean as 'pg_amqp.so', 'pg_amqp_exchange_declare'
language C immutable;

comment on function amqp.exchange_declare(integer, varchar, varchar, boolean, boolean, boolean) is
'Declares a exchange (broker_id, exchange_name, exchange_type, passive, durable, auto_delete)
auto_delete should be set to false as unexpected errors can cause disconnect/reconnect which
would trigger the auto deletion of the exchange.';

create function amqp.publish(integer, varchar, varchar, varchar, integer default null, 
			     varchar default null, varchar default null, varchar default null)
returns boolean as 'pg_amqp.so', 'pg_amqp_publish'
language C immutable;

comment on function amqp.publish(integer, varchar, varchar, varchar, integer,
				 varchar, varchar, varchar) is
'Publishes a message (broker_id, exchange, routing_key, message).  The message will only
be published if the containing PostgreSQL transaction successfully commits.  Under certain
circumstances, the AMQP commit might fail.  In this case, a WARNING is emitted. The last
four parameters are optional and set the following message properties: delivery_mode (either 1 or 2), 
content_type, reply_to and correlation_id.

Publish returns a boolean indicating if the publish command was successful.  Note that as
AMQP publish is asynchronous, you may find out later it was unsuccessful.';

create function amqp.autonomous_publish(integer, varchar, varchar, varchar, integer default null,
					varchar default null, varchar default null, varchar default null)
returns boolean as 'pg_amqp.so', 'pg_amqp_autonomous_publish'
language C immutable;

comment on function amqp.autonomous_publish(integer, varchar, varchar, varchar, integer,
					    varchar, varchar, varchar) is
'Works as amqp.publish does, but the message is published immediately irrespective of the
current transaction state.  PostgreSQL commit and rollback at a later point will have no
effect on this message being sent to AMQP.';

create function amqp.disconnect(integer)
returns void as 'pg_amqp.so', 'pg_amqp_disconnect'
language C immutable strict;

comment on function amqp.disconnect(integer) is
'Explicitly disconnect the specified (broker_id) if it is current connected. Broker
connections, once established, live until the PostgreSQL backend terminated.  This
allows for more precise control over that.
select amqp.disconnect(broker_id) from amqp.broker
will disconnect any brokers that may be connected.';

create table amqp.broker (
  broker_id serial not null,
  host text not null,
  port integer not null default 5672,
  vhost text,
  username text,
  password text,
  primary key(broker_id, host, port)
);

COMMIT;
