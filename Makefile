EXTENSION    = amqp
EXTVERSION   = $(shell grep default_version $(EXTENSION).control | \
               sed -e "s/default_version[[:space:]]*=[[:space:]]*'\([^']*\)'/\1/")
DATA         = $(filter-out $(wildcard sql/*--*.sql),$(wildcard sql/*.sql))
DOCS         = $(wildcard doc/*.*)
TESTS        = $(wildcard test/sql/*.sql)
REGRESS      = $(patsubst test/sql/%.sql,%,$(TESTS))
REGRESS_OPTS = --inputdir=test
MODULE_big   = $(patsubst %.c,%,$(wildcard src/*.c))
OBJS         = src/pg_amqp.o \
	src/librabbitmq/amqp_api.o src/librabbitmq/amqp_connection.o src/librabbitmq/amqp_debug.o \
	src/librabbitmq/amqp_framing.o src/librabbitmq/amqp_mem.o src/librabbitmq/amqp_socket.o \
	src/librabbitmq/amqp_table.o

PG_CONFIG    = pg_config
PG91         = $(shell $(PG_CONFIG) --version | grep -qE " 8\.| 9\.0" && echo no || echo yes)

ifeq ($(PG91),yes)
all: sql/$(EXTENSION)--$(EXTVERSION).sql

sql/$(EXTENSION)--$(EXTVERSION).sql: sql/$(EXTENSION).sql
# strip first two lines (BEGIN, CREATE SCHEMA) and last line (COMMIT).
	sed '1,2d;$$d' $< > $@

DATA = $(wildcard sql/*--*.sql) sql/$(EXTENSION)--$(EXTVERSION).sql
EXTRA_CLEAN = sql/$(EXTENSION)--$(EXTVERSION).sql
endif

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
