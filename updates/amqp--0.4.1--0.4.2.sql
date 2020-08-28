ALTER TABLE @extschema@.broker
ADD COLUMN requiressl boolean DEFAULT false,
ADD COLUMN verify_cert boolean DEFAULT true,
ADD COLUMN verify_cn boolean DEFAULT true,
ADD COLUMN cert text,
ADD COLUMN key text,
ADD COLUMN key_password character varying,
ADD COLUMN ca text;
