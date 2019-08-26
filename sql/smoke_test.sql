CREATE EXTENSION amqp;
INSERT INTO amqp.broker (broker_id, host, port, vhost, username, password) VALUES ('10000', '127.0.0.1', '5672', '/', 'guest', 'guest');
SELECT amqp.publish(10000, 'amq.direct', 'foo', 'message');
SELECT amqp.publish(10000, 'amq.direct', 'foo', 'message', 1, 'application/json', 'some_reply_to', 'correlation_id');
DELETE FROM amqp.broker WHERE broker_id = '10000';