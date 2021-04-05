tcc \
	-g \
	-I . \
	-lssl \
	-lcrypto \
	-lsqlite3 \
	-D MG_ENABLE_OPENSSL=1 \
	-D MG_ENABLE_LOG=0 \
	main.c \
	mongoose.c \
	simclist.c \
	termbox.c \
	utf8.c \
	-run

