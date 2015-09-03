CREATE TABLE @extschema@.broker (
  broker_id serial NOT NULL,
  host text NOT NULL,
  port integer NOT NULL DEFAULT 5672,
  vhost text,
  username text,
  password text,
  PRIMARY KEY (broker_id, host, port)
);


