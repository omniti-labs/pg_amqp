ALTER EXTENSION amqp ADD table @extschema@.broker;
ALTER EXTENSION amqp ADD sequence @extschema@.broker_broker_id_seq;
ALTER EXTENSION amqp ADD function @extschema@.disconnect(integer);
ALTER EXTENSION amqp ADD function @extschema@.autonomous_publish(integer,character varying,character varying,character varying, integer default null,
								 varchar default null, varchar default null, varchar default null);
ALTER EXTENSION amqp ADD function @extschema@.publish(integer,character varying,character varying,character varying, integer default null,
						      varchar default null, varchar default null, varchar default null);
ALTER EXTENSION amqp ADD function @extschema@.exchange_declare(integer,character varying,character varying,boolean,boolean,boolean);
