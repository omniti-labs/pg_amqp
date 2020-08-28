EXTENSION    = amqp
EXTVERSION   = $(shell grep default_version $(EXTENSION).control | \
               sed -e "s/default_version[[:space:]]*=[[:space:]]*'\([^']*\)'/\1/")
PG_CONFIG    = pg_config
PG91         = $(shell $(PG_CONFIG) --version | grep -qE " 8\.| 9\.0" && echo no || echo yes)

ifeq ($(PG91),yes)
DOCS         = $(wildcard doc/*.*)
#TESTS        = $(wildcard test/sql/*.sql)
#REGRESS      = $(patsubst test/sql/%.sql,%,$(TESTS))
#REGRESS_OPTS = --inputdir=test
MODULE_big   = $(patsubst src/%.c,%,$(wildcard src/*.c))
OBJS         = src/pg_amqp.o \
	src/librabbitmq/amqp_api.o src/librabbitmq/amqp_connection.o src/librabbitmq/amqp_debug.o \
	src/librabbitmq/amqp_framing.o src/librabbitmq/amqp_mem.o src/librabbitmq/amqp_socket.o \
	src/librabbitmq/amqp_table.o


all: sql/$(EXTENSION)--$(EXTVERSION).sql

sql/$(EXTENSION)--$(EXTVERSION).sql: sql/tables/*.sql sql/functions/*.sql
	cat $^ > $@

DATA = $(wildcard updates/*--*.sql) sql/$(EXTENSION)--$(EXTVERSION).sql
EXTRA_CLEAN = sql/$(EXTENSION)--$(EXTVERSION).sql
else
$(error Minimum version of PostgreSQL required is 9.1.0)
endif

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
