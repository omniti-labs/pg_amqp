BEGIN;
create schema amqp;

create function amqp.publish(integer, varchar, varchar, varchar)
returns boolean as 'pg_amqp.so', 'pg_amqp_publish'
language C immutable;

create function amqp.disconnect(integer)
returns void as 'pg_amqp.so', 'pg_amqp_disconnect'
language C immutable strict;

create table amqp.broker (
  broker_id serial not null primary key,
  host text,
  port integer,
  vhost text,
  username text,
  password text
);

COMMIT;
