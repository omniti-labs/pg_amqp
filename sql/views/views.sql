CREATE OR REPLACE VIEW @extschema@.broker AS
select * from json_to_recordset(current_setting('amqp.broker')::json)
as x(broker_id int, host text, port int, vhost text, username text, password text);
