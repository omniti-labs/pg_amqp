-- Incorporate the latest stable version of the librabbitmq library (0.7.1) with some bug fixes as of March 30, 2016 (Github Pull Request #18).
-- Set config table data to be dumped on pg_dump

SELECT pg_catalog.pg_extension_config_dump('broker', '');
