MODULE_big	= pg_amqp
OBJS		= pg_amqp.o \
	librabbitmq/amqp_api.o librabbitmq/amqp_connection.o librabbitmq/amqp_debug.o \
	librabbitmq/amqp_framing.o librabbitmq/amqp_mem.o librabbitmq/amqp_socket.o \
	librabbitmq/amqp_table.o
DOCS		= README.pg_amqp
DATA		= uninstall_pg_amqp.sql pg_amqp.sql

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_amqp
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

