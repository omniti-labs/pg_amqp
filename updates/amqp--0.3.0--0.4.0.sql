-- Add support for commonly used amqp message properties (delivery_mode, content_type, reply_to and correlation_id) for publish() and autonomous_publish(). Many thanks to netalien for the contribution. (Github PR #13).
-- This software is now only supported as a PostgreSQL Extension. PG versions prior to 9.1 and means to install it without the extension system are no longer officially supported. PostgreSQL 9.0 is EOL as of Sept 2015 (http://www.postgresql.org/support/versioning/)
-- Added support for upgrading extension installation from 0.3.0 to 0.4.0. If pg_amqp is not installed as an extension, you must turn it into one before updating. See the README for instructions on doing this.
-- Named all SQL function parameters so explicit value setting can be used.

-- TODO Retain privileges of dropped functions

CREATE TEMP TABLE amqp_preserve_privs_temp (statement text);

INSERT INTO amqp_preserve_privs_temp 
SELECT 'GRANT EXECUTE ON FUNCTION @extschema@.autonomous_publish(integer, varchar, varchar, varchar, integer, varchar, varchar, varchar) TO '||array_to_string(array_agg(grantee::text), ',')||';' 
FROM information_schema.routine_privileges
WHERE routine_schema = '@extschema@'
AND routine_name = 'autonomous_publish'; 

INSERT INTO amqp_preserve_privs_temp 
SELECT 'GRANT EXECUTE ON FUNCTION @extschema@.disconnect(integer) TO '||array_to_string(array_agg(grantee::text), ',')||';' 
FROM information_schema.routine_privileges
WHERE routine_schema = '@extschema@'
AND routine_name = 'disconnect'; 

INSERT INTO amqp_preserve_privs_temp 
SELECT 'GRANT EXECUTE ON FUNCTION @extschema@.exchange_declare(integer, varchar, varchar, boolean, boolean, boolean) TO '||array_to_string(array_agg(grantee::text), ',')||';' 
FROM information_schema.routine_privileges
WHERE routine_schema = '@extschema@'
AND routine_name = 'exchange_declare'; 

INSERT INTO amqp_preserve_privs_temp 
SELECT 'GRANT EXECUTE ON FUNCTION @extschema@.publish(integer, varchar, varchar, varchar, integer, varchar, varchar, varchar) TO '||array_to_string(array_agg(grantee::text), ',')||';' 
FROM information_schema.routine_privileges
WHERE routine_schema = '@extschema@'
AND routine_name = 'publish'; 

DROP FUNCTION @extschema@.autonomous_publish(integer, varchar, varchar, varchar);
DROP FUNCTION @extschema@.disconnect(integer);
DROP FUNCTION @extschema@.exchange_declare(integer, varchar, varchar, boolean, boolean, boolean);
DROP FUNCTION @extschema@.publish(integer, varchar, varchar, varchar);

CREATE FUNCTION @extschema@.autonomous_publish(
    broker_id integer
    , exchange varchar
    , routing_key varchar
    , message varchar
    , delivery_mode integer default null
    , content_type varchar default null
    , reply_to varchar default null
    , correlation_id varchar default null
)

RETURNS boolean AS 'pg_amqp.so', 'pg_amqp_autonomous_publish'
LANGUAGE C IMMUTABLE;

COMMENT ON FUNCTION @extschema@.autonomous_publish(integer, varchar, varchar, varchar, integer, varchar, varchar, varchar) IS
'Works as amqp.publish does, but the message is published immediately irrespective of the
current transaction state.  PostgreSQL commit and rollback at a later point will have no
effect on this message being sent to AMQP.';


CREATE FUNCTION amqp.disconnect(broker_id integer)
RETURNS void AS 'pg_amqp.so', 'pg_amqp_disconnect'
LANGUAGE C IMMUTABLE STRICT;

COMMENT ON FUNCTION amqp.disconnect(integer) IS
'Explicitly disconnect the specified (broker_id) if it is current connected. Broker
connections, once established, live until the PostgreSQL backend terminated.  This
allows for more precise control over that.
select amqp.disconnect(broker_id) from amqp.broker
will disconnect any brokers that may be connected.';


CREATE FUNCTION amqp.exchange_declare(
    broker_id integer
    , exchange varchar 
    , exchange_type varchar
    , passive boolean
    , durable boolean
    , auto_delete boolean DEFAULT false
)
RETURNS boolean AS 'pg_amqp.so', 'pg_amqp_exchange_declare'
LANGUAGE C IMMUTABLE;

COMMENT ON FUNCTION amqp.exchange_declare(integer, varchar, varchar, boolean, boolean, boolean) IS
'Declares a exchange (broker_id, exchange_name, exchange_type, passive, durable, auto_delete)
auto_delete should be set to false (default) as unexpected errors can cause disconnect/reconnect which
would trigger the auto deletion of the exchange.';


CREATE FUNCTION @extschema@.publish(
    broker_id integer
    , exchange varchar
    , routing_key varchar
    , message varchar
    , delivery_mode integer default null
    , content_type varchar default null
    , reply_to varchar default null
    , correlation_id varchar default null
)
RETURNS boolean AS 'pg_amqp.so', 'pg_amqp_publish'
LANGUAGE C IMMUTABLE;

COMMENT ON FUNCTION @extschema@.publish(integer, varchar, varchar, varchar, integer, varchar, varchar, varchar) IS
'Publishes a message (broker_id, exchange, routing_key, message). 
The message will only be published if the containing PostgreSQL transaction successfully commits.  
Under certain circumstances, the AMQP commit might fail.  In this case, a WARNING is emitted. 
The last four parameters are optional and set the following message properties: 
delivery_mode (either 1 or 2), content_type, reply_to and correlation_id.

Publish returns a boolean indicating if the publish command was successful.  Note that as
AMQP publish is asynchronous, you may find out later it was unsuccessful.';

-- Restore dropped object privileges
DO $$
DECLARE
v_row   record;
BEGIN
    FOR v_row IN SELECT statement FROM amqp_preserve_privs_temp LOOP
        IF v_row.statement IS NOT NULL THEN
            EXECUTE v_row.statement;
        END IF;
    END LOOP;
END
$$;

DROP TABLE IF EXISTS amqp_preserve_privs_temp;
