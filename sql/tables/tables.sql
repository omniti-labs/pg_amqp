CREATE TABLE @extschema@.broker (
  broker_id serial NOT NULL,
  host text NOT NULL,
  port integer NOT NULL DEFAULT 5672,
  vhost text,
  username text,
  password text,
  requiressl boolean DEFAULT false,
  verify_cert boolean DEFAULT true,
  verify_cn boolean DEFAULT true,
  cert text,
  key text,
  key_password character varying,
  ca text,
  PRIMARY KEY (broker_id, host, port)
);


